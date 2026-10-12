#include "overlay.hpp"

#include <cstdio>
#include <cstring>

#include "dashboard.hpp"
#include "settings.hpp"
#include "setup.hpp"
#include "theme.hpp"

namespace hc::ui::overlay {

namespace {

namespace t = theme;

// the touch panel often reports a finger from the edge only some pixels down: the zone is high
// enough to catch it there (kZoneHeight)
constexpr lv_coord_t kZoneWidth = 160;
constexpr lv_coord_t kDragDistance = 24;
constexpr uint32_t kUpdateMs = 500;
constexpr lv_opa_t kStripOpacity = LV_OPA_60;
// a Home Assistant problem is shown once it lasts this long: a reconnect or a restart of Home
// Assistant does not flash the strip
constexpr uint32_t kHassProblemMs = 10000;
// the strip slides down when it appears and up when it goes (the panel dialogs' time)
constexpr uint32_t kSlideMs = 250;
// the progress of a slide: 0 above the screen, kShown in place
constexpr int32_t kShown = 1024;
constexpr size_t kTextSize = 128;

struct State {
    const Backend *backend = nullptr;
    lv_obj_t *zone = nullptr;
    lv_obj_t *strip = nullptr;
    lv_obj_t *strip_label = nullptr;
    // the page a tap on the strip opens
    settings::Page strip_page = settings::Page::Quick;
    char strip_text[kTextSize] = {};
    lv_coord_t strip_for_width = 0;
    // the strip is coming or there (it slides in), else going or gone
    bool strip_in = false;
    int32_t shown = 0; // the progress of its slide
    // the event message of show(); empty while there is none
    char event_text[kTextSize] = {};
    uint32_t event_color = 0;
    uint32_t event_since = 0;
    uint32_t event_ms = 0;
    void (*event_tap)() = nullptr; // a tap on the event message calls it; nullptr: the tap closes it
    // the strip shows the event message: a tap closes it
    bool showing_event = false;
    void (*moved)() = nullptr; // on_strip_moved()
    lv_coord_t press_y = 0;
    bool dragged = false;
    // Settings was opened by the swipe since the start; the hint is stored as seen once the
    // onboarding is complete too
    bool swiped = false;
    // lv_tick_get() when the Home Assistant problem started, 0 while there is none
    uint32_t hass_problem_since = 0;
};

State g;

void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// The zone and the strip both open Settings when the finger moves down far enough.
void on_drag(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const lv_event_code_t code = lv_event_get_code(event);
    if (dashboard::editing()) {
        return; // the strip shows an event message over the edit bar: no Settings
    }
    if (code == LV_EVENT_PRESSED) {
        g.press_y = point.y;
        g.dragged = false;
    } else if (code == LV_EVENT_PRESSING && !g.dragged && point.y - g.press_y > kDragDistance) {
        g.dragged = true;
        g.swiped = true;
        lv_indev_wait_release(indev);
        settings::open(settings::Page::Quick);
    }
}

void on_update(lv_timer_t *);

void on_strip_click(lv_event_t *)
{
    if (g.showing_event && g.event_tap != nullptr) {
        // the message stays until its code shows the next one or clears it
        g.event_tap();
    } else if (g.showing_event) {
        g.event_text[0] = 0;
        on_update(nullptr);
    } else if (!g.dragged && !dashboard::editing()) {
        settings::open(g.strip_page);
    }
}

// the event message of show() while it lasts
bool event_message(char *out, size_t size, uint32_t &color)
{
    if (g.event_text[0] == 0) {
        return false;
    }
    if (g.event_ms != 0 && lv_tick_elaps(g.event_since) >= g.event_ms) {
        g.event_text[0] = 0;
        return false;
    }
    std::snprintf(out, size, "%s", g.event_text);
    color = g.event_color;
    return true;
}

// A Home Assistant problem that lasts kHassProblemMs, in words; false while there is none.
bool hass_problem(char *out, size_t size)
{
    HassInfo h{};
    g.backend->hass_info(h);
    if (!h.failed) {
        g.hass_problem_since = 0;
        return false;
    }
    if (g.hass_problem_since == 0) {
        g.hass_problem_since = lv_tick_get() | 1;
    }
    if (lv_tick_elaps(g.hass_problem_since) < kHassProblemMs) {
        return false;
    }
    // a long error is cut: the strip shows the gist, the page the whole text
    std::snprintf(out, size, LV_SYMBOL_WARNING "  Home Assistant: %.60s", h.error[0] != 0 ? h.error : h.state);
    return true;
}

// The hint "Swipe down for Settings" stays while the onboarding runs (the start screen's "Set up
// Home Assistant" leads there too) and goes once Settings was opened by the swipe after it is
// complete: Wi-Fi up and Home Assistant connected.
bool hint_done(const WifiInfo &w)
{
    HassInfo h{};
    g.backend->hass_info(h);
    // without an address and token the start screen offers the setup: the hint is back too (also
    // after a factory reset of Home Assistant's settings, or when it was seen before this rule)
    if (!h.configured) {
        return false;
    }
    if (g.backend->hint_seen()) {
        return true;
    }
    if (!g.swiped || !w.connected || !h.connected) {
        return false;
    }
    g.backend->set_hint_seen();
    return true;
}

// Picks the message of the strip: a Wi-Fi problem, then connecting, then a Home Assistant
// problem (it cannot connect without Wi-Fi anyway), then the hint. Empty hides it.
void strip_message(char *out, size_t size, uint32_t &color, settings::Page &page)
{
    WifiInfo w{};
    g.backend->wifi_info(w);
    out[0] = 0;
    page = settings::Page::Wifi;
    if (!w.ready) {
        // the network component has not reported yet (the P4 waits for its coprocessor): no
        // saved networks is not known yet
        return;
    }
    if (!w.connected && w.error[0] != 0) {
        std::snprintf(out, size, LV_SYMBOL_WARNING "  %s", w.error);
        color = t::kAlert;
    } else if (!w.has_networks) {
        std::snprintf(out, size, LV_SYMBOL_WARNING "  Wi-Fi is not set up");
        color = t::kAlert;
    } else if (w.connecting) {
        std::snprintf(out, size, LV_SYMBOL_WIFI "  %s %s", w.state, w.ssid);
        color = t::kTextValue;
    } else if (hass_problem(out, size)) {
        color = t::kAlert;
        page = settings::Page::HomeAssistant;
    } else if (!hint_done(w)) {
        std::snprintf(out, size, LV_SYMBOL_DOWN "  Swipe down for Settings");
        color = t::kTextLabel;
        page = settings::Page::Quick;
    }
}

// the strip at `progress` of its slide: its whole height above the screen at 0, in place at kShown
void place(int32_t progress)
{
    g.shown = progress;
    const lv_coord_t height = lv_obj_get_height(g.strip);
    lv_obj_set_style_translate_y(g.strip, static_cast<lv_coord_t>(-height * (kShown - progress) / kShown), 0);
    if (g.moved != nullptr) {
        g.moved();
    }
}

// The strip slides in (down) or out (up), from where it is; at the end of a slide out it is hidden.
void slide(bool in)
{
    if (in == g.strip_in) {
        return;
    }
    g.strip_in = in;
    lv_anim_del(g.strip, nullptr);
    if (in && lv_obj_has_flag(g.strip, LV_OBJ_FLAG_HIDDEN)) {
        set_hidden(g.strip, false);
        lv_obj_update_layout(g.strip); // its height, for the start above the screen
        place(0);
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, g.strip);
    lv_anim_set_values(&anim, g.shown, in ? kShown : 0);
    // the whole way takes kSlideMs, the rest of an interrupted slide its part of it
    const int32_t way = in ? kShown - g.shown : g.shown;
    lv_anim_set_time(&anim, static_cast<uint32_t>(kSlideMs * way / kShown));
    lv_anim_set_path_cb(&anim, in ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&anim, [](void *, int32_t progress) { place(progress); });
    if (!in) {
        lv_anim_set_ready_cb(&anim, [](lv_anim_t *) { set_hidden(g.strip, true); });
    }
    lv_anim_start(&anim);
}

// gone at once, without the slide (Settings covers the screen)
void remove_strip()
{
    lv_anim_del(g.strip, nullptr);
    set_hidden(g.strip, true);
    g.strip_in = false;
    g.shown = 0;
    g.strip_text[0] = 0;
    g.showing_event = false;
    if (g.moved != nullptr) {
        g.moved();
    }
}

// `events_only`: while the dashboard is edited, only the event messages
void update_strip(bool events_only)
{
    char text[sizeof(g.strip_text)];
    uint32_t color = t::kText;
    g.showing_event = event_message(text, sizeof(text), color);
    if (!g.showing_event) {
        text[0] = 0;
        if (!events_only) {
            strip_message(text, sizeof(text), color, g.strip_page);
        }
    }
    if (text[0] == 0) {
        g.strip_text[0] = 0;
        slide(false);
        return;
    }
    // the strip fits its message, up to 90 % of the width (the display may have turned)
    const lv_coord_t width = lv_obj_get_width(lv_layer_top());
    if (std::strcmp(text, g.strip_text) != 0 || width != g.strip_for_width) {
        std::snprintf(g.strip_text, sizeof(g.strip_text), "%s", text);
        g.strip_for_width = width;
        lv_label_set_text(g.strip_label, text);
        const lv_font_t *font = lv_obj_get_style_text_font(g.strip_label, 0);
        const lv_coord_t max_width = width * 9 / 10 - 2 * t::kRadius;
        lv_point_t size;
        lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_width(g.strip_label, LV_MIN(size.x + 1, max_width));
        if (g.strip_in) {
            // a new text on the strip that is there: its height may have changed
            lv_obj_update_layout(g.strip);
            place(g.shown);
        }
    }
    lv_obj_set_style_text_color(g.strip_label, t::color(color), 0);
    slide(true);
}

void on_update(lv_timer_t *)
{
    if (g.zone == nullptr) {
        return;
    }
    // Settings and the first setup have their own frame: no pull-down, no strip. The edit mode has
    // its bar: no pull-down, only event messages.
    const bool frame = settings::is_open() || setup::active();
    const bool editing = dashboard::editing();
    set_hidden(g.zone, frame || editing);
    if (frame) {
        remove_strip();
    } else {
        update_strip(editing);
    }
}

void add_drag(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(obj, on_drag, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(obj, on_drag, LV_EVENT_PRESSING, nullptr);
}

} // namespace

void update()
{
    on_update(nullptr);
}

lv_coord_t strip_bottom()
{
    if (g.strip == nullptr || lv_obj_has_flag(g.strip, LV_OBJ_FLAG_HIDDEN)) {
        return 0;
    }
    // aligned kRadius above the screen edge, moved up by the slide
    const lv_coord_t bottom = lv_obj_get_y(g.strip) + lv_obj_get_height(g.strip) +
                              lv_obj_get_style_translate_y(g.strip, LV_PART_MAIN);
    return LV_MAX(bottom, 0);
}

void on_strip_moved(void (*callback)())
{
    g.moved = callback;
}

void clear()
{
    g.event_tap = nullptr;
    if (g.event_text[0] != 0) {
        g.event_text[0] = 0;
        on_update(nullptr);
    }
}

void show(const char *text, uint32_t color, uint32_t ms, void (*on_tap)())
{
    g.event_tap = on_tap;
    std::snprintf(g.event_text, sizeof(g.event_text), "%s", text);
    g.event_color = color;
    g.event_since = lv_tick_get();
    g.event_ms = ms;
    on_update(nullptr);
}

void init(const Backend &backend)
{
    g = {};
    g.backend = &backend;
    lv_obj_t *top = lv_layer_top();

    g.zone = t::plain(top);
    lv_obj_set_size(g.zone, kZoneWidth, kZoneHeight);
    lv_obj_align(g.zone, LV_ALIGN_TOP_MID, 0, 0);
    add_drag(g.zone);

    // the top corners are above the screen edge, only the bottom ones are seen
    g.strip = t::plain(top);
    lv_obj_set_size(g.strip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(g.strip, LV_ALIGN_TOP_MID, 0, -t::kRadius);
    lv_obj_set_style_bg_opa(g.strip, kStripOpacity, 0);
    lv_obj_set_style_bg_color(g.strip, lv_color_black(), 0);
    lv_obj_set_style_radius(g.strip, t::kRadius, 0);
    lv_obj_set_style_pad_top(g.strip, t::kRadius + 8, 0);
    lv_obj_set_style_pad_bottom(g.strip, 8, 0);
    lv_obj_set_style_pad_hor(g.strip, t::kRadius + 4, 0);
    add_drag(g.strip);
    lv_obj_add_event_cb(g.strip, on_strip_click, LV_EVENT_CLICKED, nullptr);

    g.strip_label = t::label(g.strip, "", t::font_normal(), t::kText);
    lv_label_set_long_mode(g.strip_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g.strip_label, LV_TEXT_ALIGN_CENTER, 0);
    set_hidden(g.strip, true);

    lv_timer_create(on_update, kUpdateMs, nullptr);
    on_update(nullptr);
}

} // namespace hc::ui::overlay
