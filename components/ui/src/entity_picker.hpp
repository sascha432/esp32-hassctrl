#pragma once

// The entity search of the edit mode (docs/ui/lvgl/lvgl-dashboard.md section 7.2): a page over the
// edit layer with a search field, the on-screen keyboard and the entities Home Assistant finds
// (Backend::find_entities, every word in the id or the friendly name). A tap on a row asks for the
// details the new tile needs (Backend::fetch_entity) and hands the entity to `on_pick`; the page
// closes then. OK on the keyboard and the keyboard button beside the field hide it, so the list
// gets the height; a tap on the field or the button brings it back.

#include "hc/core/entity.hpp"
#include "lvgl.h"

namespace hc::ui::picker {

using Picked = void (*)(const char *entity_id, bool dimmer);

// opens the page as a child of `parent` (the edit layer), over all of it; the bar's left button
// is Back when `on_back` is given (the page closes, then it is called), else Cancel
void open(lv_obj_t *parent, const char *title, Picked on_pick, void (*on_back)() = nullptr);
// the page goes (Cancel, the edit ends); its objects go with `parent` when that is deleted
void close();
bool is_open();

// A page of the add steps (add_dialog.hpp) over all of `parent` that takes every touch: a column
// with the bar of the edit mode, its `title`, the `left` button (Cancel, Back) and an optional
// `right` one in the action color (Next, Add).
lv_obj_t *frame(lv_obj_t *parent, const char *title, const char *left, lv_event_cb_t on_left, const char *right,
                lv_event_cb_t on_right);

// the answers of the backend (dashboard::take_events)
void on_list(core::EntityList list);
void on_details(core::EntityDetails details);

} // namespace hc::ui::picker
