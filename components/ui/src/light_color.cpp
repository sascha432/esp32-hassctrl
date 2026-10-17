// The color controls of the light panel (docs/ui/lvgl/lvgl-panels.md): the wheel of hue and
// saturation, and the slider of the color temperature.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "light_parts.hpp"
#include "lvgl_compat.hpp"
#include "sound.hpp"
#include "theme.hpp"

namespace hc::ui::light_panel::detail {

namespace {

namespace t = theme;
using panel::set_hidden;
using panel::set_text;

// the color wheel at most this large, its knob
constexpr lv_coord_t kWheelSize = 196;
constexpr lv_coord_t kKnobSize = 28;
constexpr float kPi = 3.14159265f;
// the color temperature range when the light does not report one
constexpr float kDefaultMinKelvin = 2000;
constexpr float kDefaultMaxKelvin = 6500;

// the color of a color temperature (Tanner Helland's approximation of the black body)
lv_color_t kelvin_color(float kelvin)
{
    const float k = kelvin / 100.0f;
    float r = 255;
    float gr = 0;
    float b = 255;
    if (k > 66) {
        r = 329.698727446f * std::pow(k - 60, -0.1332047592f);
        gr = 288.1221695283f * std::pow(k - 60, -0.0755148492f);
    } else {
        gr = 99.4708025861f * std::log(k) - 161.1195681661f;
        b = k <= 19 ? 0 : 138.5177312231f * std::log(k - 10) - 305.0447927307f;
    }
    const auto clamp = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f)); };
    return lv_color_make(clamp(r), clamp(gr), clamp(b));
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

} // namespace

void build_color(lv_coord_t w, lv_coord_t h)
{
    // even, so the center falls between two pixels like the touch math expects
    g.wheel_size = static_cast<lv_coord_t>(std::min({kWheelSize, w, h}) & ~1);
    render_wheel(g.wheel_size);
    g.wheel = lv_canvas_create(g.control);
    compat::canvas_set_buffer(g.wheel, g.pixels.data(), g.wheel_size, g.wheel_size);
    lv_obj_set_pos(g.wheel, static_cast<lv_coord_t>((w - g.wheel_size) / 2), static_cast<lv_coord_t>((h - g.wheel_size) / 2));
    lv_obj_add_flag(g.wheel, LV_OBJ_FLAG_CLICKABLE);
    sound::tappable(g.wheel);
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

namespace {

// ------------------------------------------------------------------------------- temperature

std::string kelvin_text(float kelvin)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%ld K", std::lround(kelvin));
    return text;
}

} // namespace

void build_temp(lv_coord_t w, lv_coord_t h)
{
    const core::EntityState &s = state();
    float min = !std::isnan(s.min_color_temp) && s.min_color_temp > 0 ? s.min_color_temp : kDefaultMinKelvin;
    float max = !std::isnan(s.max_color_temp) && s.max_color_temp > min ? s.max_color_temp : kDefaultMaxKelvin;
    const panel::SliderPlace place = slider_place(w, h);
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

} // namespace hc::ui::light_panel::detail
