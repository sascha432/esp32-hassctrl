#include "light_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "dashboard.hpp"
#include "history_view.hpp"
#include "icons.hpp"
#include "lvgl_compat.hpp"
#include "sound.hpp"
#include "theme.hpp"
#include "tiles.hpp"
#include "light_parts.hpp"

namespace hc::ui::light_panel::detail {

namespace {

namespace t = theme;

constexpr const char *kModeGlyphs[kModes] = {"brightness-6", "palette", "thermometer", "creation"};

// the gap between the steppers and the slider; in a large landscape frame (the P4's dialog) further
using panel::kStepGap;
constexpr lv_coord_t kSliderStepGap = 20;
constexpr lv_coord_t kSliderStepGapLarge = 40;

// the level steps are sent once the taps pause this long
constexpr uint32_t kStepSendMs = 500;
using panel::kStepRepeatMs;
// the light step of docs/architecture.md section 5 when the tile has none
constexpr float kDefaultStep = 1;

using panel::set_hidden;
using panel::set_text;

} // namespace

State g;

// ------------------------------------------------------------------------------- helpers
const core::EntityState &state()
{
    return dashboard::state(g.entity);
}

bool available()
{
    const core::EntityState &s = state();
    return s.known != 0 && !s.unavailable();
}

namespace {

// The modes the light offers, from supported_color_modes and effect_list. Before they arrived a
// dimmer is taken to have a level.
uint8_t offered_modes(const core::EntityState &s)
{
    uint8_t modes = 0;
    if (s.color_modes.empty()) {
        modes |= bit(Mode::Level);
    }
    for (const auto &mode : s.color_modes) {
        if (mode != "onoff") {
            modes |= bit(Mode::Level);
        }
        if (mode == "hs" || mode == "xy" || mode == "rgb" || mode == "rgbw" || mode == "rgbww") {
            modes |= bit(Mode::Color);
        }
        if (mode == "color_temp") {
            modes |= bit(Mode::Temp);
        }
    }
    if (!s.effect_list.empty()) {
        modes |= bit(Mode::Effect);
    }
    return modes;
}

bool has_level(const core::EntityState &s)
{
    return (offered_modes(s) & bit(Mode::Level)) != 0;
}

} // namespace

void send(core::IntentKind kind, float value, float value2, std::string_view text, core::Expectation expected)
{
    core::Intent intent;
    intent.kind = kind;
    intent.entity = g.entity;
    intent.value = value;
    intent.value2 = value2;
    intent.text.assign(text.data(), text.size());
    dashboard::act(intent, std::move(expected));
}

namespace {

core::Expectation level_expectation(int level)
{
    core::Expectation expected;
    expected.state = level > 0 ? "on" : "off";
    if (level > 0) {
        expected.brightness = static_cast<int16_t>((level * 255 + 50) / 100);
    }
    return expected;
}

// ------------------------------------------------------------------------------- the slider

float slider_value_at(const Slider &s, lv_coord_t y)
{
    lv_area_t area;
    lv_obj_get_coords(s.track, &area);
    const auto height = static_cast<float>(std::max(1, static_cast<int>(lv_area_get_height(&area))));
    const float part = std::clamp((area.y2 - y) / height, 0.0f, 1.0f);
    return s.min + part * (s.max - s.min);
}

// the part of the track a value fills, 0 to 1
float slider_part(const Slider &s, float value)
{
    return s.max > s.min ? std::clamp((value - s.min) / (s.max - s.min), 0.0f, 1.0f) : 0.0f;
}

} // namespace

void slider_show(Slider &s, float value, bool known)
{
    const lv_coord_t height = s.height;
    const auto offset = static_cast<lv_coord_t>(std::lround(slider_part(s, value) * height));
    if (s.fill != nullptr) {
        lv_obj_set_height(s.fill, offset);
        set_hidden(s.fill, !known || offset == 0);
    }
    if (s.marker != nullptr) {
        // at the ends as far from the track's edge as from its sides
        constexpr lv_coord_t marker = kMarkerHeight;
        lv_obj_set_y(s.marker, static_cast<lv_coord_t>(std::clamp<int>(height - offset - marker / 2, kMarkerInset,
                                                                       height - marker - kMarkerInset)));
        set_hidden(s.marker, !known);
    }
}

namespace {

void on_slider(lv_event_t *event)
{
    auto &s = *static_cast<Slider *>(lv_event_get_user_data(event));
    lv_point_t point{};
    if (lv_indev_t *indev = lv_indev_get_act()) {
        lv_indev_get_point(indev, &point);
    }
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
    case LV_EVENT_PRESSING:
        if (!available()) {
            return;
        }
        s.dragging = true;
        s.drag_value = slider_value_at(s, point.y);
        update();
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (s.dragging) {
            s.dragging = false;
            s.on_release(s.drag_value);
        }
        break;
    default:
        break;
    }
}

} // namespace

Slider make_slider(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, float min, float max,
                   void (*on_release)(float))
{
    Slider s;
    s.height = h;
    s.min = min;
    s.max = max;
    s.on_release = on_release;
    s.track = panel::slider_track(parent, x, y, w, h);
    return s;
}

// the event handlers get the slider's address, so they are added once it has its place in g
void slider_events(Slider &s)
{
    for (const lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(s.track, on_slider, code, &s);
    }
}

lv_obj_t *slider_label(Slider &s, uint32_t color)
{
    s.value = t::label(s.track, "", t::font_huge(), color);
    lv_obj_center(s.value);
    return s.value;
}

// the level and the color temperature sliders look the same
panel::SliderPlace slider_place(lv_coord_t room, lv_coord_t h)
{
    return panel::slider_place(g.frame, room, h);
}

namespace {

// ------------------------------------------------------------------------------- level

void send_steps()
{
    if (g.send_timer != nullptr) {
        lv_timer_del(g.send_timer);
        g.send_timer = nullptr;
    }
    if (g.step_level >= 0) {
        const int level = g.step_level;
        g.step_level = -1;
        send(core::IntentKind::SetLevel, static_cast<float>(level), 0, {}, level_expectation(level));
    }
}

void on_step(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED_REPEAT && lv_tick_elaps(g.last_step) < kStepRepeatMs) {
        return;
    }
    if (!available()) {
        return;
    }
    g.last_step = lv_tick_get();
    const core::Tile &tile = dashboard::layout().tiles[g.tile];
    const int step = static_cast<int>(tile.step > 0 ? tile.step : kDefaultStep);
    int base = g.step_level;
    if (base < 0) {
        bool on = false;
        base = tiles::shown_level(state(), dashboard::pending(g.entity), on);
        if (base < 0) {
            base = std::max(0, (state().last_brightness * 100 + 127) / 255);
        }
    }
    const bool up = lv_event_get_target(event) == g.plus;
    // from a level between two steps, the next step of the grid
    int target = up ? (base / step + 1) * step : ((base + step - 1) / step - 1) * step;
    target = std::clamp(target, 0, 100);
    if (target == base) {
        return;
    }
    g.step_level = target;
    // shown at once as pending, sent when the taps pause
    dashboard::expect(g.entity, level_expectation(target));
    if (g.send_timer != nullptr) {
        lv_timer_reset(g.send_timer);
    } else {
        g.send_timer = lv_timer_create([](lv_timer_t *) { send_steps(); }, kStepSendMs, nullptr);
    }
}

void build_level(lv_coord_t w, lv_coord_t h)
{
    if (!has_level(state())) {
        // only on and off: the power button is the control, the state is shown large
        g.note = t::label(g.control, "", t::font_huge(), t::kText);
        lv_obj_center(g.note);
        return;
    }
    const lv_coord_t step = std::min<lv_coord_t>(panel::step_size(g.frame), (h - kStepGap) / 2);
    const lv_coord_t gap = g.frame.large && !g.frame.portrait ? kSliderStepGapLarge : kSliderStepGap;
    const panel::SliderPlace place = slider_place(static_cast<lv_coord_t>(w - gap - step), h);
    const lv_coord_t slider_w = place.w;
    const lv_coord_t slider_h = place.h;
    const lv_coord_t x = (w - (slider_w + gap + step)) / 2;
    const lv_coord_t y = place.y;
    g.level = make_slider(g.control, x, y, slider_w, slider_h, 0, 100, [](float value) {
        const int level = static_cast<int>(std::lround(value));
        send(core::IntentKind::SetLevel, static_cast<float>(level), 0, {}, level_expectation(level));
    });
    slider_events(g.level);
    g.level.fill = t::plain(g.level.track);
    lv_obj_set_width(g.level.fill, LV_PCT(100));
    lv_obj_align(g.level.fill, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(g.level.fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.level.fill, t::color(t::kActive), 0);
    lv_obj_clear_flag(g.level.fill, LV_OBJ_FLAG_CLICKABLE);
    slider_label(g.level, t::kText);

    // the steppers centered beside the slider
    const lv_coord_t step_x = x + slider_w + gap;
    const lv_coord_t step_y = y + (slider_h - (2 * step + kStepGap)) / 2;
    g.plus = panel::step_button(g.control, step_x, step_y, step, "plus", on_step);
    g.minus = panel::step_button(g.control, step_x, static_cast<lv_coord_t>(step_y + step + kStepGap), step, "minus", on_step);
}

void update_level(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    bool on = false;
    if (g.note != nullptr) {
        tiles::shown_level(s, pending, on);
        set_text(g.note, !avail ? "--" : on ? "On" : "Off");
        return;
    }
    if (g.level.track == nullptr) {
        return;
    }
    int level = 0;
    if (g.level.dragging) {
        level = static_cast<int>(std::lround(g.level.drag_value));
        on = level > 0;
    } else {
        level = tiles::shown_level(s, pending, on);
    }
    slider_show(g.level, static_cast<float>(std::max(level, 0)), avail && level > 0);
    set_text(g.level.value, !avail ? "--" : level < 0 ? "On" : on ? std::to_string(level) + " %" : "Off");
    for (lv_obj_t *step : {g.plus, g.minus}) {
        if (avail) {
            lv_obj_clear_state(step, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(step, LV_STATE_DISABLED);
        }
    }
}

// ------------------------------------------------------------------------------- effects

void on_effect(lv_event_t *event)
{
    if (!available()) {
        return;
    }
    const auto index = static_cast<size_t>(lv_obj_get_index(lv_event_get_current_target(event)));
    if (index >= g.effects.size()) {
        return;
    }
    const memory::String effect = g.effects[index];
    core::Expectation expected;
    expected.state = "on";
    expected.effect = effect;
    send(core::IntentKind::SetEffect, 0, 0, effect, std::move(expected));
}


void build_effects(lv_coord_t w, lv_coord_t h)
{
    g.list = panel::list(g.control, w, h);
    g.effects.clear();
}

void update_effects(const core::EntityState &s, const core::Expectation *pending)
{
    if (g.list == nullptr) {
        return;
    }
    const std::string_view current = pending != nullptr && !pending->effect.empty() ? std::string_view(pending->effect)
                                                                                    : std::string_view(s.effect);
    const lv_coord_t w = lv_area_get_width(&g.frame.control);
    const lv_coord_t h = lv_area_get_height(&g.frame.control);
    const bool filled = s.effect_list != g.effects;
    if (filled) {
        g.effects = s.effect_list;
        panel::list_fill(g.list, w, h, g.effects, nullptr, nullptr, on_effect);
    }
    panel::list_mark(g.list, g.effects, current);
    if (filled) {
        // the running effect in view when the list opens
        panel::list_reveal(g.list, g.effects, current, h);
    }
}

// ------------------------------------------------------------------------------- modes

void clear_control()
{
    send_steps();
    history_view::forget();
    if (g.control != nullptr) {
        lv_obj_del(g.control);
    }
    g.control = nullptr;
    g.level = {};
    g.plus = g.minus = g.note = nullptr;
    g.wheel = g.knob = nullptr;
    g.wheel_dragging = false;
    g.pixels.clear();
    g.pixels.shrink_to_fit();
    g.temp = {};
    g.list = nullptr;
    g.effects.clear();
}

void show_mode(Mode mode)
{
    clear_control();
    g.mode = mode;
    const lv_area_t &area = g.frame.control;
    const lv_coord_t w = lv_area_get_width(&area);
    const lv_coord_t h = lv_area_get_height(&area);
    g.control = t::plain(g.frame.root);
    lv_obj_set_pos(g.control, area.x1, area.y1);
    lv_obj_set_size(g.control, w, h);
    lv_obj_clear_flag(g.control, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g.control, LV_OBJ_FLAG_CLICKABLE);
    if (g.show_history) {
        // the on/off timeline in the place of the mode's control; the mode is kept for the way back
        history_view::build(g.control, {0, 0, static_cast<lv_coord_t>(w - 1), static_cast<lv_coord_t>(h - 1)}, g.tile,
                            core::HistoryKind::States, {});
        return;
    }
    switch (mode) {
    case Mode::Level:
        build_level(w, h);
        break;
    case Mode::Color:
        build_color(w, h);
        break;
    case Mode::Temp:
        build_temp(w, h);
        break;
    case Mode::Effect:
        build_effects(w, h);
        break;
    case Mode::Count:
        break;
    }
}

void on_mode(lv_event_t *event)
{
    const auto mode = static_cast<Mode>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (mode != g.mode || g.show_history) {
        g.show_history = false;
        show_mode(mode);
        update();
    }
}

void on_history_button(lv_event_t *)
{
    g.show_history = !g.show_history;
    show_mode(g.mode);
    update();
}

// The buttons of the offered modes: a column in landscape, a row in portrait. With only the level
// there is nothing to choose, and no button.
void layout_buttons(uint8_t offered)
{
    g.offered = offered;
    panel::layout_options(g.frame, g.buttons, kModes, offered);
}

void on_power(lv_event_t *)
{
    if (!available() && dashboard::pending(g.entity) == nullptr) {
        return;
    }
    // a level the steps collected goes first, the toggle then works on it
    send_steps();
    bool on = false;
    tiles::shown_level(state(), dashboard::pending(g.entity), on);
    core::Expectation expected;
    expected.state = on ? "off" : "on";
    send(core::IntentKind::Toggle, 0, 0, {}, std::move(expected));
}

void update_power(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    bool on = false;
    tiles::shown_level(s, pending, on);
    on = on && avail;
    lv_obj_set_style_bg_color(g.power, t::color(on ? t::kActive : t::kCard), 0);
    // no pending look: the panel shows the expected state at once
    lv_obj_set_style_border_color(g.power, t::color(on ? t::kActive : t::kBorder), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(g.power, 0), t::color(on ? t::kText : t::kTextLabel), 0);
}

void update_buttons()
{
    for (int i = 0; i < kModes; i++) {
        panel::mark_option(g.buttons[i], !g.show_history && static_cast<int>(g.mode) == i);
    }
    panel::mark_option(g.history, g.show_history);
}

} // namespace

} // namespace hc::ui::light_panel::detail

namespace hc::ui::light_panel {

using namespace detail;

void build(const panel::Frame &frame, core::TileIndex tile)
{
    forget();
    g.frame = frame;
    g.tile = tile;
    g.entity = dashboard::layout().tiles[tile].entity;

    g.power = panel::card(frame.root, frame.action);
    lv_obj_center(t::label(g.power, icons::glyph("power"), icons::small(), t::kTextLabel));
    lv_obj_add_event_cb(g.power, on_power, LV_EVENT_CLICKED, nullptr);
    const lv_area_t history = panel::left_of_action(frame);
    g.history = panel::card(frame.root, history);
    lv_obj_center(t::label(g.history, icons::glyph("chart-timeline-variant"), icons::small(), t::kTextLabel));
    lv_obj_add_event_cb(g.history, on_history_button, LV_EVENT_CLICKED, nullptr);
    panel::name_before(frame, history);

    for (int i = 0; i < kModes; i++) {
        g.buttons[i] = panel::card(frame.root, frame.options);
        lv_obj_center(t::label(g.buttons[i], icons::glyph(kModeGlyphs[i]), icons::small(), t::kTextLabel));
        lv_obj_add_event_cb(g.buttons[i], on_mode, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }
    layout_buttons(offered_modes(state()));
    show_mode(Mode::Level);
}

void update()
{
    if (g.power == nullptr) {
        return;
    }
    const core::EntityState &s = state();
    const core::Expectation *pending = dashboard::pending(g.entity);
    const bool avail = available();

    const uint8_t offered = offered_modes(s);
    if (offered != g.offered) {
        layout_buttons(offered);
    }
    // the level control differs for a light without a level, which is only known once the color
    // modes arrived
    const bool level_kind_changed = g.mode == Mode::Level && (g.note != nullptr) == has_level(s);
    if (!g.show_history && ((g.mode != Mode::Level && (offered & bit(g.mode)) == 0) || level_kind_changed)) {
        show_mode(Mode::Level);
    }
    update_power(s, pending, avail);
    update_buttons();
    if (g.show_history) {
        history_view::update();
        return;
    }
    switch (g.mode) {
    case Mode::Level:
        update_level(s, pending, avail);
        break;
    case Mode::Color:
        update_color(s, pending, avail);
        break;
    case Mode::Temp:
        update_temp(s, pending, avail);
        break;
    case Mode::Effect:
        update_effects(s, pending);
        break;
    case Mode::Count:
        break;
    }
}

void forget()
{
    if (g.send_timer != nullptr) {
        lv_timer_del(g.send_timer);
    }
    history_view::forget();
    const int level = g.step_level;
    const core::EntityIndex entity = g.entity;
    // the objects go with the panel's root; the pixels may go now, nothing draws them any more
    g = {};
    if (level >= 0) {
        // the steps are not lost when the panel closes
        core::Intent intent;
        intent.kind = core::IntentKind::SetLevel;
        intent.entity = entity;
        intent.value = static_cast<float>(level);
        dashboard::act(intent, level_expectation(level));
    }
}

} // namespace hc::ui::light_panel
