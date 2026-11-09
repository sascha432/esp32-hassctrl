#pragma once

// The forecast page of the weather panel (docs/ui/lvgl/lvgl-panels.md section 6.1): the forecast
// of the panel's subscription in the panel's whole area below its header, opened by a tap on the
// panel's forecast. Landscape draws graphs: a column per day or hour with the condition, the
// temperature as a curve (the day's high and low with the band between them), the chance of
// precipitation as a bar, the amount and the wind. Portrait draws lists: a row per day with its
// range on the week's scale, or per hour under the heading of its day. The days fit without
// scrolling; the hourly graph scrolls sideways, the hourly list down. Each view is one object
// drawn by a draw callback, not a label per value. One page exists at a time.

#include <string>

#include "hc/core/entity.hpp"
#include "lvgl.h"

namespace hc::ui::forecast_page {

// the units of the entity, empty when it has none
struct Units {
    std::string temperature;   // "°C"
    std::string precipitation; // "mm"
    std::string wind;          // "km/h"
};

// Builds the page in `area` of `parent` (in its coordinates): graphs, or lists when `portrait`.
// `dialog`: the P4's dialog, where the background runs to the dialog's edges (no card in the
// dialog, like the history view). `focus`: the entry the first forecast scrolls into view (the
// hour tapped in the panel), -1 none.
void build(lv_obj_t *parent, const lv_area_t &area, bool portrait, bool dialog, int focus);

// Draws `entries` of `type` (copied). Another answer of the same type keeps where the view was
// scrolled to.
void show(core::ForecastType type, const memory::Vector<core::ForecastEntry> &entries, const Units &units);
// the forecast is gone (the other type was asked for)
void clear();
// the text in place of the forecast ("Loading...", an error), "" for none
void message(const char *text);
// the temperature now, a dot on today's range in the daily list
void set_current(float temperature);

// the objects are gone with their parent: drops the pointers and the entries
void forget();

} // namespace hc::ui::forecast_page
