// The arc of the climate panel (docs/ui/lvgl/lvgl-panels.md): one setpoint on a 270 degree arc with
// its steppers, or the two setpoints of heat_cool as two knobs on one ring.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include "climate_parts.hpp"
#include "dashboard.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::climate_panel::detail {

namespace {

namespace t = theme;
using panel::set_hidden;
using panel::set_text;

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
// the arc's 270 degrees start at the bottom left (LVGL's angles: 0 at the right, clockwise)
constexpr float kArcRotation = 135;
constexpr float kArcSweep = 270;
constexpr float kRadiansPerDegree = std::numbers::pi_v<float> / 180;
// a touch this far outside the ring still takes a knob of the range
constexpr lv_coord_t kRingSlop = 10;

float clamp_target(float target)
{
    target = std::round(target / g.step) * g.step;
    return std::clamp(target, g.min, g.max);
}

} // namespace

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
    if (!std::isnan(g.step_range[kLow])) {
        const float low = g.step_range[kLow];
        const float high = g.step_range[kHigh];
        g.step_range[kLow] = g.step_range[kHigh] = core::kUnknown;
        send_range(g.entity, low, high);
    }
}

bool collecting_steps()
{
    return !std::isnan(g.step_target) || !std::isnan(g.step_range[kLow]);
}

namespace {

// steps that are collected give way to a drag
void drop_steps()
{
    if (g.send_timer != nullptr) {
        lv_timer_del(g.send_timer);
        g.send_timer = nullptr;
    }
    g.step_target = core::kUnknown;
    g.step_range[kLow] = g.step_range[kHigh] = core::kUnknown;
}

// the range the panel shows: a drag, then the pending action, then the entity's
bool shown_range(const core::EntityState &s, const core::Expectation *pending, float &low, float &high)
{
    if (g.dragging && g.range) {
        low = g.drag_range[kLow];
        high = g.drag_range[kHigh];
        return true;
    }
    return tiles::climate_range(s, pending, low, high);
}

// a setpoint of the range moved to `value`, not past the other one
void set_end(float (&range)[kEnds], int end, float value)
{
    range[end] = end == kLow ? std::min(value, range[kHigh]) : std::max(value, range[kLow]);
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
    if (!available()) {
        return;
    }
    const core::EntityState &s = state();
    const float delta = lv_event_get_target(event) == g.plus ? g.step : -g.step;
    if (g.range) {
        // the selected setpoint of the range
        float range[kEnds] = {g.step_range[kLow], g.step_range[kHigh]};
        if (std::isnan(range[kLow]) && !shown_range(s, dashboard::pending(g.entity), range[kLow], range[kHigh])) {
            return;
        }
        g.last_step = lv_tick_get();
        const float before = range[g.selected];
        set_end(range, g.selected, clamp_target(before + delta));
        if (range[g.selected] == before) {
            return;
        }
        g.step_range[kLow] = range[kLow];
        g.step_range[kHigh] = range[kHigh];
        // shown at once as pending, sent when the taps pause
        dashboard::expect(g.entity, range_expectation(range[kLow], range[kHigh]));
    } else {
        const float base = !std::isnan(g.step_target) ? g.step_target : shown_target(s, dashboard::pending(g.entity));
        if (std::isnan(base)) {
            return;
        }
        g.last_step = lv_tick_get();
        const float target = clamp_target(base + delta);
        if (target == base) {
            return;
        }
        g.step_target = target;
        core::Expectation expected;
        expected.target = target;
        dashboard::expect(g.entity, std::move(expected));
    }
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
        drop_steps();
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

// ------------------------------------------------------------------------------- range

// the angle on the arc (0 at its start, kArcSweep at its end) of a setpoint
float angle_of(float value)
{
    return (std::clamp(value, g.min, g.max) - g.min) / (g.max - g.min) * kArcSweep;
}

// a point of the screen from the ring's center: its distance and its angle on the arc (0 at the
// start; above kArcSweep in the gap at the bottom)
void polar(const lv_point_t &point, float &distance, float &angle)
{
    lv_area_t box;
    lv_obj_get_coords(g.ring, &box);
    const float dx = static_cast<float>(point.x) - static_cast<float>(box.x1 + box.x2) / 2;
    const float dy = static_cast<float>(point.y) - static_cast<float>(box.y1 + box.y2) / 2;
    distance = std::sqrt(dx * dx + dy * dy);
    angle = std::fmod(std::atan2(dy, dx) / kRadiansPerDegree - kArcRotation + 720.0f, 360.0f);
}

// The setpoint at a point of the screen; a point in the gap at the bottom is the nearer end.
float value_at(const lv_point_t &point)
{
    float distance = 0;
    float angle = 0;
    polar(point, distance, angle);
    if (angle > kArcSweep) {
        angle = angle > (360.0f + kArcSweep) / 2 ? 0 : kArcSweep;
    }
    return clamp_target(g.min + angle / kArcSweep * (g.max - g.min));
}

// only the band of the ring takes touches: not the middle, the corners or the gap at the bottom
// (where the portrait steppers are)
void on_ring_hit(lv_event_t *event)
{
    auto *info = static_cast<lv_hit_test_info_t *>(lv_event_get_param(event));
    float distance = 0;
    float angle = 0;
    polar(*info->point, distance, angle);
    const float outer = static_cast<float>(g.arc_size) / 2;
    const bool band = distance >= outer - static_cast<float>(g.ring_width + kRingSlop) && distance <= outer + kRingSlop;
    // the rounded ends reach a little into the gap
    info->res = band && (angle <= kArcSweep + 10 || angle >= 350);
}

void on_ring(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (g.dragging) {
            g.dragging = false;
            if (g.drag_range[kLow] != g.drag_from[kLow] || g.drag_range[kHigh] != g.drag_from[kHigh]) {
                send_range(g.entity, g.drag_range[kLow], g.drag_range[kHigh]);
            }
            update();
        }
        return;
    }
    if (!available()) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);
    const float value = value_at(point);
    if (code == LV_EVENT_PRESSED) {
        float low = 0;
        float high = 0;
        if (!shown_range(state(), dashboard::pending(g.entity), low, high)) {
            return;
        }
        drop_steps();
        // the nearer knob; of two that touch, the one on the finger's side
        g.selected = value < low ? kLow : value > high ? kHigh : value - low < high - value ? kLow : kHigh;
        g.drag_range[kLow] = g.drag_from[kLow] = low;
        g.drag_range[kHigh] = g.drag_from[kHigh] = high;
        g.dragging = true;
    }
    if (g.dragging) {
        set_end(g.drag_range, g.selected, value);
        update();
    }
}

void on_end(lv_event_t *event)
{
    g.selected = lv_event_get_current_target(event) == g.ends[kHigh] ? kHigh : kLow;
    update();
}

void place_knob(lv_obj_t *knob, float value)
{
    const float radians = (kArcRotation + angle_of(value)) * kRadiansPerDegree;
    const float radius = static_cast<float>(g.arc_size - g.ring_width) / 2;
    const float center = static_cast<float>(g.arc_size - g.ring_width) / 2;
    lv_obj_set_pos(knob, static_cast<lv_coord_t>(g.arc_x + std::lround(center + radius * std::cos(radians))),
                   static_cast<lv_coord_t>(g.arc_y + std::lround(center + radius * std::sin(radians))));
}

// the ring's touch area and the two knobs, over the arc that only draws
void build_range()
{
    lv_obj_clear_flag(g.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(g.arc, LV_OPA_TRANSP, LV_PART_KNOB);
    g.ring = t::plain(g.control);
    lv_obj_set_pos(g.ring, g.arc_x, g.arc_y);
    lv_obj_set_size(g.ring, g.arc_size, g.arc_size);
    lv_obj_add_flag(g.ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g.ring, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(g.ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g.ring, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(g.ring, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(g.ring, on_ring_hit, LV_EVENT_HIT_TEST, nullptr);
    for (const lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(g.ring, on_ring, code, nullptr);
    }
    for (lv_obj_t *&knob : g.knobs) {
        // as thick as the ring, like the knob of the arc; the touches go through to the ring
        knob = t::plain(g.control);
        lv_obj_set_size(knob, g.ring_width, g.ring_width);
        lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
        lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    }
}

lv_obj_t *column_label(lv_obj_t *parent, const lv_font_t &font, uint32_t color)
{
    lv_obj_t *label = t::label(parent, "", font, color);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

} // namespace

void build_arc(lv_coord_t w, lv_coord_t h)
{
    const core::EntityState &s = state();
    g.min = !std::isnan(s.min_temp) ? s.min_temp : kDefaultMin;
    g.max = !std::isnan(s.max_temp) && s.max_temp > g.min ? s.max_temp : std::max(kDefaultMax, g.min + 1);
    g.step = tiles::climate_step(dashboard::layout().tiles[g.tile], s);

    // landscape: the steppers stacked right of the arc (+ above -, like the bars of the tile);
    // portrait: a row under it (- left of +)
    const bool portrait = g.frame.portrait;
    const lv_coord_t step = std::min<lv_coord_t>(panel::step_size(g.frame), (h - panel::kStepGap) / 2);
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
    g.arc_x = arc_x;
    g.arc_y = arc_y;
    g.arc_size = size;
    g.ring_width = ring;
    float low = 0;
    float high = 0;
    g.range = tiles::climate_range(s, dashboard::pending(g.entity), low, high);
    if (g.range) {
        build_range();
    }
    // The arc's hit test takes the whole ring, its gap at the bottom too: the portrait steppers lie
    // in it and the arc, made after them, took their taps. They go in front of it.
    lv_obj_move_foreground(g.minus);
    lv_obj_move_foreground(g.plus);

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
    if (g.range) {
        // the two setpoints side by side, low left of high
        lv_obj_t *row = t::plain(col);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        for (lv_obj_t *&end : g.ends) {
            end = t::label(row, "", inner >= 170 ? t::font_huge() : t::font_large(), t::kText);
            // a finger's room around the digits
            lv_obj_set_style_pad_hor(end, 6, 0);
            lv_obj_set_style_pad_ver(end, 8, 0);
            lv_obj_add_flag(end, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(end, on_end, LV_EVENT_CLICKED, nullptr);
        }
    } else {
        g.setpoint = column_label(col, t::font_huge(), t::kText);
    }
    lv_obj_t *caption = column_label(col, t::font_small(), t::kTextLabel);
    lv_label_set_text(caption, large ? "Current temperature" : "Current");
    g.current = column_label(col, large ? t::font_medium() : t::font_normal(), t::kTextValue);
}

namespace {

// the arc of one setpoint
void update_target(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    const float target = shown_target(s, pending);
    const bool known = avail && !std::isnan(target);
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
    set_text(g.setpoint, known ? tiles::temperature(target) + "\xc2\xb0" : std::string("--"));
    // a setpoint on its way is drawn in the pending color
    const bool waiting = !g.dragging && pending != nullptr && !std::isnan(pending->target);
    lv_obj_set_style_text_color(g.setpoint, t::color(waiting ? t::kAccent : t::kText), 0);
    set_hidden(g.plus, !known);
    set_hidden(g.minus, !known);
}

// the arc of a range: the indicator between the knobs, the selected setpoint white
void update_range(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    float range[kEnds] = {};
    const bool known = avail && shown_range(s, pending, range[kLow], range[kHigh]);
    lv_obj_set_style_arc_opa(g.arc, static_cast<lv_opa_t>(known ? LV_OPA_COVER : LV_OPA_TRANSP), LV_PART_INDICATOR);
    if (known) {
        lv_arc_set_angles(g.arc, static_cast<uint16_t>(std::lround(angle_of(range[kLow]))),
                          static_cast<uint16_t>(std::lround(angle_of(range[kHigh]))));
    }
    // the range on its way is drawn in the pending color
    const bool waiting = !g.dragging && pending != nullptr && !std::isnan(pending->target_low);
    for (int end = kLow; end < kEnds; end++) {
        const bool selected = end == g.selected;
        set_hidden(g.knobs[end], !known);
        if (known) {
            place_knob(g.knobs[end], range[end]);
        }
        lv_obj_set_style_bg_color(g.knobs[end], t::color(selected ? t::kText : t::kTextLabel), 0);
        set_text(g.ends[end], known ? tiles::temperature(range[end]) + "\xc2\xb0" : std::string("--"));
        lv_obj_set_style_text_color(g.ends[end], t::color(!selected ? t::kTextLabel : waiting ? t::kAccent : t::kText), 0);
    }
    // the selected knob in front where the two touch
    lv_obj_move_foreground(g.knobs[g.selected]);
    set_hidden(g.plus, !known);
    set_hidden(g.minus, !known);
}

} // namespace

void update_arc(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    const bool on = avail && s.state != "off";
    lv_obj_set_style_arc_color(g.arc, t::color(on ? t::kActive : t::kTextMuted), LV_PART_INDICATOR);
    if (g.range) {
        update_range(s, pending, avail);
    } else {
        update_target(s, pending, avail);
    }
    std::string action;
    if (avail) {
        action = !s.hvac_action.empty() ? tiles::words(s.hvac_action) : tiles::words(s.state);
    }
    set_text(g.action, action);
    lv_obj_set_style_text_color(g.action, t::color(on ? t::kActive : t::kTextLabel), 0);
    set_text(g.current, avail && !std::isnan(s.current) ? tiles::temperature(s.current) + "\xc2\xb0" : std::string("--"));
}

} // namespace hc::ui::climate_panel::detail
