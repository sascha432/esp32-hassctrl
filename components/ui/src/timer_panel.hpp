#pragma once

// The timer panel (docs/ui/lvgl/lvgl-panels.md section 7): two buttons choose between the timer
// control and the timeline of the timer (history_view). The control is the countdown in large
// digits with the state under it, the duration with -/+ steppers (a minute per step), and the
// round buttons Start (Resume, Restart), Pause, Cancel and Finish, those the state allows.

#include "hc/core/layout.hpp"
#include "panel.hpp"

namespace hc::ui::timer_panel {

// builds the parts of the timer `tile` in the areas of `frame`
void build(const panel::Frame &frame, core::TileIndex tile);

// draws the panel from the state of its entity and the action that is pending for it
void update();

// the panel's objects are gone: drops the pointers, stops the timers
void forget();

} // namespace hc::ui::timer_panel
