#pragma once

// The dashboard screen (docs/ui/lvgl/lvgl-dashboard.md, docs/architecture.md section 4): the pages of the
// layout as grids of tiles. It owns the entity store and the pending actions, both only touched in
// the LVGL task. The values come from the backend (take_event, drained every 20 ms), the tiles'
// gestures go out as intents (call_service). A new layout (an import, an edit in the web UI)
// rebuilds the screen from the main page; turning the display rebuilds the page. A long press
// starts the edit mode (edit_mode.hpp), which draws an edited copy of the layout.

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"
#include "hc/core/pending.hpp"
#include "hc/ui/backend.hpp"
#include "hc/ui/ui.hpp"
#include "tiles.hpp"
#include "lvgl.h"

namespace hc::ui::dashboard {

// Draws the dashboard on `screen` once the backend has one, under what lies on the screen already
// (the start screen). `leave` (may be nullptr) is called once, before the dashboard takes the
// screen: the test screen of a board in bring-up goes then.
void init(const Backend &backend, lv_obj_t *screen, void (*leave)(), const Options &options);

// how the board's screens move and look (the panels ask whether they are dialogs)
const Options &options();

// true while a dashboard is shown
bool shown();

// true once the values of a page came from Home Assistant, the first time since the start
bool loaded();

// the backend the dashboard was started with (the panels ask it for history and the clock)
const Backend &backend();

// what the tiles read while they draw and act; only valid while shown()
const core::Layout &layout();
const core::EntityState &state(core::EntityIndex entity);
const core::Expectation *pending(core::EntityIndex entity);

// With Options::slide_pages an area page slides in from the right and its parent from the left.
void open_page(core::PageIndex page);
// to the parent of the current page
void back();
// The media browser of the media `tile` (Settings' Media page alone: `<` comes back here); without
// the player a message in the alert strip.
void open_media(core::TileIndex tile);
// A swipe (swipe.hpp): `direction` -1 for a swipe right, which goes back on a swipe page (an
// area with `swipe`); 1 for a swipe left, which does nothing.
void swiped(int direction);

// Opens the panel of `tile` over the page (a light with `dimmer`, a climate) and subscribes its fields;
// close_panel() goes back to the page. Opening another page closes it.
void open_panel(core::TileIndex tile);
void close_panel();
// the tile whose panel is open, core::kNone
core::TileIndex panel_tile();

// Asks for the camera image of the picture `tile` (from Home Assistant, or the first frame of its
// stream) as a `width` x `height` frame, or `whole`: the whole picture about as large as covers
// it; returns the generation the answer will carry (tiles::show_image, the picture panel).
uint32_t fetch_image(core::TileIndex tile, uint16_t width, uint16_t height, bool whole = false);
// plays the `stream` of the picture `tile` (whole frames about as large as cover `width` x
// `height`) until stop_stream(); the same stream in another size goes on. Returns the frames' generation
uint32_t play_stream(core::TileIndex tile, uint16_t width, uint16_t height);
void stop_stream();
// the view of `tile` on the page, nullptr when it is not there
const tiles::View *view_of(core::TileIndex tile);

// Sends `intent`; the tiles of its entity show `expected` (when it says anything) until the
// entity reports it, or for at most PendingTracker::kTimeoutMs.
void act(const core::Intent &intent, core::Expectation expected);
// shows `expected` as pending without sending anything yet (steps that are collected)
void expect(core::EntityIndex entity, core::Expectation expected);
// draws the tiles of `entity` again (a drag in progress), and its panel when one is open
void redraw(core::EntityIndex entity);

// the tile's own name, else the entity's friendly name, else the entity id
memory::String tile_name(core::TileIndex tile);

// ------------------------------------------------- edit mode (edit_mode.hpp, lvgl-dashboard.md section 7)

// true while the page is edited: a copy of the layout is drawn, the edit layer takes the touches
bool editing();
// edits a copy of the layout from now on, on the page that is shown
void start_editing();
// the edited copy after a move, a removal, a reset (same entities) or an added tile (new entities
// at the end); the page is built again
void set_edited(core::Layout layout);
// builds the page again from the edited copy (a drag that did not fit goes back)
void redraw_page();
// Done: stores the copy when it changed and shows the stored dashboard on the same page; false
// with the problem in `error` (the copy stays, stop_editing() drops it)
bool save_editing(char *error, size_t size);
// Cancel: the copy is dropped, the page is drawn from the layout it started from
void stop_editing();

core::PageIndex current_page();
// the views of the page that is shown
const memory::Vector<tiles::View> &views();

// the grid of the page as it is drawn, in screen coordinates
struct Grid {
    lv_coord_t left = 0;
    lv_coord_t top = 0;
    lv_coord_t cell_w = 0;
    lv_coord_t cell_h = 0;
    lv_coord_t gap = 0;
    uint8_t cols = 1;
    uint8_t rows = 1;
    core::Orientation orientation = core::Orientation::Landscape;
};
const Grid &grid();

} // namespace hc::ui::dashboard
