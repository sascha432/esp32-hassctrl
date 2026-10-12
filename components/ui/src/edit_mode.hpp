#pragma once

// The edit mode of the dashboard (docs/ui/lvgl/lvgl-dashboard.md section 7). A long press on a page
// starts it; the dashboard then draws a copy of the layout and this layer lies over the page: a
// bar with Reset, Cancel and Done at the top, and a transparent layer that takes every touch. A
// drag moves a tile (core::move_tile: the tiles in the way are pushed aside), a long press on a
// tile asks to remove it, a tap on an area or the back tile changes the page. Done stores the
// copy, Cancel drops it.

#include "lvgl.h"

namespace hc::ui::edit {

// the bar over the page while editing; the grid starts below it
constexpr lv_coord_t kBarHeight = 44;
// how long a finger rests on the page (or on a tile while editing) for a long press
constexpr uint32_t kLongPressMs = 1000;

// a long press on `obj` (a tile, a part of it, the page) starts edit mode
void watch(lv_obj_t *obj);

// the layer over `screen`, while editing
void open(lv_obj_t *screen);
// the bar after the page was built (its name, Reset only on an arranged page)
void refresh();
void close();

// tells that the edit was dropped: the dashboard was changed from outside meanwhile
void dropped();

} // namespace hc::ui::edit
