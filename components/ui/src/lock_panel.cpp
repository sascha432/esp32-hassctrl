#include "lock_panel.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "dashboard.hpp"
#include "history_view.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::lock_panel {

namespace {

namespace t = theme;
using panel::set_hidden;
using panel::set_text;

enum class Mode : uint8_t { Control, History, Count };
constexpr int kModes = static_cast<int>(Mode::Count);
constexpr const char *kModeGlyphs[kModes] = {"lock", "chart-timeline-variant"};

// the thumb: this far inside the track, rounded like this
constexpr lv_coord_t kThumbInset = 6;
constexpr lv_coord_t kThumbRadius = 18;
// a press that moves this far is a drag, else a tap
constexpr lv_coord_t kDragStart = 8;
// the open button: this far from the slider, armed this long by the first tap
constexpr lv_coord_t kOpenGap = 20;
constexpr uint32_t kArmedMs = 3000;

// where the thumb is
enum class Place : uint8_t { Top, Middle, Bottom };

struct State {
    panel::Frame frame;
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    lv_obj_t *buttons[kModes] = {};
    Mode mode = Mode::Control;
    lv_obj_t *control = nullptr; // the container of `mode`

    // the slider
    lv_obj_t *track = nullptr;
    lv_obj_t *thumb = nullptr;
    lv_obj_t *glyph = nullptr;
    lv_obj_t *word = nullptr;
    // as built: LVGL lays the objects out only before the next frame
    lv_coord_t track_h = 0;
    lv_coord_t thumb_h = 0;
    bool pressed = false;
    bool dragging = false;
    lv_coord_t press_y = 0;
    lv_coord_t thumb_start = 0; // the thumb's y when the press began

    // the open button, only with the OPEN feature
    bool with_open = false;
    lv_obj_t *open = nullptr;
    lv_obj_t *open_note = nullptr;
    lv_timer_t *armed = nullptr; // running while the first tap waits for the second
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

bool can_open(const core::EntityState &s)
{
    return (s.features & core::kLockOpen) != 0;
}

// the state the panel shows: the pending action's, else the entity's
std::string_view shown_state(const core::EntityState &s, const core::Expectation *pending)
{
    if (pending != nullptr && !pending->state.empty()) {
        return pending->state;
    }
    return s.state;
}

bool is_locked(std::string_view state)
{
    return state == "locked" || state == "locking";
}

bool is_unlocked(std::string_view state)
{
    return state == "unlocked" || state == "unlocking" || state == "open" || state == "opening";
}

Place place_of(std::string_view state)
{
    return is_locked(state) ? Place::Top : is_unlocked(state) ? Place::Bottom : Place::Middle;
}

lv_coord_t thumb_y(Place place)
{
    switch (place) {
    case Place::Top:
        return kThumbInset;
    case Place::Bottom:
        return static_cast<lv_coord_t>(g.track_h - kThumbInset - g.thumb_h);
    case Place::Middle:
        break;
    }
    return static_cast<lv_coord_t>((g.track_h - g.thumb_h) / 2);
}

void send(float action, core::Expectation expected)
{
    core::Intent intent;
    intent.kind = core::IntentKind::SetLock;
    intent.entity = g.entity;
    intent.value = action;
    dashboard::act(intent, std::move(expected));
}

void send_place(Place place)
{
    core::Expectation expected;
    expected.state = place == Place::Top ? "locked" : "unlocked";
    send(place == Place::Top ? core::kLock : core::kUnlock, std::move(expected));
}

// ------------------------------------------------------------------------------- the slider

void on_track(lv_event_t *event)
{
    lv_point_t point{};
    if (lv_indev_t *indev = lv_indev_get_act()) {
        lv_indev_get_point(indev, &point);
    }
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        if (!available()) {
            return;
        }
        g.pressed = true;
        g.dragging = false;
        g.press_y = point.y;
        g.thumb_start = lv_obj_get_y(g.thumb);
        break;
    case LV_EVENT_PRESSING:
        if (!g.pressed) {
            return;
        }
        if (!g.dragging && std::abs(point.y - g.press_y) >= kDragStart) {
            g.dragging = true;
        }
        if (g.dragging) {
            // the thumb follows the finger between its two places
            lv_obj_set_y(g.thumb, std::clamp<lv_coord_t>(static_cast<lv_coord_t>(g.thumb_start + point.y - g.press_y),
                                                         thumb_y(Place::Top), thumb_y(Place::Bottom)));
        }
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
        if (!g.pressed) {
            return;
        }
        g.pressed = false;
        const bool dragged = g.dragging;
        g.dragging = false;
        if (lv_event_get_code(event) == LV_EVENT_PRESS_LOST) {
            update();
            return;
        }
        // the half of the thumb's middle after a drag, the half of the tap after a tap
        lv_area_t area;
        lv_obj_get_coords(g.track, &area);
        const lv_coord_t middle = static_cast<lv_coord_t>(dragged ? lv_obj_get_y(g.thumb) + g.thumb_h / 2 : point.y - area.y1);
        const Place target = middle < g.track_h / 2 ? Place::Top : Place::Bottom;
        if (target != place_of(shown_state(state(), dashboard::pending(g.entity)))) {
            send_place(target);
        }
        // the thumb goes to the expected place, or back
        update();
        break;
    }
    default:
        break;
    }
}

// builds the slider and returns where it ends at the right
lv_coord_t build_slider(lv_coord_t w, lv_coord_t h)
{
    const lv_coord_t step = panel::step_size(g.frame);
    const lv_coord_t beside = g.with_open ? static_cast<lv_coord_t>(kOpenGap + step) : 0;
    const panel::SliderPlace place = panel::slider_place(g.frame, static_cast<lv_coord_t>(w - beside), h);
    const lv_coord_t x = static_cast<lv_coord_t>((w - (place.w + beside)) / 2);
    g.track = panel::slider_track(g.control, x, place.y, place.w, place.h);
    for (const lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(g.track, on_track, code, nullptr);
    }
    g.track_h = place.h;
    g.thumb_h = static_cast<lv_coord_t>(place.h / 2 - kThumbInset);
    g.thumb = t::plain(g.track);
    lv_obj_set_size(g.thumb, static_cast<lv_coord_t>(place.w - 2 * kThumbInset), g.thumb_h);
    lv_obj_set_pos(g.thumb, kThumbInset, thumb_y(Place::Middle));
    lv_obj_set_style_radius(g.thumb, kThumbRadius, 0);
    lv_obj_set_style_bg_opa(g.thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g.thumb, 1, 0);
    lv_obj_set_flex_flow(g.thumb, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g.thumb, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g.thumb, 2, 0);
    lv_obj_clear_flag(g.thumb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g.thumb, LV_OBJ_FLAG_SCROLLABLE);
    // the large glyph when the thumb has room for it and the word
    const bool large = g.thumb_h >= lv_font_get_line_height(&icons::large()) + lv_font_get_line_height(&t::font_normal()) + 8;
    g.glyph = t::label(g.thumb, "", large ? icons::large() : icons::small(), t::kText);
    g.word = t::label(g.thumb, "", t::font_normal(), t::kText);
    return static_cast<lv_coord_t>(x + place.w);
}

void update_slider(const core::EntityState &s, const core::Expectation *pending, bool avail)
{
    if (g.track == nullptr) {
        return;
    }
    const std::string_view shown = shown_state(s, pending);
    const bool jammed = shown == "jammed";
    const bool unlocked = is_unlocked(shown);
    lv_obj_set_style_opa(g.track, avail ? LV_OPA_COVER : LV_OPA_50, 0);
    if (!g.dragging) {
        lv_obj_set_y(g.thumb, thumb_y(avail ? place_of(shown) : Place::Middle));
    }
    const uint32_t fill = !avail ? t::kCard : jammed ? t::kLockJammed : unlocked ? t::kLockUnlocked : t::kLockLocked;
    lv_obj_set_style_bg_color(g.thumb, t::color(fill), 0);
    lv_obj_set_style_border_color(g.thumb, t::color(fill == t::kCard ? t::kBorder : fill), 0);
    set_text(g.glyph, icons::glyph(jammed ? "lock-alert" : unlocked ? "lock-open" : "lock"));
    set_text(g.word, !s.known ? std::string("...") : !avail ? std::string("--") : tiles::words(shown));
}

// ------------------------------------------------------------------------------- the open button

void disarm()
{
    if (g.armed != nullptr) {
        lv_timer_del(g.armed);
        g.armed = nullptr;
    }
    update();
}

void on_open(lv_event_t *event)
{
    // a step button repeats while held: only the tap counts
    if (lv_event_get_code(event) != LV_EVENT_SHORT_CLICKED || !available()) {
        return;
    }
    if (g.armed == nullptr) {
        // the first tap asks for a second one, like HA's "Really open?"
        g.armed = lv_timer_create([](lv_timer_t *) { disarm(); }, kArmedMs, nullptr);
        update();
        return;
    }
    // the lock goes back to locked by itself: nothing to wait for
    send(core::kOpen, {});
    disarm();
}

// the button right of the slider, which ends at `slider_end` (as built: LVGL lays out later)
void build_open(lv_coord_t w, lv_coord_t h, lv_coord_t slider_end)
{
    const lv_coord_t step = panel::step_size(g.frame);
    const lv_coord_t x = static_cast<lv_coord_t>(slider_end + kOpenGap);
    const lv_coord_t y = static_cast<lv_coord_t>((h - step) / 2);
    g.open = panel::step_button(g.control, x, y, step, "door-open", on_open);
    // the note under the button, as wide as the room right of the slider
    const auto note_w = static_cast<lv_coord_t>(std::max<lv_coord_t>(step, static_cast<lv_coord_t>(w - x)));
    g.open_note = t::label(g.control, "Tap again\nto open", t::font_small(), t::kActive);
    lv_obj_set_width(g.open_note, note_w);
    lv_obj_set_style_text_align(g.open_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.open_note, static_cast<lv_coord_t>(x + step / 2 - note_w / 2), static_cast<lv_coord_t>(y + step + 6));
}

void update_open(bool avail)
{
    if (g.open == nullptr) {
        return;
    }
    const bool armed = g.armed != nullptr && avail;
    lv_obj_set_style_bg_color(g.open, t::color(armed ? t::kActive : t::kCard), 0);
    lv_obj_set_style_border_color(g.open, t::color(armed ? t::kActive : t::kBorder), 0);
    if (avail) {
        lv_obj_clear_state(g.open, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(g.open, LV_STATE_DISABLED);
    }
    set_hidden(g.open_note, !armed);
}

// ------------------------------------------------------------------------------- modes

void clear_control()
{
    history_view::forget();
    if (g.armed != nullptr) {
        lv_timer_del(g.armed);
    }
    if (g.control != nullptr) {
        lv_obj_del(g.control);
    }
    g.control = nullptr;
    g.track = g.thumb = g.glyph = g.word = nullptr;
    g.pressed = g.dragging = false;
    g.open = g.open_note = nullptr;
    g.armed = nullptr;
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
        legend.on = "Unlocked";
        legend.off = "Locked";
        legend.problem = "Jammed";
        legend.on_color = t::kLockUnlocked;
        legend.off_color = t::kLockLocked;
        legend.problem_color = t::kLockJammed;
        history_view::build(g.control, {0, 0, static_cast<lv_coord_t>(w - 1), static_cast<lv_coord_t>(h - 1)}, g.tile,
                            core::HistoryKind::States, legend);
        return;
    }
    g.with_open = can_open(state());
    const lv_coord_t slider_end = build_slider(w, h);
    if (g.with_open) {
        build_open(w, h, slider_end);
    }
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
    const core::EntityState &s = state();
    // the open button comes with supported_features, which arrives with the panel's fields
    if (g.mode == Mode::Control && g.with_open != can_open(s) && !g.pressed) {
        show_mode(Mode::Control);
    }
    for (int i = 0; i < kModes; i++) {
        panel::mark_option(g.buttons[i], static_cast<int>(g.mode) == i);
    }
    if (g.mode == Mode::History) {
        history_view::update();
        return;
    }
    const bool avail = available();
    update_slider(s, dashboard::pending(g.entity), avail);
    update_open(avail);
}

void forget()
{
    history_view::forget();
    if (g.armed != nullptr) {
        lv_timer_del(g.armed);
    }
    // the objects go with the panel's root
    g = {};
}

} // namespace hc::ui::lock_panel
