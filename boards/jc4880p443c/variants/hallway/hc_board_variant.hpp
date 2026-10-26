// The JC4880P443C in the hallway (COM15): camera and microphone, the development console and the
// FPS / CPU overlay on (the board's defaults); no vibration motor, relay or motion sensor on the
// expansion header. A copy of components/board/src/variant/hc_board_variant.hpp
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
    info.motion_gpio = -1;
    return info;
}

} // namespace hc::board::variant
