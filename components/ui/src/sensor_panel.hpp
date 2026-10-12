#pragma once

// The sensor panel (docs/ui/lvgl/lvgl-panels.md): the icon, the name and the live value of the
// entity above its history (history_view: the range buttons and the graph, the 5 minute means of
// the long term statistics as a curve, or the timeline of an on/off entity).

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"
#include "panel.hpp"

namespace hc::ui::sensor_panel {

// builds the parts of the sensor `tile` in the areas of `frame` and asks for its history
void build(const panel::Frame &frame, core::TileIndex tile);

// draws the live value from the state of the entity
void update();

// the panel's objects are gone: drops the pointers, stops the refresh
void forget();

} // namespace hc::ui::sensor_panel
