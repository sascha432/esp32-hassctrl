// Settings, Alarm (docs/ui/lvgl/lvgl-settings.md section 3, Alarm): only with an alarm (alarm::available()).
// Every change is stored at once. Choosing a sound plays it once as a preview, at full volume like
// the alarm; leaving the page stops it.

#include <cstdio>
#include <cstring>

#include "alarm.hpp"
#include "hc/core/alarm.hpp"
#include "settings.hpp"
#include "settings_pages.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

constexpr size_t kMaxSounds = 12;
constexpr uint8_t kSnoozeMinutes[] = {5, 10, 15, 20, 30};
// the days in the order they are shown, Monday first (bit numbers of AlarmSettings::days)
constexpr uint8_t kDayOrder[] = {1, 2, 3, 4, 5, 6, 0};

struct Page {
    char sounds[kMaxSounds][kSoundNameSize];
    size_t sound_count = 0;
    // the rows of the sounds; row 0 is the beeps
    lv_obj_t *sound_rows[kMaxSounds + 1] = {};
    lv_obj_t *next_row = nullptr;
    lv_obj_t *day_buttons[7] = {};
    lv_obj_t *snooze_row = nullptr;
    bool previewing = false;
};

Page g_page;

AlarmSettings alarm_settings()
{
    AlarmSettings s{};
    backend().alarm_settings(s);
    return s;
}

void store(const AlarmSettings &s)
{
    backend().set_alarm_settings(s);
    alarm::settings_changed();
}

// "Rings in 7 h 12 min" under the switch, or why it does not
void next_text()
{
    if (g_page.next_row == nullptr) {
        return;
    }
    const AlarmSettings s = alarm_settings();
    const int64_t now = backend().clock();
    char text[48];
    if (!s.on) {
        std::snprintf(text, sizeof(text), "Off");
    } else if (now == 0) {
        std::snprintf(text, sizeof(text), "Waiting for the clock");
    } else {
        std::tm local{};
        backend().local_time(now, local);
        // within a day the hours and minutes, later the day
        const int32_t minutes = core::minutes_until({s.hour, s.minute, s.days}, local);
        const int32_t day_minutes = 24 * 60;
        if (minutes < day_minutes) {
            std::snprintf(text, sizeof(text), "in %ld h %ld min", static_cast<long>(minutes / 60),
                          static_cast<long>(minutes % 60));
        } else {
            const int32_t days = (local.tm_hour * 60 + local.tm_min + minutes) / day_minutes;
            std::snprintf(text, sizeof(text), "%s %02u:%02u", core::day_name(static_cast<uint8_t>((local.tm_wday + days) % 7)),
                          s.hour, s.minute);
        }
    }
    t::line_text(t::row_value(g_page.next_row), text);
}

void snooze_text()
{
    char text[16];
    std::snprintf(text, sizeof(text), "%u min", alarm_settings().snooze_min);
    t::line_text(t::row_value(g_page.snooze_row), text);
}

// --------------------------------------------------------------------------------- time

// "00\n01\n...": the options of a roller from 0 to count - 1
void number_options(char *out, size_t size, int count)
{
    size_t used = 0;
    for (int i = 0; i < count && used + 4 < size; i++) {
        used += static_cast<size_t>(std::snprintf(out + used, size - used, i == 0 ? "%02d" : "\n%02d", i));
    }
}

lv_obj_t *roller(lv_obj_t *parent, int count, int value)
{
    char options[60 * 3 + 1];
    number_options(options, sizeof(options), count);
    lv_obj_t *obj = lv_roller_create(parent);
    // not LV_ROLLER_MODE_INFINITE: its seven copies of 60 minutes in the large font drew the
    // selected row from another place (LVGL 8.4)
    lv_roller_set_options(obj, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(obj, 3);
    lv_roller_set_selected(obj, static_cast<uint16_t>(value), LV_ANIM_OFF);
    lv_obj_set_width(obj, t::scaled(80));
    // the selected row in the same font, or it is drawn at another height than the others
    lv_obj_set_style_text_font(obj, &t::font_large(), 0);
    lv_obj_set_style_text_font(obj, &t::font_large(), LV_PART_SELECTED);
    lv_obj_set_style_text_color(obj, t::color(t::kTextLabel), 0);
    lv_obj_set_style_bg_color(obj, t::color(t::kCard), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, t::color(t::kActive), LV_PART_SELECTED);
    lv_obj_set_style_text_color(obj, t::color(t::kBackground), LV_PART_SELECTED);
    return obj;
}

void time_card(lv_obj_t *parent, const AlarmSettings &s)
{
    lv_obj_t *card = t::plain(parent);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, t::color(t::kCard), 0);
    lv_obj_set_style_radius(card, t::kRadius, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 8, 0);

    lv_obj_t *hour = roller(card, 24, s.hour);
    t::label(card, ":", t::font_large(), t::kText);
    lv_obj_t *minute = roller(card, 60, s.minute);
    lv_obj_add_event_cb(
        hour,
        [](lv_event_t *event) {
            AlarmSettings changed = alarm_settings();
            changed.hour = static_cast<uint8_t>(lv_roller_get_selected(lv_event_get_target(event)));
            changed.on = true; // setting a time means the alarm is wanted
            store(changed);
            next_text();
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(
        minute,
        [](lv_event_t *event) {
            AlarmSettings changed = alarm_settings();
            changed.minute = static_cast<uint8_t>(lv_roller_get_selected(lv_event_get_target(event)));
            changed.on = true;
            store(changed);
            next_text();
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
}

// --------------------------------------------------------------------------------- days

void show_days(uint8_t days)
{
    for (const uint8_t bit : kDayOrder) {
        lv_obj_t *button = g_page.day_buttons[bit];
        const bool on = (days & (1u << bit)) != 0;
        lv_obj_set_style_bg_color(button, t::color(on ? t::kActive : t::kCard), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0), t::color(on ? t::kBackground : t::kText), 0);
    }
}

void on_day(lv_event_t *event)
{
    const auto bit = static_cast<uint8_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    AlarmSettings changed = alarm_settings();
    changed.days ^= static_cast<uint8_t>(1u << bit);
    store(changed);
    show_days(changed.days);
    next_text();
}

void day_buttons(lv_obj_t *parent, const AlarmSettings &s)
{
    lv_obj_t *row = t::plain(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    // seven in one line, also at 320 px
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 4, 0);
    for (const uint8_t bit : kDayOrder) {
        lv_obj_t *button =
            t::button(row, core::day_name(bit), on_day, reinterpret_cast<void *>(static_cast<intptr_t>(bit)));
        lv_obj_set_style_min_width(button, 0, 0);
        lv_obj_set_style_pad_hor(button, 0, 0);
        lv_obj_set_width(button, 1);
        lv_obj_set_flex_grow(button, 1);
        g_page.day_buttons[bit] = button;
    }
    show_days(s.days);
}

// -------------------------------------------------------------------------------- sounds

const char *sound_name(size_t row)
{
    return row == 0 ? "" : g_page.sounds[row - 1];
}

void mark_sound(const char *selected)
{
    for (size_t row = 0; row <= g_page.sound_count; row++) {
        t::line_text(t::row_value(g_page.sound_rows[row]),
                     std::strcmp(sound_name(row), selected) == 0 ? LV_SYMBOL_OK : "");
    }
}

void on_sound(lv_event_t *event)
{
    const auto row = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    AlarmSettings changed = alarm_settings();
    std::snprintf(changed.sound, sizeof(changed.sound), "%s", sound_name(row));
    store(changed);
    mark_sound(changed.sound);
    backend().play_alarm(changed.sound, false);
    g_page.previewing = true;
}

void sound_list(lv_obj_t *parent, const AlarmSettings &s)
{
    g_page.sound_count = backend().alarm_sounds(g_page.sounds, kMaxSounds);
    for (size_t row = 0; row <= g_page.sound_count; row++) {
        g_page.sound_rows[row] = t::row(parent, row == 0 ? "Beeps" : g_page.sounds[row - 1], "", false, on_sound,
                                        reinterpret_cast<void *>(static_cast<intptr_t>(row)));
    }
    mark_sound(s.sound);
}

} // namespace

Built alarm_page(lv_obj_t *content)
{
    g_page = {};
    lv_obj_t *col = t::column(content);
    // the preview stops when the page goes (unless the alarm itself rings meanwhile)
    lv_obj_add_event_cb(
        col,
        [](lv_event_t *) {
            if (g_page.previewing && !alarm::ringing()) {
                backend().stop_alarm();
            }
            g_page.next_row = nullptr;
        },
        LV_EVENT_DELETE, nullptr);
    const AlarmSettings s = alarm_settings();

    lv_obj_t *on = t::row(col, "Alarm", "", false, nullptr);
    t::toggle(on, s.on, [](lv_event_t *event) {
        AlarmSettings changed = alarm_settings();
        changed.on = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
        store(changed);
        next_text();
    });
    g_page.next_row = t::row(col, "Next", "", false, nullptr);
    next_text();
    time_card(col, s);

    section(col, "Days (none: once)");
    day_buttons(col, s);

    // a tap steps through the snooze times
    g_page.snooze_row = t::row(col, "Snooze", "", false, [](lv_event_t *) {
        AlarmSettings changed = alarm_settings();
        size_t i = 0;
        while (i < sizeof(kSnoozeMinutes) && kSnoozeMinutes[i] != changed.snooze_min) {
            i++;
        }
        changed.snooze_min = kSnoozeMinutes[(i + 1) % sizeof(kSnoozeMinutes)];
        store(changed);
        snooze_text();
    });
    snooze_text();

    section(col, "Sound (a tap plays it)");
    sound_list(col, s);
    text_block(col, "The alarm rings at full volume, also when the volume is off, until Stop or Snooze, at most "
                    "10 minutes.");
    return {"Alarm", next_text};
}

} // namespace hc::ui::settings::pages
