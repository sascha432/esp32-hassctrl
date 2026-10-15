#include "sound.hpp"

#include <algorithm>
#include <cstdlib>

#include "lvgl_compat.hpp"

namespace hc::ui::sound {

namespace {

// LVGL's user flags for the plain objects that are tap targets (tappable()) and drag controls
// (draggable())
constexpr lv_obj_flag_t kTappable = LV_OBJ_FLAG_USER_1;
constexpr lv_obj_flag_t kDraggable = LV_OBJ_FLAG_USER_2;
// a slider pressed and released farther apart than LVGL's scroll limit was dragged, not tapped
constexpr lv_coord_t kMoveLimit = 10;
// A slider ticks at every 1/25 of its range, a drag control every 14 px: the faster it moves, the
// more ticks. Never two within 25 ms, so a fast swipe does not turn into a buzz.
constexpr int32_t kSliderTicks = 25;
constexpr lv_coord_t kDragTickPx = 14;
constexpr uint32_t kMinTickMs = 25;

struct State {
    const Backend *backend = nullptr;
    bool tap_sound = false;
    bool slider_sound = false;
    bool vibrate = false;
    // one click per tap: the feedback comes for every object an event bubbles to
    bool clicked = false;
    // one pulse per touch, for the same reason; cleared when the finger lifts
    bool buzzed = false;
    lv_point_t pressed_at{};
    // where the last tick was: the value of a slider, the point of a drag control
    int32_t tick_value = 0;
    lv_point_t tick_point{};
    uint32_t tick_ms = 0;
};

State g;

bool is_slider(const lv_obj_t *obj)
{
    return obj != nullptr && (lv_obj_has_class(obj, &lv_slider_class) || lv_obj_has_class(obj, &lv_arc_class));
}

// the value of a slider or an arc, and the size of one tick
int32_t slider_value(lv_obj_t *obj, int32_t &step)
{
    int32_t min = 0;
    int32_t max = 0;
    int32_t value = 0;
    if (lv_obj_has_class(obj, &lv_slider_class)) {
        min = lv_slider_get_min_value(obj);
        max = lv_slider_get_max_value(obj);
        value = lv_slider_get_value(obj);
    } else {
        min = lv_arc_get_min_value(obj);
        max = lv_arc_get_max_value(obj);
        value = lv_arc_get_value(obj);
    }
    step = std::max<int32_t>(1, (max - min) / kSliderTicks);
    return value;
}

// A tap on `obj` does something: it is an input widget or a plain object marked as a tap target,
// or its events bubble up to one. A container that is only clickable (the scrolling column of a
// page, the dashboard page, a backdrop) is neither.
bool is_target(lv_obj_t *obj)
{
    for (; obj != nullptr; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, kTappable) || lv_obj_has_class(obj, &lv_btn_class) ||
            lv_obj_has_class(obj, &lv_switch_class) || is_slider(obj) || lv_obj_has_class(obj, &lv_btnmatrix_class)) {
            return true;
        }
        if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE)) {
            return false;
        }
    }
    return false;
}

void tick()
{
    const uint32_t now = lv_tick_get();
    if (now - g.tick_ms >= kMinTickMs) {
        g.tick_ms = now;
        g.backend->play_slider_tick();
    }
}

void on_pressed(lv_indev_t *indev)
{
    g.clicked = false;
    lv_indev_get_point(indev, &g.pressed_at);
    g.tick_point = g.pressed_at;
    lv_obj_t *obj = lv_indev_get_obj_act();
    if (is_slider(obj)) {
        int32_t step = 0;
        g.tick_value = slider_value(obj, step);
    }
    // the motor buzzes when the finger touches, not when it lifts: also for a press that turns
    // into a scroll or a drag, which nobody can tell yet
    if (g.vibrate && !g.buzzed && is_target(obj)) {
        g.buzzed = true;
        g.backend->vibrate_click();
    }
}

// a slider that moved by a step since the last tick ticks
void on_value_changed()
{
    lv_obj_t *obj = lv_indev_get_obj_act();
    if (!g.slider_sound || !is_slider(obj)) {
        return;
    }
    int32_t step = 0;
    const int32_t value = slider_value(obj, step);
    if (std::abs(value - g.tick_value) >= step) {
        g.tick_value = value;
        tick();
    }
}

// a drag control (the brightness bar of the light panel) ticks by distance
void on_pressing(lv_indev_t *indev)
{
    lv_obj_t *obj = lv_indev_get_obj_act();
    if (!g.slider_sound || obj == nullptr || !lv_obj_has_flag(obj, kDraggable)) {
        return;
    }
    lv_point_t now{};
    lv_indev_get_point(indev, &now);
    if (std::abs(now.x - g.tick_point.x) + std::abs(now.y - g.tick_point.y) >= kDragTickPx) {
        g.tick_point = now;
        tick();
    }
}

void on_clicked(lv_indev_t *indev)
{
    if (g.clicked) {
        return;
    }
    g.clicked = true;
    // a switch that just turned the tap sound off makes none
    lv_obj_t *obj = lv_indev_get_obj_act();
    if (!g.tap_sound || !is_target(obj)) {
        return;
    }
    // LVGL sends no click after a drag that scrolled, but a slider or a drag control gets one after
    // it was dragged: only there the distance tells a tap from a drag. Elsewhere it missed taps:
    // while the screen renders the touch is read seldom, and the release came with a point of the
    // lifting finger 10 px away (2026-10-09, quick taps).
    if (is_slider(obj) || lv_obj_has_flag(obj, kDraggable)) {
        lv_point_t now{};
        lv_indev_get_point(indev, &now);
        if (std::abs(now.x - g.pressed_at.x) > kMoveLimit || std::abs(now.y - g.pressed_at.y) > kMoveLimit) {
            return;
        }
    }
    g.backend->play_touch_sound();
}

// the code of every event a touch sends, once for each object it reaches
void on_feedback(uint8_t code)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == nullptr) {
        return;
    }
    switch (code) {
    case LV_EVENT_PRESSED:
        on_pressed(indev);
        break;
    case LV_EVENT_PRESSING:
        on_pressing(indev);
        break;
    case LV_EVENT_VALUE_CHANGED:
        on_value_changed();
        break;
    case LV_EVENT_CLICKED:
        on_clicked(indev);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        g.buzzed = false;
        break;
    default:
        break;
    }
}

} // namespace

void init(const Backend &backend)
{
    g = {};
    g.backend = &backend;
    const bool click = backend.play_touch_sound != nullptr && available();
    const bool vibrate = backend.vibrate_click != nullptr && vibration_available();
    if (!click && !vibrate) {
        return;
    }
    settings_changed();
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            compat::set_touch_feedback(indev, on_feedback);
        }
    }
}

bool available()
{
    SoundSettings s{};
    return g.backend != nullptr && g.backend->sound_settings != nullptr && g.backend->sound_settings(s);
}

bool vibration_available()
{
    bool on = false;
    return g.backend != nullptr && g.backend->vibration != nullptr && g.backend->vibration(on);
}

void settings_changed()
{
    SoundSettings s{};
    const bool sound = g.backend->sound_settings != nullptr && g.backend->sound_settings(s) && s.volume > 0;
    g.tap_sound = sound && s.tap_sound && g.backend->play_touch_sound != nullptr;
    g.slider_sound = sound && s.slider_sound && g.backend->play_slider_tick != nullptr;
    bool vibrate = false;
    g.vibrate = g.backend->vibration != nullptr && g.backend->vibration(vibrate) && vibrate &&
                g.backend->vibrate_click != nullptr;
}

void confirm()
{
    if (g.backend == nullptr) {
        return;
    }
    if (g.vibrate) {
        g.backend->vibrate_click();
    }
    if (g.tap_sound) {
        g.backend->play_touch_sound();
    }
}

void snap()
{
    if (g.backend != nullptr && g.slider_sound) {
        tick();
    }
}

void tappable(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, kTappable);
}

void draggable(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, kDraggable);
}

} // namespace hc::ui::sound
