// The JC4880P443C in the hallway (COM15): camera and microphone, the development console and the
// FPS / CPU overlay on (the board's defaults), a motion sensor on GPIO 28 of the expansion header;
// no vibration motor or relay. A copy of components/board/src/variant/hc_board_variant.hpp
// (docs/adding-a-board.md "Variants"); built as jc4880p443c.hallway.
#pragma once

#include "hc/board/board.hpp"

namespace hc::board::variant {

constexpr Info apply(Info info)
{
    info.variant = "hallway";
    info.vibration_gpio = -1;
    info.relay_gpios = {-1, -1, -1, -1};
    info.relay_pullup = {false, false, false, false};
    // motion sensor on GPIO 28, high while it sees motion (active high turns on the internal
    // pulldown); input_boolean.<device>_motion in Home Assistant
    info.motion_gpio = 28;
    info.motion_active_high = true;
    return info;
}

} // namespace hc::board::variant
