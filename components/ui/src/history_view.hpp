#pragma once

// The history of an entity in a panel (docs/ui/lvgl/lvgl-panels.md): "History" with the range
// buttons (48, 24 and 12 hours) and the card below it with the graph: the 5 minute means of the
// long term statistics as a curve, or the timeline of an on/off entity. The history is asked for
// when the view is built, when the range changes and once a minute. The sensor panel and the
// timeline of the lock panel use it; one view exists at a time.

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"
#include "lvgl.h"
#include "theme.hpp"

namespace hc::ui::history_view {

// The timeline's states: their words in the legend and their colors. The problem state (a jammed
// lock; nullptr: none) is in the legend only while the window has one.
struct Legend {
    const char *on = "On";
    const char *off = "Off";
    const char *problem = nullptr;
    uint32_t on_color = theme::kActive;
    uint32_t off_color = theme::kBorder;
    uint32_t problem_color = theme::kError;
};

// Builds the view of `tile` in `area` of `parent` (in its coordinates) and asks for the history.
void build(lv_obj_t *parent, const lv_area_t &area, core::TileIndex tile, core::HistoryKind kind, const Legend &legend);

// draws the range buttons and the unit of the curve
void update();

// draws an answer to the view's request (an older one is dropped)
void on_history(core::History history);

// the view's objects are gone (with their parent): drops the pointers, stops the refresh
void forget();

} // namespace hc::ui::history_view
