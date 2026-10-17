#include "climate_panel.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include "dashboard.hpp"
#include "history_view.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "tiles.hpp"
#include "climate_parts.hpp"

namespace hc::ui::climate_panel::detail {

namespace {

namespace t = theme;
using panel::set_hidden;
using panel::set_text;

constexpr const char *kOptionNames[kOptions] = {"Mode", "Preset", "Fan"};

// a pill is at most this tall in the landscape column, this far from the next one
constexpr lv_coord_t kPillHeight = 64;
constexpr lv_coord_t kPillGap = 6;
// a pill this wide has room for the glyph left of its texts
constexpr lv_coord_t kPillGlyphWidth = 100;

} // namespace

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

namespace {

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

} // namespace

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

core::Expectation range_expectation(float low, float high)
{
    core::Expectation expected;
    expected.target_low = low;
    expected.target_high = high;
    return expected;
}

void send_range(core::EntityIndex entity, float low, float high)
{
    core::Intent intent;
    intent.kind = core::IntentKind::SetTemperatureRange;
    intent.entity = entity;
    intent.value = low;
    intent.value2 = high;
    dashboard::act(intent, range_expectation(low, high));
}

namespace {

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
    const lv_area_t square = shown ? panel::left_of_action(g.frame) : g.frame.action;
    lv_obj_set_pos(g.history, square.x1, square.y1);
    panel::name_before(g.frame, square);
}

// ------------------------------------------------------------------------------- pills

void clear_control()
{
    send_steps();
    history_view::forget();
    if (g.control != nullptr) {
        lv_obj_del(g.control);
    }
    g.control = nullptr;
    g.arc = g.action = g.setpoint = g.current = g.plus = g.minus = nullptr;
    g.ring = g.knobs[kLow] = g.knobs[kHigh] = g.ends[kLow] = g.ends[kHigh] = nullptr;
    g.range = false;
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
    if (g.show_history) {
        // the room temperature over time in the place of the arc; the option is kept for the way back
        history_view::build(g.control, {0, 0, static_cast<lv_coord_t>(w - 1), static_cast<lv_coord_t>(h - 1)}, g.tile,
                            core::HistoryKind::Temperature, {});
        return;
    }
    if (option < 0) {
        build_arc(w, h);
    } else {
        build_list(w, h);
    }
}

void on_pill(lv_event_t *event)
{
    const auto option = static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    // a second tap on the pill of the list goes back to the arc; from the history the pill's list opens
    const bool from_history = g.show_history;
    g.show_history = false;
    show_control(option == g.list_option && !from_history ? -1 : option);
    update();
}

void on_history_button(lv_event_t *)
{
    g.show_history = !g.show_history;
    show_control(g.list_option);
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
        // "Heat cool" in a narrow pill: the small font before "..."
        t::line_text(g.pill_values[i], words.c_str(), &t::font_small());
        const char *glyph = option_glyph(option, value);
        if (glyph != nullptr && std::string_view(glyph) != lv_label_get_text(g.pill_glyphs[i])) {
            lv_label_set_text(g.pill_glyphs[i], glyph);
        }
    }
}

} // namespace

} // namespace hc::ui::climate_panel::detail

namespace hc::ui::climate_panel {

using namespace detail;

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
    const lv_area_t history = panel::left_of_action(frame);
    g.history = panel::card(frame.root, history);
    lv_obj_center(t::label(g.history, icons::glyph("chart-timeline-variant"), icons::small(), t::kTextLabel));
    lv_obj_add_event_cb(g.history, on_history_button, LV_EVENT_CLICKED, nullptr);
    panel::name_before(frame, history);

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
            g.list_option = -1;
            if (!g.show_history) {
                show_control(-1);
            }
        }
    }
    // the range of the arc arrives with the panel's fields, after the panel was built; a mode
    // change brings or takes the two setpoints of heat_cool
    float low = 0;
    float high = 0;
    if (!g.show_history && g.list_option < 0 && !g.dragging && !collecting_steps() &&
        ((!std::isnan(s.min_temp) && s.min_temp != g.min) || (!std::isnan(s.max_temp) && s.max_temp != g.max) ||
         tiles::climate_step(dashboard::layout().tiles[g.tile], s) != g.step ||
         tiles::climate_range(s, pending, low, high) != g.range)) {
        show_control(-1);
    }
    // decided as soon as the mode and the modes are known (the panel's fields come after it opens)
    const std::string_view mode = shown_value(s, pending, Option::Mode);
    if (g.modes_first && avail && !mode.empty() && (offered & (1u << static_cast<int>(Option::Mode))) != 0) {
        g.modes_first = false;
        if (mode == "off" && g.list_option < 0 && !g.dragging && !collecting_steps() && !g.show_history) {
            show_control(static_cast<int>(Option::Mode));
        }
    }
    update_power(s, pending, avail);
    update_pills(s, pending, avail);
    panel::mark_option(g.history, g.show_history);
    if (g.show_history) {
        history_view::update();
    } else if (g.list_option < 0) {
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
    history_view::forget();
    const float target = g.step_target;
    const float low = g.step_range[kLow];
    const float high = g.step_range[kHigh];
    const core::EntityIndex entity = g.entity;
    // the objects go with the panel's root
    g = {};
    if (!std::isnan(low)) {
        send_range(entity, low, high);
    }
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
