#pragma once

// PWM backlight on one GPIO through the LEDC peripheral, shared by the board implementations.

#include <cstdint>

#include "esp_check.h"
#include "esp_lcd_types.h"
#include "hal/gpio_types.h"
#include "hc/board/board.hpp"

namespace hc::board::detail {

// Configures the pin with the backlight off, at the board's frequency and minimum. The backlight
// turns off by itself before esp_restart() resets the chip.
esp_err_t backlight_init(gpio_num_t pin, uint32_t hz, uint16_t min_duty);

void backlight_set(uint8_t percent, BacklightCurve curve);
void backlight_set_minimum(uint16_t duty);
uint16_t backlight_minimum();
void backlight_set_duty(uint16_t duty);
uint16_t backlight_max_duty();
float backlight_level();
esp_err_t backlight_set_frequency(uint32_t hz);

// the panel the restart switches off (display off, sleep) after the backlight; call once the
// panel is initialized
void set_restart_panel(esp_lcd_panel_handle_t panel);

} // namespace hc::board::detail
