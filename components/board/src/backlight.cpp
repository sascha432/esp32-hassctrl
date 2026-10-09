#include "backlight.hpp"

#include <algorithm>
#include <cmath>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_system.h"

namespace hc::board::detail {

namespace {

constexpr const char *kTag = "backlight";
constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_0;
// 10 bits work up to 78 kHz with the 80 MHz clock
constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_10_BIT;
constexpr uint16_t kMaxDuty = (1u << 10) - 1;

bool g_ready = false;
gpio_num_t g_pin = GPIO_NUM_NC;
// the panel that is switched off before a restart, nullptr until set_restart_panel()
esp_lcd_panel_handle_t g_panel = nullptr;
// lowest readable duty (calibration), 1..100 % are spread from here to full brightness
uint16_t g_min_duty = 0;

// Share of full luminance that looks like `percent` of full brightness: the inverse of the CIE
// 1976 lightness L*, with L* = percent.
float perceptual(uint8_t percent)
{
    const float lightness = static_cast<float>(percent);
    if (lightness <= 8.0f) {
        return lightness / 903.3f;
    }
    const float t = (lightness + 16.0f) / 116.0f;
    return t * t * t;
}

// the duty set now, for backlight_level()
uint16_t g_duty = 0;

void write_duty(uint16_t duty)
{
    g_duty = duty;
    ledc_set_duty(kMode, kChannel, duty);
    ledc_update_duty(kMode, kChannel);
}

// esp_restart() (Settings, an update, the console): the backlight goes dark before the chip
// resets, and where the chip keeps a pad's hold through a software reset, the pin stays low until
// backlight_init() of the next start. The ESP32-P4 does not keep it: its pin is undriven for the
// 1.3 s until then and the backlight lights the ST7701S, which shows cyan without a signal. So the
// panel is switched off and put to sleep as well: black, whatever the backlight does, until the
// next start resets it.
void off_before_restart()
{
    ledc_stop(kMode, kChannel, 0);
    gpio_hold_en(g_pin);
    if (g_panel != nullptr) {
        esp_lcd_panel_disp_on_off(g_panel, false);
        esp_lcd_panel_disp_sleep(g_panel, true);
    }
}

} // namespace

esp_err_t backlight_init(gpio_num_t pin, uint32_t hz, uint16_t min_duty)
{
    g_min_duty = std::min(min_duty, kMaxDuty);
    g_pin = pin;
    // held low by off_before_restart() of the last run
    gpio_hold_dis(pin);
    ledc_timer_config_t timer{};
    timer.speed_mode = kMode;
    timer.duty_resolution = kResolution;
    timer.timer_num = kTimer;
    timer.freq_hz = hz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kTag, "timer");

    ledc_channel_config_t channel{};
    channel.gpio_num = pin;
    channel.speed_mode = kMode;
    channel.channel = kChannel;
    channel.timer_sel = kTimer;
    channel.duty = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), kTag, "channel");

    g_ready = true;
    ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(off_before_restart), kTag, "shutdown handler");
    return ESP_OK;
}

void backlight_set(uint8_t percent, BacklightCurve curve)
{
    if (!g_ready) {
        return;
    }
    percent = std::min<uint8_t>(percent, 100);
    uint16_t duty = 0;
    if (percent > 0) {
        const float share = (curve == BacklightCurve::Perceptual) ? perceptual(percent) : percent / 100.0f;
        // 1 % is the calibrated minimum, never darker, and never off
        const float span = static_cast<float>(kMaxDuty - g_min_duty);
        duty = static_cast<uint16_t>(g_min_duty + std::lround(share * span));
        duty = std::max<uint16_t>(duty, std::max<uint16_t>(g_min_duty, 1));
    }
    write_duty(duty);
    ESP_LOGD(kTag, "%u %% %s, duty %u/%u", percent, curve == BacklightCurve::Perceptual ? "perceptual" : "linear",
             duty, kMaxDuty);
}

void backlight_set_minimum(uint16_t duty)
{
    g_min_duty = std::min(duty, kMaxDuty);
}

uint16_t backlight_minimum()
{
    return g_min_duty;
}

void backlight_set_duty(uint16_t duty)
{
    if (g_ready) {
        write_duty(std::min(duty, kMaxDuty));
    }
}

uint16_t backlight_max_duty()
{
    return kMaxDuty;
}

float backlight_level()
{
    return static_cast<float>(g_duty) / static_cast<float>(kMaxDuty);
}

void set_restart_panel(esp_lcd_panel_handle_t panel)
{
    g_panel = panel;
}

esp_err_t backlight_set_frequency(uint32_t hz)
{
    ESP_RETURN_ON_FALSE(g_ready, ESP_ERR_INVALID_STATE, kTag, "not initialized");
    return ledc_set_freq(kMode, kTimer, hz);
}

} // namespace hc::board::detail
