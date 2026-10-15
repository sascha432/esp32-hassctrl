#include "edit_mode.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>

#include "dashboard.hpp"
#include "overlay.hpp"
#include "settings.hpp"
#include "sound.hpp"
#include "theme.hpp"

namespace hc::ui::edit {

namespace {

namespace t = theme;

// a finger that moves this far is a drag, not a press
constexpr lv_coord_t kSlop = 10;
// the outline of the block a drag would take
constexpr uint32_t kFits = 0x2ecc71;
constexpr lv_coord_t kOutline = 3;
// the outline of the tile under the finger
constexpr uint32_t kPressed = 0xff9800;

bool touch_point(lv_point_t &out)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == nullptr) {
        return false;
    }
    lv_indev_get_point(indev, &out);
    return true;
}

bool moved(lv_point_t from, lv_point_t to)
{
    return LV_ABS(to.x - from.x) > kSlop || LV_ABS(to.y - from.y) > kSlop;
}

// An error: one short line in the alert strip ("Changes not saved"), the reason in the log
// (docs/ui/lvgl/lvgl-design.md section 8)
void message(const char *line, const char *reason)
{
    LV_LOG_WARN("edit: %s: %s", line, reason);
    char text[64];
    std::snprintf(text, sizeof(text), LV_SYMBOL_WARNING "  %s", line);
    overlay::show(text);
}

// ------------------------------------------------------------------- the long press that starts

struct Watch {
    lv_obj_t *obj = nullptr; // the object the finger is on
    lv_point_t at{};
    lv_timer_t *timer = nullptr;
};

Watch g_watch;
// the finger that started edit mode is still down: its hold must not ask to remove a tile
bool g_carried = false;

void cancel_watch()
{
    if (g_watch.timer != nullptr) {
        lv_timer_del(g_watch.timer);
        g_watch.timer = nullptr;
    }
    g_watch.obj = nullptr;
}

void on_watch_timer(lv_timer_t *)
{
    // a timer with one repeat is deleted after this call
    g_watch.timer = nullptr;
    g_watch.obj = nullptr;
    // the tile under the finger goes with the page that is built again, which ends its press: the
    // tile does not act on the release, and the finger goes on as a press on the edit layer
    sound::confirm();
    dashboard::start_editing();
    // the finger is still down: its hold must not ask to remove a tile, a drag may move it
    g_carried = true;
}

void on_watch(lv_event_t *event)
{
    lv_point_t point;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        cancel_watch();
        if (!touch_point(point) || dashboard::panel_tile() != core::kNone) {
            return;
        }
        g_watch.obj = lv_event_get_current_target(event);
        g_watch.at = point;
        g_watch.timer = lv_timer_create(on_watch_timer, kLongPressMs, nullptr);
        lv_timer_set_repeat_count(g_watch.timer, 1);
        break;
    case LV_EVENT_PRESSING:
        if (g_watch.timer != nullptr && touch_point(point) && moved(g_watch.at, point)) {
            cancel_watch(); // a dimmer's drag, a swipe
        }
        break;
    case LV_EVENT_DELETE:
        if (lv_event_get_current_target(event) == g_watch.obj) {
            cancel_watch();
        }
        break;
    default: // released, press lost
        cancel_watch();
        break;
    }
}

// --------------------------------------------------------------------------------- the layer

struct Layer {
    lv_obj_t *root = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *reset = nullptr;
    lv_obj_t *outline = nullptr;

    // the press on the layer
    lv_point_t at{};
    core::TileIndex tile = core::kNone; // the tile under the finger
    bool back = false;                  // the back tile is under the finger
    lv_obj_t *card = nullptr;           // its card
    lv_timer_t *timer = nullptr;        // the long press that asks to remove the tile
    bool long_pressed = false;
    bool dragging = false;
    lv_point_t grab{};                  // the finger's offset from the card's top left
    core::Cell target{0xFF, 0xFF};
    // the layout with the tile moved to `target`, null when it does not go there
    std::unique_ptr<core::Layout> moved;
};

Layer g;

core::Orientation orientation()
{
    return dashboard::grid().orientation;
}

void cancel_timer()
{
    if (g.timer != nullptr) {
        lv_timer_del(g.timer);
        g.timer = nullptr;
    }
}

// the view under `point`; false on an empty cell
bool hit(lv_point_t point)
{
    g.tile = core::kNone;
    g.back = false;
    g.card = nullptr;
    for (const tiles::View &view : dashboard::views()) {
        if (view.card == nullptr) {
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(view.card, &area);
        if (point.x >= area.x1 && point.x <= area.x2 && point.y >= area.y1 && point.y <= area.y2) {
            g.tile = view.tile;
            g.back = view.tile == core::kNone;
            g.card = view.card;
            return true;
        }
    }
    return false;
}

// the tile under the finger is outlined orange, the outline of the dashboard's edit look returns
// on release
void mark_pressed(bool pressed)
{
    if (g.card == nullptr) {
        return;
    }
    lv_obj_set_style_outline_color(g.card, t::color(pressed ? kPressed : (g.tile == core::kNone ? t::kBorder : t::kTextLabel)), 0);
}

void on_remove(void *context)
{
    const auto tile = static_cast<core::TileIndex>(reinterpret_cast<uintptr_t>(context));
    auto removed = core::remove_tile(dashboard::layout(), tile);
    if (!removed) {
        message("Tile not removed", removed.error().message.c_str());
        return;
    }
    dashboard::set_edited(std::move(*removed));
}

void on_long_press(lv_timer_t *)
{
    g.timer = nullptr;
    g.long_pressed = true;
    if (g.tile == core::kNone) {
        return;
    }
    const core::Layout &layout = dashboard::layout();
    const core::Tile &tile = layout.tiles[g.tile];
    char title[64];
    char text[96];
    std::snprintf(title, sizeof(title), "Remove %s?", dashboard::tile_name(g.tile).c_str());
    if (tile.kind == core::TileKind::Area) {
        std::snprintf(text, sizeof(text), "The area goes with its page and the %u tiles on it.",
                      static_cast<unsigned>(layout.pages[tile.opens].tiles.size()));
    } else {
        std::snprintf(text, sizeof(text), "The tile goes from the dashboard when you tap Done.");
    }
    sound::confirm();
    settings::confirm(title, text, "Remove", true, on_remove, reinterpret_cast<void *>(static_cast<uintptr_t>(g.tile)));
}

// the block of `cell` for the dragged tile, outlined green when it fits there
void show_target(core::Cell cell)
{
    const dashboard::Grid &grid = dashboard::grid();
    const core::Layout &layout = dashboard::layout();
    const core::Span span = layout.tiles[g.tile].in(grid.orientation).size;
    g.target = cell;
    auto copy = std::make_unique<core::Layout>(layout);
    g.moved = core::move_tile(*copy, g.tile, grid.orientation, cell) ? std::move(copy) : nullptr;
    if (g.outline == nullptr) {
        g.outline = t::plain(g.root);
        lv_obj_clear_flag(g.outline, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_width(g.outline, kOutline, 0);
        lv_obj_set_style_radius(g.outline, 8, 0);
    }
    const lv_coord_t x = static_cast<lv_coord_t>(grid.left + cell.col * (grid.cell_w + grid.gap));
    const lv_coord_t y = static_cast<lv_coord_t>(grid.top + cell.row * (grid.cell_h + grid.gap));
    lv_obj_set_pos(g.outline, x, y);
    lv_obj_set_size(g.outline, static_cast<lv_coord_t>(span.cols * grid.cell_w + (span.cols - 1) * grid.gap),
                    static_cast<lv_coord_t>(span.rows * grid.cell_h + (span.rows - 1) * grid.gap));
    lv_obj_set_style_border_color(g.outline, t::color(g.moved ? kFits : t::kError), 0);
}

void drag(lv_point_t point)
{
    const dashboard::Grid &grid = dashboard::grid();
    lv_obj_t *parent = lv_obj_get_parent(g.card);
    lv_area_t origin;
    lv_obj_get_coords(parent, &origin);
    const lv_coord_t x = static_cast<lv_coord_t>(point.x - g.grab.x);
    const lv_coord_t y = static_cast<lv_coord_t>(point.y - g.grab.y);
    lv_obj_set_pos(g.card, static_cast<lv_coord_t>(x - origin.x1), static_cast<lv_coord_t>(y - origin.y1));
    // the cell nearest to the card's top left
    const lv_coord_t step_x = static_cast<lv_coord_t>(grid.cell_w + grid.gap);
    const lv_coord_t step_y = static_cast<lv_coord_t>(grid.cell_h + grid.gap);
    const int col = LV_CLAMP(0, (x - grid.left + step_x / 2) / step_x, grid.cols - 1);
    const int row = LV_CLAMP(0, (y - grid.top + step_y / 2) / step_y, grid.rows - 1);
    const core::Cell cell{static_cast<uint8_t>(col), static_cast<uint8_t>(row)};
    if (!(cell == g.target)) {
        show_target(cell);
        sound::snap();
    }
}

void end_drag()
{
    if (g.outline != nullptr) {
        lv_obj_del(g.outline);
        g.outline = nullptr;
    }
    g.dragging = false;
    g.target = {0xFF, 0xFF};
    if (g.moved) {
        auto moved = std::move(g.moved);
        dashboard::set_edited(std::move(*moved));
    } else {
        dashboard::redraw_page(); // the card goes back to its cell
    }
}

void on_layer(lv_event_t *event)
{
    lv_point_t point;
    if (!touch_point(point)) {
        return;
    }
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        cancel_timer();
        g.at = point;
        g.long_pressed = false;
        g.dragging = false;
        if (hit(point)) {
            mark_pressed(true);
        }
        if (g.card != nullptr && !g.back && !g_carried) {
            g.timer = lv_timer_create(on_long_press, kLongPressMs, nullptr);
            lv_timer_set_repeat_count(g.timer, 1);
        }
        break;
    case LV_EVENT_PRESSING:
        if (!g.dragging && !g.long_pressed && moved(g.at, point)) {
            cancel_timer();
            if (g.tile != core::kNone && g.card != nullptr) {
                lv_area_t area;
                lv_obj_get_coords(g.card, &area);
                g.grab = {static_cast<lv_coord_t>(g.at.x - area.x1), static_cast<lv_coord_t>(g.at.y - area.y1)};
                lv_obj_move_foreground(g.card);
                lv_obj_set_style_opa(g.card, LV_OPA_80, 0);
                g.dragging = true;
            }
        }
        if (g.dragging) {
            drag(point);
        }
        break;
    case LV_EVENT_RELEASED:
        cancel_timer();
        mark_pressed(false);
        g_carried = false;
        if (g.dragging) {
            end_drag();
        } else if (!g.long_pressed && !moved(g.at, point)) {
            // a tap: only the back tile changes the page, areas do not open
            if (g.back) {
                dashboard::back();
            }
        }
        break;
    case LV_EVENT_PRESS_LOST:
        cancel_timer();
        mark_pressed(false);
        g_carried = false;
        if (g.dragging) {
            g.moved.reset();
            end_drag();
        }
        break;
    default:
        break;
    }
}

void on_done(lv_event_t *)
{
    char error[160];
    if (!dashboard::save_editing(error, sizeof(error))) {
        dashboard::stop_editing();
        message("Changes not saved", error);
    }
}

void on_cancel(lv_event_t *)
{
    dashboard::stop_editing();
}

void on_reset_confirmed(void *)
{
    // cannot fail: a tile without room in list order is left out, not refused
    core::Layout copy = dashboard::layout();
    core::reset_arrangement(copy, dashboard::current_page(), orientation());
    dashboard::set_edited(std::move(copy));
}

void on_reset(lv_event_t *)
{
    settings::confirm("Automatic order?",
                      orientation() == core::Orientation::Landscape
                          ? "The tiles of this page go back to the order of the list (landscape)."
                          : "The tiles of this page go back to the order of the list (portrait).",
                      "Reset", false, on_reset_confirmed, nullptr);
}

lv_obj_t *bar_button(lv_obj_t *bar, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *button = t::button(bar, text, callback);
    lv_obj_set_height(button, kBarHeight - 8);
    return button;
}

} // namespace

void watch(lv_obj_t *obj)
{
    for (const lv_event_code_t code :
         {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST, LV_EVENT_DELETE}) {
        lv_obj_add_event_cb(obj, on_watch, code, nullptr);
    }
}

void open(lv_obj_t *screen)
{
    close();
    g = {};
    g.root = t::plain(screen);
    lv_obj_set_size(g.root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(g.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g.root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    for (const lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(g.root, on_layer, code, nullptr);
    }

    lv_obj_t *bar = t::plain(g.root);
    lv_obj_set_size(bar, LV_PCT(100), kBarHeight);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, t::color(t::kBar), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, t::color(t::kBorder), 0);
    lv_obj_set_style_pad_hor(bar, t::kPad, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE); // a tap on the bar is not a tap on the page
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    g.title = t::label(bar, "", t::font_normal(), t::kTextValue);
    lv_obj_set_flex_grow(g.title, 1);
    g.reset = bar_button(bar, "Reset", on_reset);
    bar_button(bar, "Cancel", on_cancel);
    lv_obj_t *done = bar_button(bar, "Done", on_done);
    lv_obj_set_style_bg_color(done, t::color(t::kActive), 0);
    lv_obj_set_style_border_color(done, t::color(t::kActive), 0);
    refresh();
}

void refresh()
{
    if (g.root == nullptr || !dashboard::editing()) {
        return;
    }
    const core::Layout &layout = dashboard::layout();
    const core::PageIndex page = dashboard::current_page();
    const core::Orientation o = orientation();
    const core::Page &shown = layout.pages[page];
    char text[80];
    std::snprintf(text, sizeof(text), "Editing %s", shown.area == core::kNone ? "the main page" : layout.tiles[shown.area].name.c_str());
    t::line_text(g.title, text);
    bool arranged = false;
    for (const core::TileIndex tile : shown.tiles) {
        arranged = arranged || layout.tiles[tile].in(o).has_cell;
    }
    if (arranged) {
        lv_obj_clear_flag(g.reset, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g.reset, LV_OBJ_FLAG_HIDDEN);
    }
    if (g.outline != nullptr) {
        lv_obj_move_foreground(g.outline);
    }
}

void close()
{
    cancel_timer();
    if (g.root != nullptr) {
        // called from the events of the bar's buttons
        lv_obj_del_async(g.root);
    }
    g.root = nullptr;
    g.outline = nullptr;
    g.moved.reset();
    g.dragging = false;
}

void dropped()
{
    message("Edit dropped", "the dashboard was saved in the web UI meanwhile");
}

} // namespace hc::ui::edit
