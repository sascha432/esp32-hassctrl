#pragma once

// The lock panel (docs/ui/lvgl/lvgl-panels.md): two buttons choose between the lock control and
// the timeline of the lock (history_view). The control is a slider with two places like Home
// Assistant's: the thumb at the top is locked, at the bottom unlocked; dragging it to the other
// half (or a tap there) locks or unlocks. A lock that can open the door (supported_features OPEN)
// has a round open button beside it that asks for a second tap.

#include "hc/core/layout.hpp"
#include "panel.hpp"

namespace hc::ui::lock_panel {

// builds the parts of the lock `tile` in the areas of `frame`
void build(const panel::Frame &frame, core::TileIndex tile);

// draws the panel from the state of its entity and the action that is pending for it
void update();

// the panel's objects are gone: drops the pointers, stops the timers
void forget();

} // namespace hc::ui::lock_panel
