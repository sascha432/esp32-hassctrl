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
// enough to catch it there
constexpr lv_coord_t kZoneWidth = 160;
constexpr lv_coord_t kZoneHeight = 40;
constexpr lv_coord_t kDragDistance = 24;
constexpr uint32_t kUpdateMs = 500;
constexpr lv_opa_t kStripOpacity = LV_OPA_60;
// a Home Assistant problem is shown once it lasts this long: a reconnect or a restart of Home
// Assistant does not flash the strip
constexpr uint32_t kHassProblemMs = 10000;

struct State {
    const Backend *backend = nullptr;
    lv_obj_t *zone = nullptr;
    lv_obj_t *strip = nullptr;
    lv_obj_t *strip_label = nullptr;
    // the page a tap on the strip opens
    settings::Page strip_page = settings::Page::Quick;
    char strip_text[96] = {};
    lv_coord_t strip_for_width = 0;
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

void on_strip_click(lv_event_t *)
{
    if (!g.dragged) {
        settings::open(g.strip_page);
    }
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

void update_strip()
{
    char text[sizeof(g.strip_text)];
    uint32_t color = t::kText;
    strip_message(text, sizeof(text), color, g.strip_page);
    if (text[0] == 0) {
        set_hidden(g.strip, true);
        g.strip_text[0] = 0;
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
    }
    lv_obj_set_style_text_color(g.strip_label, t::color(color), 0);
    set_hidden(g.strip, false);
}

void on_update(lv_timer_t *)
{
    if (g.zone == nullptr) {
        return;
    }
    // Settings and the first setup have their own frame, the edit mode its bar: no pull-down, no strip
    const bool open = settings::is_open() || setup::active() || dashboard::editing();
    set_hidden(g.zone, open);
    if (open) {
        set_hidden(g.strip, true);
        g.strip_text[0] = 0;
    } else {
        update_strip();
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
