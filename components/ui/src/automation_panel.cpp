#include "automation_panel.hpp"

#include <algorithm>
#include <string>

#include "dashboard.hpp"
#include "history_view.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::automation_panel {

namespace {

namespace t = theme;
using panel::set_hidden;
using panel::set_text;

enum class Mode : uint8_t { Control, History, Count };
constexpr int kModes = static_cast<int>(Mode::Count);

// the rows of the control: between them, the Enabled row's size and its padding
constexpr lv_coord_t kRowGap = 12;
constexpr lv_coord_t kEnabledWidth = 320;
constexpr lv_coord_t kEnabledHeight = 52;
constexpr lv_coord_t kEnabledPad = 16;
// the Run button: at least this large
constexpr lv_coord_t kSmallestButton = 44;

struct State {
    panel::Frame frame;
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    bool script = false; // a script.* entity, else an automation
    lv_obj_t *buttons[kModes] = {};
    Mode mode = Mode::Control;
    lv_obj_t *control = nullptr; // the container of `mode`

    lv_obj_t *enabled = nullptr; // the Enabled row (an automation)
    lv_obj_t *toggle = nullptr;
    lv_obj_t *caption = nullptr; // "Last triggered"
    lv_obj_t *last = nullptr;    // "Today 10:52"
    lv_obj_t *run = nullptr;
    lv_obj_t *run_glyph = nullptr;
    lv_obj_t *word = nullptr;    // "Run actions", "Run script", "Stop"
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

// the on state the panel shows: the pending action's, else the entity's
bool shown_on(const core::EntityState &s, const core::Expectation *pending)
{
    if (pending != nullptr && !pending->state.empty()) {
        return pending->state == "on";
    }
    return s.is_on();
}

// enables or disables the automation (automation.toggle), from the state the panel shows
void send_enabled(bool on)
{
    if (!available() || shown_on(state(), dashboard::pending(g.entity)) == on) {
        update();
        return;
    }
    core::Intent intent;
    intent.kind = core::IntentKind::Toggle;
    intent.entity = g.entity;
    core::Expectation expected;
    expected.state = on ? "on" : "off";
    dashboard::act(intent, std::move(expected));
}

// runs the actions, the script, or stops the script while it runs (the tile's double tap does the same)
void send_run()
{
    const core::EntityState &s = state();
    core::Intent intent;
    intent.kind = core::IntentKind::Run;
    intent.entity = g.entity;
    intent.value = g.script && shown_on(s, dashboard::pending(g.entity)) ? core::kStop : core::kRun;
    core::Expectation expected;
    if (g.script) {
        // on while it runs; a short script is off again before the next values come, the
        // expectation ends by its timeout then
        expected.state = intent.value == core::kStop ? "off" : "on";
    }
    dashboard::act(intent, std::move(expected));
}

// ------------------------------------------------------------------------------- the control

void on_enabled_row(lv_event_t *)
{
    // a tap anywhere on the row flips the switch, as the rows of the settings do
    send_enabled(!shown_on(state(), dashboard::pending(g.entity)));
}

void on_toggle(lv_event_t *event)
{
    send_enabled(lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED));
}

void on_run(lv_event_t *event)
{
    // a step button repeats while held: only the tap counts
    if (lv_event_get_code(event) != LV_EVENT_SHORT_CLICKED || !available()) {
        return;
    }
    send_run();
}

// The rows of the control, centered in it: the Enabled row (an automation), the time of the
// last run under its caption, the Run button with its word. The button takes the room that is
// left, between kSmallestButton and the stepper size.
void build_control(lv_coord_t w, lv_coord_t h)
{
    const lv_coord_t caption_h = lv_font_get_line_height(&t::font_small());
    const lv_coord_t word_h = lv_font_get_line_height(&t::font_small());
    const lv_coord_t last_h = lv_font_get_line_height(&t::font_large());
    const lv_coord_t enabled = g.script ? 0 : static_cast<lv_coord_t>(kEnabledHeight + kRowGap);
    const auto rows = static_cast<lv_coord_t>(enabled + caption_h + last_h + kRowGap + word_h + 2 + kRowGap);
    const lv_coord_t button = std::clamp<lv_coord_t>(static_cast<lv_coord_t>(h - rows), kSmallestButton, panel::step_size(g.frame));
    const lv_coord_t total = static_cast<lv_coord_t>(rows + button);
    lv_coord_t y = std::max<lv_coord_t>(0, static_cast<lv_coord_t>((h - total) / 2));

    if (!g.script) {
        const lv_coord_t row_w = std::min<lv_coord_t>(kEnabledWidth, w);
        g.enabled = panel::card(g.control, {static_cast<lv_coord_t>((w - row_w) / 2), y,
                                            static_cast<lv_coord_t>((w - row_w) / 2 + row_w - 1),
                                            static_cast<lv_coord_t>(y + kEnabledHeight - 1)});
        lv_obj_add_event_cb(g.enabled, on_enabled_row, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *label = t::label(g.enabled, "Enabled", t::font_normal(), t::kText);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, kEnabledPad, 0);
        g.toggle = t::toggle(g.enabled, false, on_toggle);
        lv_obj_align(g.toggle, LV_ALIGN_RIGHT_MID, -kEnabledPad, 0);
        y = static_cast<lv_coord_t>(y + kEnabledHeight + kRowGap);
    }

    g.caption = t::label(g.control, "Last triggered", t::font_small(), t::kTextLabel);
    lv_obj_set_width(g.caption, w);
    lv_obj_set_style_text_align(g.caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.caption, 0, y);
    y = static_cast<lv_coord_t>(y + caption_h);

    g.last = t::label(g.control, "", t::font_large(), t::kText);
    lv_obj_set_width(g.last, w);
    lv_obj_set_style_text_align(g.last, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.last, 0, y);
    y = static_cast<lv_coord_t>(y + last_h + kRowGap);

    g.run = panel::step_button(g.control, static_cast<lv_coord_t>((w - button) / 2), y, button, "play", on_run);
    g.run_glyph = lv_obj_get_child(g.run, 0);
    g.word = t::label(g.control, "", t::font_small(), t::kTextLabel);
    lv_obj_set_width(g.word, w);
    lv_obj_set_style_text_align(g.word, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.word, 0, static_cast<lv_coord_t>(y + button + 2));
}

void update_control(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    if (g.last == nullptr) {
        return;
    }
    const bool on = shown_on(s, pending);
    const bool waiting = pending != nullptr;
    if (g.enabled != nullptr) {
        // the switch shows the expected state at once, the row's border says it is on its way
        if (on) {
            lv_obj_add_state(g.toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(g.toggle, LV_STATE_CHECKED);
        }
        lv_obj_set_style_border_color(g.enabled, t::color(waiting ? t::kAccent : t::kBorder), 0);
        lv_obj_set_style_opa(g.enabled, avail ? LV_OPA_COVER : LV_OPA_40, 0);
    }
    set_text(g.last, !s.known ? std::string("...") : !avail ? std::string("Unavailable") : tiles::when_text(s.last_triggered, true));
    // a running script is stopped by the button
    const bool stop = g.script && on && avail;
    set_text(g.run_glyph, icons::glyph(stop ? "stop" : "play"));
    set_text(g.word, stop ? "Stop" : g.script ? "Run script" : "Run actions");
    lv_obj_set_style_border_color(g.run, t::color(g.script && waiting ? t::kAccent : t::kBorder), 0);
    if (avail) {
        lv_obj_clear_state(g.run, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(g.run, LV_STATE_DISABLED);
    }
    lv_obj_set_style_opa(g.run, avail ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_opa(g.word, avail ? LV_OPA_COVER : LV_OPA_40, 0);
}

// ------------------------------------------------------------------------------- modes

void clear_control()
{
    history_view::forget();
    if (g.control != nullptr) {
        lv_obj_del(g.control);
    }
    g.control = nullptr;
    g.enabled = g.toggle = g.caption = g.last = g.run = g.run_glyph = g.word = nullptr;
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
        // an automation's state is whether it is enabled, a script's whether it runs
        history_view::Legend legend;
        legend.on = g.script ? "Running" : "Enabled";
        legend.off = g.script ? "Idle" : "Disabled";
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
    const core::Layout &layout = dashboard::layout();
    g.entity = layout.tiles[tile].entity;
    const std::string_view entity_id = layout.entity_id(layout.tiles[tile]);
    g.script = entity_id.substr(0, entity_id.find('.')) == "script";
    const char *glyphs[kModes] = {g.script ? "script-text" : "robot", "chart-timeline-variant"};
    for (int i = 0; i < kModes; i++) {
        g.buttons[i] = panel::card(frame.root, frame.options);
        lv_obj_center(t::label(g.buttons[i], icons::glyph(glyphs[i]), icons::small(), t::kTextLabel));
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
    // the objects go with the panel's root
    g = {};
}

} // namespace hc::ui::automation_panel
