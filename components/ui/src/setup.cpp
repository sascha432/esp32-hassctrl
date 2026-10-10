#include "setup.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "settings.hpp"
#include "theme.hpp"

namespace hc::ui::setup {

namespace {

namespace t = theme;

constexpr uint32_t kTickMs = 500;
constexpr uint32_t kAnimationMs = 200;
// the hotspot's address, where the captive portal sends a phone
constexpr const char *kHotspotAddress = "http://192.168.4.1";

enum class View : uint8_t {
    None,
    Welcome,
    Result,
};

struct Context {
    const Backend *backend = nullptr;
    lv_obj_t *dashboard = nullptr;
    lv_obj_t *screen = nullptr;
    lv_obj_t *content = nullptr;
    lv_obj_t *hotspot_icon = nullptr;
    View view = View::None;
    // the decision at boot (no network saved: setup) is made once
    bool decided = false;
    // saved networks when the setup opened: one more means a network was added
    size_t networks_at_start = 0;
    // what the views show now, so they are built again only when it changes
    char welcome_password[12] = {};
    bool welcome_hotspot = false;
    char result_key[128] = {};
};

Context g;
std::atomic<bool> g_requested{false};

const Backend &be()
{
    return *g.backend;
}

size_t saved_count()
{
    char ssids[kMaxSavedNetworks][kSsidSize];
    return be().saved_networks(ssids, kMaxSavedNetworks, nullptr);
}

lv_obj_t *text(lv_obj_t *parent, const char *value, const lv_font_t &font = t::font_normal(),
               uint32_t color = t::kTextValue)
{
    lv_obj_t *label = t::label(parent, value, font, color);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

void info_line(lv_obj_t *parent, const char *name, const char *value)
{
    lv_obj_t *line = t::plain(parent);
    lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_t *label = t::label(line, name, t::font_normal(), t::kTextLabel);
    lv_obj_set_width(label, 80);
    t::label(line, value, t::font_normal(), t::kText);
}

lv_obj_t *buttons(lv_obj_t *parent)
{
    lv_obj_t *row = t::plain(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_pad_row(row, 8, 0);
    lv_obj_set_style_pad_top(row, 8, 0);
    return row;
}

void choose_wifi(lv_event_t *)
{
    settings::open_alone(settings::Page::AddNetwork, g.screen);
}

// ------------------------------------------------------------------------------- welcome

void show_welcome(const WifiInfo &w)
{
    g.view = View::Welcome;
    std::snprintf(g.welcome_password, sizeof(g.welcome_password), "%s", w.hotspot_password);
    g.welcome_hotspot = w.hotspot_on;
    lv_obj_clean(g.content);

    lv_obj_t *wrap = t::split(g.content);
    lv_obj_t *screen_path = lv_obj_get_child(wrap, 0);
    text(screen_path, "Set up on this screen", t::font_medium(), t::kText);
    text(screen_path, "Choose your Wi-Fi network and enter its password.");
    t::button(buttons(screen_path), "Choose Wi-Fi", choose_wifi);

    // the phone path: join the hotspot, the captive portal opens the setup page
    lv_obj_t *phone = t::plain(wrap);
    lv_obj_set_height(phone, LV_SIZE_CONTENT);
    // half the width beside the screen path, the full width below it (portrait)
    lv_obj_add_event_cb(
        wrap,
        [](lv_event_t *event) {
            lv_obj_t *box = lv_event_get_target(event);
            const bool wide = lv_obj_get_content_width(box) > lv_obj_get_content_height(box);
            lv_obj_set_width(lv_obj_get_child(box, 1), wide ? lv_pct(48) : lv_pct(100));
        },
        LV_EVENT_SIZE_CHANGED, nullptr);
    lv_obj_set_flex_flow(phone, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(phone, 6, 0);
    text(phone, "With your phone", t::font_medium(), t::kText);
    char qr[128];
    t::wifi_qr_text(qr, sizeof(qr), w.hotspot_ssid, w.hotspot_password);
    t::qr_card(phone, qr, 120);
    text(phone, "1. Scan to join the hotspot");
    t::caption_value(phone, "Hotspot", w.hotspot_ssid, t::font_medium());
    t::caption_value(phone, "Password", w.hotspot_password, t::font_medium());
    text(phone, "2. The setup page opens by itself, or open");
    text(phone, kHotspotAddress, t::font_normal(), t::kText);
    if (!w.hotspot_on) {
        text(phone, "The hotspot is off now: Settings, Hotspot.", t::font_normal(), t::kAlert);
    }
    lv_event_send(wrap, LV_EVENT_SIZE_CHANGED, nullptr);
}

// -------------------------------------------------------------------------------- result

void finish(lv_event_t *)
{
    g.view = View::None;
    // the setup screen is deleted when the animation is done
    lv_scr_load_anim(g.dashboard, LV_SCR_LOAD_ANIM_FADE_ON, kAnimationMs, 0, true);
    g.screen = nullptr;
    g.content = nullptr;
}

void show_result(const WifiInfo &w)
{
    // connected, connecting or failed, and the details: built again only when one changes
    char key[sizeof(g.result_key)];
    const bool failed = !w.connected && !w.connecting && w.error[0] != 0;
    std::snprintf(key, sizeof(key), "%d|%d|%.32s|%s|%.40s", w.connected, failed, w.ssid, w.ip, failed ? w.error : "");
    if (g.view == View::Result && std::strcmp(key, g.result_key) == 0) {
        return;
    }
    std::snprintf(g.result_key, sizeof(g.result_key), "%s", key);
    g.view = View::Result;
    lv_obj_clean(g.content);

    lv_obj_t *col = t::column(g.content);
    lv_obj_set_style_pad_all(col, 16, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    char line[96];
    if (w.connected) {
        std::snprintf(line, sizeof(line), "Connected to %s", w.ssid);
        text(col, line, t::font_large(), t::kText);
        DeviceInfo device{};
        be().device_info(device);
        char name[40];
        std::snprintf(name, sizeof(name), "%s.local", device.name);
        info_line(col, "Address", w.ip);
        info_line(col, "Name", name);
        text(col, "The web UI is at this address. It also takes the Home Assistant connection.");
        lv_obj_t *row = buttons(col);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        t::button(row, "Next", finish);
    } else if (failed) {
        std::snprintf(line, sizeof(line), "Could not connect to %s", w.ssid[0] != 0 ? w.ssid : "the network");
        text(col, line, t::font_large(), t::kText);
        text(col, w.error, t::font_normal(), t::kAlert);
        lv_obj_t *row = buttons(col);
        t::button(row, "Other network", choose_wifi);
        t::button(row, "Try again", [](lv_event_t *) {
            be().reconnect();
            g.result_key[0] = 0; // shows "Connecting" at once
        });
    } else {
        std::snprintf(line, sizeof(line), "Connecting to %s ...", w.ssid[0] != 0 ? w.ssid : "the network");
        text(col, line, t::font_large(), t::kText);
        lv_obj_t *spinner = lv_spinner_create(col, 1000, 60);
        lv_obj_set_size(spinner, 48, 48);
        lv_obj_set_style_arc_color(spinner, t::color(t::kActive), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(spinner, t::color(t::kBorder), LV_PART_MAIN);
    }
}

// --------------------------------------------------------------------------------- frame

void build_screen()
{
    g.screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(g.screen);
    lv_obj_set_style_bg_opa(g.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.screen, t::color(t::kBackground), 0);
    lv_obj_set_style_text_color(g.screen, t::color(t::kText), 0);
    lv_obj_set_flex_flow(g.screen, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *bar = t::plain(g.screen);
    lv_obj_set_size(bar, lv_pct(100), t::kStatusBarHeight);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, t::color(t::kBar), 0);
    lv_obj_set_style_pad_hor(bar, t::kPad, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *title = t::label(bar, "Setup", t::font_normal(), t::kText);
    lv_obj_set_flex_grow(title, 1);
    g.hotspot_icon = t::label(bar, "Hotspot", t::font_small(), t::kActive);

    g.content = t::plain(g.screen);
    lv_obj_set_width(g.content, lv_pct(100));
    lv_obj_set_flex_grow(g.content, 1);
}

void show()
{
    WifiInfo w{};
    be().wifi_info(w);
    g.networks_at_start = saved_count();
    g.result_key[0] = 0;
    if (g.screen == nullptr) {
        build_screen();
        lv_scr_load_anim(g.screen, LV_SCR_LOAD_ANIM_FADE_ON, kAnimationMs, 0, false);
    }
    show_welcome(w);
}

void on_tick(lv_timer_t *)
{
    if (g_requested.exchange(false) && !settings::is_open()) {
        show();
    }
    WifiInfo w{};
    be().wifi_info(w);
    if (!g.decided && w.ready) {
        g.decided = true;
        if (!w.has_networks) {
            show();
        }
    }
    if (g.view == View::None) {
        return;
    }
    if (w.hotspot_on) {
        lv_obj_clear_flag(g.hotspot_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g.hotspot_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (g.view == View::Welcome) {
        // a network was saved, on this screen or with the phone: follow the connection
        if (saved_count() > g.networks_at_start) {
            show_result(w);
        } else if (std::strcmp(w.hotspot_password, g.welcome_password) != 0 || w.hotspot_on != g.welcome_hotspot) {
            show_welcome(w);
        }
    } else {
        show_result(w);
    }
}

} // namespace

void init(const Backend &backend, lv_obj_t *dashboard)
{
    g = {};
    g.backend = &backend;
    g.dashboard = dashboard;
    lv_timer_create(on_tick, kTickMs, nullptr);
}

void request()
{
    g_requested = true;
}

bool active()
{
    return g.view != View::None;
}

} // namespace hc::ui::setup
