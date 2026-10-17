#pragma once

// The automation panel (docs/ui/lvgl/lvgl-panels.md): the panel of an `automation.*` or a
// `script.*` entity. Two buttons choose between the control and the timeline (history_view). The
// control, as Home Assistant's dialog: the Enabled switch (an automation), the time of the last
// run, and a round Run button that runs the actions of the automation (`automation.trigger`,
// conditions skipped) or the script (`script.turn_on`; `script.turn_off` while it runs).

#include "hc/core/layout.hpp"
#include "panel.hpp"

namespace hc::ui::automation_panel {

// builds the parts of the automation `tile` in the areas of `frame`
void build(const panel::Frame &frame, core::TileIndex tile);

// draws the panel from the state of its entity and the action that is pending for it
void update();

// the panel's objects are gone: drops the pointers
void forget();

} // namespace hc::ui::automation_panel
