#include "light_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "dashboard.hpp"
#include "icons.hpp"
#include "lvgl_compat.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::light_panel {

namespace {

namespace t = theme;

enum class Mode : uint8_t { Level, Color, Temp, Effect, Count };
constexpr int kModes = static_cast<int>(Mode::Count);
constexpr const char *kModeGlyphs[kModes] = {"brightness-6", "palette", "thermometer", "creation"};

constexpr uint8_t bit(Mode mode)
{
    return static_cast<uint8_t>(1u << static_cast<int>(mode));
}

// the buttons of the modes: at most this tall in the landscape column, this far apart
constexpr lv_coord_t kButtonHeight = 40;
constexpr lv_coord_t kButtonGap = 6;
// the gap between the steppers and the slider
using panel::kStepGap;
constexpr lv_coord_t kSliderStepGap = 20;
// the sliders: at most this wide, the track rounded like this
constexpr lv_coord_t kSliderWidth = 113;
// the level slider of a large frame (the P4's dialog), wider. Portrait: this part of the height,
// centered; landscape: further from the steppers and this far above the bottom of the control.
constexpr lv_coord_t kSliderWidthLarge = 160;
constexpr int kSliderHeightLargePct = 80;
constexpr lv_coord_t kSliderWidthLargeLandscape = 140;
constexpr lv_coord_t kSliderStepGapLarge = 40;
constexpr lv_coord_t kSliderBottomLarge = 16;
constexpr lv_coord_t kSliderRadius = 24;
// the marker of the color temperature: a white bar this tall, this far from the track's sides
constexpr lv_coord_t kMarkerHeight = 12;
constexpr lv_coord_t kMarkerInset = 6;
constexpr lv_coord_t kMarkerRadius = 4;
// the color wheel at most this large, its knob
constexpr lv_coord_t kWheelSize = 196;
constexpr lv_coord_t kKnobSize = 28;

// the level steps are sent once the taps pause this long
constexpr uint32_t kStepSendMs = 500;
using panel::kStepRepeatMs;
// the light step of docs/architecture.md section 5 when the tile has none
constexpr float kDefaultStep = 1;
constexpr float kPi = 3.14159265f;
// the color temperature range when the light does not report one
constexpr float kDefaultMinKelvin = 2000;
constexpr float kDefaultMaxKelvin = 6500;

// a vertical slider: a track with a fill (level) or a marker (color temperature) and the value in
// its middle, dragged like the dimmer tile
struct Slider {
    lv_obj_t *track = nullptr;
    lv_obj_t *fill = nullptr;
    lv_obj_t *marker = nullptr;
    lv_obj_t *value = nullptr;
    lv_coord_t height = 0; // as built: LVGL lays the objects out only before the next frame
    float min = 0;
    float max = 100;
    bool dragging = false;
    float drag_value = 0;
    void (*on_release)(float value) = nullptr;
};

struct State {
    panel::Frame frame;
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    lv_obj_t *power = nullptr;
    lv_obj_t *buttons[kModes] = {};
    uint8_t offered = 0; // the modes the buttons are laid out for
    Mode mode = Mode::Level;
    lv_obj_t *control = nullptr; // the container of the control of `mode`

    // level
    Slider level;
    lv_obj_t *plus = nullptr;
    lv_obj_t *minus = nullptr;
    lv_obj_t *note = nullptr; // a light without a level: its state
    int step_level = -1;      // the level the steps have collected, -1: none
    lv_timer_t *send_timer = nullptr;
    uint32_t last_step = 0;

    // color
    lv_obj_t *wheel = nullptr;
    lv_obj_t *knob = nullptr;
    memory::Vector<compat::Pixel> pixels;
    lv_coord_t wheel_size = 0;
    bool wheel_dragging = false;
    float drag_hue = 0;
    float drag_saturation = 0;

    // color temperature
    Slider temp;

    // effects
    lv_obj_t *list = nullptr;
    memory::Vector<memory::String> effects; // the list the items were made from
};

State g;

// ------------------------------------------------------------------------------- helpers

using panel::set_hidden;
using panel::set_text;

const core::EntityState &state()
{
    return dashboard::state(g.entity);
}

bool available()
{
    const core::EntityState &s = state();
    return s.known != 0 && !s.unavailable();
}

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

core::Expectation level_expectation(int level)
{
    core::Expectation expected;
    expected.state = level > 0 ? "on" : "off";
    if (level > 0) {
        expected.brightness = static_cast<int16_t>(std::lround(level * 255 / 100.0));
    }
    return expected;
}

// the color of a color temperature (Tanner Helland's approximation of the black body)
lv_color_t kelvin_color(float kelvin)
{
    const double k = kelvin / 100.0;
    double r = 255;
    double gr = 0;
    double b = 255;
    if (k > 66) {
        r = 329.698727446 * std::pow(k - 60, -0.1332047592);
        gr = 288.1221695283 * std::pow(k - 60, -0.0755148492);
    } else {
        gr = 99.4708025861 * std::log(k) - 161.1195681661;
        b = k <= 19 ? 0 : 138.5177312231 * std::log(k - 10) - 305.0447927307;
    }
    const auto clamp = [](double v) { return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0)); };
    return lv_color_make(clamp(r), clamp(gr), clamp(b));
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

Slider make_slider(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, float min, float max,
                   void (*on_release)(float))
{
    Slider s;
    s.height = h;
    s.min = min;
    s.max = max;
    s.on_release = on_release;
    s.track = t::plain(parent);
    lv_obj_set_pos(s.track, x, y);
    lv_obj_set_size(s.track, w, h);
    lv_obj_set_style_radius(s.track, std::min<lv_coord_t>(kSliderRadius, w / 2), 0);
    lv_obj_set_style_clip_corner(s.track, true, 0);
    lv_obj_set_style_bg_opa(s.track, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s.track, t::color(t::kCardAlt), 0);
    lv_obj_set_style_border_width(s.track, 1, 0);
    lv_obj_set_style_border_color(s.track, t::color(t::kBorder), 0);
    lv_obj_set_style_border_post(s.track, true, 0);
    lv_obj_clear_flag(s.track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.track, LV_OBJ_FLAG_CLICKABLE);
    // the drag does not bubble up as a gesture of the screen
    lv_obj_clear_flag(s.track, LV_OBJ_FLAG_GESTURE_BUBBLE);
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

// The size of a slider in a control of `h` with `room` for its width, and its place from the
// top; the level and the color temperature sliders look the same.
struct SliderPlace {
    lv_coord_t w;
    lv_coord_t h;
    lv_coord_t y;
};

SliderPlace slider_place(lv_coord_t room, lv_coord_t h)
{
    if (!g.frame.large) {
        return {std::min(kSliderWidth, room), h, 0};
    }
    if (g.frame.portrait) {
        const auto slider_h = static_cast<lv_coord_t>(h * kSliderHeightLargePct / 100);
        return {std::min(kSliderWidthLarge, room), slider_h, static_cast<lv_coord_t>((h - slider_h) / 2)};
    }
    return {std::min(kSliderWidthLargeLandscape, room), static_cast<lv_coord_t>(h - kSliderBottomLarge), 0};
}

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
            base = std::max(0, static_cast<int>(std::lround(state().last_brightness * 100 / 255.0)));
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
    const SliderPlace place = slider_place(static_cast<lv_coord_t>(w - gap - step), h);
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

// ------------------------------------------------------------------------------- color

// The wheel of hue and saturation: the hue goes round counterclockwise from red at the right, the
// saturation grows from white in the center to the full color at the rim. Drawn once into a buffer
// of the large heap, the edge blended into the background.
void render_wheel(lv_coord_t size)
{
    g.pixels.assign(static_cast<size_t>(size) * size, compat::pixel(t::color(t::kBackground)));
    const lv_color_t background = t::color(t::kBackground);
    const float radius = size / 2.0f;
    for (lv_coord_t y = 0; y < size; y++) {
        for (lv_coord_t x = 0; x < size; x++) {
            const float dx = x + 0.5f - radius;
            const float dy = radius - (y + 0.5f);
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance > radius) {
                continue;
            }
            float hue = std::atan2(dy, dx) * 180.0f / kPi;
            if (hue < 0) {
                hue += 360;
            }
            const auto saturation = static_cast<uint8_t>(std::lround(std::min(1.0f, distance / radius) * 100));
            lv_color_t color = lv_color_hsv_to_rgb(static_cast<uint16_t>(hue) % 360, saturation, 100);
            if (distance > radius - 1) {
                // the last pixel of the rim: as much of it as the circle covers
                color = lv_color_mix(color, background, static_cast<lv_opa_t>((radius - distance) * 255));
            }
            g.pixels[static_cast<size_t>(y) * size + x] = compat::pixel(color);
        }
    }
}

void knob_show(float hue, float saturation, bool known)
{
    set_hidden(g.knob, !known);
    if (!known) {
        return;
    }
    const float radius = g.wheel_size / 2.0f;
    const float r = std::clamp(saturation, 0.0f, 100.0f) / 100.0f * radius;
    const float angle = hue * kPi / 180.0f;
    const lv_coord_t x = lv_obj_get_x(g.wheel) + static_cast<lv_coord_t>(std::lround(radius + std::cos(angle) * r));
    const lv_coord_t y = lv_obj_get_y(g.wheel) + static_cast<lv_coord_t>(std::lround(radius - std::sin(angle) * r));
    lv_obj_set_pos(g.knob, static_cast<lv_coord_t>(x - kKnobSize / 2), static_cast<lv_coord_t>(y - kKnobSize / 2));
    lv_obj_set_style_bg_color(g.knob,
                              lv_color_hsv_to_rgb(static_cast<uint16_t>(std::lround(hue)) % 360,
                                                  static_cast<uint8_t>(std::lround(std::clamp(saturation, 0.0f, 100.0f))), 100),
                              0);
}

void on_wheel(lv_event_t *event)
{
    lv_point_t point{};
    if (lv_indev_t *indev = lv_indev_get_act()) {
        lv_indev_get_point(indev, &point);
    }
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
    case LV_EVENT_PRESSING: {
        if (!available()) {
            return;
        }
        lv_area_t area;
        lv_obj_get_coords(g.wheel, &area);
        const float radius = g.wheel_size / 2.0f;
        const float dx = point.x - (area.x1 + radius);
        const float dy = (area.y1 + radius) - point.y;
        float hue = std::atan2(dy, dx) * 180.0f / kPi;
        if (hue < 0) {
            hue += 360;
        }
        g.wheel_dragging = true;
        g.drag_hue = std::round(hue);
        g.drag_saturation = std::round(std::min(1.0f, std::sqrt(dx * dx + dy * dy) / radius) * 100);
        knob_show(g.drag_hue, g.drag_saturation, true);
        break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (g.wheel_dragging) {
            g.wheel_dragging = false;
            core::Expectation expected;
            expected.state = "on";
            expected.hue = g.drag_hue;
            expected.saturation = g.drag_saturation;
            send(core::IntentKind::SetColor, g.drag_hue, g.drag_saturation, {}, std::move(expected));
        }
        break;
    default:
        break;
    }
}

void build_color(lv_coord_t w, lv_coord_t h)
{
    // even, so the center falls between two pixels like the touch math expects
    g.wheel_size = static_cast<lv_coord_t>(std::min({kWheelSize, w, h}) & ~1);
    render_wheel(g.wheel_size);
    g.wheel = lv_canvas_create(g.control);
    compat::canvas_set_buffer(g.wheel, g.pixels.data(), g.wheel_size, g.wheel_size);
    lv_obj_set_pos(g.wheel, static_cast<lv_coord_t>((w - g.wheel_size) / 2), static_cast<lv_coord_t>((h - g.wheel_size) / 2));
    lv_obj_add_flag(g.wheel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g.wheel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    for (const lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(g.wheel, on_wheel, code, nullptr);
    }
    // the knob shows the color that is set; the white ring keeps it apart from the wheel
    g.knob = t::plain(g.control);
    lv_obj_set_size(g.knob, kKnobSize, kKnobSize);
    lv_obj_set_style_radius(g.knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g.knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g.knob, 3, 0);
    lv_obj_set_style_border_color(g.knob, t::color(t::kText), 0);
    lv_obj_set_style_outline_width(g.knob, 1, 0);
    lv_obj_set_style_outline_color(g.knob, t::color(t::kBackground), 0);
    lv_obj_clear_flag(g.knob, LV_OBJ_FLAG_CLICKABLE);
}

void update_color(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    if (g.wheel == nullptr) {
        return;
    }
    lv_obj_set_style_img_opa(g.wheel, static_cast<lv_opa_t>(avail ? LV_OPA_COVER : LV_OPA_40), 0);
    if (g.wheel_dragging) {
        return;
    }
    float hue = s.hue;
    float saturation = s.saturation;
    if (pending != nullptr && !std::isnan(pending->hue)) {
        hue = pending->hue;
        saturation = pending->saturation;
    }
    knob_show(hue, saturation, avail && !std::isnan(hue) && !std::isnan(saturation));
}

// ------------------------------------------------------------------------------- temperature

std::string kelvin_text(float kelvin)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%ld K", std::lround(kelvin));
    return text;
}

void build_temp(lv_coord_t w, lv_coord_t h)
{
    const core::EntityState &s = state();
    float min = !std::isnan(s.min_color_temp) && s.min_color_temp > 0 ? s.min_color_temp : kDefaultMinKelvin;
    float max = !std::isnan(s.max_color_temp) && s.max_color_temp > min ? s.max_color_temp : kDefaultMaxKelvin;
    const SliderPlace place = slider_place(w, h);
    g.temp = make_slider(g.control, static_cast<lv_coord_t>((w - place.w) / 2), place.y, place.w, place.h, min, max, [](float value) {
        // HA works in mireds underneath, steps of 10 K are as fine as it goes
        const float kelvin = std::round(value / 10) * 10;
        core::Expectation expected;
        expected.state = "on";
        expected.color_temp = kelvin;
        send(core::IntentKind::SetColorTemp, kelvin, 0, {}, std::move(expected));
    });
    slider_events(g.temp);
    // cool white at the top, warm at the bottom
    lv_obj_set_style_bg_color(g.temp.track, kelvin_color(max), 0);
    lv_obj_set_style_bg_grad_color(g.temp.track, kelvin_color(min), 0);
    lv_obj_set_style_bg_grad_dir(g.temp.track, LV_GRAD_DIR_VER, 0);
    g.temp.marker = t::plain(g.temp.track);
    lv_obj_set_size(g.temp.marker, static_cast<lv_coord_t>(place.w - 2 * kMarkerInset), kMarkerHeight);
    lv_obj_set_x(g.temp.marker, kMarkerInset);
    lv_obj_set_style_radius(g.temp.marker, kMarkerRadius, 0);
    lv_obj_set_style_bg_opa(g.temp.marker, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.temp.marker, t::color(t::kText), 0);
    lv_obj_clear_flag(g.temp.marker, LV_OBJ_FLAG_CLICKABLE);
    // dark on the light colors of the track
    slider_label(g.temp, t::kBackground);
}

void update_temp(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    if (g.temp.track == nullptr) {
        return;
    }
    float kelvin = g.temp.dragging ? g.temp.drag_value : s.color_temp;
    if (!g.temp.dragging && pending != nullptr && !std::isnan(pending->color_temp)) {
        kelvin = pending->color_temp;
    }
    // a light in a color mode has no color temperature
    const bool known = avail && !std::isnan(kelvin);
    slider_show(g.temp, known ? kelvin : 0, known);
    set_text(g.temp.value, !avail ? "--" : known ? kelvin_text(std::round(kelvin / 10) * 10) : "");
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
    if (mode != g.mode) {
        show_mode(mode);
        update();
    }
}

// The buttons of the offered modes: a column in landscape, a row in portrait. With only the level
// there is nothing to choose, and no button.
void layout_buttons(uint8_t offered)
{
    g.offered = offered;
    int count = 0;
    for (int i = 0; i < kModes; i++) {
        count += (offered >> i) & 1;
    }
    const lv_area_t &area = g.frame.options;
    const lv_coord_t aw = lv_area_get_width(&area);
    const lv_coord_t ah = lv_area_get_height(&area);
    lv_coord_t w = aw;
    lv_coord_t h = ah;
    if (count > 0) {
        if (g.frame.portrait) {
            w = static_cast<lv_coord_t>((aw - (count - 1) * kButtonGap) / count);
        } else {
            h = std::min<lv_coord_t>(kButtonHeight, static_cast<lv_coord_t>((ah - (count - 1) * kButtonGap) / count));
        }
    }
    int shown = 0;
    for (int i = 0; i < kModes; i++) {
        lv_obj_t *button = g.buttons[i];
        const bool visible = count > 1 && ((offered >> i) & 1) != 0;
        set_hidden(button, !visible);
        if (!visible) {
            continue;
        }
        const lv_coord_t x = g.frame.portrait ? static_cast<lv_coord_t>(area.x1 + shown * (w + kButtonGap)) : area.x1;
        const lv_coord_t y = g.frame.portrait ? area.y1 : static_cast<lv_coord_t>(area.y1 + shown * (h + kButtonGap));
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, w, h);
        shown++;
    }
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
        const bool selected = static_cast<int>(g.mode) == i;
        lv_obj_set_style_bg_color(g.buttons[i], t::color(selected ? t::kText : t::kCard), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(g.buttons[i], 0), t::color(selected ? t::kBackground : t::kTextLabel), 0);
    }
}

} // namespace

void build(const panel::Frame &frame, core::TileIndex tile)
{
    forget();
    g.frame = frame;
    g.tile = tile;
    g.entity = dashboard::layout().tiles[tile].entity;

    g.power = panel::card(frame.root, frame.action);
    lv_obj_center(t::label(g.power, icons::glyph("power"), icons::small(), t::kTextLabel));
    lv_obj_add_event_cb(g.power, on_power, LV_EVENT_CLICKED, nullptr);

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
    if ((g.mode != Mode::Level && (offered & bit(g.mode)) == 0) || level_kind_changed) {
        show_mode(Mode::Level);
    }
    update_power(s, pending, avail);
    update_buttons();
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
