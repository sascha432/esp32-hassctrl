// Settings, Display (docs/ui/lvgl/lvgl-settings.md section 3): brightness, the automatic brightness
// of a light sensor, the idle times, wake on motion and the rotation. The brightness slider is also
// on quick settings (brightness_card()).

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "power.hpp"
#include "rotation.hpp"
#include "settings.hpp"
#include "settings_pages.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

// ------------------------------------------------------------------- display settings

DisplaySettings display_settings()
{
    DisplaySettings s{};
    backend().display_settings(s);
    return s;
}

void store(const DisplaySettings &s)
{
    backend().set_display_settings(s);
    power::settings_changed();
}

} // namespace

void brightness_card(lv_obj_t *parent)
{
    slider_card(parent, "Brightness", 1, 100, display_settings().brightness, [](lv_event_t *event) {
        DisplaySettings s = display_settings();
        s.brightness = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
        store(s);
    });
}

namespace {

// The idle times the - and + buttons step through: the step grows with the time, like the old
// firmware's buttons, so an hour or a day is a few taps away. 0 is "Never"; -1 ends the list.
constexpr int32_t kDimTimes[] = {0,   10,  15,  20,   30,   45,   60,   90,   120,
                                 180, 300, 600, 900, 1200, 1800, 2700, 3600, -1};
constexpr int32_t kOffTimes[] = {0,    60,   120,  180,   300,   600,   900,   1200,  1800, 2700,
                                 3600, 5400, 7200, 10800, 14400, 21600, 28800, 43200, 86400, -1};
// the motion hold, 1 min to 1 h (hc::motion::kMinHoldS, kMaxHoldS)
constexpr int32_t kHoldTimes[] = {60, 120, 180, 240, 300, 600, 900, 1200, 1800, 2700, 3600, -1};

void idle_time_text(int32_t s, char *out, size_t size)
{
    if (s == 0) {
        std::snprintf(out, size, "Never");
    } else if (s < 60) {
        std::snprintf(out, size, "%d s", static_cast<int>(s));
    } else if (s < 3600 && s % 60 != 0) {
        std::snprintf(out, size, "%d min %d s", static_cast<int>(s / 60), static_cast<int>(s % 60));
    } else if (s < 3600) {
        std::snprintf(out, size, "%d min", static_cast<int>(s / 60));
    } else if (s % 3600 != 0) {
        std::snprintf(out, size, "%d h %d min", static_cast<int>(s / 3600), static_cast<int>(s % 3600 / 60));
    } else {
        std::snprintf(out, size, "%d h", static_cast<int>(s / 3600));
    }
}

// A row with "-  value  +"; holding a button repeats. Deleted with its row.
struct Stepper {
    const int32_t *times;
    int index;
    lv_obj_t *value;
    void (*on_change)(int32_t seconds);
    uint32_t last_step = 0; // lv_tick_get()
};

// holding a button steps this often: the indev repeats every 100 ms, too fast to stop on a value
constexpr uint32_t kStepRepeatMs = 250;

void stepper_show(const Stepper &st)
{
    char text[16];
    idle_time_text(st.times[st.index], text, sizeof(text));
    // the row's value label belongs to line_text, which sets its text again on size changes
    t::line_text(st.value, text);
}

void stepper_step(lv_event_t *event)
{
    auto &st = *static_cast<Stepper *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED_REPEAT && lv_tick_elaps(st.last_step) < kStepRepeatMs) {
        return;
    }
    st.last_step = lv_tick_get();
    const bool up = lv_obj_get_index(lv_event_get_target(event)) > lv_obj_get_index(st.value);
    const int next = st.index + (up ? 1 : -1);
    if (next < 0 || st.times[next] < 0) {
        return;
    }
    st.index = next;
    stepper_show(st);
    st.on_change(st.times[next]);
}

lv_obj_t *stepper_button(lv_obj_t *row, const char *symbol, Stepper *st)
{
    lv_obj_t *button = lv_btn_create(row);
    // more padding around the symbol: 3 px, 5 px in settings mode (the P4)
    const lv_coord_t extra = t::dim(100) != 100 ? 5 : 3;
    lv_obj_set_size(button, t::dim(40) + extra, t::dim(36) + extra);
    lv_obj_set_style_radius(button, t::radius(), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, t::color(t::kBorder), 0);
    lv_obj_set_style_bg_color(button, t::color(t::kActive), LV_STATE_PRESSED);
    lv_obj_center(t::label(button, symbol, t::font_normal(), t::kText));
    lv_obj_add_event_cb(button, stepper_step, LV_EVENT_SHORT_CLICKED, st);
    lv_obj_add_event_cb(button, stepper_step, LV_EVENT_LONG_PRESSED_REPEAT, st);
    return button;
}

void stepper_row(lv_obj_t *parent, const char *name, const int32_t *times, int32_t value,
                 void (*on_change)(int32_t seconds))
{
    // a stored time between two steps shows the next longer one
    int index = 0;
    while (times[index + 1] >= 0 && times[index] < value) {
        index++;
    }
    lv_obj_t *row = t::row(parent, name, "", false, nullptr);
    auto *st = new Stepper{times, index, t::row_value(row), on_change, 0};
    lv_obj_add_event_cb(
        row, [](lv_event_t *e) { delete static_cast<Stepper *>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, st);
    lv_obj_t *minus = stepper_button(row, LV_SYMBOL_MINUS, st);
    lv_obj_move_to_index(minus, 1);
    stepper_button(row, LV_SYMBOL_PLUS, st);
    // the row grows with the buttons: at least 6 px above and below them
    lv_obj_update_layout(minus); // the button's size is only set in the layout
    lv_obj_set_height(row, LV_MAX(t::row_height(), lv_obj_get_height(minus) + 12));
    // a fixed width, so the buttons stay where they are while the text changes
    lv_obj_set_width(st->value, t::dim(84));
    lv_obj_set_style_text_align(st->value, LV_TEXT_ALIGN_CENTER, 0);
    stepper_show(*st);
}

// ---------------------------------------------------------------------------- display page

lv_obj_t *g_auto_row = nullptr;
lv_obj_t *g_rotation_row = nullptr;
// the two rows that set the lights of the automatic brightness (with a light sensor)
lv_obj_t *g_light_min_row = nullptr;
lv_obj_t *g_light_max_row = nullptr;
// the row whose last tap was refused (the light was not on the right side of the other point), and when
lv_obj_t *g_light_refused = nullptr;
uint32_t g_light_refused_at = 0;
constexpr uint32_t kRefusedShownMs = 2000;

void light_row_text(lv_obj_t *row, float lux, const char *refused)
{
    char text[24];
    if (row == g_light_refused && lv_tick_elaps(g_light_refused_at) < kRefusedShownMs) {
        std::snprintf(text, sizeof(text), "%s", refused);
    } else {
        std::snprintf(text, sizeof(text), "%ld lx", std::lround(lux));
    }
    t::line_text(t::row_value(row), text);
}

void display_refresh()
{
    if (g_light_min_row != nullptr && g_light_max_row != nullptr) {
        const DisplaySettings s = display_settings();
        light_row_text(g_light_min_row, s.light_min_lux, "not darker");
        light_row_text(g_light_max_row, s.light_max_lux, "not brighter");
    }
    float lux = 0.0f;
    if (g_auto_row != nullptr && backend().ambient_lux(lux)) {
        char text[24];
        std::snprintf(text, sizeof(text), "%ld lx, %u %%", std::lround(lux), power::backlight());
        t::line_text(t::row_value(g_auto_row), text);
    }
    if (g_rotation_row != nullptr) {
        // the name says the rotation, the value what a tap does
        char text[32];
        std::snprintf(text, sizeof(text), "Rotation %u\xc2\xb0", rotation::degrees());
        t::line_text(lv_obj_get_child(g_rotation_row, 0), text);
        t::line_text(t::row_value(g_rotation_row),
                     rotation::has_sensor() && !rotation::locked() ? "Lock" : "Rotate");
    }
}

// the rotation popup: Rotate turns the screen clockwise; with a sensor, "Use the sensor" hands
// the rotation back to it. It closes 5 s after the last tap, or with a tap outside.
lv_obj_t *g_popup_degrees = nullptr;

void popup_degrees_text()
{
    if (g_popup_degrees != nullptr) {
        lv_label_set_text_fmt(g_popup_degrees, "%u\xc2\xb0", rotation::degrees());
    }
}

void open_rotation_popup()
{
    lv_obj_t *card = settings::popup("Rotation", 5000, [] {
        g_popup_degrees = nullptr;
        reload();
    });
    // the degrees under the title, the square Rotate button at the exact center of the card, the
    // sensor button at the bottom
    g_popup_degrees = t::label(card, "", t::font_normal(), t::kTextValue);
    lv_obj_align(g_popup_degrees, LV_ALIGN_TOP_MID, 0, t::dim(26));
    popup_degrees_text();
    lv_obj_t *rotate = t::button(card, "", [](lv_event_t *) {
        rotation::rotate_clockwise();
        settings::popup_touch();
        popup_degrees_text();
    });
    lv_obj_set_size(rotate, t::dim(72), t::dim(72));
    lv_obj_set_style_min_width(rotate, 0, 0);
    lv_obj_set_style_text_align(lv_obj_get_child(rotate, 0), LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lv_obj_get_child(rotate, 0), LV_SYMBOL_REFRESH "\nRotate");
    lv_obj_align(rotate, LV_ALIGN_CENTER, 0, 0);
    if (rotation::has_sensor()) {
        lv_obj_t *sensor = t::button(card, "Use the sensor", [](lv_event_t *) {
            rotation::set_locked(false);
            settings::close_popup();
        });
        lv_obj_set_height(sensor, t::dim(32));
        lv_obj_align(sensor, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
}

} // namespace

Built display_page(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    const DisplaySettings s = display_settings();
    brightness_card(col);
    // only with a light sensor
    float lux = 0.0f;
    g_auto_row = nullptr;
    g_rotation_row = nullptr;
    g_light_min_row = nullptr;
    g_light_max_row = nullptr;
    g_light_refused = nullptr;
    if (backend().ambient_lux(lux)) {
        g_auto_row = t::row(col, "Automatic", "", false, nullptr);
        t::toggle(g_auto_row, s.auto_brightness, [](lv_event_t *event) {
            DisplaySettings changed = display_settings();
            changed.auto_brightness = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
            store(changed);
        });
        // Each row takes the light in the room now. The screen is at its lowest at the first light and
        // at the slider's brightness at the second and above; between them it follows the light.
        g_light_min_row = t::row(col, "Lowest brightness at", "", false, [](lv_event_t *) {
            float now = 0.0f;
            DisplaySettings changed = display_settings();
            if (!backend().ambient_lux(now)) {
                return;
            }
            if (now >= changed.light_max_lux) {
                g_light_refused = g_light_min_row;
                g_light_refused_at = lv_tick_get();
            } else {
                changed.light_min_lux = now;
                store(changed);
            }
            display_refresh();
        });
        g_light_max_row = t::row(col, "Full brightness at", "", false, [](lv_event_t *) {
            float now = 0.0f;
            DisplaySettings changed = display_settings();
            if (!backend().ambient_lux(now)) {
                return;
            }
            if (now <= changed.light_min_lux) {
                g_light_refused = g_light_max_row;
                g_light_refused_at = lv_tick_get();
            } else {
                changed.light_max_lux = now;
                store(changed);
            }
            display_refresh();
        });
        text_block(col, "Automatic: the brightness follows the room light. Tap a row while the room is as dark, "
                        "or as bright, as you want that setting: the screen is at its lowest at the first light, "
                        "and at the brightness of the slider at the second.");
    }
    stepper_row(col, "Dim after", kDimTimes, s.dim_after_s, [](int32_t seconds) {
        DisplaySettings changed = display_settings();
        changed.dim_after_s = static_cast<uint16_t>(seconds);
        store(changed);
    });
    lv_obj_t *dim = slider_card(col, "Dimmed brightness", 1, 100, s.dim_brightness, [](lv_event_t *event) {
        DisplaySettings changed = display_settings();
        changed.dim_brightness = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
        backend().set_display_settings(changed);
        power::preview(changed.dim_brightness); // shows the dimmed level while it is dragged
    });
    lv_obj_add_event_cb(dim, [](lv_event_t *) { power::settings_changed(); }, LV_EVENT_RELEASED, nullptr);
    stepper_row(col, "Screen off", kOffTimes, static_cast<int32_t>(s.off_after_s), [](int32_t seconds) {
        DisplaySettings changed = display_settings();
        changed.off_after_s = static_cast<uint32_t>(seconds);
        store(changed);
    });
    // only with a motion sensor
    bool occupied = false;
    if (backend().motion(occupied)) {
        lv_obj_t *motion = t::row(col, "Wake on motion", "", false, nullptr);
        t::toggle(motion, s.wake_on_motion, [](lv_event_t *event) {
            DisplaySettings changed = display_settings();
            changed.wake_on_motion = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
            store(changed);
        });
        stepper_row(col, "Motion hold", kHoldTimes, static_cast<int32_t>(s.motion_hold_s), [](int32_t seconds) {
            DisplaySettings changed = display_settings();
            changed.motion_hold_s = static_cast<uint32_t>(seconds);
            store(changed);
        });
        text_block(col, "A touch wakes the screen; when it was off, that touch only wakes it. Wake on motion: "
                        "motion wakes it too and keeps it on while the sensor sees someone. Motion hold: Home "
                        "Assistant sees \"Detected\" until the sensor saw nobody for this long.");
    } else {
        text_block(col, "A touch wakes the screen; when it was off, that touch only wakes it.");
    }

    // Rotation: one row. With the sensor in charge it locks the rotation; locked or without a
    // sensor it opens the popup that turns the screen.
    g_rotation_row = t::row(col, "Rotation", "", true, [](lv_event_t *) {
        if (rotation::has_sensor() && !rotation::locked()) {
            rotation::set_locked(true);
            reload();
        } else {
            open_rotation_popup();
        }
    });
    display_refresh();
    return {"Display", display_refresh};
}

} // namespace hc::ui::settings::pages
