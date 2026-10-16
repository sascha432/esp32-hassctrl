#include "tiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <iterator>

#include "dashboard.hpp"
#include "hc/core/weather.hpp"
#include "icons.hpp"
#include "runner.hpp"
#include "sound.hpp"
#include "swipe.hpp"
#include "theme.hpp"

namespace hc::ui::tiles {

namespace {

namespace t = theme;
using core::TileKind;

constexpr lv_coord_t kTileRadius = 8;
// a cell at least this tall gets the large fonts (480x320 with 3 rows has 98 px)
constexpr lv_coord_t kLargeCell = 90;
// the middle of a climate card with the bars at least this high stacks action, setpoint and room
// temperature (a 1x2 on 480x320 with 3 rows has 132 px)
constexpr lv_coord_t kFullClimate = 110;
// a finger has to move this far before a dimmer drag starts; less is a tap
constexpr lv_coord_t kDragStart = 10;
// a second tap within this time is a double tap (dimmer: on/off)
constexpr uint32_t kDoubleTapMs = 300;
// the steps of a climate tile are sent once the taps pause this long
constexpr uint32_t kStepSendMs = 700;
// a camera image that did not come within this time is asked for again (its answer was lost); the
// slow cameras of a page take their turns meanwhile
constexpr uint32_t kImageWaitMs = 60000;
constexpr float kDefaultClimateStep = 0.5f;
// a clock looks at the time this often: once a second, four times with seconds (so a second is
// never more than a quarter late); it redraws only when the shown time changed
constexpr uint32_t kClockTickMs = 1000;
constexpr uint32_t kClockSecondsTickMs = 250;

const lv_font_t &value_font(const View &view)
{
    return view.large ? t::font_huge() : t::font_large();
}

const lv_font_t &name_font(const View &view)
{
    return view.large ? t::font_medium() : t::font_small();
}

std::string_view domain_of(std::string_view entity_id)
{
    return entity_id.substr(0, entity_id.find('.'));
}

std::string number(float value, int decimals)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%.*f", decimals, static_cast<double>(value));
    return text;
}

void set_text(lv_obj_t *label, const std::string &text)
{
    if (label != nullptr && text != lv_label_get_text(label)) {
        lv_label_set_text(label, text.c_str());
    }
}

lv_coord_t text_width(const char *text, const lv_font_t &font)
{
    return lv_txt_get_width(text, static_cast<uint32_t>(std::strlen(text)), &font, 0, LV_TEXT_FLAG_NONE);
}

// the first of `fonts` (largest first) that draws `text` within `width` and `height`, else the last
const lv_font_t &largest_font(std::initializer_list<const lv_font_t *> fonts, const char *text, lv_coord_t width,
                              lv_coord_t height = LV_COORD_MAX)
{
    for (const lv_font_t *font : fonts) {
        if (text_width(text, *font) <= width && lv_font_get_line_height(font) <= height) {
            return *font;
        }
    }
    return **(fonts.end() - 1);
}

// The value in the largest font of the tile that fits its width ("Detected" and "612 ppm" in a
// small cell), down to the small font, which is cut off.
void set_value(const View &view, const std::string &text)
{
    if (view.value == nullptr) {
        return;
    }
    const lv_font_t *chosen = &largest_font({&value_font(view), &t::font_large(), &t::font_medium(), &t::font_normal(),
                                             &t::font_small()},
                                            text.c_str(), view.value_width);
    if (lv_obj_get_style_text_font(view.value, LV_PART_MAIN) != chosen) {
        lv_obj_set_style_text_font(view.value, chosen, 0);
    }
    set_text(view.value, text);
}

void set_color(lv_obj_t *label, uint32_t color)
{
    if (label != nullptr) {
        lv_obj_set_style_text_color(label, t::color(color), 0);
    }
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

// ------------------------------------------------------------------------------- the parts

lv_obj_t *card(lv_obj_t *parent, lv_area_t area)
{
    lv_obj_t *obj = t::plain(parent);
    lv_obj_set_pos(obj, area.x1, area.y1);
    lv_obj_set_size(obj, lv_area_get_width(&area), lv_area_get_height(&area));
    lv_obj_set_style_radius(obj, kTileRadius, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, t::color(t::kCard), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, t::color(t::kBorder), 0);
    lv_obj_set_style_pad_all(obj, 6, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    sound::tappable(obj);
    // a pressed tile is a shade lighter
    lv_obj_set_style_bg_color(obj, t::color(t::kBorder), LV_STATE_PRESSED);
    return obj;
}

// the centered column of icon, value and name
lv_obj_t *column(lv_obj_t *parent)
{
    lv_obj_t *obj = t::plain(parent);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(obj, 2, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    return obj;
}

// a row of labels as wide as its content, `gap` apart, centered in the column
lv_obj_t *flex_row(lv_obj_t *parent, lv_coord_t gap)
{
    lv_obj_t *row = t::plain(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

lv_obj_t *text(lv_obj_t *parent, const lv_font_t &font, uint32_t color)
{
    lv_obj_t *label = t::label(parent, "", font, color);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

lv_obj_t *name_label(View &view, lv_obj_t *parent)
{
    lv_obj_t *label = text(parent, name_font(view), t::kTextLabel);
    lv_obj_set_width(label, LV_PCT(100));
    return label;
}

// a step bar of a climate card: across the top or bottom (w 100 %), or down the left or right
lv_obj_t *bar(lv_obj_t *parent, const char *glyph, lv_coord_t w, lv_coord_t h, lv_align_t align)
{
    lv_obj_t *obj = t::plain(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_align(obj, align, 0, 0);
    lv_obj_set_style_radius(obj, kTileRadius - 2, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    sound::tappable(obj);
    lv_obj_t *label = t::label(obj, glyph != nullptr ? glyph : "", icons::small(), t::kBackground);
    lv_obj_center(label);
    return obj;
}

// The wide shape: the icon left, right of it a column of texts as wide as the room left. The
// texts are added by the caller.
lv_obj_t *wide_row(View &view, const lv_font_t &icon_font)
{
    lv_obj_t *row = t::plain(view.card);
    lv_obj_set_size(row, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
    view.icon = t::label(row, "", icon_font, t::kTextLabel);
    lv_obj_t *col = t::plain(row);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
    // the room left of the icon
    view.value_width = static_cast<lv_coord_t>(view.value_width - lv_font_get_line_height(&icon_font) - 12);
    lv_obj_set_width(col, view.value_width);
    return col;
}

// ------------------------------------------------------------------------------- the gestures

core::EntityIndex entity_of(const View &view)
{
    return view.entity;
}

void send(const View &view, core::IntentKind kind, float value, core::Expectation expected)
{
    core::Intent intent;
    intent.kind = kind;
    intent.entity = entity_of(view);
    intent.value = value;
    dashboard::act(intent, std::move(expected));
}

// the state the tile shows: the pending action's, else the entity's
bool shown_on(const View &view)
{
    const core::Expectation *pending = dashboard::pending(view.entity);
    if (pending != nullptr && !pending->state.empty()) {
        return pending->state == "on";
    }
    return dashboard::state(view.entity).is_on();
}

void toggle(const View &view)
{
    core::Expectation expected;
    expected.state = shown_on(view) ? "off" : "on";
    send(view, core::IntentKind::Toggle, 0, std::move(expected));
}

// the single tap of a switch, once no second one came: the toggle
void on_single_tap_toggle(lv_timer_t *timer)
{
    auto &view = *static_cast<View *>(timer->user_data);
    view.tap_timer = nullptr;
    lv_timer_del(timer);
    if (dashboard::state(view.entity).unavailable() && dashboard::pending(view.entity) == nullptr) {
        return;
    }
    toggle(view);
}

void on_single_tap(lv_timer_t *timer);

// A double tap on a lock tile: lock when it is unlocked or open, unlock when it is locked, from the
// state it shows (the pending action's, else the entity's). Jammed, unavailable or unknown: the
// panel opens instead, it says why.
void toggle_lock(const View &view)
{
    const core::Expectation *pending = dashboard::pending(view.entity);
    const std::string_view state = pending != nullptr && !pending->state.empty()
                                       ? std::string_view(pending->state)
                                       : std::string_view(dashboard::state(view.entity).state);
    const bool locked = state == "locked" || state == "locking";
    const bool unlocked = state == "unlocked" || state == "unlocking" || state == "open" || state == "opening";
    if (!locked && !unlocked) {
        dashboard::open_panel(view.tile);
        return;
    }
    core::Expectation expected;
    expected.state = locked ? "unlocked" : "locked";
    send(view, core::IntentKind::SetLock, locked ? core::kUnlock : core::kLock, std::move(expected));
}

void on_click(lv_event_t *event)
{
    auto &view = *static_cast<View *>(lv_event_get_user_data(event));
    const core::Layout &layout = dashboard::layout();
    if (view.tile == core::kNone) {
        dashboard::back();
        return;
    }
    const core::Tile &tile = layout.tiles[view.tile];
    switch (view.kind) {
    case TileKind::Area:
        dashboard::open_page(tile.opens);
        break;
    case TileKind::Switch:
    case TileKind::Light: {
        // a second tap within the window opens the panel with the timeline, a single one toggles
        // once the window has passed (docs/ui/lvgl/lvgl-dashboard.md section 3)
        const uint32_t now = lv_tick_get();
        if (view.tap_timer != nullptr && now - view.last_tap <= kDoubleTapMs) {
            lv_timer_del(view.tap_timer);
            view.tap_timer = nullptr;
            dashboard::open_panel(view.tile);
        } else {
            view.last_tap = now;
            if (view.tap_timer == nullptr) {
                view.tap_timer = lv_timer_create(on_single_tap_toggle, kDoubleTapMs, &view);
            }
        }
        break;
    }
    case TileKind::Button: {
        core::Intent intent;
        intent.kind = core::IntentKind::Press;
        intent.entity = view.entity;
        dashboard::act(intent, {});
        break;
    }
    case TileKind::Lock: {
        // a double tap locks or unlocks, a single one opens the panel once the window has passed
        // (as a dimmer; docs/ui/lvgl/lvgl-dashboard.md section 3)
        const uint32_t now = lv_tick_get();
        if (view.tap_timer != nullptr && now - view.last_tap <= kDoubleTapMs) {
            lv_timer_del(view.tap_timer);
            view.tap_timer = nullptr;
            toggle_lock(view);
        } else {
            view.last_tap = now;
            if (view.tap_timer == nullptr) {
                view.tap_timer = lv_timer_create(on_single_tap, kDoubleTapMs, &view);
            }
        }
        break;
    }
    case TileKind::Climate:
    case TileKind::Sensor:
    case TileKind::Picture:
    case TileKind::Weather:
        dashboard::open_panel(view.tile);
        break;
    default:
        break;
    }
}

int level_of(const View &view, lv_coord_t y)
{
    lv_area_t area;
    lv_obj_get_coords(view.card, &area);
    const int height = std::max(1, static_cast<int>(lv_area_get_height(&area)));
    return std::clamp((area.y2 - y) * 100 / height, 0, 100);
}

void on_single_tap(lv_timer_t *timer)
{
    auto &view = *static_cast<View *>(timer->user_data);
    view.tap_timer = nullptr;
    lv_timer_del(timer);
    dashboard::open_panel(view.tile);
}

void on_dimmer(lv_event_t *event)
{
    auto &view = *static_cast<View *>(lv_event_get_user_data(event));
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point{};
    if (indev != nullptr) {
        lv_indev_get_point(indev, &point);
    }
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        view.press_x = point.x;
        view.press_y = point.y;
        view.dragging = false;
        break;
    case LV_EVENT_PRESSING:
        // a drag that goes more up or down than sideways; a sideways one is a swipe of the page
        if (!view.dragging && !swipe::active() && std::abs(point.y - view.press_y) >= kDragStart &&
            std::abs(point.y - view.press_y) > std::abs(point.x - view.press_x)) {
            view.dragging = true;
        }
        if (view.dragging) {
            view.drag_level = level_of(view, point.y);
            dashboard::redraw(view.entity);
        }
        break;
    case LV_EVENT_RELEASED: {
        if (view.dragging) {
            view.dragging = false;
            core::Expectation expected;
            expected.state = view.drag_level > 0 ? "on" : "off";
            if (view.drag_level > 0) {
                expected.brightness = static_cast<int16_t>(std::lround(view.drag_level * 255 / 100.0));
            }
            send(view, core::IntentKind::SetLevel, static_cast<float>(view.drag_level), std::move(expected));
            break;
        }
        // a tap: a second one within the window switches the light, a single one opens the panel
        const uint32_t now = lv_tick_get();
        if (view.tap_timer != nullptr && now - view.last_tap <= kDoubleTapMs) {
            lv_timer_del(view.tap_timer);
            view.tap_timer = nullptr;
            toggle(view);
        } else {
            view.last_tap = now;
            if (view.tap_timer == nullptr) {
                view.tap_timer = lv_timer_create(on_single_tap, kDoubleTapMs, &view);
            }
        }
        break;
    }
    default:
        break;
    }
}

void on_step_send(lv_timer_t *timer)
{
    auto &view = *static_cast<View *>(timer->user_data);
    view.send_timer = nullptr;
    lv_timer_del(timer);
    core::Expectation expected;
    expected.target = view.step_target;
    send(view, core::IntentKind::SetTemperature, view.step_target, std::move(expected));
}

void on_step(lv_event_t *event)
{
    auto &view = *static_cast<View *>(lv_event_get_user_data(event));
    const core::EntityState &state = dashboard::state(view.entity);
    if (state.unavailable() || std::isnan(state.target)) {
        return;
    }
    const core::Tile &tile = dashboard::layout().tiles[view.tile];
    const float step = climate_step(tile, state);
    const core::Expectation *pending = dashboard::pending(view.entity);
    float target = view.send_timer != nullptr ? view.step_target
                   : pending != nullptr && !std::isnan(pending->target) ? pending->target
                                                                         : state.target;
    target += lv_event_get_target(event) == view.plus ? step : -step;
    target = std::round(target / step) * step;
    if (!std::isnan(state.min_temp)) {
        target = std::max(target, state.min_temp);
    }
    if (!std::isnan(state.max_temp)) {
        target = std::min(target, state.max_temp);
    }
    view.step_target = target;
    // shown at once as pending, sent when the taps pause
    core::Expectation expected;
    expected.target = target;
    dashboard::expect(view.entity, std::move(expected));
    if (view.send_timer != nullptr) {
        lv_timer_reset(view.send_timer);
    } else {
        view.send_timer = lv_timer_create(on_step_send, kStepSendMs, &view);
    }
}

} // namespace

// the next camera image of a picture tile; not while a panel covers the page (a stream that plays
// full screen would wait for it), not while the last one is on its way: an image that took longer
// than the refresh would be thrown away for the newer request, and the queue would only grow
void request_image(View &view)
{
    if (view.image == nullptr) {
        return;
    }
    if (dashboard::panel_tile() != core::kNone) {
        return;
    }
    if (view.image_asked != 0 && lv_tick_elaps(view.image_asked) < kImageWaitMs) {
        return;
    }
    view.image_generation = dashboard::fetch_image(view.tile, view.image_width, view.image_height);
    view.image_asked = view.image_generation != 0 ? lv_tick_get() | 1 : 0;
}

namespace {

void on_delete(lv_event_t *event)
{
    auto &view = *static_cast<View *>(lv_event_get_user_data(event));
    if (view.image_timer != nullptr) {
        lv_timer_del(view.image_timer);
        view.image_timer = nullptr;
    }
    if (view.clock_timer != nullptr) {
        lv_timer_del(view.clock_timer);
        view.clock_timer = nullptr;
    }
    if (view.pixels) {
        compat::image_forget(view.image_dsc);
    }
    if (view.tap_timer != nullptr) {
        lv_timer_del(view.tap_timer);
        view.tap_timer = nullptr;
    }
    if (view.send_timer != nullptr) {
        // the steps are not lost when the page changes
        on_step_send(view.send_timer);
    }
    view.card = nullptr;
}

// ------------------------------------------------------------------------------- the looks

struct Look {
    bool active = false;      // filled with the active color
    bool pending = false;     // the border of an action on its way
    bool unavailable = false;
    uint32_t color = t::kActive; // the fill while active (a lock: the color of its state)
};

void apply_look(View &view, const Look &look)
{
    const uint32_t background = look.active ? look.color : look.unavailable ? t::kCardAlt : t::kCard;
    lv_obj_set_style_bg_color(view.card, t::color(background), 0);
    lv_obj_set_style_border_color(view.card, t::color(look.active ? look.color : t::kBorder), 0);
    runner::set(view.card, look.pending);
    set_color(view.icon, look.active ? t::kText : t::kTextLabel);
    // the name of a dimmer stands on its fill, off it is a label like on the other tiles
    const bool on_fill = view.fill != nullptr && !lv_obj_has_flag(view.fill, LV_OBJ_FLAG_HIDDEN);
    set_color(view.name, look.active || on_fill ? t::kText : t::kTextLabel);
    set_color(view.value, t::kText);
}

void update_name(View &view, const core::Layout &layout, const core::EntityState &state)
{
    if (view.name == nullptr) {
        return;
    }
    std::string name;
    if (view.tile == core::kNone) {
        name = "Back";
    } else {
        const core::Tile &tile = layout.tiles[view.tile];
        name = std::string(core::display_name(state, tile, layout.entity_id(tile)));
    }
    if (std::string_view(name) != std::string_view(view.shown_name)) {
        view.shown_name.assign(name.data(), name.size());
        t::line_text(view.name, name.c_str());
        if (view.caption != nullptr) {
            t::line_text(view.caption, name.c_str());
        }
    }
}

void update_icon(View &view, const core::Layout &layout, const core::EntityState &state, bool on)
{
    if (view.icon == nullptr || view.tile == core::kNone) {
        return;
    }
    const core::Tile &tile = layout.tiles[view.tile];
    const char *glyph = icons::for_tile(tile, layout.entity_id(tile), state, on);
    set_hidden(view.icon, glyph == nullptr);
    if (glyph != nullptr && std::strcmp(glyph, lv_label_get_text(view.icon)) != 0) {
        lv_label_set_text(view.icon, glyph);
    }
}

void update_climate(View &view, const core::EntityState &state, const core::Expectation *pending, bool available)
{
    const bool on = available && state.state != "off";
    const float target = pending != nullptr && !std::isnan(pending->target) ? pending->target : state.target;
    set_value(view, available ? temperature(target) + "°" : std::string("--"));
    std::string action;
    if (available) {
        action = !state.hvac_action.empty() ? words(state.hvac_action) : words(state.state);
    }
    set_text(view.action, action);
    set_color(view.action, on ? t::kActive : t::kTextLabel);
    if (view.current != nullptr) {
        std::string current;
        if (available && !std::isnan(state.current)) {
            // after the action in the line of a small tile
            current = (view.full ? "" : "\xe2\x80\xa2 ") + temperature(state.current) + "°";
        }
        set_text(view.current, current);
    }
    for (lv_obj_t *step : {view.plus, view.minus}) {
        if (step == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_color(step, t::color(on ? t::kActive : t::kCardAlt), 0);
        set_color(lv_obj_get_child(step, 0), on ? t::kBackground : t::kTextLabel);
        set_hidden(step, !available || std::isnan(state.target));
    }
}

void update_weather(View &view, const core::EntityState &state, bool known, bool available)
{
    set_value(view, !known                                    ? std::string("...")
                    : !available || std::isnan(state.temperature) ? std::string("--")
                                                                  : temperature(state.temperature) + "°");
    std::string condition = available ? core::condition_words(core::condition_of(state.state)) : "";
    std::string humidity;
    if (available && !std::isnan(state.humidity)) {
        humidity = number(state.humidity, 0) + "%";
    }
    if (view.current != nullptr) {
        // the tall look: the humidity on a line of its own
        set_text(view.action, condition);
        set_text(view.current, humidity);
        set_hidden(lv_obj_get_parent(view.current), humidity.empty());
        return;
    }
    if (!humidity.empty()) {
        condition += (condition.empty() ? "" : "  \xe2\x80\xa2  ") + humidity;
    }
    // a long condition ("Lightning, rainy") ends in "..." in a narrow tile
    t::line_text(view.action, condition.c_str());
}

// ------------------------------------------------------------------------------- the clock

// Draws the time of the clock view: the device's clock, read through the backend, in the tile's
// zone (core::local_time) or the device's (the backend's local time, the same as the status bar).
// `force` redraws although the minute (the second with `seconds`) did not change.
void tick_clock(View &view, bool force)
{
    const Backend &backend = dashboard::backend();
    const int64_t now = backend.clock();
    const int64_t key = now == 0 ? 0 : view.seconds ? now : now / 60;
    if (!force && key == view.clock_shown) {
        return;
    }
    view.clock_shown = key;
    if (now == 0) {
        // the clock is not set yet: dashes, the hands at twelve with a muted hub
        set_text(view.value, view.seconds ? "--:--:--" : "--:--");
        set_hidden(view.suffix, true);
        set_hidden(view.date, true);
        if (view.face != nullptr) {
            lv_meter_set_indicator_value(view.face, view.hour_hand, 0);
            lv_meter_set_indicator_value(view.face, view.minute_hand, 0);
            if (view.second_hand != nullptr) {
                lv_meter_set_indicator_value(view.face, view.second_hand, 0);
            }
            lv_obj_set_style_bg_color(view.face, t::color(t::kTextMuted), LV_PART_INDICATOR);
        }
        return;
    }
    std::tm local{};
    if (view.has_zone) {
        core::local_time(now, view.zone_rule, local);
    } else {
        backend.local_time(now, local);
    }
    char text[24];
    int hour = local.tm_hour;
    if (view.hours12) {
        hour = hour % 12 == 0 ? 12 : hour % 12;
    }
    if (view.seconds) {
        std::snprintf(text, sizeof(text), view.hours12 ? "%d:%02d:%02d" : "%02d:%02d:%02d", hour, local.tm_min, local.tm_sec);
    } else {
        std::snprintf(text, sizeof(text), view.hours12 ? "%d:%02d" : "%02d:%02d", hour, local.tm_min);
    }
    set_text(view.value, text);
    if (view.suffix != nullptr) {
        set_text(view.suffix, local.tm_hour < 12 ? "AM" : "PM");
        set_hidden(view.suffix, false);
    }
    if (view.face != nullptr) {
        // the hour hand moves on with the minutes, the minute hand with the seconds when they show
        lv_meter_set_indicator_value(view.face, view.hour_hand, (local.tm_hour % 12) * 60 + local.tm_min);
        lv_meter_set_indicator_value(view.face, view.minute_hand,
                                     view.seconds ? local.tm_min * 60 + local.tm_sec : local.tm_min);
        if (view.second_hand != nullptr) {
            lv_meter_set_indicator_value(view.face, view.second_hand, local.tm_sec);
        }
        lv_obj_set_style_bg_color(view.face, t::color(t::kText), LV_PART_INDICATOR);
    }
    if (view.date != nullptr) {
        // "Tue 13 Oct"
        if (std::strftime(text, sizeof(text), "%a %d %b", &local) != 0) {
            set_text(view.date, text);
        }
        set_hidden(view.date, false);
    }
}

void on_clock_tick(lv_timer_t *timer)
{
    tick_clock(*static_cast<View *>(timer->user_data), false);
}

// The digital clock: a column of the optional icon, the time (with AM/PM beside it), the date and
// the name. The time takes the largest font (the digit fonts of 72 and 48 px, then the text fonts)
// that fits the width and leaves the lines below their room; chosen once, so the digits stay put.
void create_digital_clock(View &view, const core::Tile &tile, lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *col = column(view.card);
    const bool with_icon = !tile.icon.empty() && tile.icon != "none";
    const bool with_name = !tile.name.empty();
    const lv_font_t &date_font = view.large ? t::font_normal() : t::font_small();
    const lv_font_t &suffix_font = view.large ? t::font_medium() : t::font_small();
    // the lines above and below the time, with the column's gaps
    lv_coord_t lines = 0;
    if (with_icon) {
        lines += lv_font_get_line_height(&icons::small()) + 2;
    }
    if (tile.date) {
        lines += lv_font_get_line_height(&date_font) + 2;
    }
    if (with_name) {
        lines += lv_font_get_line_height(&name_font(view)) + 2;
    }
    const char *sample = tile.seconds ? "00:00:00" : "00:00";
    lv_coord_t room = view.value_width;
    if (tile.hours12) {
        room -= text_width("PM", suffix_font) + 4;
    }
    const lv_font_t &font = largest_font({&t::font_digits_72(), &t::font_digits_48(), &t::font_huge(), &t::font_large(),
                                          &t::font_medium(), &t::font_normal(), &t::font_small()},
                                         sample, room, static_cast<lv_coord_t>(height - 14 - lines));
    static_cast<void>(width);
    if (with_icon) {
        view.icon = t::label(col, "", icons::small(), t::kTextLabel);
    }
    lv_obj_t *row = flex_row(col, 4);
    view.value = t::label(row, "", font, t::kText);
    if (tile.hours12) {
        view.suffix = t::label(row, "", suffix_font, t::kTextLabel);
    }
    if (tile.date) {
        view.date = text(col, date_font, t::kTextValue);
    }
    if (with_name) {
        view.name = name_label(view, col);
    }
}

// The face of an analog clock, `d` px across: an lv_meter (the hands are its needles, the ticks
// its scales; a tick of the hands redraws only their strips). Three scales: the hours (0 to 720
// minutes of twelve hours), the minutes (0 to 3600 seconds with the second hand, so it creeps
// on; else 0 to 60) and the seconds. The ticks follow the tile's `ticks`: the hour marks on the
// hours scale, the minute marks on the minutes scale below them. No major ticks: they draw labels.
lv_obj_t *analog_face(View &view, const core::Tile &tile, lv_obj_t *parent, lv_coord_t d)
{
    lv_obj_t *face = lv_meter_create(parent);
    lv_obj_remove_style_all(face);
    lv_obj_set_size(face, d, d);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(face, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_coord_t pad = 0;
    if (tile.border) {
        const lv_coord_t rim = d >= 200 ? 3 : 2;
        lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(face, rim, 0);
        lv_obj_set_style_border_color(face, t::color(t::kTextLabel), 0);
        pad = rim + 2;
    }
    lv_obj_set_style_pad_all(face, pad, 0);
    const lv_coord_t r = d / 2 - pad;
    // the hands have round ends; the hub is the meter's own middle
    lv_obj_set_style_line_rounded(face, true, LV_PART_ITEMS);
    const lv_coord_t hub = std::max<lv_coord_t>(5, r / 8) | 1;
    lv_obj_set_style_width(face, hub, LV_PART_INDICATOR);
    lv_obj_set_style_height(face, hub, LV_PART_INDICATOR);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(face, t::color(t::kText), LV_PART_INDICATOR);

    // scales: 360 degrees from twelve o'clock (270 in LVGL's terms), clockwise. The minutes scale
    // first, so the hour marks of the hours scale draw over its minute marks.
    const auto scale = [&](int32_t max) {
        lv_meter_scale_t *s = lv_meter_add_scale(face);
        lv_meter_set_scale_range(face, s, 0, max, 360, 270);
        lv_meter_set_scale_ticks(face, s, 0, 0, 0, t::color(t::kTextMuted));
        return s;
    };
    lv_meter_scale_t *minutes = scale(tile.seconds ? 3600 : 60);
    lv_meter_scale_t *hours = scale(720);
    lv_meter_scale_t *seconds = tile.seconds ? scale(60) : nullptr;
    // ticks: a face under 30 px of radius shows at most the hour marks
    core::ClockTicks ticks = tile.ticks;
    if (ticks == core::ClockTicks::Minute && r < 30) {
        ticks = core::ClockTicks::Hour;
    }
    switch (ticks) {
    case core::ClockTicks::Quarter:
        lv_meter_set_scale_ticks(face, hours, 5, std::max<lv_coord_t>(2, r / 25), r / 5, t::color(t::kTextLabel));
        break;
    case core::ClockTicks::Minute:
        lv_meter_set_scale_ticks(face, minutes, 61, 1, r / 10, t::color(t::kTextMuted));
        [[fallthrough]];
    case core::ClockTicks::Hour:
        lv_meter_set_scale_ticks(face, hours, 13, std::max<lv_coord_t>(2, r / 40), r / 6, t::color(t::kTextLabel));
        break;
    case core::ClockTicks::None:
        break;
    }
    // the hands, hour first: a later needle draws over an earlier one; lengths as parts of r
    view.hour_hand = lv_meter_add_needle_line(face, hours, static_cast<uint16_t>(std::max<lv_coord_t>(3, r / 14)),
                                              t::color(t::kText), static_cast<int16_t>(-r * 45 / 100));
    view.minute_hand = lv_meter_add_needle_line(face, minutes, static_cast<uint16_t>(std::max<lv_coord_t>(2, r / 20)),
                                                t::color(t::kText), static_cast<int16_t>(-r * 20 / 100));
    if (seconds != nullptr) {
        view.second_hand = lv_meter_add_needle_line(face, seconds, r >= 100 ? 2 : 1, t::color(t::kActive),
                                                    static_cast<int16_t>(-r * 10 / 100));
    }
    view.face = face;
    return face;
}

// The analog clock: the face with the date and the name below it; a wide tile puts them beside
// the face. A face that would be smaller than 40 px drops the date, then the name.
void create_analog_clock(View &view, const core::Tile &tile, lv_coord_t width, lv_coord_t height)
{
    const lv_coord_t inner_w = width - 14;
    const lv_coord_t inner_h = height - 14;
    bool with_name = !tile.name.empty();
    bool with_date = tile.date;
    const lv_font_t &date_font = view.large ? t::font_normal() : t::font_small();
    const lv_coord_t name_h = lv_font_get_line_height(&name_font(view)) + 2;
    const lv_coord_t date_h = lv_font_get_line_height(&date_font) + 2;
    constexpr lv_coord_t kSmallestFace = 40;

    if (view.wide && (with_name || with_date)) {
        // the face left, the texts right of it
        lv_obj_t *row = t::plain(view.card);
        lv_obj_set_size(row, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
        const lv_coord_t d = inner_h;
        analog_face(view, tile, row, d);
        lv_obj_t *col = t::plain(row);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 2, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
        view.value_width = static_cast<lv_coord_t>(inner_w - d - 12);
        lv_obj_set_width(col, view.value_width);
        if (with_name) {
            view.name = t::label(col, "", name_font(view), t::kTextLabel);
            lv_obj_set_width(view.name, LV_PCT(100));
        }
        if (with_date) {
            view.date = t::label(col, "", date_font, t::kTextValue);
            lv_obj_set_width(view.date, LV_PCT(100));
        }
        return;
    }

    lv_coord_t d = std::min(inner_w, static_cast<lv_coord_t>(inner_h - (with_name ? name_h : 0) - (with_date ? date_h : 0)));
    if (d < kSmallestFace && with_date) {
        with_date = false;
        d = std::min(inner_w, static_cast<lv_coord_t>(inner_h - (with_name ? name_h : 0)));
    }
    if (d < kSmallestFace && with_name) {
        with_name = false;
        d = std::min(inner_w, inner_h);
    }
    lv_obj_t *col = column(view.card);
    analog_face(view, tile, col, d);
    if (with_date) {
        view.date = text(col, date_font, t::kTextValue);
    }
    if (with_name) {
        view.name = name_label(view, col);
    }
}

void create_clock(View &view, const core::Tile &tile, lv_coord_t width, lv_coord_t height)
{
    view.hours12 = tile.hours12;
    view.seconds = tile.seconds;
    if (const char *rule = tile.zone.empty() ? nullptr : core::time_zone_rule(tile.zone)) {
        view.has_zone = core::parse_tz_rule(rule, view.zone_rule); // finish() checked the name
    }
    if (tile.style == core::ClockStyle::Analog) {
        create_analog_clock(view, tile, width, height);
    } else {
        create_digital_clock(view, tile, width, height);
    }
    view.clock_timer = lv_timer_create(on_clock_tick, tile.seconds ? kClockSecondsTickMs : kClockTickMs, &view);
}

void update_dimmer(View &view, const core::EntityState &state, const core::Expectation *pending, bool available)
{
    int level = 0;
    bool on = false;
    if (view.dragging) {
        level = view.drag_level;
        on = level > 0;
    } else {
        level = shown_level(state, pending, on);
    }
    lv_obj_set_height(view.fill, static_cast<lv_coord_t>(view.inner_height * std::max(level, 0) / 100));
    set_hidden(view.fill, level <= 0);
    // off and unavailable look like a plain light: the icon over the name
    const bool off = !available || level == 0;
    set_hidden(view.value, off);
    set_hidden(view.off, !off);
    lv_obj_t *const parent = off ? view.off : view.card;
    if (lv_obj_get_parent(view.name) != parent) {
        lv_obj_set_parent(view.name, parent);
        if (!off) {
            lv_obj_align(view.name, LV_ALIGN_BOTTOM_MID, 0, 0);
        }
    }
    if (!off) {
        // switched on, but the level it comes on at is not known yet: no number until HA reports it
        set_value(view, level < 0 ? std::string("On") : std::to_string(level) + " %");
    }
}

// a light without `dimmer`: the level under the icon while it is on and reports one; the icon
// of a large one-cell card is the small one while the level shows, else the large one
void update_light(View &view, const core::EntityState &state, const core::Expectation *pending, bool available)
{
    if (view.value == nullptr) {
        return;
    }
    bool on = false;
    const int level = shown_level(state, pending, on);
    const bool show = available && on && level > 0 && (state.brightness >= 0 || (pending != nullptr && pending->brightness >= 0));
    set_hidden(view.value, !show);
    if (show) {
        set_value(view, std::to_string(level) + " %");
    }
    if (view.icon != nullptr && !view.wide) {
        const lv_font_t *font = show || !view.large ? &icons::small() : &icons::large();
        if (lv_obj_get_style_text_font(view.icon, LV_PART_MAIN) != font) {
            lv_obj_set_style_text_font(view.icon, font, 0);
        }
    }
}

} // namespace

const char *binary_words(std::string_view device_class, bool on)
{
    if (device_class == "door" || device_class == "garage_door" || device_class == "window" ||
        device_class == "opening") {
        return on ? "Open" : "Closed";
    }
    if (device_class == "motion" || device_class == "occupancy" || device_class == "presence") {
        return on ? "Detected" : "Clear";
    }
    if (device_class == "moisture") {
        return on ? "Wet" : "Dry";
    }
    if (device_class == "connectivity") {
        return on ? "Connected" : "Disconnected";
    }
    if (device_class == "lock") {
        return on ? "Unlocked" : "Locked";
    }
    return on ? "On" : "Off";
}

std::string sensor_text(const core::Tile &tile, std::string_view entity_id, const core::EntityState &state)
{
    if (domain_of(entity_id) == "binary_sensor") {
        return binary_words(state.device_class, state.is_on());
    }
    std::string value(core::display_value(state, tile));
    const char *end = nullptr;
    std::strtod(value.c_str(), const_cast<char **>(&end));
    const bool numeric = !value.empty() && end != nullptr && *end == 0;
    if (!numeric) {
        // a template sensor can report several lines; a tile has room for the first one
        value = words(value.substr(0, value.find('\n')));
    }
    const std::string_view unit = core::display_unit(state, tile);
    if (numeric && !unit.empty()) {
        value += ' ';
        value += unit;
    }
    return value;
}

std::string words(std::string_view state)
{
    std::string out(state);
    std::replace(out.begin(), out.end(), '_', ' ');
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

std::string temperature(float value)
{
    return std::isnan(value) ? std::string("--") : number(value, std::fabs(value - std::round(value)) < 0.05f ? 0 : 1);
}

float climate_step(const core::Tile &tile, const core::EntityState &state)
{
    if (!std::isnan(state.temp_step) && state.temp_step > 0) {
        return state.temp_step;
    }
    return tile.step > 0 ? tile.step : kDefaultClimateStep;
}

int shown_level(const core::EntityState &state, const core::Expectation *pending, bool &on)
{
    on = state.is_on();
    int brightness = state.brightness;
    bool unknown = false;
    if (pending != nullptr) {
        if (!pending->state.empty()) {
            on = pending->state == "on";
        }
        if (pending->brightness >= 0) {
            brightness = pending->brightness;
        } else if (on && brightness < 0) {
            // switched on (by this tile or another one of the light): HA brings back the last level
            brightness = state.last_brightness;
            unknown = brightness < 0;
        }
    }
    if (!on) {
        return 0;
    }
    if (unknown) {
        return -1;
    }
    if (brightness < 0) {
        return 100; // a light without a level
    }
    return std::max(1, static_cast<int>(std::lround(brightness * 100 / 255.0)));
}

void create(View &view, lv_obj_t *parent, const core::Layout &layout, lv_area_t area, core::Span span)
{
    const lv_coord_t height = lv_area_get_height(&area);
    const lv_coord_t width = lv_area_get_width(&area);
    // inside the padding and the border of the card
    view.value_width = static_cast<lv_coord_t>(width - 14);
    view.card = card(parent, area);
    lv_obj_add_event_cb(view.card, on_delete, LV_EVENT_DELETE, &view);
    if (view.tile == core::kNone) {
        // the back tile of an area page
        view.large = height >= kLargeCell;
        lv_obj_t *col = column(view.card);
        view.icon = t::label(col, icons::glyph("arrow-left"), view.large ? icons::large() : icons::small(), t::kTextLabel);
        lv_obj_add_event_cb(view.card, on_click, LV_EVENT_CLICKED, &view);
        return;
    }
    const core::Tile &tile = layout.tiles[view.tile];
    view.kind = tile.kind;
    view.entity = tile.entity;
    // the shape follows the cells; a tall or wide look needs the room of two cells of 90 px
    view.tall = span.rows > span.cols && height >= 2 * kLargeCell;
    view.wide = span.cols > span.rows && width >= 2 * kLargeCell;
    // the fonts follow the size of the card (a 2x2 of small cells is a large card); the tall
    // look stacks more, so it needs twice the height
    view.large = width >= kLargeCell && (view.tall ? height / 2 : height) >= kLargeCell;

    if (tile.kind == TileKind::Light && tile.dimmer) {
        view.fill = t::plain(view.card);
        // over the padding of the card, inside its border
        lv_obj_set_width(view.fill, width - 2);
        view.inner_height = height - 2;
        lv_obj_align(view.fill, LV_ALIGN_BOTTOM_MID, 0, 5);
        lv_obj_set_style_radius(view.fill, kTileRadius - 1, 0);
        lv_obj_set_style_bg_opa(view.fill, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(view.fill, t::color(t::kActive), 0);
        lv_obj_clear_flag(view.fill, LV_OBJ_FLAG_CLICKABLE);
        view.value = text(view.card, value_font(view), t::kText);
        lv_obj_center(view.value);
        // off: the icon and the name of a plain light; the name moves into the column (update_dimmer)
        view.off = column(view.card);
        view.icon = t::label(view.off, "", view.large ? icons::large() : icons::small(), t::kTextLabel);
        lv_obj_add_flag(view.off, LV_OBJ_FLAG_HIDDEN);
        view.name = name_label(view, view.card);
        lv_obj_align(view.name, LV_ALIGN_BOTTOM_MID, 0, 0);
        for (const lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED}) {
            lv_obj_add_event_cb(view.card, on_dimmer, code, &view);
        }
        // the drag does not bubble up as a gesture of the screen
        lv_obj_clear_flag(view.card, LV_OBJ_FLAG_GESTURE_BUBBLE);
        return;
    }

    if (tile.kind == TileKind::Climate) {
        // the step bars: across the top and bottom of a tall card, down the left and right of a
        // wide one; the one-cell look has none (the panel steps)
        lv_coord_t middle_top = 0;
        lv_coord_t middle_bottom = 0;
        lv_coord_t middle_left = 0;
        lv_coord_t middle_right = 0;
        if (view.tall) {
            const lv_coord_t thickness = std::max<lv_coord_t>(28, height / 10);
            view.plus = bar(view.card, icons::glyph("plus"), LV_PCT(100), thickness, LV_ALIGN_TOP_MID);
            view.minus = bar(view.card, icons::glyph("minus"), LV_PCT(100), thickness, LV_ALIGN_BOTTOM_MID);
            middle_top = thickness;
            middle_bottom = thickness;
        } else if (view.wide) {
            const lv_coord_t thickness = std::max<lv_coord_t>(28, width / 10);
            view.minus = bar(view.card, icons::glyph("minus"), thickness, LV_PCT(100), LV_ALIGN_LEFT_MID);
            view.plus = bar(view.card, icons::glyph("plus"), thickness, LV_PCT(100), LV_ALIGN_RIGHT_MID);
            middle_left = thickness;
            middle_right = thickness;
        }
        if (view.plus != nullptr) {
            lv_obj_add_event_cb(view.plus, on_step, LV_EVENT_CLICKED, &view);
            lv_obj_add_event_cb(view.minus, on_step, LV_EVENT_CLICKED, &view);
        }
        lv_obj_t *col = column(view.card);
        const auto col_height = static_cast<lv_coord_t>(height - 12 - middle_top - middle_bottom);
        const auto col_width = static_cast<lv_coord_t>(width - 12 - middle_left - middle_right);
        lv_obj_set_size(col, col_width, col_height);
        lv_obj_align(col, LV_ALIGN_TOP_LEFT, middle_left, middle_top);
        view.value_width = static_cast<lv_coord_t>(col_width - 2);
        view.full = col_height >= kFullClimate;
        if (view.full) {
            view.action = text(col, t::font_normal(), t::kTextLabel);
            view.value = text(col, value_font(view), t::kText);
            // the room temperature under the setpoint
            lv_obj_t *row = flex_row(col, 2);
            t::label(row, icons::glyph("thermometer"), icons::small(), t::kTextLabel);
            view.current = t::label(row, "", t::font_medium(), t::kTextValue);
        } else {
            // the action and the room temperature share one line in the small font, so every
            // tile has room for it
            lv_obj_t *row = flex_row(col, 4);
            view.action = text(row, t::font_small(), t::kTextLabel);
            view.current = t::label(row, "", t::font_small(), t::kTextValue);
            view.value = text(col, value_font(view), t::kText);
        }
        view.name = name_label(view, col);
        // a tap beside the step bars opens the climate panel
        lv_obj_add_event_cb(view.card, on_click, LV_EVENT_CLICKED, &view);
        return;
    }

    if (tile.kind == TileKind::Clock) {
        create_clock(view, tile, width, height);
        return;
    }

    if (tile.kind == TileKind::Weather && view.wide) {
        // the large icon left, right of it the temperature, the condition and the name
        lv_obj_t *col = wide_row(view, icons::large());
        view.value = t::label(col, "", value_font(view), t::kText);
        view.action = t::label(col, "", t::font_normal(), t::kTextValue);
        lv_obj_set_width(view.action, LV_PCT(100));
        view.name = t::label(col, "", name_font(view), t::kTextLabel);
        lv_obj_set_width(view.name, LV_PCT(100));
        lv_obj_add_event_cb(view.card, on_click, LV_EVENT_CLICKED, &view);
        return;
    }
    if (tile.kind == TileKind::Weather) {
        lv_obj_t *col = column(view.card);
        if (view.tall) {
            view.icon = t::label(col, "", icons::large(), t::kTextLabel);
            view.value = text(col, value_font(view), t::kText);
            view.action = text(col, t::font_normal(), t::kTextValue);
            lv_obj_set_width(view.action, LV_PCT(100));
            // the humidity under the condition, like the room temperature of a climate tile
            lv_obj_t *row = flex_row(col, 2);
            t::label(row, icons::glyph("water-percent"), icons::small(), t::kTextLabel);
            view.current = t::label(row, "", t::font_medium(), t::kTextValue);
        } else {
            // the icon beside the temperature: icon, line and value above each other are taller than
            // a cell of 98 px (480x320 with 3 rows)
            lv_obj_t *row = flex_row(col, 4);
            view.icon = t::label(row, "", icons::small(), t::kTextLabel);
            view.value = text(row, value_font(view), t::kText);
            view.value_width = static_cast<lv_coord_t>(view.value_width - lv_font_get_line_height(&icons::small()) - 4);
            // the condition and the humidity
            view.action = text(col, t::font_small(), t::kTextValue);
            lv_obj_set_width(view.action, LV_PCT(100));
        }
        view.name = name_label(view, col);
        lv_obj_add_event_cb(view.card, on_click, LV_EVENT_CLICKED, &view);
        return;
    }

    // a light without `dimmer` shows its level while it is on (update_light)
    const bool light_level = tile.kind == TileKind::Light;
    const bool has_value = tile.kind == TileKind::Sensor || tile.kind == TileKind::Lock;
    if (view.wide && tile.kind != TileKind::Picture) {
        // the icon left, the value and the name right of it
        lv_obj_t *col = wide_row(view, view.large ? icons::large() : icons::small());
        if (has_value || light_level) {
            view.value = t::label(col, "", value_font(view), t::kText);
            lv_obj_set_width(view.value, LV_PCT(100));
            lv_label_set_long_mode(view.value, LV_LABEL_LONG_CLIP);
            set_hidden(view.value, light_level);
        }
        view.name = t::label(col, "", name_font(view), t::kTextLabel);
        lv_obj_set_width(view.name, LV_PCT(100));
        lv_obj_add_event_cb(view.card, on_click, LV_EVENT_CLICKED, &view);
        return;
    }
    lv_obj_t *col = column(view.card);
    // without a value the icon is the large one
    const bool big_icon = !has_value && view.large;
    view.icon = t::label(col, "", big_icon ? icons::large() : icons::small(), t::kTextLabel);
    if (has_value || light_level) {
        view.value = text(col, value_font(view), t::kText);
        lv_obj_set_width(view.value, LV_PCT(100));
        lv_label_set_long_mode(view.value, LV_LABEL_LONG_CLIP);
        set_hidden(view.value, light_level);
    }
    view.name = name_label(view, col);
    lv_obj_add_event_cb(view.card, on_click, LV_EVENT_CLICKED, &view);

    if (tile.kind == TileKind::Picture) {
        // the image covers the card inside its border, the card rounds its corners
        view.image_width = static_cast<uint16_t>(width - 2);
        view.image_height = static_cast<uint16_t>(height - 2);
        lv_obj_set_style_clip_corner(view.card, true, 0);
        view.image = lv_img_create(view.card);
        lv_obj_set_size(view.image, view.image_width, view.image_height);
        lv_obj_align(view.image, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(view.image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(view.image, LV_OBJ_FLAG_HIDDEN);
        view.caption = t::label(view.card, "", name_font(view), t::kText);
        // over the card's padding, edge to edge inside its border
        lv_obj_set_width(view.caption, view.image_width);
        lv_obj_set_style_text_align(view.caption, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_opa(view.caption, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(view.caption, t::color(t::kBackground), 0);
        lv_obj_set_style_pad_hor(view.caption, 6, 0);
        lv_obj_set_style_pad_ver(view.caption, 2, 0);
        lv_obj_align(view.caption, LV_ALIGN_BOTTOM_MID, 0, 6);
        lv_obj_add_flag(view.caption, LV_OBJ_FLAG_HIDDEN);
        request_image(view);
        const uint32_t seconds = tile.refresh_s > 0 ? tile.refresh_s : core::kDefaultRefresh;
        view.image_timer = lv_timer_create([](lv_timer_t *timer) { request_image(*static_cast<View *>(timer->user_data)); },
                                           seconds * 1000, &view);
    }
}

void show_image(View &view, const core::Image &image)
{
    if (view.image == nullptr || image.generation != view.image_generation) {
        return;
    }
    view.image_asked = 0;
    if (!image.error.empty() || !image.pixels) {
        // the last image stays; the camera icon while there is none
        LV_LOG_WARN("camera: %s", image.error.c_str());
        return;
    }
    if (view.pixels) {
        compat::image_forget(view.image_dsc);
    }
    view.pixels = image.pixels;
    view.source_width = image.source_width;
    view.source_height = image.source_height;
    compat::image_dsc(view.image_dsc, view.pixels.get(), image.width, image.height);
    lv_img_set_src(view.image, &view.image_dsc);
    lv_obj_invalidate(view.image);
    lv_obj_clear_flag(view.image, LV_OBJ_FLAG_HIDDEN);
    // the icon and the name of the column give way to the band
    lv_obj_add_flag(lv_obj_get_parent(view.icon), LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(view.caption, LV_OBJ_FLAG_HIDDEN);
    if (!view.shown_name.empty()) {
        t::line_text(view.caption, view.shown_name.c_str());
    }
}

void update(View &view, const core::Layout &layout, const core::EntityState &state, const core::Expectation *pending)
{
    if (view.card == nullptr) {
        return;
    }
    update_name(view, layout, state);
    if (view.tile == core::kNone) {
        return;
    }
    const core::Tile &tile = layout.tiles[view.tile];
    if (tile.kind == TileKind::Area) {
        update_icon(view, layout, state, true);
        return;
    }
    if (tile.kind == TileKind::Clock) {
        apply_look(view, Look{});
        update_icon(view, layout, state, true);
        tick_clock(view, true);
        return;
    }
    const bool known = state.known != 0;
    const bool available = known && !state.unavailable();
    Look look;
    look.pending = pending != nullptr;
    look.unavailable = known && !available;
    bool on = state.is_on();
    if (pending != nullptr && !pending->state.empty()) {
        on = pending->state == "on";
    }

    switch (tile.kind) {
    case TileKind::Light:
        if (tile.dimmer) {
            update_dimmer(view, state, pending, available);
            break;
        }
        look.active = on;
        update_light(view, state, pending, available);
        break;
    case TileKind::Switch:
        look.active = on;
        break;
    case TileKind::Climate:
        update_climate(view, state, pending, available);
        break;
    case TileKind::Sensor:
        // only a binary sensor is active; a number is not a state to highlight
        look.active = available && domain_of(layout.entity_id(tile)) == "binary_sensor" && on;
        set_value(view, !known ? std::string("...") : !available ? std::string("--")
                                                                      : sensor_text(tile, layout.entity_id(tile), state));
        break;
    case TileKind::Lock: {
        // the state in words ("Unlocking"), the expected one at once, filled in the color of the
        // state: unlocked green, locked orange, jammed red
        const std::string_view shown = pending != nullptr && !pending->state.empty() ? std::string_view(pending->state)
                                                                                      : std::string_view(state.state);
        const bool unlocked = shown == "unlocked" || shown == "unlocking" || shown == "open" || shown == "opening";
        const bool locked = shown == "locked" || shown == "locking";
        const bool jammed = shown == "jammed";
        look.active = available && (unlocked || locked || jammed);
        look.color = jammed ? t::kLockJammed : unlocked ? t::kLockUnlocked : t::kLockLocked;
        set_value(view, !known ? std::string("...") : !available ? std::string("--") : words(shown));
        break;
    }
    case TileKind::Weather:
        update_weather(view, state, known, available);
        break;
    default:
        break;
    }
    apply_look(view, look);
    // the off glyph only for what can be off
    const bool binary = tile.kind == TileKind::Switch || tile.kind == TileKind::Light ||
                        domain_of(layout.entity_id(tile)) == "binary_sensor";
    update_icon(view, layout, state, binary ? on : true);
}

} // namespace hc::ui::tiles
