#include "hc/touch_input/touch_input.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace hc::touch_input {

namespace {

constexpr const char *kTag = "touch";
// Changes waiting for LVGL: about a second of moving finger.
constexpr size_t kQueueSize = 96;
// about 1.5 KB used (JC3248W535, 2026-10-17)
constexpr uint32_t kStackSize = 2048;
// above the LVGL task (4): it samples while LVGL renders
constexpr UBaseType_t kPriority = 5;
constexpr uint8_t kFingers = 2;

struct Sample {
    bool pressed;
    lv_coord_t x;
    lv_coord_t y;
};

// the fingers the controller reports now, in its coordinates
struct Fingers {
    uint8_t count;
    lv_point_t point[kFingers];
};

struct State {
    Config config{};
    esp_lcd_touch_handle_t touch = nullptr;
    std::mutex lock;
    Sample queue[kQueueSize]{};
    size_t head = 0;
    size_t count = 0;
    // what LVGL got last: repeated while nothing changes
    Sample last{};
    // LVGL's pointer follows the finger that came down first, by its id: the controller's list
    // can change its order (the FT6336U's did)
    uint8_t pointer_id = 0;
    Fingers fingers{};
    lv_indev_drv_t driver{};
};

State g;
std::atomic<bool> g_trace{false};

void push(const Sample &sample)
{
    std::lock_guard<std::mutex> guard(g.lock);
    if (g.count == kQueueSize) {
        // LVGL fell behind by a second: the newest move gives way (a release after it keeps its
        // own point). Only a full queue of presses and releases loses one, which a finger cannot
        // make.
        Sample &newest = g.queue[(g.head + g.count - 1) % kQueueSize];
        if (newest.pressed) {
            newest = sample;
        }
        return;
    }
    g.queue[(g.head + g.count) % kQueueSize] = sample;
    g.count++;
}

bool pop(Sample &out)
{
    std::lock_guard<std::mutex> guard(g.lock);
    if (g.count == 0) {
        return false;
    }
    out = g.queue[g.head];
    g.head = (g.head + 1) % kQueueSize;
    g.count--;
    return true;
}

// one sample of the controller: `previous` when the read failed or was a misread
Sample sample(const Sample &previous)
{
    Sample now = previous;
    const esp_err_t read = esp_lcd_touch_read_data(g.touch);
    if (read != ESP_OK && g_trace) {
        std::printf("touch %lu: read failed, %s\n", static_cast<unsigned long>(xTaskGetTickCount()),
                    esp_err_to_name(read));
    }
    esp_lcd_touch_point_data_t points[kFingers]{};
    uint8_t count = 0;
    if (read != ESP_OK || esp_lcd_touch_get_data(g.touch, points, &count, kFingers) != ESP_OK) {
        return now;
    }
    if (g_trace && (count != 0 || previous.pressed)) {
        std::printf("touch %lu: %u point(s) %u,%u", static_cast<unsigned long>(xTaskGetTickCount()), count,
                    points[0].x, points[0].y);
        if (count > 1) {
            std::printf(" and %u,%u", points[1].x, points[1].y);
        }
        std::printf("\n");
    }
    for (uint8_t i = 0; i < count; i++) {
        if (points[i].x > g.config.max_x || points[i].y > g.config.max_y) {
            return now;
        }
    }
    {
        std::lock_guard<std::mutex> guard(g.lock);
        g.fingers.count = count;
        for (uint8_t i = 0; i < count; i++) {
            g.fingers.point[i] = {static_cast<lv_coord_t>(points[i].x), static_cast<lv_coord_t>(points[i].y)};
        }
    }
    if (count == 0) {
        return {};
    }
    const esp_lcd_touch_point_data_t *pointer = &points[0];
    for (uint8_t i = 1; previous.pressed && i < count; i++) {
        if (points[i].track_id == g.pointer_id) {
            pointer = &points[i];
        }
    }
    if (!previous.pressed || pointer->track_id != g.pointer_id) {
        g.pointer_id = pointer->track_id;
    }
    return {true, static_cast<lv_coord_t>(pointer->x), static_cast<lv_coord_t>(pointer->y)};
}

void sample_task(void *)
{
    Sample previous{};
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(g.config.sample_ms));
        const Sample now = sample(previous);
        if (now.pressed != previous.pressed || (now.pressed && (now.x != previous.x || now.y != previous.y))) {
            push(now);
            previous = now;
        }
    }
}

void read(lv_indev_drv_t *, lv_indev_data_t *data)
{
    Sample next{};
    if (pop(next)) {
        if (next.pressed) {
            g.last = next;
        } else {
            g.last.pressed = false; // a release keeps the last point
        }
        std::lock_guard<std::mutex> guard(g.lock);
        data->continue_reading = g.count > 0;
    }
    data->point.x = g.last.x;
    data->point.y = g.last.y;
    data->state = g.last.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// a point of the controller as LVGL's pointer gets it: turned with the display
// (lv_indev.c, indev_pointer_proc)
lv_point_t turned(lv_point_t point)
{
    const lv_disp_drv_t *driver = g.config.disp->driver;
    if (driver->rotated == LV_DISP_ROT_180 || driver->rotated == LV_DISP_ROT_270) {
        point.x = static_cast<lv_coord_t>(driver->hor_res - point.x - 1);
        point.y = static_cast<lv_coord_t>(driver->ver_res - point.y - 1);
    }
    if (driver->rotated == LV_DISP_ROT_90 || driver->rotated == LV_DISP_ROT_270) {
        const lv_coord_t y = point.y;
        point.y = point.x;
        point.x = static_cast<lv_coord_t>(driver->ver_res - y - 1);
    }
    return point;
}

} // namespace

uint8_t fingers(lv_point_t out[2])
{
    if (g.touch == nullptr) {
        return 0;
    }
    Fingers now;
    {
        std::lock_guard<std::mutex> guard(g.lock);
        now = g.fingers;
    }
    for (uint8_t i = 0; i < now.count; i++) {
        out[i] = turned(now.point[i]);
    }
    return now.count;
}

void set_trace(bool on)
{
    g_trace = on;
}

bool trace()
{
    return g_trace;
}

lv_indev_t *add(const Config &config)
{
    if (g.touch != nullptr || config.touch == nullptr) {
        return nullptr;
    }
    g.config = config;
    g.touch = config.touch;
    if (xTaskCreate(sample_task, "touch", kStackSize, nullptr, kPriority, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "no memory for the task");
        g.touch = nullptr;
        return nullptr;
    }
    lv_indev_drv_init(&g.driver);
    g.driver.type = LV_INDEV_TYPE_POINTER;
    g.driver.disp = config.disp;
    g.driver.read_cb = read;
    return lv_indev_drv_register(&g.driver);
}

} // namespace hc::touch_input
