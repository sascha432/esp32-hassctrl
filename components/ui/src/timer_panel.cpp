#include "timer_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include "dashboard.hpp"
#include "history_view.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::timer_panel {

namespace {

namespace t = theme;
using panel::set_hidden;
using panel::set_text;
using tiles::TimerState;

enum class Mode : uint8_t { Control, History, Count };
constexpr int kModes = static_cast<int>(Mode::Count);
constexpr const char *kModeGlyphs[kModes] = {"timer-outline", "chart-timeline-variant"};

// the actions, in the order of their buttons
enum class Action : uint8_t { Start, Pause, Cancel, Finish, Count };
constexpr int kActions = static_cast<int>(Action::Count);
constexpr const char *kActionGlyphs[kActions] = {"play", "pause", "stop", "check"};
constexpr const char *kActionWords[kActions] = {"Start", "Pause", "Cancel", "Finish"};

// the rows of the control: between them, and between the rollers of the duration
constexpr lv_coord_t kRowGap = 8;
constexpr lv_coord_t kRollerGap = 4;
// the round buttons: at least this large (the T4 has little room), this far apart at most
constexpr lv_coord_t kSmallestButton = 36;
constexpr lv_coord_t kWidestGap = 24;
// the duration's rollers: hours, minutes, seconds (as the alarm's time)
constexpr int kRollers = 3;
constexpr int kRollerCounts[kRollers] = {24, 60, 60};
constexpr int kRollerSeconds[kRollers] = {3600, 60, 1};
constexpr float kLongest = 24 * 3600 - 1;
// the countdown looks at the clock this often
constexpr uint32_t kTickMs = 1000;

struct State {
    panel::Frame frame;
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    lv_obj_t *buttons[kModes] = {};
    Mode mode = Mode::Control;
    lv_obj_t *control = nullptr; // the container of `mode`

    lv_obj_t *countdown = nullptr;
    lv_obj_t *status = nullptr;  // "Ends at 22:51", "Paused", "Idle"
    lv_obj_t *rollers[kRollers] = {};
    lv_obj_t *actions[kActions] = {};
    lv_obj_t *words[kActions] = {};
    float chosen = core::kUnknown; // the duration the rollers set, NaN: the entity's
    lv_timer_t *tick = nullptr;
};

State g;

// ------------------------------------------------------------------------------- helpers

const core::EntityState &state()
{
    return dashboard::state(g.entity);
}

bool available()
{
    const core::EntityState &s = state();
    return s.known != 0 && !s.unavailable();
}

// the duration the panel shows: the rollers' choice, else the entity's
float shown_duration(const core::EntityState &s)
{
    return !std::isnan(g.chosen) ? g.chosen : s.duration;
}

void send(float action, float duration, core::Expectation expected)
{
    core::Intent intent;
    intent.kind = core::IntentKind::SetTimer;
    intent.entity = g.entity;
    intent.value = action;
    intent.value2 = duration;
    dashboard::act(intent, std::move(expected));
}

// ------------------------------------------------------------------------------- the control

// a roller turned: the duration is the three of them
void on_roller(lv_event_t *)
{
    if (!available()) {
        update(); // back to what the entity has
        return;
    }
    int seconds = 0;
    for (int i = 0; i < kRollers; i++) {
        seconds += lv_roller_get_selected(g.rollers[i]) * kRollerSeconds[i];
    }
    g.chosen = static_cast<float>(seconds);
    update();
}

void on_action(lv_event_t *event)
{
    // a step button repeats while held: only the tap counts
    if (lv_event_get_code(event) != LV_EVENT_SHORT_CLICKED || !available()) {
        return;
    }
    lv_obj_t *target = lv_event_get_target(event);
    if (lv_obj_has_state(target, LV_STATE_DISABLED)) {
        return;
    }
    int index = 0;
    while (index < kActions && g.actions[index] != target) {
        index++;
    }
    const core::EntityState &s = state();
    core::Expectation expected;
    switch (static_cast<Action>(index)) {
    case Action::Start: {
        // with the duration the rollers chose; without one the entity's, or the rest of a
        // paused timer
        // the chosen duration stays shown (as expected) until the entity reports it
        const float duration = !std::isnan(g.chosen) && g.chosen != s.duration ? g.chosen : 0;
        expected.state = "active";
        if (duration > 0) {
            expected.duration = duration;
        }
        send(core::kTimerStart, duration, std::move(expected));
        break;
    }
    case Action::Pause:
        expected.state = "paused";
        send(core::kTimerPause, 0, std::move(expected));
        break;
    case Action::Cancel:
        expected.state = "idle";
        send(core::kTimerCancel, 0, std::move(expected));
        break;
    case Action::Finish:
        expected.state = "idle";
        send(core::kTimerFinish, 0, std::move(expected));
        break;
    case Action::Count:
        break;
    }
}

void on_tick(lv_timer_t *)
{
    update();
}

// The rows of the control, centered in it: the countdown, its state, the duration on three
// rollers (hours, minutes, seconds), the action buttons with their words. The countdown takes the
// largest digit font that leaves the buttons at least kSmallestButton with three rows of the
// rollers seen, else with one; the buttons shrink to the room that is left.
void build_control(lv_coord_t w, lv_coord_t h)
{
    const lv_coord_t status_h = lv_font_get_line_height(&t::font_normal());
    const lv_coord_t word_h = lv_font_get_line_height(&t::font_small());
    const lv_coord_t largest = std::min<lv_coord_t>(panel::step_size(g.frame), static_cast<lv_coord_t>((w - 3 * kRowGap) / 4));
    const lv_coord_t colon_w = lv_txt_get_width(":", 1, &t::font_large(), 0, LV_TEXT_FLAG_NONE);
    const auto roller_w = std::min<lv_coord_t>(
        t::scaled(80), static_cast<lv_coord_t>((w - (kRollers - 1) * (colon_w + 2 * kRollerGap)) / kRollers));
    for (int i = 0; i < kRollers; i++) {
        g.rollers[i] = t::roller(g.control, kRollerCounts[i], 0);
        lv_obj_set_width(g.rollers[i], roller_w);
        lv_obj_add_event_cb(g.rollers[i], on_roller, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    const lv_font_t *font = &t::font_large();
    lv_coord_t button = kSmallestButton;
    lv_coord_t roller_h = 0;
    bool fits = false;
    for (const uint8_t seen : {3, 1}) {
        for (lv_obj_t *roller : g.rollers) {
            lv_roller_set_visible_row_count(roller, seen);
        }
        roller_h = lv_obj_get_style_height(g.rollers[0], LV_PART_MAIN);
        for (const lv_font_t *candidate : {&t::font_digits_72(), &t::font_digits_48(), &t::font_huge(), &t::font_large()}) {
            const auto room = static_cast<lv_coord_t>(
                h - (lv_font_get_line_height(candidate) + status_h + roller_h + word_h + 2 + 3 * kRowGap));
            fits = room >= kSmallestButton;
            if (fits || (seen == 1 && candidate == &t::font_large())) {
                font = candidate;
                button = std::clamp<lv_coord_t>(room, kSmallestButton, largest);
                break;
            }
        }
        if (fits) {
            break;
        }
    }
    const lv_coord_t count_h = lv_font_get_line_height(font);
    const lv_coord_t total = static_cast<lv_coord_t>(count_h + status_h + roller_h + button + word_h + 2 + 3 * kRowGap);
    lv_coord_t y = std::max<lv_coord_t>(0, static_cast<lv_coord_t>((h - total) / 2));

    g.countdown = t::label(g.control, "", *font, t::kText);
    lv_obj_set_width(g.countdown, w);
    lv_obj_set_style_text_align(g.countdown, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.countdown, 0, y);
    y = static_cast<lv_coord_t>(y + count_h + kRowGap);

    g.status = t::label(g.control, "", t::font_normal(), t::kTextLabel);
    lv_obj_set_width(g.status, w);
    lv_obj_set_style_text_align(g.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.status, 0, y);
    y = static_cast<lv_coord_t>(y + status_h + kRowGap);

    // the rollers with a colon between them
    const auto row_w = static_cast<lv_coord_t>(kRollers * roller_w + (kRollers - 1) * (colon_w + 2 * kRollerGap));
    lv_coord_t x = static_cast<lv_coord_t>((w - row_w) / 2);
    const auto colon_y = static_cast<lv_coord_t>(y + (roller_h - lv_font_get_line_height(&t::font_large())) / 2);
    for (int i = 0; i < kRollers; i++) {
        lv_obj_set_pos(g.rollers[i], x, y);
        x = static_cast<lv_coord_t>(x + roller_w + kRollerGap);
        if (i + 1 < kRollers) {
            lv_obj_set_pos(t::label(g.control, ":", t::font_large(), t::kText), x, colon_y);
            x = static_cast<lv_coord_t>(x + colon_w + kRollerGap);
        }
    }
    y = static_cast<lv_coord_t>(y + roller_h + kRowGap);

    // the actions, spread by at most kWidestGap
    const lv_coord_t gap = std::clamp<lv_coord_t>(static_cast<lv_coord_t>((w - kActions * button) / (kActions - 1)), kRowGap,
                                                  kWidestGap);
    x = static_cast<lv_coord_t>((w - (kActions * button + (kActions - 1) * gap)) / 2);
    for (int i = 0; i < kActions; i++) {
        g.actions[i] = panel::step_button(g.control, x, y, button, kActionGlyphs[i], on_action);
        g.words[i] = t::label(g.control, kActionWords[i], t::font_small(), t::kTextLabel);
        lv_obj_set_width(g.words[i], static_cast<lv_coord_t>(button + gap));
        lv_obj_set_style_text_align(g.words[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g.words[i], static_cast<lv_coord_t>(x - gap / 2), static_cast<lv_coord_t>(y + button + 2));
        x = static_cast<lv_coord_t>(x + button + gap);
    }
    g.tick = lv_timer_create(on_tick, kTickMs, nullptr);
}

void enable(int index, bool on)
{
    lv_obj_t *button = g.actions[index];
    if (on) {
        lv_obj_clear_state(button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    lv_obj_set_style_opa(button, on ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_opa(g.words[index], on ? LV_OPA_COVER : LV_OPA_40, 0);
}

void update_control(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    if (g.countdown == nullptr) {
        return;
    }
    const TimerState shown = tiles::timer_shown(s, pending);
    const Backend &backend = dashboard::backend();
    const int64_t now = backend.clock();
    const bool waiting = pending != nullptr;
    if (!std::isnan(g.chosen) && g.chosen == s.duration) {
        g.chosen = core::kUnknown; // the entity has the chosen duration now
    }
    // the countdown: the time left while it runs or is paused, the duration while idle; a start
    // with a new duration on its way shows that duration
    std::string count;
    std::string status;
    if (!s.known) {
        count = "--:--";
    } else if (!avail) {
        count = "--:--";
        status = "Unavailable";
    } else if (shown == TimerState::Idle) {
        count = tiles::timer_text(shown_duration(s));
        status = "Idle";
    } else if (waiting && !std::isnan(pending->duration)) {
        count = tiles::timer_text(pending->duration);
        status = "Active";
    } else {
        count = tiles::timer_text(tiles::timer_left(s, now));
        if (shown == TimerState::Paused) {
            status = "Paused";
        } else if (now != 0 && s.finishes_at != 0 && !waiting) {
            std::tm local{};
            backend.local_time(s.finishes_at, local);
            char text[32];
            std::snprintf(text, sizeof(text), "Ends at %02d:%02d", local.tm_hour, local.tm_min);
            status = text;
        } else {
            status = "Active";
        }
    }
    set_text(g.countdown, count);
    lv_obj_set_style_text_color(g.countdown, t::color(waiting ? panel::kPendingBorder : shown == TimerState::Idle ? t::kTextValue : t::kText), 0);
    set_text(g.status, status);
    lv_obj_set_style_text_color(g.status, t::color(shown == TimerState::Active ? t::kActive : t::kTextLabel), 0);

    // the rollers show the duration (a longer one as 23:59:59), unless a finger is on them
    const bool changed = !std::isnan(g.chosen) && g.chosen != s.duration;
    const float duration = shown_duration(s);
    auto seconds = static_cast<int>(std::clamp(std::isnan(duration) ? 0.0f : duration, 0.0f, kLongest));
    for (int i = 0; i < kRollers; i++) {
        lv_obj_t *roller = g.rollers[i];
        const int value = seconds / kRollerSeconds[i];
        seconds %= kRollerSeconds[i];
        if (!lv_obj_has_state(roller, LV_STATE_PRESSED) && lv_roller_get_selected(roller) != value) {
            lv_roller_set_selected(roller, static_cast<uint16_t>(value), LV_ANIM_ON);
        }
        lv_obj_set_style_opa(roller, avail ? LV_OPA_COVER : LV_OPA_40, 0);
    }

    // Start starts an idle timer, resumes a paused one, restarts a running one; not for 0:00:00
    // a restart has its own glyph, a start and a resume the play glyph
    set_text(lv_obj_get_child(g.actions[static_cast<int>(Action::Start)], 0),
             icons::glyph(shown == TimerState::Active ? "restart" : "play"));
    set_text(g.words[static_cast<int>(Action::Start)],
             shown == TimerState::Active ? "Restart" : shown == TimerState::Paused && !changed ? "Resume" : "Start");
    enable(static_cast<int>(Action::Start), avail && !(changed && g.chosen <= 0));
    enable(static_cast<int>(Action::Pause), avail && shown == TimerState::Active);
    enable(static_cast<int>(Action::Cancel), avail && shown != TimerState::Idle);
    enable(static_cast<int>(Action::Finish), avail && shown != TimerState::Idle);
}

// ------------------------------------------------------------------------------- modes

void clear_control()
{
    history_view::forget();
    if (g.tick != nullptr) {
        lv_timer_del(g.tick);
        g.tick = nullptr;
    }
    if (g.control != nullptr) {
        lv_obj_del(g.control);
    }
    g.control = nullptr;
    g.countdown = g.status = nullptr;
    for (lv_obj_t *&roller : g.rollers) {
        roller = nullptr;
    }
    for (int i = 0; i < kActions; i++) {
        g.actions[i] = g.words[i] = nullptr;
    }
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
    if (mode == Mode::History) {
        history_view::Legend legend;
        legend.on = "Active";
        legend.off = "Idle";
        legend.problem = "Paused";
        legend.on_color = t::kActive;
        legend.off_color = t::kBorder;
        legend.problem_color = t::kTimerPaused;
        history_view::build(g.control, {0, 0, static_cast<lv_coord_t>(w - 1), static_cast<lv_coord_t>(h - 1)}, g.tile,
                            core::HistoryKind::States, legend);
        return;
    }
    build_control(w, h);
}

void on_mode(lv_event_t *event)
{
    const auto mode = static_cast<Mode>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (mode != g.mode) {
        show_mode(mode);
        update();
    }
}

} // namespace

void build(const panel::Frame &frame, core::TileIndex tile)
{
    forget();
    g.frame = frame;
    g.tile = tile;
    g.entity = dashboard::layout().tiles[tile].entity;
    for (int i = 0; i < kModes; i++) {
        g.buttons[i] = panel::card(frame.root, frame.options);
        lv_obj_center(t::label(g.buttons[i], icons::glyph(kModeGlyphs[i]), icons::small(), t::kTextLabel));
        lv_obj_add_event_cb(g.buttons[i], on_mode, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }
    panel::layout_options(frame, g.buttons, kModes, (1u << kModes) - 1);
    show_mode(Mode::Control);
}

void update()
{
    if (g.control == nullptr) {
        return;
    }
    for (int i = 0; i < kModes; i++) {
        panel::mark_option(g.buttons[i], static_cast<int>(g.mode) == i);
    }
    if (g.mode == Mode::History) {
        history_view::update();
        return;
    }
    update_control(state(), dashboard::pending(g.entity), available());
}

void forget()
{
    history_view::forget();
    if (g.tick != nullptr) {
        lv_timer_del(g.tick);
    }
    // the objects go with the panel's root
    g = {};
}

} // namespace hc::ui::timer_panel
