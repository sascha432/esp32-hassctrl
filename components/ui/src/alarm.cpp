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
    // epoch seconds when a snoozed alarm rings again, 0: none
    int64_t snooze_until = 0;
    bool ringing = false;
    uint32_t ring_started = 0;
    lv_obj_t *screen = nullptr;
    lv_obj_t *time = nullptr;
};

State g;
std::atomic<bool> g_ring_requested{false};

AlarmSettings settings()
{
    AlarmSettings s{};
    g.backend->alarm_settings(s);
    return s;
}

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
    g.snooze_until = g.backend->clock() + static_cast<int64_t>(settings().snooze_min) * 60;
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

void ring()
{
    const AlarmSettings s = settings();
    if (g.ringing) {
        return;
    }
    g.ringing = true;
    g.ring_started = lv_tick_get();
    power::wake();
    open_screen(s);
    g.backend->play_alarm(s.sound, true);
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
        ring();
        return;
    }
    if (g.snooze_until != 0 && now >= g.snooze_until) {
        g.snooze_until = 0;
        ring();
        return;
    }
    const int64_t minute = now / 60;
    if (minute == g.checked_minute) {
        return;
    }
    g.checked_minute = minute;
    AlarmSettings s = settings();
    std::tm local{};
    g.backend->local_time(now, local);
    if (!s.on || !core::alarm_due({s.hour, s.minute, s.days}, local)) {
        return;
    }
    if (s.days == 0) {
        // once: done after this ring
        s.on = false;
        g.backend->set_alarm_settings(s);
    }
    ring();
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
    return g.backend != nullptr && g.backend->alarm_settings != nullptr;
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

void summary(char *out, size_t size)
{
    const AlarmSettings s = settings();
    if (g.snooze_until != 0) {
        std::tm local{};
        g.backend->local_time(g.snooze_until, local);
        std::snprintf(out, size, "Snoozed until %02d:%02d", local.tm_hour, local.tm_min);
        return;
    }
    if (!s.on) {
        std::snprintf(out, size, "Off");
        return;
    }
    char days[40];
    core::days_text(s.days, days, sizeof(days));
    std::snprintf(out, size, "%02u:%02u, %s", s.hour, s.minute, days);
}

} // namespace hc::ui::alarm
