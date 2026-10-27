// The gestures of a tile (docs/ui/lvgl/lvgl-touch.md, docs/ui/lvgl/lvgl-dashboard.md section 3):
// taps, double taps, the drag of a dimmer and the steps of a climate tile, turned into intents
// through the dashboard.

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "dashboard.hpp"
#include "swipe.hpp"
#include "tiles_parts.hpp"

namespace hc::ui::tiles::detail {

namespace {

using core::TileKind;

// a finger has to move this far before a dimmer drag starts; less is a tap
constexpr lv_coord_t kDragStart = 10;
// a second tap within this time is a double tap (dimmer: on/off)
constexpr uint32_t kDoubleTapMs = 300;
// the steps of a climate tile are sent once the taps pause this long
constexpr uint32_t kStepSendMs = 700;

core::EntityIndex entity_of(const View &view)
{
    return view.entity;
}

void send(const View &view, core::IntentKind kind, float value, core::Expectation expected)
{
    core::Intent intent;
    intent.kind = kind;
    intent.entity = entity_of(view);
    intent.value = value;
    dashboard::act(intent, std::move(expected));
}

// the state the tile shows: the pending action's, else the entity's
bool shown_on(const View &view)
{
    const core::Expectation *pending = dashboard::pending(view.entity);
    if (pending != nullptr && !pending->state.empty()) {
        return pending->state == "on";
    }
    return dashboard::state(view.entity).is_on();
}

void toggle(const View &view)
{
    core::Expectation expected;
    expected.state = shown_on(view) ? "off" : "on";
    send(view, core::IntentKind::Toggle, 0, std::move(expected));
}

// the single tap of a switch, once no second one came: the toggle
void on_single_tap_toggle(lv_timer_t *timer)
{
    auto &view = *static_cast<View *>(timer->user_data);
    view.tap_timer = nullptr;
    lv_timer_del(timer);
    if (dashboard::state(view.entity).unavailable() && dashboard::pending(view.entity) == nullptr) {
        return;
    }
    toggle(view);
}

void on_single_tap(lv_timer_t *timer);

// A double tap on a lock tile: lock when it is unlocked or open, unlock when it is locked, from the
// state it shows (the pending action's, else the entity's). Jammed, unavailable or unknown: the
// panel opens instead, it says why.
void toggle_lock(const View &view)
{
    const core::Expectation *pending = dashboard::pending(view.entity);
    const std::string_view state = pending != nullptr && !pending->state.empty()
                                       ? std::string_view(pending->state)
                                       : std::string_view(dashboard::state(view.entity).state);
    const bool locked = state == "locked" || state == "locking";
    const bool unlocked = state == "unlocked" || state == "unlocking" || state == "open" || state == "opening";
    if (!locked && !unlocked) {
        dashboard::open_panel(view.tile);
        return;
    }
    core::Expectation expected;
    expected.state = locked ? "unlocked" : "locked";
    send(view, core::IntentKind::SetLock, locked ? core::kUnlock : core::kLock, std::move(expected));
}

// A double tap on a timer tile: cancels a running timer, starts an idle or a paused one (resumed
// with the time it has left), from the state it shows. Unavailable or unknown: the panel opens.
void toggle_timer(const View &view)
{
    const TimerState shown = timer_shown(dashboard::state(view.entity), dashboard::pending(view.entity));
    if (shown == TimerState::Unknown) {
        dashboard::open_panel(view.tile);
        return;
    }
    core::Intent intent;
    intent.kind = core::IntentKind::SetTimer;
    intent.entity = view.entity;
    intent.value = shown == TimerState::Active ? core::kTimerCancel : core::kTimerStart;
    core::Expectation expected;
    expected.state = shown == TimerState::Active ? "idle" : "active";
    dashboard::act(intent, std::move(expected));
}

// A double tap on an automation tile runs its actions, on a script tile the script, or stops
// the script while it runs (what the panel's button does). Unavailable or unknown: the panel opens.
void run(const View &view)
{
    const core::EntityState &state = dashboard::state(view.entity);
    if (!state.known || state.unavailable()) {
        dashboard::open_panel(view.tile);
        return;
    }
    const core::Layout &layout = dashboard::layout();
    const bool script = domain_of(layout.entity_id(layout.tiles[view.tile])) == "script";
    core::Intent intent;
    intent.kind = core::IntentKind::Run;
    intent.entity = view.entity;
    intent.value = script && state.is_on() ? core::kStop : core::kRun;
    core::Expectation expected;
    if (script) {
        // on while it runs; a short script is off again before the next values come, the
        // expectation ends by its timeout then
        expected.state = intent.value == core::kStop ? "off" : "on";
    }
    dashboard::act(intent, std::move(expected));
}

} // namespace

void on_click(lv_event_t *event)
{
    auto &view = *static_cast<View *>(lv_event_get_user_data(event));
    const core::Layout &layout = dashboard::layout();
    if (view.tile == core::kNone) {
        dashboard::back();
        return;
    }
    const core::Tile &tile = layout.tiles[view.tile];
    switch (view.kind) {
    case TileKind::Area:
        dashboard::open_page(tile.opens);
        break;
    case TileKind::Media:
        dashboard::open_media(view.tile);
        break;
    case TileKind::Switch:
    case TileKind::Light: {
        // a second tap within the window opens the panel with the timeline, a single one toggles
        // once the window has passed (docs/ui/lvgl/lvgl-dashboard.md section 3)
        const uint32_t now = lv_tick_get();
        if (view.tap_timer != nullptr && now - view.last_tap <= kDoubleTapMs) {
            lv_timer_del(view.tap_timer);
            view.tap_timer = nullptr;
            dashboard::open_panel(view.tile);
        } else {
            view.last_tap = now;
            if (view.tap_timer == nullptr) {
                view.tap_timer = lv_timer_create(on_single_tap_toggle, kDoubleTapMs, &view);
            }
        }
        break;
    }
    case TileKind::Button: {
        core::Intent intent;
        intent.kind = core::IntentKind::Press;
        intent.entity = view.entity;
        dashboard::act(intent, {});
        break;
    }
    case TileKind::Lock:
    case TileKind::Timer:
    case TileKind::Automation: {
        // a double tap locks or unlocks (starts or cancels a timer, runs an automation or a
        // script), a single one opens the panel once the window has passed (as a dimmer;
        // docs/ui/lvgl/lvgl-dashboard.md section 3)
        const uint32_t now = lv_tick_get();
        if (view.tap_timer != nullptr && now - view.last_tap <= kDoubleTapMs) {
            lv_timer_del(view.tap_timer);
            view.tap_timer = nullptr;
            if (view.kind == TileKind::Timer) {
                toggle_timer(view);
            } else if (view.kind == TileKind::Automation) {
                run(view);
            } else {
                toggle_lock(view);
            }
        } else {
            view.last_tap = now;
            if (view.tap_timer == nullptr) {
                view.tap_timer = lv_timer_create(on_single_tap, kDoubleTapMs, &view);
            }
        }
        break;
    }
    case TileKind::Climate:
    case TileKind::Sensor:
    case TileKind::Picture:
    case TileKind::Weather:
        dashboard::open_panel(view.tile);
        break;
    default:
        break;
    }
}

namespace {

int level_of(const View &view, lv_coord_t y)
{
    lv_area_t area;
    lv_obj_get_coords(view.card, &area);
    const int height = std::max(1, static_cast<int>(lv_area_get_height(&area)));
    return std::clamp((area.y2 - y) * 100 / height, 0, 100);
}

void on_single_tap(lv_timer_t *timer)
{
    auto &view = *static_cast<View *>(timer->user_data);
    view.tap_timer = nullptr;
    lv_timer_del(timer);
    dashboard::open_panel(view.tile);
}

} // namespace

void on_dimmer(lv_event_t *event)
{
    auto &view = *static_cast<View *>(lv_event_get_user_data(event));
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point{};
    if (indev != nullptr) {
        lv_indev_get_point(indev, &point);
    }
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        view.press_x = point.x;
        view.press_y = point.y;
        view.dragging = false;
        break;
    case LV_EVENT_PRESSING:
        // a drag that goes more up or down than sideways; a sideways one is a swipe of the page
        if (!view.dragging && !swipe::active() && std::abs(point.y - view.press_y) >= kDragStart &&
            std::abs(point.y - view.press_y) > std::abs(point.x - view.press_x)) {
            view.dragging = true;
        }
        if (view.dragging) {
            view.drag_level = level_of(view, point.y);
            dashboard::redraw(view.entity);
        }
        break;
    case LV_EVENT_RELEASED: {
        if (view.dragging) {
            view.dragging = false;
            core::Expectation expected;
            expected.state = view.drag_level > 0 ? "on" : "off";
            if (view.drag_level > 0) {
                expected.brightness = static_cast<int16_t>((view.drag_level * 255 + 50) / 100);
            }
            send(view, core::IntentKind::SetLevel, static_cast<float>(view.drag_level), std::move(expected));
            break;
        }
        // a tap: a second one within the window switches the light, a single one opens the panel
        const uint32_t now = lv_tick_get();
        if (view.tap_timer != nullptr && now - view.last_tap <= kDoubleTapMs) {
            lv_timer_del(view.tap_timer);
            view.tap_timer = nullptr;
            toggle(view);
        } else {
            view.last_tap = now;
            if (view.tap_timer == nullptr) {
                view.tap_timer = lv_timer_create(on_single_tap, kDoubleTapMs, &view);
            }
        }
        break;
    }
    default:
        break;
    }
}

void on_step_send(lv_timer_t *timer)
{
    auto &view = *static_cast<View *>(timer->user_data);
    view.send_timer = nullptr;
    lv_timer_del(timer);
    core::Expectation expected;
    expected.target = view.step_target;
    send(view, core::IntentKind::SetTemperature, view.step_target, std::move(expected));
}

void on_step(lv_event_t *event)
{
    auto &view = *static_cast<View *>(lv_event_get_user_data(event));
    const core::EntityState &state = dashboard::state(view.entity);
    if (state.unavailable() || std::isnan(state.target)) {
        return;
    }
    const core::Tile &tile = dashboard::layout().tiles[view.tile];
    const float step = climate_step(tile, state);
    const core::Expectation *pending = dashboard::pending(view.entity);
    float target = view.send_timer != nullptr ? view.step_target
                   : pending != nullptr && !std::isnan(pending->target) ? pending->target
                                                                         : state.target;
    target += lv_event_get_target(event) == view.plus ? step : -step;
    target = std::round(target / step) * step;
    if (!std::isnan(state.min_temp)) {
        target = std::max(target, state.min_temp);
    }
    if (!std::isnan(state.max_temp)) {
        target = std::min(target, state.max_temp);
    }
    view.step_target = target;
    // shown at once as pending, sent when the taps pause
    core::Expectation expected;
    expected.target = target;
    dashboard::expect(view.entity, std::move(expected));
    if (view.send_timer != nullptr) {
        lv_timer_reset(view.send_timer);
    } else {
        view.send_timer = lv_timer_create(on_step_send, kStepSendMs, &view);
    }
}

} // namespace hc::ui::tiles::detail
