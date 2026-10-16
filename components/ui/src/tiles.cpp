#include "tiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// The value in the largest font of the tile that fits its width ("Detected" and "612 ppm" in a
// small cell), down to the small font, which is cut off.
void set_value(const View &view, const std::string &text)
{
    if (view.value == nullptr) {
        return;
    }
    const lv_font_t *const fonts[] = {&value_font(view), &t::font_large(), &t::font_medium(), &t::font_normal(),
                                      &t::font_small()};
    const lv_coord_t width = view.value_width;
    const lv_font_t *chosen = fonts[std::size(fonts) - 1];
    for (const lv_font_t *font : fonts) {
        if (lv_txt_get_width(text.c_str(), static_cast<uint32_t>(text.size()), font, 0, LV_TEXT_FLAG_NONE) <= width) {
            chosen = font;
            break;
        }
    }
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
    case TileKind::Climate:
    case TileKind::Sensor:
    case TileKind::Lock:
    case TileKind::Picture:
    case TileKind::Weather:
        // a lock is never locked or unlocked by a tap on its tile, only in its panel
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
    // the name of a dimmer stands on its fill
    set_color(view.name, look.active || view.fill != nullptr ? t::kText : t::kTextLabel);
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
    // switched on, but the level it comes on at is not known yet: no number until HA reports it
    set_value(view, !available ? std::string("--")
                    : level < 0 ? std::string("On")
                    : on        ? std::to_string(level) + " %"
                                : std::string("Off"));
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

    const bool has_value = tile.kind == TileKind::Sensor || tile.kind == TileKind::Lock;
    if (view.wide && tile.kind != TileKind::Picture) {
        // the icon left, the value and the name right of it
        lv_obj_t *col = wide_row(view, view.large ? icons::large() : icons::small());
        if (has_value) {
            view.value = t::label(col, "", value_font(view), t::kText);
            lv_obj_set_width(view.value, LV_PCT(100));
            lv_label_set_long_mode(view.value, LV_LABEL_LONG_CLIP);
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
    if (has_value) {
        view.value = text(col, value_font(view), t::kText);
        lv_obj_set_width(view.value, LV_PCT(100));
        lv_label_set_long_mode(view.value, LV_LABEL_LONG_CLIP);
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
