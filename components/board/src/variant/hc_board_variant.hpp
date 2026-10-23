// The variant of a board (docs/adding-a-board.md "Variants"): one unit's add-ons. A private copy
// of this file in boards/<board>/variants/<name>/ (not in git) changes the fields of the board's
// Info that differ on that unit; the board file in the repository keeps an example of every
// add-on. tools/build.py <board>.<name> puts the copy on the include path instead of this one,
// which changes nothing. apply() runs at compile time: no cost in the firmware.
#pragma once

#include "hc/board/board.hpp"

namespace hc::board::variant {

constexpr Info apply(Info info)
{
    // info.variant = "kitchen";                   // logged at boot, shown in the System info
    // info.light_ldr_gpio = -1;                   // no LDR on this unit (an ADC1 GPIO: one)
    // info.vibration_gpio = 27;                   // the motor's GPIO, -1: none
    // info.vibration_click_ms = 15;               // console `vibrate <ms>`
    // info.relay_gpios = {32, -1, -1, -1};        // relays 1 to 4, -1: none
    // info.relay_pullup = {true, false, false, false}; // relay 1 on the internal pullup, closed at boot
    // info.motion_gpio = 34;                      // a motion sensor, -1: none
    // info.motion_active_high = false;            // its output low while it sees motion
    // info.sd_clk_gpio = -1;                      // no SD slot on this unit (CONFIG_HC_SDCARD only)
    // info.imu_axes = {0, 1, 0, 1, 0, 0};         // console `orient capture`, both lines
    // info.imu_offset = {0, 0, 0};
    // info.light_sensor_scale = 1.0f;             // console `light calibrate`
    // info.light_sensor_backlight_lux = 0.0f;     // console `light backlight`
    // info.backlight_min_duty = 10;               // test screen, tab Backlight
    // info.ui_tile_gap = 4;                       // and the other ui_* fields
    return info;
}

} // namespace hc::board::variant
