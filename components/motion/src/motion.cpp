#include "hc/motion/motion.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace hc::motion {

namespace {

constexpr const char *kTag = "motion";
// a PIR holds its output for seconds: sampling is enough, no interrupt
constexpr uint64_t kSampleUs = 100 * 1000;
// a level has to hold for this many samples before it is the state
constexpr unsigned kStableSamples = 2;

struct State {
    int gpio = -1;
    bool active_high = true;
    Handler on_change = nullptr;
    esp_timer_handle_t timer = nullptr;
    // written by the timer task (hold_s by any), read by any task
    volatile bool occupied = false;
    volatile bool detected = false;
    volatile uint32_t hold_s = kDefaultHoldS;
    bool started = false;
    // the last sample with motion (esp_timer_get_time)
    int64_t last_motion_us = 0;
    // the level seen last and how many samples in a row it held
    bool last_level = false;
    unsigned samples = 0;
};

State g;

bool read_occupied()
{
    const bool high = gpio_get_level(static_cast<gpio_num_t>(g.gpio)) != 0;
    return high == g.active_high;
}

void on_timer(void *)
{
    const bool level = read_occupied();
    if (level == g.last_level) {
        if (g.samples < kStableSamples) {
            g.samples++;
        }
    } else {
        g.last_level = level;
        g.samples = 1;
    }
    if (g.samples < kStableSamples) {
        return;
    }
    if (!g.started || level != g.occupied) {
        g.occupied = level;
        ESP_LOGD(kTag, "sensor %s", level ? "motion" : "none");
    }
    // motion turns it on at once and restarts the hold; it turns off a hold time after the last motion
    const int64_t now_us = esp_timer_get_time();
    if (level) {
        g.last_motion_us = now_us;
    }
    const bool detected =
        level || (g.started && g.detected && now_us - g.last_motion_us < static_cast<int64_t>(g.hold_s) * 1000000);
    if (g.started && detected == g.detected) {
        return;
    }
    g.detected = detected;
    g.started = true;
    ESP_LOGI(kTag, "%s", detected ? "detected" : "clear");
    if (g.on_change != nullptr) {
        g.on_change(detected);
    }
}

} // namespace

esp_err_t start(int gpio, bool active_high, Handler on_change)
{
    ESP_RETURN_ON_FALSE(g.timer == nullptr, ESP_ERR_INVALID_STATE, kTag, "started twice");
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(gpio), ESP_ERR_INVALID_ARG, kTag, "GPIO %d cannot be an input", gpio);
    g.gpio = gpio;
    g.active_high = active_high;
    g.on_change = on_change;

    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << gpio;
    io.mode = GPIO_MODE_INPUT;
    // pulled to the level that means clear: an unplugged sensor reads clear
    io.pull_up_en = active_high ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
    io.pull_down_en = active_high ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io), kTag, "GPIO %d", gpio);

    const esp_timer_create_args_t args{.callback = on_timer, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
                                       .name = "motion", .skip_unhandled_events = true};
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &g.timer), kTag, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(g.timer, kSampleUs), kTag, "timer start");
    ESP_LOGI(kTag, "sensor on GPIO %d, active %s", gpio, active_high ? "high" : "low");
    return ESP_OK;
}

bool running()
{
    return g.timer != nullptr;
}

bool occupied()
{
    return g.occupied;
}

bool detected()
{
    return g.detected;
}

void set_hold_s(uint32_t seconds)
{
    g.hold_s = seconds < kMinHoldS ? kMinHoldS : seconds > kMaxHoldS ? kMaxHoldS : seconds;
}

uint32_t hold_s()
{
    return g.hold_s;
}

int gpio()
{
    return g.timer != nullptr ? g.gpio : -1;
}

bool active_high()
{
    return g.active_high;
}

} // namespace hc::motion
