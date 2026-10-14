#pragma once

// The panel of a weather tile (docs/ui/lvgl/lvgl-panels.md section 6): the condition's icon, the
// name and the temperature in the header, the current values below it (feels like, humidity,
// pressure, wind, ...: as many rows as fit), and the forecast at the bottom, daily or hourly, a
// row of columns that scrolls sideways. The forecast is subscribed while the panel is open
// (Backend::subscribe_forecast); no history.

#include "hc/core/entity.hpp"
#include "panel.hpp"

namespace hc::ui::weather_panel {

void build(const panel::Frame &frame, core::TileIndex tile);
void update();
// a forecast of the panel's subscription (an older one is dropped)
void on_forecast(core::Forecast forecast);
// the objects are gone (with the panel's root): ends the subscription, drops the pointers
void forget();

} // namespace hc::ui::weather_panel
