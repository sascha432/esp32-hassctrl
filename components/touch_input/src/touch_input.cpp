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
constexpr TickType_t kSamplePeriod = pdMS_TO_TICKS(10);
// Changes waiting for LVGL: about a second of moving finger.
constexpr size_t kQueueSize = 96;
constexpr uint32_t kStackSize = 3072;
// above the LVGL task (4): it samples while LVGL renders
constexpr UBaseType_t kPriority = 5;

struct Sample {
    bool pressed;
    lv_coord_t x;
    lv_coord_t y;
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

void sample_task(void *)
{
    Sample previous{};
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, kSamplePeriod);
        Sample now = previous; // a failed read or a misread changes nothing
        const esp_err_t read = esp_lcd_touch_read_data(g.touch);
        if (read != ESP_OK && g_trace) {
            std::printf("touch %lu: read failed, %s\n", static_cast<unsigned long>(xTaskGetTickCount()),
                        esp_err_to_name(read));
        }
        if (read == ESP_OK) {
            esp_lcd_touch_point_data_t point{};
            uint8_t count = 0;
            if (esp_lcd_touch_get_data(g.touch, &point, &count, 1) == ESP_OK) {
                if (g_trace && (count != 0 || previous.pressed)) {
                    std::printf("touch %lu: %u point(s) %u,%u\n", static_cast<unsigned long>(xTaskGetTickCount()),
                                count, point.x, point.y);
                }
                if (count == 0) {
                    now = {};
                } else if (point.x <= g.config.max_x && point.y <= g.config.max_y) {
                    now = {true, static_cast<lv_coord_t>(point.x), static_cast<lv_coord_t>(point.y)};
                }
            }
        }
        if (now.pressed != previous.pressed || (now.pressed && (now.x != previous.x || now.y != previous.y))) {
            push(now);
            previous = now;
        }
    }
}

void read(lv_indev_drv_t *, lv_indev_data_t *data)
{
    Sample sample{};
    if (pop(sample)) {
        if (sample.pressed) {
            g.last = sample;
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

} // namespace

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
