// Settings, Wi-Fi and Hotspot (docs/ui/lvgl/lvgl-settings.md section 3): the saved networks, a
// saved network, adding one from the scan, its password, and the hotspot with its QR code.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "keyboard.hpp"
#include "settings.hpp"
#include "settings_pages.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

// ------------------------------------------------------------------------------ Wi-Fi page

lv_obj_t *g_wifi_status = nullptr;
int g_wifi_saved_built = -1;
uint8_t g_wifi_needed_built = 0;
char g_wifi_connected_built[kSsidSize] = {};

void wifi_status_text(const WifiInfo &w, char *out, size_t size)
{
    if (w.connected) {
        std::snprintf(out, size, "Connected to %s\n%s, %d dBm", w.ssid, w.ip, w.rssi);
    } else if (!w.has_networks) {
        std::snprintf(out, size, "No saved network");
    } else {
        std::snprintf(out, size, "%s%s%s%s%s", w.state, w.ssid[0] != 0 ? " " : "", w.ssid, w.error[0] != 0 ? "\n" : "",
                      w.error);
    }
}

void wifi_refresh()
{
    WifiInfo w{};
    backend().wifi_info(w);
    char text[128];
    wifi_status_text(w, text, sizeof(text));
    lv_label_set_text(g_wifi_status, text);
    char saved[kMaxSavedNetworks][kSsidSize];
    uint8_t needed = 0;
    const int count = static_cast<int>(backend().saved_networks(saved, kMaxSavedNetworks, &needed));
    const char *connected = w.connected ? w.ssid : "";
    if (count != g_wifi_saved_built || needed != g_wifi_needed_built ||
        std::strcmp(connected, g_wifi_connected_built) != 0) {
        reload();
    }
}

} // namespace

Built wifi_page(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    WifiInfo w{};
    backend().wifi_info(w);
    char text[128];
    wifi_status_text(w, text, sizeof(text));
    g_wifi_status = text_block(col, text);

    char saved[kMaxSavedNetworks][kSsidSize];
    uint8_t needed = 0;
    const size_t count = backend().saved_networks(saved, kMaxSavedNetworks, &needed);
    g_wifi_saved_built = static_cast<int>(count);
    g_wifi_needed_built = needed;
    std::snprintf(g_wifi_connected_built, sizeof(g_wifi_connected_built), "%s", w.connected ? w.ssid : "");

    char heading[48];
    std::snprintf(heading, sizeof(heading), "Saved networks (%u of %u)", static_cast<unsigned>(count),
                  static_cast<unsigned>(kMaxSavedNetworks));
    section(col, heading);
    for (size_t i = 0; i < count; i++) {
        char name[kSsidSize + 16];
        std::snprintf(name, sizeof(name), "%u  %.32s", static_cast<unsigned>(i + 1), saved[i]);
        const bool in_use = w.connected && std::strcmp(saved[i], w.ssid) == 0;
        const char *value = in_use ? "connected" : (needed & (1u << i)) ? "password needed" : "";
        t::row(col, name, value, true,
               [](lv_event_t *event) {
                   const int index = from_data(event);
                   char ssids[kMaxSavedNetworks][kSsidSize];
                   const size_t n = backend().saved_networks(ssids, kMaxSavedNetworks, nullptr);
                   if (index < static_cast<int>(n)) {
                       push(Page::NetworkDetail, ssids[index], index);
                   }
               },
               as_data(static_cast<int>(i)));
    }

    lv_obj_t *buttons = button_row(col);
    if (count < kMaxSavedNetworks) {
        t::button(buttons, "Add network", [](lv_event_t *) { push(Page::AddNetwork); });
    }
    if (count > 0) {
        t::button(buttons, "Reconnect", [](lv_event_t *) { backend().reconnect(); });
    }
    return {"Wi-Fi", wifi_refresh};
}

namespace {

// --------------------------------------------------------------------- saved network page

int g_detail_index = -1;

} // namespace

Built network_detail_page(lv_obj_t *content, const char *ssid, int index)
{
    g_detail_index = index;
    char ssids[kMaxSavedNetworks][kSsidSize];
    uint8_t needed = 0;
    const size_t count = backend().saved_networks(ssids, kMaxSavedNetworks, &needed);
    const bool password_needed = index >= 0 && static_cast<size_t>(index) < count && (needed & (1u << index));

    lv_obj_t *col = t::column(content);
    char text[160];
    std::snprintf(text, sizeof(text), "%.32s\nposition %d in the list%s", ssid, index + 1,
                  password_needed ? "\nPassword needed: it came from a backup without its password and is not "
                                    "tried until it has one."
                                  : "");
    text_block(col, text, t::kText);
    if (password_needed) {
        static char ssid_copy[kSsidSize];
        std::snprintf(ssid_copy, sizeof(ssid_copy), "%s", ssid);
        t::button(button_row(col), "Enter password", [](lv_event_t *) { push(Page::Password, ssid_copy); });
    }
    lv_obj_t *buttons = button_row(col);
    t::button(buttons, "Move up", [](lv_event_t *) {
        backend().move_network(static_cast<uint8_t>(g_detail_index), -1);
        pop();
    });
    t::button(buttons, "Move down", [](lv_event_t *) {
        backend().move_network(static_cast<uint8_t>(g_detail_index), 1);
        pop();
    });
    t::button(
        buttons, "Forget",
        [](lv_event_t *) {
            confirm("Forget network?", "The device no longer connects to this network.", "Forget", true,
                    [](void *) {
                        backend().remove_network(static_cast<uint8_t>(g_detail_index));
                        pop();
                    },
                    nullptr);
        },
        nullptr, true);
    return {"Saved network", nullptr};
}

namespace {

// ----------------------------------------------------------------------- add network page

constexpr size_t kMaxScanItems = 20;
int g_scan_built = -1;
// the rows of the page point into g_scan_items; the refresh compares into its own buffer
ScanItem g_scan_items[kMaxScanItems];
ScanItem g_scan_check[kMaxScanItems];

void scan_refresh()
{
    const size_t count = backend().scan_results(g_scan_check, kMaxScanItems);
    if (static_cast<int>(count) != g_scan_built ||
        std::memcmp(g_scan_check, g_scan_items, count * sizeof(ScanItem)) != 0) {
        reload();
    }
}

} // namespace

Built add_network_page(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    lv_obj_t *buttons = button_row(col);
    t::button(buttons, LV_SYMBOL_REFRESH " Rescan", [](lv_event_t *) { backend().scan(); });

    ScanItem *items = g_scan_items;
    std::memset(g_scan_items, 0, sizeof(g_scan_items));
    const size_t count = backend().scan_results(items, kMaxScanItems);
    if (g_scan_built < 0) {
        backend().scan(); // first visit: fresh results
    }
    g_scan_built = static_cast<int>(count);
    for (size_t i = 0; i < count; i++) {
        char value[32];
        std::snprintf(value, sizeof(value), "%s%d dBm%s", items[i].saved ? "saved, " : "", items[i].rssi,
                      items[i].secure ? "" : ", open");
        t::row(col, items[i].ssid, value, true,
               [](lv_event_t *event) { push(Page::Password, g_scan_items[from_data(event)].ssid); },
               as_data(static_cast<int>(i)));
    }
    t::row(col, "Other network ...", "", true, [](lv_event_t *) { push(Page::Password, ""); });
    return {"Add network", scan_refresh};
}

void forget_scan()
{
    g_scan_built = -1;
}

namespace {

// --------------------------------------------------------------------------- password page

lv_obj_t *g_name_area = nullptr;
lv_obj_t *g_password_area = nullptr;
lv_obj_t *g_keyboard = nullptr;
char g_password_ssid[kSsidSize] = {};

void on_connect(lv_event_t *)
{
    // OK or Enter in the name field goes on to the password
    if (g_name_area != nullptr && lv_keyboard_get_textarea(g_keyboard) == g_name_area) {
        focus_area(g_password_area);
        return;
    }
    const char *ssid = g_name_area != nullptr ? lv_textarea_get_text(g_name_area) : g_password_ssid;
    if (ssid[0] == 0) {
        return;
    }
    const char *password = lv_textarea_get_text(g_password_area);
    const size_t length = std::strlen(password);
    if (length > 0 && length < kMinPassword) {
        // too short for WPA: the network would never connect
        focus_area(g_password_area);
        return;
    }
    backend().add_network(ssid, password);
    set_form_dirty(false);
    pop(2); // back to the Wi-Fi page
}

} // namespace

Built password_page(lv_obj_t *content, const char *ssid)
{
    std::snprintf(g_password_ssid, sizeof(g_password_ssid), "%s", ssid);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *form = t::plain(content);
    lv_obj_set_width(form, lv_pct(100));
    lv_obj_set_flex_grow(form, 1);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(form, t::pad(), 0);
    lv_obj_set_style_pad_row(form, t::dim(6), 0);
    lv_obj_add_flag(form, LV_OBJ_FLAG_SCROLLABLE);

    g_name_area = nullptr;
    if (ssid[0] == 0) {
        lv_obj_t *name_row = button_row(form);
        lv_obj_set_flex_flow(name_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_top(name_row, 0, 0);
        g_name_area = text_area(name_row, "network name", false);
    }
    lv_obj_t *line = button_row(form);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(line, 0, 0);
    g_password_area = text_area(line, "password", true);
    lv_obj_t *eye = t::button(line, LV_SYMBOL_EYE_OPEN, [](lv_event_t *) {
        lv_textarea_set_password_mode(g_password_area, !lv_textarea_get_password_mode(g_password_area));
    });
    lv_obj_set_style_min_width(eye, t::row_height(), 0);
    t::button(line, "Connect", on_connect);

    g_keyboard = keyboard::create(content);
    lv_obj_set_size(g_keyboard, lv_pct(100), lv_pct(55));
    set_form(g_keyboard, g_name_area, g_password_area, on_connect);
    focus_area(g_name_area != nullptr ? g_name_area : g_password_area);
    // OK and Enter connect (or go from the name to the password, set_form); leaving the page is
    // `<`, which asks before it drops the input
    return {ssid[0] != 0 ? ssid : "Other network", nullptr};
}

namespace {

// ---------------------------------------------------------------------------- hotspot page

lv_obj_t *g_hotspot_switch = nullptr;
lv_obj_t *g_hotspot_state = nullptr;
char g_hotspot_password_built[12] = {};

void hotspot_state_text(const WifiInfo &w, char *out, size_t size)
{
    if (!w.hotspot_on) {
        std::snprintf(out, size, "Off");
    } else if (w.hotspot_off_in_s != 0) {
        std::snprintf(out, size, "%u connected, turns off in %lu min", w.hotspot_clients,
                      static_cast<unsigned long>((w.hotspot_off_in_s + 59) / 60));
    } else {
        std::snprintf(out, size, "%u connected", w.hotspot_clients);
    }
}

void hotspot_refresh()
{
    WifiInfo w{};
    backend().wifi_info(w);
    if (std::strcmp(w.hotspot_password, g_hotspot_password_built) != 0) {
        reload();
        return;
    }
    char text[64];
    hotspot_state_text(w, text, sizeof(text));
    lv_label_set_text(g_hotspot_state, text);
    if (w.hotspot_on != lv_obj_has_state(g_hotspot_switch, LV_STATE_CHECKED)) {
        if (w.hotspot_on) {
            lv_obj_add_state(g_hotspot_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(g_hotspot_switch, LV_STATE_CHECKED);
        }
    }
}

} // namespace

Built hotspot_page(lv_obj_t *content)
{
    WifiInfo w{};
    backend().wifi_info(w);
    std::snprintf(g_hotspot_password_built, sizeof(g_hotspot_password_built), "%s", w.hotspot_password);

    lv_obj_t *wrap = t::split(content);
    lv_obj_t *info = lv_obj_get_child(wrap, 0);

    lv_obj_t *row = t::row(info, "Hotspot", "", false, nullptr);
    g_hotspot_switch = t::toggle(row, w.hotspot_on, on_hotspot_switch);
    // the name and the password are typed on a phone: never cut, also in a narrow column
    t::caption_value(info, "Name", w.hotspot_ssid);
    t::caption_value(info, "Password", w.hotspot_password);
    char text[64];
    hotspot_state_text(w, text, sizeof(text));
    g_hotspot_state = text_block(info, text);
    lv_obj_t *buttons = button_row(info);
    t::button(buttons, "New password", [](lv_event_t *) {
        confirm("New hotspot password?", "Devices connected to the hotspot have to join again with the new password.",
                "New password", false, [](void *) { backend().new_hotspot_password(); }, nullptr);
    });

    char qr[128];
    t::wifi_qr_text(qr, sizeof(qr), w.hotspot_ssid, w.hotspot_password);
    t::qr_card(wrap, qr, t::dim(140));
    return {"Hotspot", hotspot_refresh};
}

} // namespace hc::ui::settings::pages
