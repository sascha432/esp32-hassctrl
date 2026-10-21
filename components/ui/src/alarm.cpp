#include "alarm.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>

#include "hc/core/alarm.hpp"
#include "power.hpp"
#include "theme.hpp"

namespace hc::ui::alarm {

namespace {

namespace t = theme;

constexpr uint32_t kCheckMs = 1000;
// an alarm nobody stops goes quiet after this long (as a snooze would, but it does not come back)
constexpr uint32_t kRingMs = 10 * 60 * 1000;

struct State {
    const Backend *backend = nullptr;
    // the minute (epoch / 60) that was checked last: an alarm rings once in its minute
    int64_t checked_minute = -1;
    // epoch seconds when a snoozed alarm rings again, 0: none; one snooze at a time
    int64_t snooze_until = 0;
    // the alarm that rings (or was snoozed): its snooze time, sound and switch
    AlarmSettings alarm{};
    bool ringing = false;
    uint32_t ring_started = 0;
    lv_obj_t *screen = nullptr;
    lv_obj_t *time = nullptr;
};

State g;
std::atomic<bool> g_ring_requested{false};

Alarms stored()
{
    Alarms a{};
    g.backend->alarms(a);
    return a;
}

// rings when the list is empty (the console's `sound ring`): the beeps with the motor
constexpr AlarmSettings kPlainAlarm{
    .on = true, .hour = 0, .minute = 0, .days = 0, .snooze_min = 10, .sound = {}, .vibrate = true};

void close_screen()
{
    g.ringing = false;
    g.backend->stop_alarm();
    if (g.screen != nullptr) {
        lv_obj_del(g.screen);
        g.screen = nullptr;
        g.time = nullptr;
    }
}

void update_time()
{
    char text[8];
    if (g.time != nullptr && g.backend->time_text(text, sizeof(text))) {
        lv_label_set_text(g.time, text);
    }
}

void on_snooze(lv_event_t *)
{
    g.snooze_until = g.backend->clock() + static_cast<int64_t>(g.alarm.snooze_min) * 60;
    close_screen();
}

void on_stop(lv_event_t *)
{
    g.snooze_until = 0;
    close_screen();
}

// The alarm screen: the time large, Snooze as the large target (a sleepy tap finds it), Stop
// below it. It follows the display when it turns (sizes in percent, a centered column).
void open_screen(const AlarmSettings &s)
{
    g.screen = t::plain(lv_layer_top());
    lv_obj_set_size(g.screen, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(g.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.screen, t::color(t::kBackground), 0);
    lv_obj_add_flag(g.screen, LV_OBJ_FLAG_CLICKABLE); // nothing below it takes touches
    lv_obj_set_flex_flow(g.screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g.screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g.screen, 16, 0);
    lv_obj_set_style_pad_all(g.screen, 16, 0);

    t::label(g.screen, LV_SYMBOL_BELL " Alarm", t::font_large(), t::kActive);
    g.time = t::label(g.screen, "", t::font_huge(), t::kText);
    update_time();

    char text[32];
    std::snprintf(text, sizeof(text), "Snooze %u min", s.snooze_min);
    lv_obj_t *snooze = t::button(g.screen, text, on_snooze);
    lv_obj_set_size(snooze, lv_pct(80), t::kRowHeight * 2);
    lv_obj_set_style_bg_color(snooze, t::color(t::kActive), 0);
    lv_obj_set_style_text_font(lv_obj_get_child(snooze, 0), &t::font_large(), 0);
    lv_obj_t *stop = t::button(g.screen, "Stop", on_stop);
    lv_obj_set_size(stop, lv_pct(80), t::kRowHeight);
    lv_obj_move_foreground(g.screen);
}

// `s` rings; a pending snooze ends (its alarm has had its turn)
void ring(const AlarmSettings &s)
{
    if (g.ringing) {
        return;
    }
    g.alarm = s;
    g.snooze_until = 0;
    g.ringing = true;
    g.ring_started = lv_tick_get();
    power::wake();
    open_screen(s);
    g.backend->play_alarm(s.sound, true, s.vibrate);
}

void on_check(lv_timer_t *)
{
    const int64_t now = g.backend->clock();
    if (now == 0) {
        return; // the clock is not set yet
    }
    if (g.ringing) {
        // the screen stays on while it rings
        power::wake();
        update_time();
        if (lv_tick_elaps(g.ring_started) >= kRingMs) {
            g.snooze_until = 0;
            close_screen();
        }
        return;
    }
    if (g_ring_requested.exchange(false)) {
        const Alarms a = stored();
        ring(a.count > 0 ? a.list[0] : kPlainAlarm);
        return;
    }
    if (g.snooze_until != 0 && now >= g.snooze_until) {
        ring(g.alarm);
        return;
    }
    const int64_t minute = now / 60;
    if (minute == g.checked_minute) {
        return;
    }
    g.checked_minute = minute;
    Alarms a = stored();
    std::tm local{};
    g.backend->local_time(now, local);
    // the first one due rings; every one due without days is done (also the ones that did not get
    // their own turn: they rang along)
    int due = -1;
    bool once_done = false;
    for (size_t i = 0; i < a.count; i++) {
        AlarmSettings &s = a.list[i];
        if (!s.on || !core::alarm_due({s.hour, s.minute, s.days}, local)) {
            continue;
        }
        if (due < 0) {
            due = static_cast<int>(i);
        }
        if (s.days == 0) {
            s.on = false;
            once_done = true;
        }
    }
    if (once_done) {
        g.backend->set_alarms(a);
    }
    if (due >= 0) {
        ring(a.list[due]);
    }
}

} // namespace

void init(const Backend &backend)
{
    g = {};
    g.backend = &backend;
    if (available()) {
        lv_timer_create(on_check, kCheckMs, nullptr);
    }
}

bool available()
{
    return g.backend != nullptr && g.backend->alarms != nullptr;
}

void settings_changed()
{
    g.snooze_until = 0;
}

void request_ring()
{
    g_ring_requested = true;
}

bool ringing()
{
    return g.ringing;
}

int next(const Alarms &alarms, const std::tm &local, int32_t &minutes)
{
    int found = -1;
    for (size_t i = 0; i < alarms.count; i++) {
        const AlarmSettings &s = alarms.list[i];
        if (!s.on) {
            continue;
        }
        const int32_t until = core::minutes_until({s.hour, s.minute, s.days}, local);
        if (until >= 0 && (found < 0 || until < minutes)) {
            found = static_cast<int>(i);
            minutes = until;
        }
    }
    return found;
}

void summary(char *out, size_t size)
{
    const Alarms a = stored();
    if (g.snooze_until != 0) {
        std::tm local{};
        g.backend->local_time(g.snooze_until, local);
        std::snprintf(out, size, "Snoozed until %02d:%02d", local.tm_hour, local.tm_min);
        return;
    }
    if (a.count == 0) {
        std::snprintf(out, size, "None");
        return;
    }
    // the one that rings next; without the clock the first one that is on
    int index = -1;
    int32_t minutes = 0;
    const int64_t now = g.backend->clock();
    if (now != 0) {
        std::tm local{};
        g.backend->local_time(now, local);
        index = next(a, local, minutes);
    } else {
        for (size_t i = 0; i < a.count && index < 0; i++) {
            index = a.list[i].on ? static_cast<int>(i) : -1;
        }
    }
    if (index < 0) {
        std::snprintf(out, size, "Off");
        return;
    }
    const AlarmSettings &s = a.list[index];
    char days[40];
    core::days_text(s.days, days, sizeof(days));
    std::snprintf(out, size, "%02u:%02u, %s", s.hour, s.minute, days);
}

} // namespace hc::ui::alarm
