#include "hc/vibration/vibration.hpp"

#include <algorithm>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace hc::vibration {

namespace {

constexpr const char *kTag = "vibration";

constexpr uint16_t kMaxPulseMs = 2000;
// the alarm: half a second on, half a second off
constexpr uint16_t kAlarmOnMs = 500;
constexpr uint16_t kAlarmOffMs = 500;

struct State {
    gpio_num_t gpio = GPIO_NUM_NC;
    // the pulse of a tap, a constant of the board's motor
    uint16_t click_ms = 0;
    esp_timer_handle_t timer = nullptr;
    // the callers (the LVGL task, the console) and the timer's task
    SemaphoreHandle_t lock = nullptr;
    bool alarm = false;
    // ends the alarm when it returns false, checked before each pulse; nullptr: never
    bool (*active)() = nullptr;
    bool on = false;
    // when the timer that runs now is due (esp_timer_get_time())
    int64_t due_us = 0;
};

State g;

void set_motor(bool on)
{
    g.on = on;
    gpio_set_level(g.gpio, on ? 1 : 0);
}

// the timer (re)started for `ms`; under the lock
void schedule(uint16_t ms)
{
    const int64_t us = static_cast<int64_t>(ms) * 1000;
    g.due_us = esp_timer_get_time() + us;
    esp_timer_stop(g.timer);
    esp_timer_start_once(g.timer, us);
}

// the motor on, the timer restarted for `ms`; under the lock
void run_for(uint16_t ms)
{
    set_motor(true);
    schedule(ms);
}

void on_timer(void *)
{
    xSemaphoreTake(g.lock, portMAX_DELAY);
    // A pulse or the alarm restarted the timer while this call waited for the lock: it is early
    // for theirs, which comes later. (esp_timer_is_active() cannot tell: it is true in the
    // timer's own callback.)
    if (esp_timer_get_time() >= g.due_us) {
        if (g.alarm) {
            // a pulse that began is always played in full
            if (!g.on && g.active != nullptr && !g.active()) {
                g.alarm = false;
            } else {
                set_motor(!g.on);
                schedule(g.on ? kAlarmOnMs : kAlarmOffMs);
            }
        } else {
            set_motor(false);
        }
    }
    xSemaphoreGive(g.lock);
}

} // namespace

esp_err_t start(int gpio, uint16_t click_ms)
{
    ESP_RETURN_ON_FALSE(g.timer == nullptr, ESP_ERR_INVALID_STATE, kTag, "started twice");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(gpio), ESP_ERR_INVALID_ARG, kTag, "GPIO %d", gpio);
    // a board with a motor has its click measured (docs/adding-a-board.md)
    ESP_RETURN_ON_FALSE(click_ms > 0 && click_ms <= kMaxPulseMs, ESP_ERR_INVALID_ARG, kTag, "click %u ms",
                        static_cast<unsigned>(click_ms));
    g.gpio = static_cast<gpio_num_t>(gpio);
    g.click_ms = click_ms;
    // low before it becomes an output: the motor must not run while the board starts
    gpio_set_level(g.gpio, 0);
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << gpio;
    // input too, so motor_on() reads what the pin really drives
    io.mode = GPIO_MODE_INPUT_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&io), kTag, "GPIO %d", gpio);
    g.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g.lock != nullptr, ESP_ERR_NO_MEM, kTag, "lock");
    esp_timer_create_args_t args{};
    args.callback = on_timer;
    args.name = "vibration";
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &g.timer), kTag, "timer");
    ESP_LOGI(kTag, "motor on GPIO %d, click %u ms", gpio, static_cast<unsigned>(click_ms));
    return ESP_OK;
}

bool running()
{
    return g.timer != nullptr;
}

bool motor_on()
{
    return running() && gpio_get_level(g.gpio) != 0;
}

void pulse(uint16_t ms)
{
    if (!running() || ms == 0) {
        return;
    }
    xSemaphoreTake(g.lock, portMAX_DELAY);
    if (!g.alarm) {
        run_for(std::min(ms, kMaxPulseMs));
    }
    xSemaphoreGive(g.lock);
}

void click()
{
    pulse(g.click_ms);
}

void start_alarm(bool (*active)())
{
    if (!running()) {
        return;
    }
    xSemaphoreTake(g.lock, portMAX_DELAY);
    g.alarm = true;
    g.active = active;
    run_for(kAlarmOnMs);
    xSemaphoreGive(g.lock);
}

void stop_alarm()
{
    if (!running()) {
        return;
    }
    xSemaphoreTake(g.lock, portMAX_DELAY);
    g.alarm = false;
    esp_timer_stop(g.timer);
    set_motor(false);
    xSemaphoreGive(g.lock);
}

} // namespace hc::vibration
