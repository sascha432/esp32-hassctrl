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

// the rows of the control: between them, and between the duration and its steppers
constexpr lv_coord_t kRowGap = 8;
constexpr lv_coord_t kStepGap = 16;
// the round buttons: at least this large (the T4 has little room), this far apart at most
constexpr lv_coord_t kSmallestButton = 36;
constexpr lv_coord_t kWidestGap = 24;
// a duration steps by a minute from two minutes on, by 10 s below; never under 10 s
constexpr float kMinuteStep = 60;
constexpr float kSecondsStep = 10;
constexpr float kStepLimit = 120;
constexpr float kShortest = 10;
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
    lv_obj_t *duration = nullptr;
    lv_obj_t *minus = nullptr;
    lv_obj_t *plus = nullptr;
    lv_obj_t *actions[kActions] = {};
    lv_obj_t *words[kActions] = {};
    float chosen = core::kUnknown; // the duration the steppers set, NaN: the entity's
    uint32_t last_step = 0;
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

// the duration the panel shows: the steppers' choice, else the entity's
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

void on_step(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED_REPEAT && lv_tick_elaps(g.last_step) < panel::kStepRepeatMs) {
        return;
    }
    const core::EntityState &s = state();
    if (!available()) {
        return;
    }
    g.last_step = lv_tick_get();
    const bool plus = lv_event_get_target(event) == g.plus;
    float base = shown_duration(s);
    if (std::isnan(base)) {
        base = 0;
    }
    const float step = (plus ? base >= kStepLimit : base > kStepLimit) ? kMinuteStep : kSecondsStep;
    // to the next multiple of the step, so 0:30:30 steps to 0:31:00
    const float next = plus ? std::floor(base / step + 1) * step : std::ceil(base / step - 1) * step;
    g.chosen = std::max(next, kShortest);
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
        // with the duration the steppers chose; without one the entity's, or the rest of a
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

// The rows of the control, centered in it: the countdown, its state, the duration between the
// steppers, the action buttons with their words. The countdown takes the largest digit font that
// leaves the buttons at least kSmallestButton; the buttons shrink to the room that is left.
void build_control(lv_coord_t w, lv_coord_t h)
{
    const lv_coord_t status_h = lv_font_get_line_height(&t::font_normal());
    const lv_coord_t word_h = lv_font_get_line_height(&t::font_small());
    const lv_coord_t largest = std::min<lv_coord_t>(panel::step_size(g.frame), static_cast<lv_coord_t>((w - 3 * kRowGap) / 4));
    const lv_font_t *font = &t::font_large();
    lv_coord_t button = kSmallestButton;
    for (const lv_font_t *candidate : {&t::font_digits_72(), &t::font_digits_48(), &t::font_huge(), &t::font_large()}) {
        const lv_coord_t rows = static_cast<lv_coord_t>(lv_font_get_line_height(candidate) + status_h + word_h + 2 + 3 * kRowGap);
        const auto room = static_cast<lv_coord_t>((h - rows) / 2);
        if (room >= kSmallestButton || candidate == &t::font_large()) {
            font = candidate;
            button = std::clamp<lv_coord_t>(room, kSmallestButton, largest);
            break;
        }
    }
    const lv_coord_t count_h = lv_font_get_line_height(font);
    const lv_coord_t total = static_cast<lv_coord_t>(count_h + status_h + 2 * button + word_h + 2 + 3 * kRowGap);
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

    // the duration between its steppers, as wide as "00:00:00" in its font
    const lv_font_t &duration_font = t::font_large();
    const auto duration_w = static_cast<lv_coord_t>(
        lv_txt_get_width("00:00:00", 8, &duration_font, 0, LV_TEXT_FLAG_NONE) + 8);
    const auto row_w = static_cast<lv_coord_t>(2 * button + 2 * kStepGap + duration_w);
    lv_coord_t x = static_cast<lv_coord_t>((w - row_w) / 2);
    g.minus = panel::step_button(g.control, x, y, button, "minus", on_step);
    g.duration = t::label(g.control, "", duration_font, t::kText);
    lv_obj_set_width(g.duration, duration_w);
    lv_obj_set_style_text_align(g.duration, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.duration, static_cast<lv_coord_t>(x + button + kStepGap),
                   static_cast<lv_coord_t>(y + (button - lv_font_get_line_height(&duration_font)) / 2));
    g.plus = panel::step_button(g.control, static_cast<lv_coord_t>(x + button + 2 * kStepGap + duration_w), y, button,
                                "plus", on_step);
    y = static_cast<lv_coord_t>(y + button + kRowGap);

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

    const float duration = shown_duration(s);
    set_text(g.duration, std::isnan(duration) ? std::string("-:--:--") : std::string(core::duration_text(duration)));
    // a duration the steppers changed shows in the pending color until it is started
    lv_obj_set_style_text_color(g.duration, t::color(!std::isnan(g.chosen) && g.chosen != s.duration ? panel::kPendingBorder : t::kText), 0);
    for (lv_obj_t *step : {g.minus, g.plus}) {
        lv_obj_set_style_opa(step, avail ? LV_OPA_COVER : LV_OPA_40, 0);
    }

    // Start starts an idle timer, resumes a paused one, restarts a running one
    const bool changed = !std::isnan(g.chosen) && g.chosen != s.duration;
    set_text(g.words[static_cast<int>(Action::Start)],
             shown == TimerState::Active ? "Restart" : shown == TimerState::Paused && !changed ? "Resume" : "Start");
    enable(static_cast<int>(Action::Start), avail);
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
    g.countdown = g.status = g.duration = g.minus = g.plus = nullptr;
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
