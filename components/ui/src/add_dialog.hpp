#pragma once

// Adding on the screen (docs/ui/lvgl/lvgl-dashboard.md section 7.2): a tap on a "+" cell of the edit
// mode opens these steps over the edit layer.
//
//   1. Title and type: Tile (an entity's), Area or Clock.
//   2. A tile: its entity (entity_picker.hpp).
//   3. Options of the type: the size, a light's dimmer, a sensor's decimals, the clock's look, an
//      area's swipe navigation. Add puts it into the edited copy (core::add_tile).
//   4. An area: its first tile, by the same search.
//
// A tap on a tile in edit mode opens the same steps for it (change()): the title (the type stays),
// then its options, with its entity on top for a tile of one.
//
// Back goes one step back and keeps what was chosen; Cancel drops it all.

#include "hc/core/layout.hpp"
#include "lvgl.h"

namespace hc::ui::add {

// the steps for a new tile at `cell` of `page` in `o`, as children of `parent` (the edit layer)
void open(lv_obj_t *parent, core::PageIndex page, core::Orientation o, core::Cell cell);
// the steps for the tile `tile` of the edited copy: title, entity and options in `o`
void change(lv_obj_t *parent, core::TileIndex tile, core::Orientation o);
// every step goes (Cancel, the edit ends)
void close();

} // namespace hc::ui::add
