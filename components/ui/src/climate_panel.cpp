#include "climate_panel.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "dashboard.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::climate_panel {

namespace {

namespace t = theme;
using panel::set_hidden;
using panel::set_text;

// the three options of the thermostat card
enum class Option : uint8_t { Mode, Preset, Fan, Count };
constexpr int kOptions = static_cast<int>(Option::Count);
constexpr const char *kOptionNames[kOptions] = {"Mode", "Preset", "Fan"};

// a pill is at most this tall in the landscape column, this far from the next one
constexpr lv_coord_t kPillHeight = 64;
constexpr lv_coord_t kPillGap = 6;
// a pill this wide has room for the glyph left of its texts
constexpr lv_coord_t kPillGlyphWidth = 100;
// the arc's ring is this part of its diameter, at least kRingMin
constexpr lv_coord_t kRingDivisor = 10;
constexpr lv_coord_t kRingMin = 12;
// A 270 degree arc with the gap at the bottom covers 0.85355 of its box in height (the ends are
// r * cos(45) below the center): the eye measures the gaps against the ring, so the ring is centered.
constexpr float kRingHeight = 0.85355f;
// between the arc and its steppers
constexpr lv_coord_t kArcStepGap = 16;
// the steps are sent once the taps pause this long (as on the climate tile)
constexpr uint32_t kStepSendMs = 700;
// the arc's range when the entity reports none
constexpr float kDefaultMin = 7;
constexpr float kDefaultMax = 35;

struct State {
    panel::Frame frame;
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    lv_obj_t *power = nullptr; // turns the climate off; shown only while it is on
    lv_obj_t *pills[kOptions] = {};
    lv_obj_t *pill_glyphs[kOptions] = {};
    lv_obj_t *pill_values[kOptions] = {};
    uint8_t offered = 0;     // the options the pills are laid out for
    int list_option = -1;    // the option whose list is shown, -1: the arc
    // while the climate is off the arc does nothing: the panel shows the modes first, once per
    // opening and after the power button (a tap on the pill goes to the arc)
    bool modes_first = true;
    lv_obj_t *control = nullptr;

    // arc
    lv_obj_t *arc = nullptr;
    lv_obj_t *action = nullptr;
    lv_obj_t *setpoint = nullptr;
    lv_obj_t *current = nullptr;
    lv_obj_t *plus = nullptr;
    lv_obj_t *minus = nullptr;
    float min = kDefaultMin;    // the range the arc was built for
    float max = kDefaultMax;
    float step = 0.5f;
    bool dragging = false;
    float drag_target = 0;
    float step_target = core::kUnknown; // the setpoint the steps have collected
    lv_timer_t *send_timer = nullptr;
    uint32_t last_step = 0;

    // list
    lv_obj_t *list = nullptr;
    memory::Vector<memory::String> items; // the values the items were made from
};

State g;

const core::EntityState &state()
{
    return dashboard::state(g.entity);
}

bool available()
{
    const core::EntityState &s = state();
    return s.known != 0 && !s.unavailable();
}

const memory::Vector<memory::String> &values_of(const core::EntityState &s, Option option)
{
    switch (option) {
    case Option::Preset:
        return s.preset_modes;
    case Option::Fan:
        return s.fan_modes;
    default:
        return s.hvac_modes;
    }
}

// the value an option shows: the pending action's, else the entity's
std::string_view shown_value(const core::EntityState &s, const core::Expectation *pending, Option option)
{
    switch (option) {
    case Option::Preset:
        return pending != nullptr && !pending->preset.empty() ? std::string_view(pending->preset)
                                                              : std::string_view(s.preset_mode);
    case Option::Fan:
        return pending != nullptr && !pending->fan_mode.empty() ? std::string_view(pending->fan_mode)
                                                                : std::string_view(s.fan_mode);
    default:
        return pending != nullptr && !pending->state.empty() ? std::string_view(pending->state)
                                                             : std::string_view(s.state);
    }
}

// the options the entity offers: an option with its list of values
uint8_t offered_options(const core::EntityState &s)
{
    uint8_t options = 0;
    for (int i = 0; i < kOptions; i++) {
        if (!values_of(s, static_cast<Option>(i)).empty()) {
            options |= static_cast<uint8_t>(1u << i);
        }
    }
    return options;
}

// the glyph of an hvac mode, as the HA frontend has them
const char *mode_glyph(std::string_view mode)
{
    const char *name = mode == "heat"        ? "fire"
                       : mode == "cool"      ? "snowflake"
                       : mode == "heat_cool" ? "sun-snowflake-variant"
                       : mode == "auto"      ? "thermostat-auto"
                       : mode == "dry"       ? "water-percent"
                       : mode == "fan_only"  ? "fan"
                       : mode == "off"       ? "power"
                                             : "thermostat";
    return icons::glyph(name);
}

const char *option_glyph(Option option, std::string_view value)
{
    switch (option) {
    case Option::Preset:
        return icons::glyph("record");
    case Option::Fan:
        return icons::glyph("fan");
    default:
        return mode_glyph(value);
    }
}

void send(core::IntentKind kind, float value, std::string_view text, core::Expectation expected)
{
    core::Intent intent;
    intent.kind = kind;
    intent.entity = g.entity;
    intent.value = value;
    intent.text.assign(text.data(), text.size());
    dashboard::act(intent, std::move(expected));
}

void send_target(float target)
{
    core::Expectation expected;
    expected.target = target;
    send(core::IntentKind::SetTemperature, target, {}, std::move(expected));
}

// ------------------------------------------------------------------------------- arc

float clamp_target(float target)
{
    target = std::round(target / g.step) * g.step;
    return std::clamp(target, g.min, g.max);
}

void send_steps()
{
    if (g.send_timer != nullptr) {
        lv_timer_del(g.send_timer);
        g.send_timer = nullptr;
    }
    if (!std::isnan(g.step_target)) {
        const float target = g.step_target;
        g.step_target = core::kUnknown;
        send_target(target);
    }
}

// the setpoint the panel shows: a drag, then the pending action, then the entity's
float shown_target(const core::EntityState &s, const core::Expectation *pending)
{
    if (g.dragging) {
        return g.drag_target;
    }
    if (pending != nullptr && !std::isnan(pending->target)) {
        return pending->target;
    }
    return s.target;
}

void on_step(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED_REPEAT && lv_tick_elaps(g.last_step) < panel::kStepRepeatMs) {
        return;
    }
    const core::EntityState &s = state();
    float base = !std::isnan(g.step_target) ? g.step_target : shown_target(s, dashboard::pending(g.entity));
    if (!available() || std::isnan(base)) {
        return;
    }
    g.last_step = lv_tick_get();
    const float target = clamp_target(base + (lv_event_get_target(event) == g.plus ? g.step : -g.step));
    if (target == base) {
        return;
    }
    g.step_target = target;
    // shown at once as pending, sent when the taps pause
    core::Expectation expected;
    expected.target = target;
    dashboard::expect(g.entity, std::move(expected));
    if (g.send_timer != nullptr) {
        lv_timer_reset(g.send_timer);
    } else {
        g.send_timer = lv_timer_create([](lv_timer_t *) { send_steps(); }, kStepSendMs, nullptr);
    }
}

void on_arc(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
    case LV_EVENT_VALUE_CHANGED:
        if (!available()) {
            return;
        }
        // steps that are collected give way to the drag
        if (g.send_timer != nullptr) {
            lv_timer_del(g.send_timer);
            g.send_timer = nullptr;
            g.step_target = core::kUnknown;
        }
        g.dragging = true;
        g.drag_target = clamp_target(g.min + static_cast<float>(lv_arc_get_value(g.arc)) * g.step);
        update();
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (g.dragging) {
            g.dragging = false;
            send_target(g.drag_target);
        }
        break;
    default:
        break;
    }
}

lv_obj_t *column_label(lv_obj_t *parent, const lv_font_t &font, uint32_t color)
{
    lv_obj_t *label = t::label(parent, "", font, color);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

void build_arc(lv_coord_t w, lv_coord_t h)
{
    const core::EntityState &s = state();
    g.min = !std::isnan(s.min_temp) ? s.min_temp : kDefaultMin;
    g.max = !std::isnan(s.max_temp) && s.max_temp > g.min ? s.max_temp : std::max(kDefaultMax, g.min + 1);
    g.step = tiles::climate_step(dashboard::layout().tiles[g.tile], s);

    // landscape: the steppers stacked right of the arc (+ above -, like the bars of the tile);
    // portrait: a row under it (- left of +)
    const bool portrait = g.frame.portrait;
    const lv_coord_t step = std::min<lv_coord_t>(panel::kStepSize, (h - panel::kStepGap) / 2);
    lv_coord_t size = 0;
    lv_coord_t arc_x = 0;
    lv_coord_t arc_y = 0;
    if (portrait) {
        size = static_cast<lv_coord_t>(std::min<float>(w, (h - step - kArcStepGap) / kRingHeight));
        const auto ring = static_cast<lv_coord_t>(size * kRingHeight);
        arc_x = static_cast<lv_coord_t>((w - size) / 2);
        arc_y = static_cast<lv_coord_t>((h - (ring + kArcStepGap + step)) / 2);
        const lv_coord_t row_y = static_cast<lv_coord_t>(arc_y + ring + kArcStepGap);
        const lv_coord_t row_x = static_cast<lv_coord_t>((w - (2 * step + 3 * panel::kStepGap)) / 2);
        g.minus = panel::step_button(g.control, row_x, row_y, step, "minus", on_step);
        g.plus = panel::step_button(g.control, static_cast<lv_coord_t>(row_x + step + 3 * panel::kStepGap), row_y, step,
                                    "plus", on_step);
    } else {
        size = std::min<lv_coord_t>(static_cast<lv_coord_t>(h / kRingHeight), static_cast<lv_coord_t>(w - kArcStepGap - step));
        size = std::min(size, w);
        const auto ring = static_cast<lv_coord_t>(size * kRingHeight);
        arc_x = static_cast<lv_coord_t>((w - (size + kArcStepGap + step)) / 2);
        arc_y = static_cast<lv_coord_t>((h - ring) / 2);
        const lv_coord_t step_x = static_cast<lv_coord_t>(arc_x + size + kArcStepGap);
        const lv_coord_t step_y = static_cast<lv_coord_t>((h - (2 * step + panel::kStepGap)) / 2);
        g.plus = panel::step_button(g.control, step_x, step_y, step, "plus", on_step);
        g.minus = panel::step_button(g.control, step_x, static_cast<lv_coord_t>(step_y + step + panel::kStepGap), step,
                                     "minus", on_step);
    }

    const lv_coord_t ring = std::max<lv_coord_t>(kRingMin, size / kRingDivisor);
    g.arc = lv_arc_create(g.control);
    lv_obj_remove_style_all(g.arc);
    lv_obj_set_pos(g.arc, arc_x, arc_y);
    lv_obj_set_size(g.arc, size, size);
    lv_arc_set_rotation(g.arc, 135);
    lv_arc_set_bg_angles(g.arc, 0, 270);
    lv_arc_set_range(g.arc, 0, static_cast<int16_t>(std::lround((g.max - g.min) / g.step)));
    lv_obj_set_style_arc_width(g.arc, ring, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g.arc, t::color(t::kBorder), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(g.arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g.arc, ring, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g.arc, true, LV_PART_INDICATOR);
    // the knob as thick as the ring, white
    lv_obj_set_style_bg_opa(g.arc, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(g.arc, t::color(t::kText), LV_PART_KNOB);
    lv_obj_set_style_radius(g.arc, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g.arc, 0, LV_PART_KNOB);
    // only the ring takes touches, the middle and the corners do not
    lv_obj_add_flag(g.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g.arc, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(g.arc, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(g.arc, LV_OBJ_FLAG_SCROLL_CHAIN);
    for (const lv_event_code_t code : {LV_EVENT_VALUE_CHANGED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(g.arc, on_arc, code, nullptr);
    }

    // inside: the action, the setpoint, the room temperature; a fixed box at the center
    lv_obj_t *col = t::plain(g.control);
    const auto inner = static_cast<lv_coord_t>(size - 2 * ring - 8);
    lv_obj_set_size(col, inner, inner);
    lv_obj_set_pos(col, static_cast<lv_coord_t>(arc_x + (size - inner) / 2), static_cast<lv_coord_t>(arc_y + (size - inner) / 2));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    const bool large = size >= 180;
    g.action = column_label(col, large ? t::font_medium() : t::font_normal(), t::kActive);
    g.setpoint = column_label(col, t::font_huge(), t::kText);
    column_label(col, t::font_small(), t::kTextLabel);
    lv_label_set_text(lv_obj_get_child(col, 2), large ? "Current temperature" : "Current");
    g.current = column_label(col, large ? t::font_medium() : t::font_normal(), t::kTextValue);
}

void update_arc(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    const float target = shown_target(s, pending);
    const bool known = avail && !std::isnan(target);
    const bool on = avail && s.state != "off";
    lv_obj_set_style_arc_color(g.arc, t::color(on ? t::kActive : t::kTextMuted), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(g.arc, static_cast<lv_opa_t>(known ? LV_OPA_COVER : LV_OPA_TRANSP), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g.arc, static_cast<lv_opa_t>(known ? LV_OPA_COVER : LV_OPA_TRANSP), LV_PART_KNOB);
    if (known && !g.dragging) {
        lv_arc_set_value(g.arc, static_cast<int16_t>(std::lround((std::clamp(target, g.min, g.max) - g.min) / g.step)));
    }
    if (known) {
        lv_obj_clear_state(g.arc, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(g.arc, LV_STATE_DISABLED);
    }
    std::string action;
    if (avail) {
        action = !s.hvac_action.empty() ? tiles::words(s.hvac_action) : tiles::words(s.state);
    }
    set_text(g.action, action);
    lv_obj_set_style_text_color(g.action, t::color(on ? t::kActive : t::kTextLabel), 0);
    set_text(g.setpoint, known ? tiles::temperature(target) + "\xc2\xb0" : std::string("--"));
    // a setpoint on its way is drawn in the pending color
    const bool waiting = !g.dragging && pending != nullptr && !std::isnan(pending->target);
    lv_obj_set_style_text_color(g.setpoint, t::color(waiting ? panel::kPendingBorder : t::kText), 0);
    set_text(g.current, avail && !std::isnan(s.current) ? tiles::temperature(s.current) + "\xc2\xb0" : std::string("--"));
    set_hidden(g.plus, !known);
    set_hidden(g.minus, !known);
}

// ------------------------------------------------------------------------------- lists

void show_control(int option);

void on_item(lv_event_t *event)
{
    if (!available() || g.list_option < 0) {
        return;
    }
    const auto index = static_cast<size_t>(lv_obj_get_index(lv_event_get_current_target(event)));
    if (index >= g.items.size()) {
        return;
    }
    const memory::String value = g.items[index];
    core::Expectation expected;
    core::IntentKind kind = core::IntentKind::SetHvacMode;
    switch (static_cast<Option>(g.list_option)) {
    case Option::Preset:
        kind = core::IntentKind::SetPreset;
        expected.preset = value;
        break;
    case Option::Fan:
        kind = core::IntentKind::SetFanMode;
        expected.fan_mode = value;
        break;
    default:
        expected.state = value;
        break;
    }
    send(kind, 0, value, std::move(expected));
    // the choice is made: back to the arc, after this event (the item goes with the list)
    lv_async_call(
        [](void *) {
            if (g.control != nullptr) { // the panel may have closed meanwhile
                show_control(-1);
                update();
            }
        },
        nullptr);
}


void build_list(lv_coord_t w, lv_coord_t h)
{
    g.list = panel::list(g.control, w, h);
    g.items.clear();
}

void update_list(const core::EntityState &s, const core::Expectation *pending)
{
    const auto option = static_cast<Option>(g.list_option);
    const std::string_view current = shown_value(s, pending, option);
    const lv_coord_t w = lv_area_get_width(&g.frame.control);
    const lv_coord_t h = lv_area_get_height(&g.frame.control);
    const bool filled = values_of(s, option) != g.items;
    if (filled) {
        g.items = values_of(s, option);
        panel::list_fill(g.list, w, h, g.items, option == Option::Mode ? mode_glyph : nullptr, tiles::words, on_item);
    }
    panel::list_mark(g.list, g.items, current);
    if (filled) {
        // the value that is set in view when the list opens
        panel::list_reveal(g.list, g.items, current, h);
    }
}

// ------------------------------------------------------------------------------- power

bool offers_off(const core::EntityState &s)
{
    return std::find(s.hvac_modes.begin(), s.hvac_modes.end(), "off") != s.hvac_modes.end();
}

// Off through set_hvac_mode: climate.turn_off needs the entity to declare it (a supported
// feature), "off" in the hvac modes is all this needs. Turning on again is the mode pill's job.
void on_power(lv_event_t *)
{
    const core::EntityState &s = state();
    if (!available() || !offers_off(s)) {
        return;
    }
    // a setpoint the steps collected goes first
    send_steps();
    core::Expectation expected;
    expected.state = "off";
    g.modes_first = true;
    send(core::IntentKind::SetHvacMode, 0, "off", std::move(expected));
}

// The button shows while the mode is not off (the pending one counts, so it goes at the tap); the
// name has the whole header without it.
void update_power(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    const std::string_view mode = shown_value(s, pending, Option::Mode);
    const bool shown = avail && offers_off(s) && !mode.empty() && mode != "off";
    if (shown == !lv_obj_has_flag(g.power, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    set_hidden(g.power, !shown);
    const lv_coord_t end = shown ? g.frame.header.x2 : g.frame.action.x2;
    lv_obj_set_width(g.frame.name, static_cast<lv_coord_t>(end - g.frame.header.x1 + 1));
}

// ------------------------------------------------------------------------------- pills

void clear_control()
{
    send_steps();
    if (g.control != nullptr) {
        lv_obj_del(g.control);
    }
    g.control = nullptr;
    g.arc = g.action = g.setpoint = g.current = g.plus = g.minus = nullptr;
    g.dragging = false;
    g.list = nullptr;
    g.items.clear();
}

// the arc (option -1) or the list of an option
void show_control(int option)
{
    clear_control();
    g.list_option = option;
    const lv_area_t &area = g.frame.control;
    const lv_coord_t w = lv_area_get_width(&area);
    const lv_coord_t h = lv_area_get_height(&area);
    g.control = t::plain(g.frame.root);
    lv_obj_set_pos(g.control, area.x1, area.y1);
    lv_obj_set_size(g.control, w, h);
    lv_obj_clear_flag(g.control, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g.control, LV_OBJ_FLAG_CLICKABLE);
    if (option < 0) {
        build_arc(w, h);
    } else {
        build_list(w, h);
    }
}

void on_pill(lv_event_t *event)
{
    const auto option = static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    // a second tap on the pill of the list goes back to the arc
    show_control(option == g.list_option ? -1 : option);
    update();
}

// The pills of the offered options: a column in landscape, a row in portrait.
void layout_pills(uint8_t offered)
{
    g.offered = offered;
    int count = 0;
    for (int i = 0; i < kOptions; i++) {
        count += (offered >> i) & 1;
    }
    const lv_area_t &area = g.frame.options;
    const lv_coord_t aw = lv_area_get_width(&area);
    const lv_coord_t ah = lv_area_get_height(&area);
    lv_coord_t w = aw;
    lv_coord_t h = ah;
    if (count > 0) {
        if (g.frame.portrait) {
            w = static_cast<lv_coord_t>((aw - (count - 1) * kPillGap) / count);
        } else {
            h = std::min<lv_coord_t>(kPillHeight, static_cast<lv_coord_t>((ah - (count - 1) * kPillGap) / count));
        }
    }
    int shown = 0;
    for (int i = 0; i < kOptions; i++) {
        const bool visible = ((offered >> i) & 1) != 0;
        set_hidden(g.pills[i], !visible);
        if (!visible) {
            continue;
        }
        const lv_coord_t x = g.frame.portrait ? static_cast<lv_coord_t>(area.x1 + shown * (w + kPillGap)) : area.x1;
        const lv_coord_t y = g.frame.portrait ? area.y1 : static_cast<lv_coord_t>(area.y1 + shown * (h + kPillGap));
        lv_obj_set_pos(g.pills[i], x, y);
        lv_obj_set_size(g.pills[i], w, h);
        set_hidden(g.pill_glyphs[i], w < kPillGlyphWidth);
        shown++;
    }
}

void update_pills(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    for (int i = 0; i < kOptions; i++) {
        const auto option = static_cast<Option>(i);
        const bool selected = g.list_option == i;
        const std::string_view value = shown_value(s, pending, option);
        lv_obj_set_style_bg_color(g.pills[i], t::color(selected ? t::kText : t::kCard), 0);
        const uint32_t text = selected ? t::kBackground : t::kText;
        lv_obj_set_style_text_color(g.pill_values[i], t::color(text), 0);
        lv_obj_set_style_text_color(g.pill_glyphs[i], t::color(text), 0);
        const std::string words = avail && !value.empty() ? tiles::words(value) : std::string("--");
        t::line_text(g.pill_values[i], words.c_str());
        const char *glyph = option_glyph(option, value);
        if (glyph != nullptr && std::string_view(glyph) != lv_label_get_text(g.pill_glyphs[i])) {
            lv_label_set_text(g.pill_glyphs[i], glyph);
        }
    }
}

} // namespace

void build(const panel::Frame &frame, core::TileIndex tile)
{
    forget();
    g.frame = frame;
    g.tile = tile;
    g.entity = dashboard::layout().tiles[tile].entity;

    // drawn like the power button of a light that is on
    g.power = panel::card(frame.root, frame.action);
    lv_obj_set_style_bg_color(g.power, t::color(t::kActive), 0);
    lv_obj_set_style_border_color(g.power, t::color(t::kActive), 0);
    lv_obj_center(t::label(g.power, icons::glyph("power"), icons::small(), t::kText));
    lv_obj_add_event_cb(g.power, on_power, LV_EVENT_CLICKED, nullptr);

    for (int i = 0; i < kOptions; i++) {
        lv_obj_t *pill = panel::card(frame.root, frame.options);
        g.pills[i] = pill;
        lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(pill, 8, 0);
        lv_obj_set_style_pad_column(pill, 6, 0);
        g.pill_glyphs[i] = t::label(pill, "", icons::small(), t::kText);
        // the name of the option above its value
        lv_obj_t *texts = t::plain(pill);
        lv_obj_set_height(texts, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(texts, 1);
        lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(texts, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *name = t::label(texts, kOptionNames[i], t::font_small(), t::kTextLabel);
        lv_obj_set_width(name, LV_PCT(100));
        g.pill_values[i] = t::label(texts, "", t::font_normal(), t::kText);
        lv_obj_set_width(g.pill_values[i], LV_PCT(100));
        lv_obj_add_event_cb(pill, on_pill, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }
    layout_pills(offered_options(state()));
    show_control(-1);
}

void update()
{
    if (g.control == nullptr) {
        return;
    }
    const core::EntityState &s = state();
    const core::Expectation *pending = dashboard::pending(g.entity);
    const bool avail = available();

    const uint8_t offered = offered_options(s);
    if (offered != g.offered) {
        layout_pills(offered);
        if (g.list_option >= 0 && (offered & (1u << g.list_option)) == 0) {
            show_control(-1);
        }
    }
    // the range of the arc arrives with the panel's fields, after the panel was built
    if (g.list_option < 0 && !g.dragging && std::isnan(g.step_target) &&
        ((!std::isnan(s.min_temp) && s.min_temp != g.min) || (!std::isnan(s.max_temp) && s.max_temp != g.max) ||
         tiles::climate_step(dashboard::layout().tiles[g.tile], s) != g.step)) {
        show_control(-1);
    }
    // decided as soon as the mode and the modes are known (the panel's fields come after it opens)
    const std::string_view mode = shown_value(s, pending, Option::Mode);
    if (g.modes_first && avail && !mode.empty() && (offered & (1u << static_cast<int>(Option::Mode))) != 0) {
        g.modes_first = false;
        if (mode == "off" && g.list_option < 0 && !g.dragging && std::isnan(g.step_target)) {
            show_control(static_cast<int>(Option::Mode));
        }
    }
    update_power(s, pending, avail);
    update_pills(s, pending, avail);
    if (g.list_option < 0) {
        update_arc(s, pending, avail);
    } else {
        update_list(s, pending);
    }
}

void forget()
{
    if (g.send_timer != nullptr) {
        lv_timer_del(g.send_timer);
    }
    const float target = g.step_target;
    const core::EntityIndex entity = g.entity;
    // the objects go with the panel's root
    g = {};
    if (!std::isnan(target)) {
        // the steps are not lost when the panel closes
        core::Intent intent;
        intent.kind = core::IntentKind::SetTemperature;
        intent.entity = entity;
        intent.value = target;
        core::Expectation expected;
        expected.target = target;
        dashboard::act(intent, std::move(expected));
    }
}

} // namespace hc::ui::climate_panel
