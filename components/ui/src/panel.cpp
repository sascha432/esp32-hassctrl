#include "panel.hpp"

#include <algorithm>
#include <string>

#include "climate_panel.hpp"
#include "dashboard.hpp"
#include "icons.hpp"
#include "history_view.hpp"
#include "light_panel.hpp"
#include "lock_panel.hpp"
#include "picture_panel.hpp"
#include "sensor_panel.hpp"
#include "sound.hpp"
#include "theme.hpp"
#include "weather_panel.hpp"

namespace hc::ui::panel {

namespace {

namespace t = theme;

// the margin and the gap of the dashboard's grid, so the back tile sits where the page's has it
constexpr lv_coord_t kMargin = 8;
constexpr lv_coord_t kGap = 4;
constexpr lv_coord_t kRadius = 8;
// the header row: the name, and the back tile of a portrait panel; a quarter taller in a large frame
constexpr lv_coord_t kHeaderHeight = 48;
constexpr lv_coord_t kHeaderHeightLarge = kHeaderHeight * 5 / 4;
// the gap between the header and the control
constexpr lv_coord_t kControlGap = 8;
// the name keeps this far from the back tile: the text has no card around it, so the grid's 4 px
// looked tighter than the gaps between the cards
constexpr lv_coord_t kNameGap = 12;
// landscape: between the left column and the control, twice the grid's gap (the arc and the lists
// looked glued to the buttons with 4 px)
constexpr lv_coord_t kControlSide = 2 * kGap;
// the buttons of the options: at most this tall in the landscape column, this far apart
constexpr lv_coord_t kOptionHeight = 40;
constexpr lv_coord_t kOptionGap = 6;
// the sliders: at most this wide, the track rounded like this
constexpr lv_coord_t kSliderWidth = 113;
// the slider of a large frame (the P4's dialog), wider. Portrait: this part of the height,
// centered; landscape: this far above the bottom of the control.
constexpr lv_coord_t kSliderWidthLarge = 160;
constexpr int kSliderHeightLargePct = 80;
constexpr lv_coord_t kSliderWidthLargeLandscape = 140;
constexpr lv_coord_t kSliderBottomLarge = 16;
constexpr lv_coord_t kSliderRadius = 24;
// the items of a list and the room for its scrollbar
constexpr lv_coord_t kItemHeight = 42;
constexpr lv_coord_t kItemGap = 4;
constexpr lv_coord_t kScrollbarRoom = 10;
// the row of the options of a portrait panel; a quarter taller in a large frame
constexpr lv_coord_t kOptionsRow = 44;
constexpr lv_coord_t kOptionsRowLarge = kOptionsRow * 5 / 4;
// Options::panel_dialogs: the opaque dialog in landscape and in portrait, the page behind it dimmed
// by a black veil. The dialog slides down from above the screen and back up when it closes; the
// veil darkens at once (fading it redrew the whole screen every frame, the P4 needs 50 ms for that).
// The full-screen panel of the other boards slides in from the right and out to the right, like
// an area page (your request 2026-10-14; before, it just appeared).
constexpr lv_coord_t kDialogWidth = 620;
constexpr lv_coord_t kDialogHeight = 420;
constexpr lv_coord_t kDialogPortraitWidth = 440;
constexpr lv_coord_t kDialogPortraitHeight = 600;
constexpr lv_coord_t kDialogRadius = 12;
constexpr lv_opa_t kVeilOpacity = LV_OPA_80;
constexpr uint32_t kSlideMs = 250;
// the slide's progress: 0 above the screen, kShown in the middle
constexpr int32_t kShown = 1000;

struct State {
    lv_obj_t *root = nullptr; // the panel, or the veil the dialog lies on
    bool dialog = false;
    lv_obj_t *body = nullptr; // the dialog
    int32_t shown = kShown;   // the slide's progress
    // as built: LVGL lays the objects out only before the next frame, the first step of a slide
    // read 0 from them
    lv_coord_t body_height = 0;
    lv_coord_t screen_height = 0;
    lv_coord_t screen_width = 0;
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    bool light = false; // the light panel, else the panel of the tile's kind
    lv_obj_t *name = nullptr;
    std::string shown_name;
};

State g;

lv_area_t area(lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    return {x, y, static_cast<lv_coord_t>(x + w - 1), static_cast<lv_coord_t>(y + h - 1)};
}

// The areas of the panel (panel.hpp). Landscape: the left column is one cell of a 4-column grid,
// its back tile at most a third of the height; the rest of the column holds the options.
Frame frame_of(lv_obj_t *root, lv_coord_t width, lv_coord_t height, bool with_action, bool compact, bool large)
{
    Frame f;
    f.root = root;
    f.portrait = height > width;
    f.large = large;
    const lv_coord_t header = large ? kHeaderHeightLarge : kHeaderHeight;
    if (compact) {
        f.back = area(kMargin, kMargin, header, header);
        const lv_coord_t control_y = kMargin + header + kControlGap;
        f.control = area(kMargin, control_y, static_cast<lv_coord_t>(width - 2 * kMargin),
                         static_cast<lv_coord_t>(height - kMargin - control_y));
        f.options = area(kMargin, static_cast<lv_coord_t>(height - kMargin), 0, 0);
    } else if (f.portrait) {
        f.back = area(kMargin, kMargin, header, header);
        const lv_coord_t row = large ? kOptionsRowLarge : kOptionsRow;
        f.options = area(kMargin, static_cast<lv_coord_t>(height - kMargin - row),
                         static_cast<lv_coord_t>(width - 2 * kMargin), row);
        const lv_coord_t control_y = kMargin + header + kControlGap;
        f.control = area(kMargin, control_y, static_cast<lv_coord_t>(width - 2 * kMargin),
                         static_cast<lv_coord_t>(f.options.y1 - kControlGap - control_y));
    } else {
        const lv_coord_t column = (width - 2 * kMargin - 3 * kGap) / 4;
        const lv_coord_t back = std::min<lv_coord_t>(column, (height - 2 * kMargin) / 3);
        f.back = area(kMargin, kMargin, column, back);
        f.options = area(kMargin, static_cast<lv_coord_t>(kMargin + back + kGap), column,
                         static_cast<lv_coord_t>(height - 2 * kMargin - back - kGap));
        const lv_coord_t x = kMargin + column + kControlSide;
        const lv_coord_t control_y = kMargin + header + kControlGap;
        f.control = area(x, control_y, static_cast<lv_coord_t>(width - kMargin - x),
                         static_cast<lv_coord_t>(height - kMargin - control_y));
    }
    const auto header_x = static_cast<lv_coord_t>(f.back.x2 + 1 + kNameGap);
    f.action = area(static_cast<lv_coord_t>(width - kMargin - header), kMargin, header, header);
    // without an action the name has the whole header
    const lv_coord_t header_end = with_action ? static_cast<lv_coord_t>(f.action.x1 - kGap) : static_cast<lv_coord_t>(width - kMargin);
    f.header = area(header_x, kMargin, static_cast<lv_coord_t>(header_end - header_x), header);
    return f;
}

void on_back(lv_event_t *)
{
    dashboard::close_panel();
}

// the panel at `progress` of its slide: the dialog from above the screen down to the middle, the
// full-screen panel from the right edge to the left
void place(int32_t progress)
{
    g.shown = progress;
    if (!g.dialog) {
        lv_obj_set_x(g.root, static_cast<lv_coord_t>(g.screen_width * (kShown - progress) / kShown));
        return;
    }
    const lv_coord_t height = g.body_height;
    const lv_coord_t middle = static_cast<lv_coord_t>((g.screen_height - height) / 2);
    lv_obj_set_y(g.body, static_cast<lv_coord_t>(-height + (middle + height) * progress / kShown));
}

// The panel slides in or out, from where it is; at the end of a slide out the panel goes.
void slide(bool in)
{
    lv_anim_del(g.root, nullptr);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, g.root);
    lv_anim_set_values(&anim, g.shown, in ? kShown : 0);
    // the whole way takes kSlideMs, the rest of an interrupted slide its part of it
    const int32_t way = in ? kShown - g.shown : g.shown;
    lv_anim_set_time(&anim, static_cast<uint32_t>(kSlideMs * way / kShown));
    lv_anim_set_path_cb(&anim, in ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&anim, [](void *target, int32_t progress) {
        // a panel opened meanwhile has replaced this one already
        if (target == g.root) {
            place(progress);
        }
    });
    if (!in) {
        lv_anim_set_ready_cb(&anim, [](lv_anim_t *a) {
            if (a->var == g.root) {
                destroy();
            }
        });
    }
    lv_anim_start(&anim);
}

// a tap on the veil beside the dialog closes it like the back tile
void on_veil(lv_event_t *event)
{
    if (lv_event_get_target(event) == lv_event_get_current_target(event)) {
        dashboard::close_panel();
    }
}

void on_root_delete(lv_event_t *)
{
    // the page was cleared under the panel (a new layout, a turn), or the panel closed
    g.root = nullptr;
    g.name = nullptr;
    light_panel::forget();
    climate_panel::forget();
    sensor_panel::forget();
    lock_panel::forget();
    picture_panel::forget();
    weather_panel::forget();
}

// A light tile gets the light panel with `dimmer`, or when its light dims: a supported color mode
// other than onoff, or a reported level. A light without `dimmer` that only switches has the
// sensor panel with its timeline.
bool light_panel_for(const core::Tile &tile, const core::EntityState &state)
{
    if (tile.kind != core::TileKind::Light) {
        return false;
    }
    if (tile.dimmer || state.brightness >= 0 || state.last_brightness >= 0) {
        return true;
    }
    return std::any_of(state.color_modes.begin(), state.color_modes.end(),
                       [](const auto &mode) { return mode != "onoff"; });
}

} // namespace

bool has_panel(const core::Tile &tile)
{
    // a switch and a light without dimmer: the sensor panel with their timeline (a double tap)
    return tile.kind == core::TileKind::Light || tile.kind == core::TileKind::Switch || tile.kind == core::TileKind::Climate ||
           tile.kind == core::TileKind::Sensor || tile.kind == core::TileKind::Lock || tile.kind == core::TileKind::Picture ||
           tile.kind == core::TileKind::Weather;
}

lv_area_t left_of_action(const Frame &frame)
{
    const lv_coord_t size = lv_area_get_height(&frame.action);
    return {static_cast<lv_coord_t>(frame.action.x1 - kGap - size), frame.action.y1,
            static_cast<lv_coord_t>(frame.action.x1 - kGap - 1), frame.action.y2};
}

void name_before(const Frame &frame, const lv_area_t &square)
{
    lv_obj_set_width(frame.name, static_cast<lv_coord_t>(square.x1 - kGap - frame.header.x1));
}

lv_obj_t *card(lv_obj_t *parent, const lv_area_t &a)
{
    lv_obj_t *obj = t::plain(parent);
    lv_obj_set_pos(obj, a.x1, a.y1);
    lv_obj_set_size(obj, lv_area_get_width(&a), lv_area_get_height(&a));
    lv_obj_set_style_radius(obj, kRadius, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, t::color(t::kCard), 0);
    lv_obj_set_style_bg_color(obj, t::color(t::kBorder), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, t::color(t::kBorder), 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    sound::tappable(obj);
    return obj;
}

void layout_options(const Frame &frame, lv_obj_t *const *buttons, int count, uint32_t shown)
{
    int visible_count = 0;
    for (int i = 0; i < count; i++) {
        visible_count += static_cast<int>((shown >> i) & 1);
    }
    const lv_area_t &options = frame.options;
    const lv_coord_t aw = lv_area_get_width(&options);
    const lv_coord_t ah = lv_area_get_height(&options);
    lv_coord_t w = aw;
    lv_coord_t h = ah;
    if (visible_count > 0) {
        if (frame.portrait) {
            w = static_cast<lv_coord_t>((aw - (visible_count - 1) * kOptionGap) / visible_count);
        } else {
            h = std::min<lv_coord_t>(kOptionHeight, static_cast<lv_coord_t>((ah - (visible_count - 1) * kOptionGap) / visible_count));
        }
    }
    int placed = 0;
    for (int i = 0; i < count; i++) {
        lv_obj_t *button = buttons[i];
        const bool visible = visible_count > 1 && ((shown >> i) & 1) != 0;
        set_hidden(button, !visible);
        if (!visible) {
            continue;
        }
        const lv_coord_t x = frame.portrait ? static_cast<lv_coord_t>(options.x1 + placed * (w + kOptionGap)) : options.x1;
        const lv_coord_t y = frame.portrait ? options.y1 : static_cast<lv_coord_t>(options.y1 + placed * (h + kOptionGap));
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, w, h);
        placed++;
    }
}

void mark_option(lv_obj_t *button, bool selected)
{
    lv_obj_set_style_bg_color(button, t::color(selected ? t::kText : t::kCard), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0), t::color(selected ? t::kBackground : t::kTextLabel), 0);
}

SliderPlace slider_place(const Frame &frame, lv_coord_t room, lv_coord_t h)
{
    if (!frame.large) {
        return {std::min(kSliderWidth, room), h, 0};
    }
    if (frame.portrait) {
        const auto slider_h = static_cast<lv_coord_t>(h * kSliderHeightLargePct / 100);
        return {std::min(kSliderWidthLarge, room), slider_h, static_cast<lv_coord_t>((h - slider_h) / 2)};
    }
    return {std::min(kSliderWidthLargeLandscape, room), static_cast<lv_coord_t>(h - kSliderBottomLarge), 0};
}

lv_obj_t *slider_track(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *track = t::plain(parent);
    lv_obj_set_pos(track, x, y);
    lv_obj_set_size(track, w, h);
    lv_obj_set_style_radius(track, std::min<lv_coord_t>(kSliderRadius, w / 2), 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(track, t::color(t::kCardAlt), 0);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_set_style_border_color(track, t::color(t::kBorder), 0);
    lv_obj_set_style_border_post(track, true, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(track, LV_OBJ_FLAG_CLICKABLE);
    sound::tappable(track);
    sound::draggable(track);
    // the drag does not bubble up as a gesture of the screen
    lv_obj_clear_flag(track, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return track;
}

lv_obj_t *step_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t size, const char *glyph,
                      lv_event_cb_t callback)
{
    lv_obj_t *button = card(parent, area(x, y, size, size));
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_center(t::label(button, icons::glyph(glyph), size >= 56 ? icons::large() : icons::small(), t::kText));
    lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
    // a stepper held down does not start a gesture of the screen
    lv_obj_clear_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return button;
}

void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == nullptr) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_text(lv_obj_t *label, const std::string &text)
{
    if (label != nullptr && text != lv_label_get_text(label)) {
        lv_label_set_text(label, text.c_str());
    }
}

// The height of an item: kItemHeight for the normal font, as much taller as font_item() is
// (Montserrat 24 on the P4, the items grow with their text).
lv_coord_t item_height()
{
    return static_cast<lv_coord_t>(kItemHeight * lv_font_get_line_height(&t::font_item()) /
                                   lv_font_get_line_height(&t::font_normal()));
}

// the height of `count` items in `columns`
lv_coord_t list_content(size_t count, size_t columns)
{
    const auto rows = static_cast<lv_coord_t>((count + columns - 1) / columns);
    return rows == 0 ? 0 : static_cast<lv_coord_t>(rows * item_height() + (rows - 1) * kItemGap);
}

// one column when every item fits in it, else two
size_t list_columns(size_t count, lv_coord_t height)
{
    return list_content(count, 1) <= height ? 1 : 2;
}

lv_obj_t *list(lv_obj_t *parent, lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *obj = t::plain(parent);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(obj, kItemGap, 0);
    lv_obj_set_style_pad_column(obj, kItemGap, 0);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(obj, t::color(t::kBorder), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(obj, 4, LV_PART_SCROLLBAR);
    return obj;
}

void list_fill(lv_obj_t *list, lv_coord_t width, lv_coord_t height, const memory::Vector<memory::String> &values,
               const char *(*glyph_of)(std::string_view), std::string (*text_of)(std::string_view), lv_event_cb_t callback)
{
    lv_obj_clean(list);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    const size_t columns = list_columns(values.size(), height);
    const lv_coord_t content = list_content(values.size(), columns);
    const bool fits = content <= height;
    const lv_coord_t item_h = item_height();
    // the items are as wide as two of them side by side, whether there are one or two columns
    const auto item_width = static_cast<lv_coord_t>((width - (fits ? 0 : kScrollbarRoom) - kItemGap) / 2);
    // a list that fits is a block centered in the area (a short one a single column), one that
    // scrolls keeps room for its bar
    const lv_coord_t list_width = columns == 1 ? item_width : width;
    lv_obj_set_style_pad_right(list, fits ? 0 : kScrollbarRoom, 0);
    lv_obj_set_size(list, list_width, fits ? content : height);
    lv_obj_set_pos(list, static_cast<lv_coord_t>((width - list_width) / 2),
                   fits ? static_cast<lv_coord_t>((height - content) / 2) : 0);
    for (const auto &value : values) {
        lv_obj_t *item = card(list, area(0, 0, item_width, item_h));
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(item, 8, 0);
        lv_obj_set_style_pad_column(item, 6, 0);
        // the glyph at the left, the name behind it
        if (const char *glyph = glyph_of != nullptr ? glyph_of(value) : nullptr) {
            t::label(item, glyph, icons::small(), t::kTextLabel);
        }
        lv_obj_t *label = t::label(item, "", t::font_item(), t::kTextLabel);
        lv_obj_set_flex_grow(label, 1);
        const std::string text = text_of != nullptr ? text_of(value) : std::string(value);
        t::line_text(label, text.c_str());
        lv_obj_add_event_cb(item, callback, LV_EVENT_CLICKED, nullptr);
    }
}

void list_mark(lv_obj_t *list, const memory::Vector<memory::String> &values, std::string_view current)
{
    for (size_t i = 0; i < values.size(); i++) {
        lv_obj_t *item = lv_obj_get_child(list, static_cast<int32_t>(i));
        const bool active = values[i] == current;
        lv_obj_set_style_bg_color(item, t::color(active ? t::kActive : t::kCard), 0);
        lv_obj_set_style_border_color(item, t::color(active ? t::kActive : t::kBorder), 0);
        for (uint32_t c = 0; c < lv_obj_get_child_cnt(item); c++) {
            lv_obj_set_style_text_color(lv_obj_get_child(item, static_cast<int32_t>(c)),
                                        t::color(active ? t::kText : t::kTextLabel), 0);
        }
    }
}

void list_reveal(lv_obj_t *list, const memory::Vector<memory::String> &values, std::string_view current, lv_coord_t height)
{
    const auto found = std::find(values.begin(), values.end(), current);
    const size_t columns = list_columns(values.size(), height);
    const lv_coord_t content = list_content(values.size(), columns);
    if (found == values.end() || content <= height) {
        return;
    }
    const auto row = static_cast<lv_coord_t>(static_cast<size_t>(found - values.begin()) / columns);
    const lv_coord_t item_h = item_height();
    const lv_coord_t top = static_cast<lv_coord_t>(row * (item_h + kItemGap));
    if (top + item_h <= height) {
        return; // in view from the start
    }
    // the item in the middle, but no further than the end of the list
    const lv_coord_t y = std::clamp<lv_coord_t>(static_cast<lv_coord_t>(top + item_h / 2 - height / 2), 0,
                                                static_cast<lv_coord_t>(content - height));
    lv_obj_scroll_to_y(list, y, LV_ANIM_OFF);
}

void build(lv_obj_t *parent, core::TileIndex tile, bool appear)
{
    destroy();
    const core::Layout &layout = dashboard::layout();
    if (tile >= layout.tiles.size() || !has_panel(layout.tiles[tile])) {
        return;
    }
    g.tile = tile;
    g.entity = layout.tiles[tile].entity;
    g.shown_name.clear();

    // the screen's size: right after a turn the parent is not laid out again yet
    lv_obj_t *screen = lv_obj_get_screen(parent);
    lv_coord_t width = lv_obj_get_width(screen);
    lv_coord_t height = lv_obj_get_height(screen);
    const core::TileKind kind = layout.tiles[tile].kind;
    const bool dialog = dashboard::options().panel_dialogs && kind != core::TileKind::Picture;
    g.dialog = dialog;
    // clickable: the page under it is not touched; opaque unless the panel is a dialog
    g.root = t::plain(parent);
    lv_obj_set_pos(g.root, 0, 0);
    lv_obj_set_size(g.root, width, height);
    lv_obj_set_style_bg_opa(g.root, dialog ? kVeilOpacity : static_cast<lv_opa_t>(LV_OPA_COVER), 0);
    lv_obj_set_style_bg_color(g.root, dialog ? lv_color_black() : t::color(t::kBackground), 0);
    lv_obj_clear_flag(g.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g.root, on_root_delete, LV_EVENT_DELETE, nullptr);
    g.screen_width = width;
    if (!dialog) {
        // opened by a tap: from the right edge; built again after a turn: in place
        place(appear ? 0 : kShown);
    }
    if (kind == core::TileKind::Picture) {
        // the image full screen, without the frame
        picture_panel::build(g.root, tile, width, height);
        if (appear) {
            slide(true);
        }
        return;
    }
    // the parts of the panel are placed in `body`: the whole screen, or the dialog
    lv_obj_t *body = g.root;
    if (dialog) {
        lv_obj_add_event_cb(g.root, on_veil, LV_EVENT_CLICKED, nullptr);
        const bool portrait = height > width;
        width = std::min<lv_coord_t>(portrait ? kDialogPortraitWidth : kDialogWidth, width);
        height = std::min<lv_coord_t>(portrait ? kDialogPortraitHeight : kDialogHeight, height);
        body = t::plain(g.root);
        lv_obj_set_size(body, width, height);
        lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(body, kDialogRadius, 0);
        lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(body, t::color(t::kDialog), 0);
        lv_obj_set_style_border_width(body, 1, 0);
        lv_obj_set_style_border_color(body, t::color(t::kBorder), 0);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        // a tap on the dialog between its parts does not close it
        lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
        g.body = body;
        g.body_height = height;
        g.screen_height = lv_obj_get_height(screen);
        // opened by a tap: from above the screen; built again after a turn: in place
        place(appear ? 0 : kShown);
    }
    // chosen once: update() fills the panel that was built
    const bool light = light_panel_for(layout.tiles[tile], dashboard::state(g.entity));
    g.light = light;
    // the sensor panel's frame: no options, the control takes the whole width
    const bool compact = !light && kind != core::TileKind::Climate && kind != core::TileKind::Lock;
    Frame frame = frame_of(body, width, height, light || kind == core::TileKind::Climate, compact, dialog);

    lv_obj_t *back = card(body, frame.back);
    lv_obj_t *arrow = t::label(back, icons::glyph("arrow-left"),
                               lv_area_get_height(&frame.back) >= 90 ? icons::large() : icons::small(), t::kTextLabel);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, nullptr);

    g.name = t::label(body, "", t::font_large(), t::kText);
    lv_obj_set_width(g.name, lv_area_get_width(&frame.header));
    lv_obj_set_pos(g.name, frame.header.x1,
                   static_cast<lv_coord_t>(frame.header.y1 +
                                           (lv_area_get_height(&frame.header) - lv_font_get_line_height(&t::font_large())) / 2));

    frame.name = g.name;
    if (light) {
        light_panel::build(frame, tile);
    } else if (kind == core::TileKind::Climate) {
        climate_panel::build(frame, tile);
    } else if (kind == core::TileKind::Lock) {
        lock_panel::build(frame, tile);
    } else if (kind == core::TileKind::Weather) {
        weather_panel::build(frame, tile);
    } else {
        sensor_panel::build(frame, tile);
    }
    update(g.entity);
    if (appear) {
        slide(true);
    }
}

void close()
{
    if (g.root == nullptr) {
        return;
    }
    // from where a slide in got to
    slide(false);
}

void destroy()
{
    if (g.root != nullptr) {
        // the delete event clears the state
        lv_obj_del(g.root);
    }
    g = {};
}

void update(core::EntityIndex entity)
{
    if (g.root == nullptr || entity != g.entity || g.name == nullptr) {
        return;
    }
    const core::Layout &layout = dashboard::layout();
    const core::Tile &tile = layout.tiles[g.tile];
    const std::string name(core::display_name(dashboard::state(entity), tile, layout.entity_id(tile)));
    if (name != g.shown_name) {
        g.shown_name = name;
        t::line_text(g.name, name.c_str());
    }
    // the panel build() chose
    if (g.light) {
        light_panel::update();
    } else if (tile.kind == core::TileKind::Climate) {
        climate_panel::update();
    } else if (tile.kind == core::TileKind::Lock) {
        lock_panel::update();
    } else if (tile.kind == core::TileKind::Weather) {
        weather_panel::update();
    } else {
        sensor_panel::update();
    }
}

void on_image(const core::Image &image)
{
    if (g.root != nullptr && image.entity == g.entity) {
        picture_panel::on_image(image);
    }
}

void on_forecast(core::Forecast forecast)
{
    if (g.root != nullptr && forecast.entity == g.entity) {
        weather_panel::on_forecast(std::move(forecast));
    }
}

void on_history(core::History history)
{
    if (g.root != nullptr && history.entity == g.entity) {
        history_view::on_history(std::move(history));
    }
}

} // namespace hc::ui::panel
