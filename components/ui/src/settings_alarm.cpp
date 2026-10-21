// Settings, Alarm (docs/ui/lvgl/lvgl-settings.md section 3, Alarm): only with an alarm (alarm::available()).
// The list of the alarms (up to kMaxAlarms) and a page per alarm. Every change is stored at once.
// Choosing a sound plays it once as a preview, at full volume like the alarm; leaving the page
// stops it.

#include <cstdio>
#include <cstring>

#include "alarm.hpp"
#include "hc/core/alarm.hpp"
#include "settings.hpp"
#include "settings_pages.hpp"
#include "sound.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

constexpr size_t kMaxSounds = 12;
constexpr uint8_t kSnoozeMinutes[] = {5, 10, 15, 20, 30};
// the days in the order they are shown, Monday first (bit numbers of AlarmSettings::days)
constexpr uint8_t kDayOrder[] = {1, 2, 3, 4, 5, 6, 0};

// a new alarm: 07:00 on weekdays, the beeps, the motor
constexpr AlarmSettings kNewAlarm{
    .on = true, .hour = 7, .minute = 0, .days = 0x3e, .snooze_min = 10, .sound = {}, .vibrate = true};

struct PageState {
    // the alarm of the detail page, -1 on the list
    int index = -1;
    char sounds[kMaxSounds][kSoundNameSize];
    size_t sound_count = 0;
    // the rows of the sounds; row 0 is the beeps
    lv_obj_t *sound_rows[kMaxSounds + 1] = {};
    lv_obj_t *next_row = nullptr;
    lv_obj_t *day_buttons[7] = {};
    lv_obj_t *snooze_row = nullptr;
    bool previewing = false;
};

PageState g_page;

Alarms alarms()
{
    Alarms a{};
    backend().alarms(a);
    return a;
}

void store(const Alarms &a)
{
    backend().set_alarms(a);
    alarm::settings_changed();
}

// the alarm of the detail page; false when it is gone
bool alarm_settings(AlarmSettings &out)
{
    const Alarms a = alarms();
    if (g_page.index < 0 || g_page.index >= a.count) {
        return false;
    }
    out = a.list[g_page.index];
    return true;
}

void store(const AlarmSettings &s)
{
    Alarms a = alarms();
    if (g_page.index >= 0 && g_page.index < a.count) {
        a.list[g_page.index] = s;
        store(a);
    }
}

// "in 7 h 12 min" of the detail page's alarm or of the next one on the list, or why none rings
void next_text()
{
    if (g_page.next_row == nullptr) {
        return;
    }
    const Alarms a = alarms();
    const int64_t now = backend().clock();
    std::tm local{};
    if (now != 0) {
        backend().local_time(now, local);
    }
    int index = -1;
    int32_t minutes = 0;
    if (g_page.index >= 0) {
        if (g_page.index < a.count && a.list[g_page.index].on) {
            index = g_page.index;
            const AlarmSettings &s = a.list[index];
            minutes = core::minutes_until({s.hour, s.minute, s.days}, local);
        }
    } else if (now != 0) {
        index = alarm::next(a, local, minutes);
    } else {
        for (size_t i = 0; i < a.count && index < 0; i++) {
            index = a.list[i].on ? static_cast<int>(i) : -1;
        }
    }
    char text[48];
    if (index < 0) {
        std::snprintf(text, sizeof(text), "Off");
    } else if (now == 0) {
        std::snprintf(text, sizeof(text), "Waiting for the clock");
    } else {
        const AlarmSettings &s = a.list[index];
        // within a day the hours and minutes, later the day
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
    AlarmSettings s{};
    if (!alarm_settings(s)) {
        return;
    }
    std::snprintf(text, sizeof(text), "%u min", s.snooze_min);
    t::line_text(t::row_value(g_page.snooze_row), text);
}

// --------------------------------------------------------------------------------- time

void time_card(lv_obj_t *parent, const AlarmSettings &s)
{
    lv_obj_t *card = t::plain(parent);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, t::color(t::kCard), 0);
    lv_obj_set_style_radius(card, t::radius(), 0);
    lv_obj_set_style_pad_all(card, t::dim(8), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, t::dim(8), 0);

    lv_obj_t *hour = t::roller(card, 24, s.hour);
    t::label(card, ":", t::font_large(), t::kText);
    lv_obj_t *minute = t::roller(card, 60, s.minute);
    lv_obj_add_event_cb(
        hour,
        [](lv_event_t *event) {
            AlarmSettings changed{};
            if (!alarm_settings(changed)) {
                return;
            }
            changed.hour = static_cast<uint8_t>(lv_roller_get_selected(lv_event_get_target(event)));
            changed.on = true; // setting a time means the alarm is wanted
            store(changed);
            next_text();
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(
        minute,
        [](lv_event_t *event) {
            AlarmSettings changed{};
            if (!alarm_settings(changed)) {
                return;
            }
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
    AlarmSettings changed{};
    if (!alarm_settings(changed)) {
        return;
    }
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
    lv_obj_set_style_pad_column(row, t::dim(4), 0);
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
    AlarmSettings changed{};
    if (!alarm_settings(changed)) {
        return;
    }
    std::snprintf(changed.sound, sizeof(changed.sound), "%s", sound_name(row));
    store(changed);
    mark_sound(changed.sound);
    backend().play_alarm(changed.sound, false, changed.vibrate);
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

// the preview stops when the page goes (unless an alarm rings meanwhile)
void on_page_deleted(lv_event_t *)
{
    if (g_page.previewing && !alarm::ringing()) {
        backend().stop_alarm();
    }
    g_page.next_row = nullptr;
}

// ------------------------------------------------------------------------------- the list

void on_list_switch(lv_event_t *event)
{
    const int index = from_data(event);
    Alarms a = alarms();
    if (index < a.count) {
        a.list[index].on = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
        store(a);
        next_text();
    }
}

void on_add(lv_event_t *)
{
    Alarms changed = alarms();
    if (changed.count >= kMaxAlarms) {
        return;
    }
    changed.list[changed.count++] = kNewAlarm;
    store(changed);
    push(Page::AlarmDetail, nullptr, changed.count - 1);
}

void delete_alarm(void *)
{
    Alarms a = alarms();
    if (g_page.index >= 0 && g_page.index < a.count) {
        for (int i = g_page.index; i + 1 < a.count; i++) {
            a.list[i] = a.list[i + 1];
        }
        a.count--;
        store(a);
    }
    pop();
}

} // namespace

Built alarm_page(lv_obj_t *content)
{
    g_page = {};
    lv_obj_t *col = t::column(content);
    lv_obj_add_event_cb(col, on_page_deleted, LV_EVENT_DELETE, nullptr);
    const Alarms a = alarms();

    g_page.next_row = t::row(col, "Next", "", false, nullptr);
    next_text();

    char heading[32];
    std::snprintf(heading, sizeof(heading), "Alarms (%u of %u)", static_cast<unsigned>(a.count),
                  static_cast<unsigned>(kMaxAlarms));
    section(col, heading);
    // a row per alarm: the time, the days and its switch; a tap opens its page
    for (size_t i = 0; i < a.count; i++) {
        const AlarmSettings &s = a.list[i];
        char time[8];
        std::snprintf(time, sizeof(time), "%02u:%02u", s.hour, s.minute);
        char days[40];
        core::days_text(s.days, days, sizeof(days));
        lv_obj_t *row = t::row(
            col, time, days, true, [](lv_event_t *event) { push(Page::AlarmDetail, nullptr, from_data(event)); },
            as_data(static_cast<int>(i)));
        t::toggle(row, s.on, on_list_switch, as_data(static_cast<int>(i)));
    }
    if (a.count < kMaxAlarms) {
        t::button(button_row(col), "Add alarm", on_add);
    }
    text_block(col, "An alarm rings at full volume, also when the volume is off, until Stop or Snooze, at most "
                    "10 minutes.");
    return {"Alarm", next_text};
}

Built alarm_detail_page(lv_obj_t *content, int index)
{
    g_page = {};
    g_page.index = index;
    lv_obj_t *col = t::column(content);
    lv_obj_add_event_cb(col, on_page_deleted, LV_EVENT_DELETE, nullptr);
    AlarmSettings s{};
    if (!alarm_settings(s)) {
        text_block(col, "This alarm is gone.", t::kText);
        return {"Alarm", nullptr};
    }

    lv_obj_t *on = t::row(col, "Alarm", "", false, nullptr);
    t::toggle(on, s.on, [](lv_event_t *event) {
        AlarmSettings changed{};
        if (alarm_settings(changed)) {
            changed.on = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
            store(changed);
            next_text();
        }
    });
    g_page.next_row = t::row(col, "Next", "", false, nullptr);
    next_text();
    time_card(col, s);

    section(col, "Days (none: once)");
    day_buttons(col, s);

    // a tap steps through the snooze times
    g_page.snooze_row = t::row(col, "Snooze", "", false, [](lv_event_t *) {
        AlarmSettings changed{};
        if (!alarm_settings(changed)) {
            return;
        }
        size_t i = 0;
        while (i < sizeof(kSnoozeMinutes) && kSnoozeMinutes[i] != changed.snooze_min) {
            i++;
        }
        changed.snooze_min = kSnoozeMinutes[(i + 1) % sizeof(kSnoozeMinutes)];
        store(changed);
        snooze_text();
    });
    snooze_text();
    // the motor's pattern while this alarm rings, only with a motor
    if (sound::vibration_available()) {
        lv_obj_t *vibrate = t::row(col, "Vibrate", "", false, nullptr);
        t::toggle(vibrate, s.vibrate, [](lv_event_t *event) {
            AlarmSettings changed{};
            if (alarm_settings(changed)) {
                changed.vibrate = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
                store(changed);
            }
        });
    }

    section(col, "Sound (a tap plays it)");
    sound_list(col, s);

    t::button(
        button_row(col), "Delete alarm",
        [](lv_event_t *) {
            confirm("Delete alarm?", "It does not ring again.", "Delete", true, delete_alarm, nullptr);
        },
        nullptr, true);
    return {"Alarm", next_text};
}

} // namespace hc::ui::settings::pages
