#pragma once

// The climate panel (docs/ui/dashboard.md section 6): the pills of the mode, the preset and the fan
// mode, and the arc of the setpoint with the action, the setpoint and the room temperature inside
// and its steppers. A tap on a pill shows the list of that option in the place of the arc.

#include "hc/core/layout.hpp"
#include "panel.hpp"

namespace hc::ui::climate_panel {

// builds the parts of the climate `tile` in the areas of `frame`
void build(const panel::Frame &frame, core::TileIndex tile);

// draws the panel from the state of its entity and the action that is pending for it
void update();

// the panel's objects are gone: drops the pointers, sends steps that are still collected
void forget();

} // namespace hc::ui::climate_panel
