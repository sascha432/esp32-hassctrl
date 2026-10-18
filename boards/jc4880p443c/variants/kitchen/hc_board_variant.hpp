// The JC4880P443C in the kitchen: vibration motor on GPIO 34, relay 1 (opto module on the internal
// pullup) on GPIO 28, a radar motion sensor on GPIO 49, all on the expansion header. A copy of components/board/src/variant/hc_board_variant.hpp (docs/adding-a-board.md
// "Variants"); built as jc4880p443c.kitchen.
#pragma once

#include "hc/board/board.hpp"

namespace hc::board::variant {

constexpr Info apply(Info info)
{
    info.variant = "kitchen";
    // 3 V / 70 mA motor through an AO3400 (300 Ohm gate, 6.2 kOhm gate pulldown), 50 ms for a clear tap
    info.vibration_gpio = 34; // yellow wire
    info.vibration_click_ms = 50;
    // relay 1, switched from Home Assistant (input_boolean.<device>_relay_1); 2 to 4 not wired.
    // GPIO 28 since 2026-10-16: brownouts with the wire on GPIO 32 (cause unclear, the pin itself works)
    info.relay_gpios = {28 /* green wire */, -1, -1, -1};
    // opto relay module without a pullup: low opens, high closes, closed while the board starts
    info.relay_pullup = {true, false, false, false};
    // radar on GPIO 49, push-pull: 3.3 V while it sees motion, 0 V idle (active high turns on the
    // internal pulldown); input_boolean.<device>_motion in Home Assistant
    info.motion_gpio = 49; // blue wire
    info.motion_active_high = true;
    return info;
}

} // namespace hc::board::variant
