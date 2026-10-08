#include "power.hpp"

#include <algorithm>
#include <cmath>

namespace hc::ui::power {

namespace {

constexpr uint32_t kCheckMs = 200;
// fades of the old firmware: dimming and turning off slowly, waking quickly
constexpr uint32_t kFadeDownMs = 3000;
constexpr uint32_t kFadeUpMs = 500;
constexpr uint32_t kFadeStepMs = 20;
// the automatic brightness moves a tenth of the way per check: about two seconds to settle, so a
// shadow or a hand over the sensor does not flicker the screen
constexpr float kAutoSmoothing = 0.1f;

enum class State : uint8_t {
    Active,
    Dimmed,
    Off,
};

struct Context {
    lv_disp_t *disp = nullptr;
    const Backend *backend = nullptr;
    State state = State::Active;
    // catches the touch that wakes the screen, on lv_layer_sys above everything
    lv_obj_t *blocker = nullptr;
    // the level asked for, and the one the backlight shows now (they differ during a fade)
    int16_t applied = -1;
    float shown = -1.0f;
    float fade_from = 0.0f;
    uint32_t fade_start = 0;
    uint32_t fade_ms = 0;
    lv_timer_t *fade_timer = nullptr;
    bool fading = false;
    // smoothed automatic brightness in percent, below 0 before the first measurement
    float auto_level = -1.0f;
    // a brightness slider is dragged: preview() owns the backlight
    bool previewing = false;
};

Context g;

// Follows the ambient light; false when the automatic brightness is off or there is no sensor.
bool update_auto(const DisplaySettings &s)
{
    float lux = 0.0f;
    if (!s.auto_brightness || !g.backend->ambient_lux(lux)) {
        g.auto_level = -1.0f;
        return false;
    }
    // linear: no light gives the minimum, kFullBrightnessLux and more the user's brightness
    const float share = std::min(lux / kFullBrightnessLux, 1.0f);
    const float target = std::max(1.0f, share * static_cast<float>(s.brightness));
    g.auto_level = g.auto_level < 0.0f ? target : g.auto_level + kAutoSmoothing * (target - g.auto_level);
    return true;
}

uint8_t active_brightness(const DisplaySettings &s)
{
    if (g.auto_level < 0.0f) {
        return s.brightness;
    }
    return static_cast<uint8_t>(std::clamp(std::lround(g.auto_level), 1L, 100L));
}

uint8_t backlight_for(State state, const DisplaySettings &s)
{
    switch (state) {
    case State::Active:
        return active_brightness(s);
    case State::Dimmed:
        return std::min(s.dim_brightness, active_brightness(s));
    case State::Off:
        return 0;
    }
    return s.brightness;
}

void show(float level)
{
    g.shown = level;
    g.backend->set_backlight(static_cast<uint8_t>(std::lround(level)));
}

void on_fade(lv_timer_t *timer)
{
    const uint32_t elapsed = lv_tick_elaps(g.fade_start);
    if (elapsed >= g.fade_ms) {
        show(g.applied);
        lv_timer_pause(timer);
        g.fading = false;
        return;
    }
    const float t = static_cast<float>(elapsed) / static_cast<float>(g.fade_ms);
    show(g.fade_from + t * (static_cast<float>(g.applied) - g.fade_from));
}

// The backlight goes to `percent`: at once, or as a fade over `fade_ms` from what it shows now.
void set_level(uint8_t percent, uint32_t fade_ms, bool cut = false)
{
    g.applied = percent;
    if (g.fading && fade_ms == 0 && !cut) {
        return; // the running fade ends at the new level
    }
    if (fade_ms == 0 || g.shown < 0.0f) {
        lv_timer_pause(g.fade_timer);
        g.fading = false;
        show(percent);
        return;
    }
    g.fading = true;
    g.fade_from = g.shown;
    g.fade_start = lv_tick_get();
    g.fade_ms = fade_ms;
    lv_timer_resume(g.fade_timer);
}

void apply(State state, bool force = false)
{
    DisplaySettings s{};
    g.backend->display_settings(s);
    const uint8_t percent = backlight_for(state, s);
    // a change of state fades, a new level within a state (automatic brightness, a slider) not
    uint32_t fade = 0;
    if (state != g.state) {
        static constexpr const char *kNames[] = {"active", "dimmed", "off"};
        LV_LOG_USER("screen %s, backlight %u %%", kNames[static_cast<int>(state)], percent);
        fade = state == State::Active ? kFadeUpMs : kFadeDownMs;
    }
    g.state = state;
    if (force || percent != g.applied) {
        set_level(percent, fade);
    }
    if (state == State::Off) {
        lv_obj_clear_flag(g.blocker, LV_OBJ_FLAG_HIDDEN);
    }
}

// What the idle time asks for. 0 means never; an off time shorter than the dim time skips dimming.
State wanted(uint32_t idle_ms, const DisplaySettings &s)
{
    if (s.off_after_s != 0 && idle_ms >= s.off_after_s * 1000u) {
        return State::Off;
    }
    if (s.dim_after_s != 0 && idle_ms >= s.dim_after_s * 1000u) {
        return State::Dimmed;
    }
    return State::Active;
}

void on_check(lv_timer_t *)
{
    if (g.state == State::Off) {
        return; // only the blocker wakes it
    }
    DisplaySettings s{};
    g.backend->display_settings(s);
    const bool automatic = update_auto(s);
    const State state = wanted(lv_disp_get_inactive_time(g.disp), s);
    if (g.previewing) {
        return;
    }
    if (state != g.state || automatic) {
        apply(state); // only writes the backlight when the level changed
    }
}

void on_blocker(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        apply(State::Active);
    } else {
        // the waking touch is over: the screen takes touches again
        lv_obj_add_flag(g.blocker, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace

void init(lv_disp_t *disp, const Backend &backend)
{
    g = {};
    g.disp = disp;
    g.backend = &backend;

    g.blocker = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(g.blocker);
    lv_obj_set_size(g.blocker, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(g.blocker, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g.blocker, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(g.blocker, on_blocker, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(g.blocker, on_blocker, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(g.blocker, on_blocker, LV_EVENT_PRESS_LOST, nullptr);

    g.fade_timer = lv_timer_create(on_fade, kFadeStepMs, nullptr);
    lv_timer_pause(g.fade_timer);
    apply(State::Active, true);
    lv_timer_create(on_check, kCheckMs, nullptr);
}

void settings_changed()
{
    g.previewing = false;
    // a changed brightness takes effect at once, not smoothed
    DisplaySettings s{};
    g.backend->display_settings(s);
    g.auto_level = -1.0f;
    update_auto(s);
    apply(g.state == State::Off ? State::Active : g.state, true);
}

uint8_t backlight()
{
    return g.applied < 0 ? 0 : static_cast<uint8_t>(g.applied);
}

void preview(uint8_t percent)
{
    g.previewing = true;
    set_level(percent, 0, true);
}

} // namespace hc::ui::power
