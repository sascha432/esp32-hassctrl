#pragma once

// The sensor panel (docs/ui/dashboard.md section 6): the icon, the name and the live value of the
// entity, the range buttons (48, 24 and 12 hours) and the history graph: the 5 minute means of the
// long term statistics as a curve, or the timeline of an on/off entity. The history is asked for
// when the panel opens, when the range changes and once a minute.

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"
#include "panel.hpp"

namespace hc::ui::sensor_panel {

// builds the parts of the sensor `tile` in the areas of `frame` and asks for its history
void build(const panel::Frame &frame, core::TileIndex tile);

// draws the live value from the state of the entity
void update();

// draws an answer to the panel's request (an older one is dropped)
void on_history(core::History history);

// the panel's objects are gone: drops the pointers, stops the refresh
void forget();

} // namespace hc::ui::sensor_panel
