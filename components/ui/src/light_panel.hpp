#pragma once

// The light panel (docs/ui/dashboard.md section 6): the power button in the header, the buttons of
// the controls the light offers, and the control that is selected: the level slider with its
// steppers, the color wheel, the color temperature slider or the list of the effects.

#include "hc/core/layout.hpp"
#include "panel.hpp"

namespace hc::ui::light_panel {

// builds the parts of the light `tile` in the areas of `frame`
void build(const panel::Frame &frame, core::TileIndex tile);

// draws the panel from the state of its entity and the action that is pending for it
void update();

// the panel's objects are gone: drops the pointers, sends steps that are still collected
void forget();

} // namespace hc::ui::light_panel
