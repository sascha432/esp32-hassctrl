// The WT32-SC01 on the bench (COM9): the developer's unit, the same add-ons as the board file.
// A copy of components/board/src/variant/hc_board_variant.hpp (docs/adding-a-board.md "Variants").
#pragma once

#include "hc/board/board.hpp"

namespace hc::board::variant {

constexpr Info apply(Info info)
{
    info.variant = "bench";
    // LDR + 10k to GND on the header, replaced unit of 2026-10-07
    info.light_ldr_gpio = 34;
    // a vibration motor on the header (2026-10-09), 15 ms since the pulse comes at touch-down
    info.vibration_gpio = 27;
    info.vibration_click_ms = 15;
    // no relays wired on this unit
    info.relay_gpios = {-1, -1, -1, -1};
    // MPU6050 of the replaced unit, `orient capture` on 2026-10-07
    info.imu_axes = {0.0106f, 0.9999f, 0.0131f, 0.9991f, -0.0111f, 0.0418f};
    info.imu_offset = {-0.1248f, -0.5314f, 0.2124f};
    return info;
}

} // namespace hc::board::variant
