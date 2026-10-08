// The pages of Settings (docs/ui/screen.md section 5). Only one page exists at a time, so each
// page keeps the objects its refresh needs in file-local pointers, set by its builder.

#include "settings_pages.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "keyboard.hpp"
#include "power.hpp"
#include "rotation.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

const Backend &be()
{
    return backend();
}

void *as_data(int value)
{
    return reinterpret_cast<void *>(static_cast<intptr_t>(value));
}

int from_data(lv_event_t *event)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
}

lv_obj_t *text_block(lv_obj_t *parent, const char *text, uint32_t color = t::kTextValue)
{
    lv_obj_t *obj = t::label(parent, text, t::font_normal(), color);
    lv_obj_set_width(obj, lv_pct(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    return obj;
}

lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *obj = t::label(parent, text, t::font_small(), t::kTextLabel);
    lv_obj_set_style_pad_top(obj, 6, 0);
    return obj;
}

lv_obj_t *button_row(lv_obj_t *parent)
{
    lv_obj_t *obj = t::plain(parent);
    lv_obj_set_size(obj, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(obj, 8, 0);
    lv_obj_set_style_pad_row(obj, 8, 0);
    lv_obj_set_style_pad_top(obj, 6, 0);
    return obj;
}

// ------------------------------------------------------------------- display settings

// A card: the name and the value in the first line, the slider over the full width below, so the
// slider stays usable on narrow screens. `on_change` gets the slider; the value label is updated
// here.
lv_obj_t *slider_card(lv_obj_t *parent, const char *name, int32_t min, int32_t max, int32_t value,
                      lv_event_cb_t on_change)
{
    lv_obj_t *card = t::plain(parent);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, t::color(t::kCard), 0);
    lv_obj_set_style_radius(card, t::kRadius, 0);
    lv_obj_set_style_pad_hor(card, 12, 0);
    lv_obj_set_style_pad_top(card, 10, 0);
    lv_obj_set_style_pad_bottom(card, 16, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *head = t::plain(card);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    t::label(head, name, t::font_medium(), t::kText);
    lv_obj_t *value_label = t::label(head, "", t::font_normal(), t::kTextValue);
    lv_label_set_text_fmt(value_label, "%d %%", static_cast<int>(value));

    // the knob reaches past the ends of the track by half its height plus its padding (theme)
    lv_obj_t *track = t::plain(card);
    lv_obj_set_size(track, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(track, 16, 0);
    lv_obj_set_style_pad_ver(track, 6, 0);
    lv_obj_t *slider = t::slider(track, min, max, value);
    lv_obj_set_width(slider, lv_pct(100));
    lv_obj_add_event_cb(
        slider,
        [](lv_event_t *event) {
            lv_label_set_text_fmt(static_cast<lv_obj_t *>(lv_event_get_user_data(event)), "%d %%",
                                  static_cast<int>(lv_slider_get_value(lv_event_get_target(event))));
        },
        LV_EVENT_VALUE_CHANGED, value_label);
    lv_obj_add_event_cb(slider, on_change, LV_EVENT_VALUE_CHANGED, nullptr);
    return slider;
}

DisplaySettings display_settings()
{
    DisplaySettings s{};
    be().display_settings(s);
    return s;
}

void store(const DisplaySettings &s)
{
    be().set_display_settings(s);
    power::settings_changed();
}

void brightness_card(lv_obj_t *parent)
{
    slider_card(parent, "Brightness", 1, 100, display_settings().brightness, [](lv_event_t *event) {
        DisplaySettings s = display_settings();
        s.brightness = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
        store(s);
    });
}

// The idle times the - and + buttons step through: the step grows with the time, like the old
// firmware's buttons, so an hour or a day is a few taps away. 0 is "Never"; -1 ends the list.
constexpr int32_t kDimTimes[] = {0,   10,  15,  20,   30,   45,   60,   90,   120,
                                 180, 300, 600, 900, 1200, 1800, 2700, 3600, -1};
constexpr int32_t kOffTimes[] = {0,    60,   120,  180,   300,   600,   900,   1200,  1800, 2700,
                                 3600, 5400, 7200, 10800, 14400, 21600, 28800, 43200, 86400, -1};

void idle_time_text(int32_t s, char *out, size_t size)
{
    if (s == 0) {
        std::snprintf(out, size, "Never");
    } else if (s < 60) {
        std::snprintf(out, size, "%d s", static_cast<int>(s));
    } else if (s < 3600 && s % 60 != 0) {
        std::snprintf(out, size, "%d min %d s", static_cast<int>(s / 60), static_cast<int>(s % 60));
    } else if (s < 3600) {
        std::snprintf(out, size, "%d min", static_cast<int>(s / 60));
    } else if (s % 3600 != 0) {
        std::snprintf(out, size, "%d h %d min", static_cast<int>(s / 3600), static_cast<int>(s % 3600 / 60));
    } else {
        std::snprintf(out, size, "%d h", static_cast<int>(s / 3600));
    }
}

// A row with "-  value  +"; holding a button repeats. Deleted with its row.
struct Stepper {
    const int32_t *times;
    int index;
    lv_obj_t *value;
    void (*on_change)(int32_t seconds);
    uint32_t last_step = 0; // lv_tick_get()
};

// holding a button steps this often: the indev repeats every 100 ms, too fast to stop on a value
constexpr uint32_t kStepRepeatMs = 250;

void stepper_show(const Stepper &st)
{
    char text[16];
    idle_time_text(st.times[st.index], text, sizeof(text));
    // the row's value label belongs to line_text, which sets its text again on size changes
    t::line_text(st.value, text);
}

void stepper_step(lv_event_t *event)
{
    auto &st = *static_cast<Stepper *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED_REPEAT && lv_tick_elaps(st.last_step) < kStepRepeatMs) {
        return;
    }
    st.last_step = lv_tick_get();
    const bool up = lv_obj_get_index(lv_event_get_target(event)) > lv_obj_get_index(st.value);
    const int next = st.index + (up ? 1 : -1);
    if (next < 0 || st.times[next] < 0) {
        return;
    }
    st.index = next;
    stepper_show(st);
    st.on_change(st.times[next]);
}

lv_obj_t *stepper_button(lv_obj_t *row, const char *symbol, Stepper *st)
{
    lv_obj_t *button = lv_btn_create(row);
    lv_obj_set_size(button, 40, 36);
    lv_obj_set_style_radius(button, t::kRadius, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, t::color(t::kBorder), 0);
    lv_obj_set_style_bg_color(button, t::color(t::kActive), LV_STATE_PRESSED);
    lv_obj_center(t::label(button, symbol, t::font_normal(), t::kText));
    lv_obj_add_event_cb(button, stepper_step, LV_EVENT_SHORT_CLICKED, st);
    lv_obj_add_event_cb(button, stepper_step, LV_EVENT_LONG_PRESSED_REPEAT, st);
    return button;
}

void stepper_row(lv_obj_t *parent, const char *name, const int32_t *times, int32_t value,
                 void (*on_change)(int32_t seconds))
{
    // a stored time between two steps shows the next longer one
    int index = 0;
    while (times[index + 1] >= 0 && times[index] < value) {
        index++;
    }
    lv_obj_t *row = t::row(parent, name, "", false, nullptr);
    auto *st = new Stepper{times, index, t::row_value(row), on_change, 0};
    lv_obj_add_event_cb(
        row, [](lv_event_t *e) { delete static_cast<Stepper *>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, st);
    lv_obj_move_to_index(stepper_button(row, LV_SYMBOL_MINUS, st), 1);
    stepper_button(row, LV_SYMBOL_PLUS, st);
    // a fixed width, so the buttons stay where they are while the text changes
    lv_obj_set_width(st->value, 84);
    lv_obj_set_style_text_align(st->value, LV_TEXT_ALIGN_CENTER, 0);
    stepper_show(*st);
}

// ------------------------------------------------------------------------------ quick page

lv_obj_t *g_quick_wifi = nullptr;
lv_obj_t *g_quick_hotspot = nullptr;
lv_obj_t *g_quick_hotspot_switch = nullptr;
lv_obj_t *g_quick_ha = nullptr;

void wifi_summary(const WifiInfo &w, char *out, size_t size)
{
    if (w.connected) {
        std::snprintf(out, size, "%s", w.ssid);
    } else if (w.error[0] != 0) {
        std::snprintf(out, size, "%s", w.error);
    } else {
        std::snprintf(out, size, "%s", w.state);
    }
}

void quick_refresh()
{
    WifiInfo w{};
    be().wifi_info(w);
    char text[64];
    wifi_summary(w, text, sizeof(text));
    t::line_text(t::row_value(g_quick_wifi), text);
    lv_obj_set_style_text_color(t::row_value(g_quick_wifi),
                                t::color(w.connected || w.error[0] == 0 ? t::kTextValue : t::kAlert), 0);
    t::line_text(t::row_value(g_quick_hotspot), w.hotspot_on ? "On" : "Off");
    HassInfo h{};
    be().hass_info(h);
    t::line_text(t::row_value(g_quick_ha), h.state);
    lv_obj_set_style_text_color(t::row_value(g_quick_ha), t::color(h.failed ? t::kAlert : t::kTextValue), 0);
    if (w.hotspot_on != lv_obj_has_state(g_quick_hotspot_switch, LV_STATE_CHECKED)) {
        if (w.hotspot_on) {
            lv_obj_add_state(g_quick_hotspot_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(g_quick_hotspot_switch, LV_STATE_CHECKED);
        }
    }
}

void on_hotspot_switch(lv_event_t *event)
{
    be().set_hotspot(lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED));
}

Built quick(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    brightness_card(col);
    g_quick_wifi = t::row(col, "Wi-Fi", "", true, [](lv_event_t *) { push(Page::Wifi); });
    g_quick_hotspot = t::row(col, "Hotspot", "", false, [](lv_event_t *) { push(Page::Hotspot); });
    g_quick_hotspot_switch = t::toggle(g_quick_hotspot, false, on_hotspot_switch);
    g_quick_ha = t::row(col, "Home Assistant", "", true, [](lv_event_t *) { push(Page::HomeAssistant); });
    t::row(col, "Display", "", true, [](lv_event_t *) { push(Page::Display); });
    t::row(col, "System", "", true, [](lv_event_t *) { push(Page::System); });
    quick_refresh();
    return {"Settings", quick_refresh};
}

// ------------------------------------------------------------------------------ Wi-Fi page

lv_obj_t *g_wifi_status = nullptr;
int g_wifi_saved_built = -1;
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
    be().wifi_info(w);
    char text[128];
    wifi_status_text(w, text, sizeof(text));
    lv_label_set_text(g_wifi_status, text);
    char saved[kMaxSavedNetworks][kSsidSize];
    const int count = static_cast<int>(be().saved_networks(saved, kMaxSavedNetworks));
    const char *connected = w.connected ? w.ssid : "";
    if (count != g_wifi_saved_built || std::strcmp(connected, g_wifi_connected_built) != 0) {
        reload();
    }
}

Built wifi(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    WifiInfo w{};
    be().wifi_info(w);
    char text[128];
    wifi_status_text(w, text, sizeof(text));
    g_wifi_status = text_block(col, text);

    char saved[kMaxSavedNetworks][kSsidSize];
    const size_t count = be().saved_networks(saved, kMaxSavedNetworks);
    g_wifi_saved_built = static_cast<int>(count);
    std::snprintf(g_wifi_connected_built, sizeof(g_wifi_connected_built), "%s", w.connected ? w.ssid : "");

    char heading[48];
    std::snprintf(heading, sizeof(heading), "Saved networks (%u of %u)", static_cast<unsigned>(count),
                  static_cast<unsigned>(kMaxSavedNetworks));
    section(col, heading);
    for (size_t i = 0; i < count; i++) {
        char name[kSsidSize + 16];
        std::snprintf(name, sizeof(name), "%u  %.32s", static_cast<unsigned>(i + 1), saved[i]);
        const bool in_use = w.connected && std::strcmp(saved[i], w.ssid) == 0;
        t::row(col, name, in_use ? "connected" : "", true,
               [](lv_event_t *event) {
                   const int index = from_data(event);
                   char ssids[kMaxSavedNetworks][kSsidSize];
                   const size_t n = be().saved_networks(ssids, kMaxSavedNetworks);
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
        t::button(buttons, "Reconnect", [](lv_event_t *) { be().reconnect(); });
    }
    return {"Wi-Fi", wifi_refresh};
}

// --------------------------------------------------------------------- saved network page

int g_detail_index = -1;

Built network_detail(lv_obj_t *content, const char *ssid, int index)
{
    g_detail_index = index;
    lv_obj_t *col = t::column(content);
    char text[80];
    std::snprintf(text, sizeof(text), "%.32s\nposition %d in the list", ssid, index + 1);
    text_block(col, text, t::kText);
    lv_obj_t *buttons = button_row(col);
    t::button(buttons, "Move up", [](lv_event_t *) {
        be().move_network(static_cast<uint8_t>(g_detail_index), -1);
        pop();
    });
    t::button(buttons, "Move down", [](lv_event_t *) {
        be().move_network(static_cast<uint8_t>(g_detail_index), 1);
        pop();
    });
    t::button(
        buttons, "Forget",
        [](lv_event_t *) {
            confirm("Forget network?", "The device no longer connects to this network.", "Forget", true,
                    [](void *) {
                        be().remove_network(static_cast<uint8_t>(g_detail_index));
                        pop();
                    },
                    nullptr);
        },
        nullptr, true);
    return {"Saved network", nullptr};
}

// ----------------------------------------------------------------------- add network page

constexpr size_t kMaxScanItems = 20;
int g_scan_built = -1;
// the rows of the page point into g_scan_items; the refresh compares into its own buffer
ScanItem g_scan_items[kMaxScanItems];
ScanItem g_scan_check[kMaxScanItems];

void scan_refresh()
{
    const size_t count = be().scan_results(g_scan_check, kMaxScanItems);
    if (static_cast<int>(count) != g_scan_built ||
        std::memcmp(g_scan_check, g_scan_items, count * sizeof(ScanItem)) != 0) {
        reload();
    }
}

Built add_network(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    lv_obj_t *buttons = button_row(col);
    t::button(buttons, LV_SYMBOL_REFRESH " Rescan", [](lv_event_t *) { be().scan(); });

    ScanItem *items = g_scan_items;
    std::memset(g_scan_items, 0, sizeof(g_scan_items));
    const size_t count = be().scan_results(items, kMaxScanItems);
    if (g_scan_built < 0) {
        be().scan(); // first visit: fresh results
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

// --------------------------------------------------------------------------- password page

lv_obj_t *g_name_area = nullptr;
lv_obj_t *g_password_area = nullptr;
lv_obj_t *g_keyboard = nullptr;
char g_password_ssid[kSsidSize] = {};

void focus_area(lv_obj_t *area)
{
    lv_obj_t *const areas[] = {g_name_area, g_password_area};
    for (lv_obj_t *other : areas) {
        if (other != nullptr && other != area) {
            lv_obj_clear_state(other, LV_STATE_FOCUSED);
        }
    }
    lv_obj_add_state(area, LV_STATE_FOCUSED);
    lv_event_send(area, LV_EVENT_FOCUSED, nullptr); // starts the cursor blinking
    lv_keyboard_set_textarea(g_keyboard, area);
}

lv_obj_t *text_area(lv_obj_t *parent, const char *placeholder, bool password)
{
    lv_obj_t *area = lv_textarea_create(parent);
    lv_textarea_set_one_line(area, true);
    lv_textarea_set_password_mode(area, password);
    lv_textarea_set_placeholder_text(area, placeholder);
    lv_obj_set_flex_grow(area, 1);
    lv_obj_set_style_bg_color(area, t::color(t::kCard), 0);
    lv_obj_set_style_text_color(area, t::color(t::kText), 0);
    lv_obj_set_style_border_color(area, t::color(t::kBorder), 0);
    lv_obj_set_style_border_color(area, t::color(t::kActive), LV_STATE_FOCUSED);
    // the default theme draws the cursor in its dark text color, invisible on the dark field
    constexpr lv_style_selector_t kCursor = static_cast<lv_style_selector_t>(LV_PART_CURSOR) | LV_STATE_FOCUSED;
    lv_obj_set_style_border_color(area, t::color(t::kActive), kCursor);
    lv_obj_set_style_border_width(area, 2, kCursor);
    lv_obj_set_style_border_side(area, LV_BORDER_SIDE_LEFT, kCursor);
    lv_obj_add_event_cb(area, [](lv_event_t *event) { focus_area(lv_event_get_target(event)); }, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_add_event_cb(area, [](lv_event_t *) { set_form_dirty(true); }, LV_EVENT_VALUE_CHANGED, nullptr);
    return area;
}

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
    be().add_network(ssid, password);
    set_form_dirty(false);
    pop(2); // back to the Wi-Fi page
}

Built password(lv_obj_t *content, const char *ssid)
{
    std::snprintf(g_password_ssid, sizeof(g_password_ssid), "%s", ssid);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *form = t::plain(content);
    lv_obj_set_width(form, lv_pct(100));
    lv_obj_set_flex_grow(form, 1);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(form, t::kPad, 0);
    lv_obj_set_style_pad_row(form, 6, 0);
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
    lv_obj_set_style_min_width(eye, t::kRowHeight, 0);
    t::button(line, "Connect", on_connect);

    g_keyboard = keyboard::create(content);
    lv_obj_set_size(g_keyboard, lv_pct(100), lv_pct(55));
    focus_area(g_name_area != nullptr ? g_name_area : g_password_area);
    // OK and Enter connect (or go from the name to the password); leaving the page is `<`, which
    // asks before it drops the input
    lv_obj_add_event_cb(g_keyboard, on_connect, LV_EVENT_READY, nullptr);
    return {ssid[0] != 0 ? ssid : "Other network", nullptr};
}

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
    be().wifi_info(w);
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

Built hotspot(lv_obj_t *content)
{
    WifiInfo w{};
    be().wifi_info(w);
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
                "New password", false, [](void *) { be().new_hotspot_password(); }, nullptr);
    });

    char qr[128];
    t::wifi_qr_text(qr, sizeof(qr), w.hotspot_ssid, w.hotspot_password);
    t::qr_card(wrap, qr, 140);
    return {"Hotspot", hotspot_refresh};
}

// ------------------------------------------------------------------- Home Assistant page

// what the page shows; it is built again when this changes
char g_ha_built[256] = {};
lv_obj_t *g_ha_retry = nullptr;

void ha_key(const HassInfo &h, char *out, size_t size)
{
    std::snprintf(out, size, "%d|%.24s|%.60s|%.20s|%.80s", h.configured, h.state, h.url, h.version, h.error);
}

void ha_retry_text(const HassInfo &h, char *out, size_t size)
{
    if (h.retry_in_s > 0) {
        std::snprintf(out, size, "Next try in %lu s", static_cast<unsigned long>(h.retry_in_s));
    } else {
        out[0] = 0;
    }
}

void ha_refresh()
{
    HassInfo h{};
    be().hass_info(h);
    char key[sizeof(g_ha_built)];
    ha_key(h, key, sizeof(key));
    if (std::strcmp(key, g_ha_built) != 0) {
        reload();
        return;
    }
    if (g_ha_retry != nullptr) {
        char text[32];
        ha_retry_text(h, text, sizeof(text));
        lv_label_set_text(g_ha_retry, text);
        if (text[0] == 0) {
            lv_obj_add_flag(g_ha_retry, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(g_ha_retry, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

Built home_assistant(lv_obj_t *content)
{
    HassInfo h{};
    be().hass_info(h);
    ha_key(h, g_ha_built, sizeof(g_ha_built));

    lv_obj_t *wrap = t::split(content);
    lv_obj_t *info = lv_obj_get_child(wrap, 0);
    if (h.configured) {
        text_block(info, h.state, h.failed ? t::kAlert : t::kText);
        if (h.error[0] != 0) {
            text_block(info, h.error, t::kAlert);
        }
        char text[32];
        ha_retry_text(h, text, sizeof(text));
        g_ha_retry = text_block(info, text);
        if (text[0] == 0) {
            lv_obj_add_flag(g_ha_retry, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_pad_bottom(g_ha_retry, 4, 0);
        t::caption_value(info, "Address", h.url);
        if (h.version[0] != 0) {
            t::caption_value(info, "Version", h.version);
        }
        if (!h.connected) {
            t::button(button_row(info), "Try now", [](lv_event_t *) { be().hass_reconnect(); });
        }
    } else {
        text_block(info, "Not set up yet.", t::kText);
        g_ha_retry = nullptr;
    }

    char address[64];
    be().web_address(address, sizeof(address));
    if (address[0] == 0) {
        text_block(info, "Address and token are entered in the web UI. Connect the device to Wi-Fi first.");
    } else {
        text_block(info, "Address and token are entered in the web UI. Scan the code or open:");
        text_block(info, address, t::kText);
        t::qr_card(wrap, address, 140);
    }
    return {"Home Assistant", ha_refresh};
}

// ---------------------------------------------------------------------------- display page

lv_obj_t *g_auto_row = nullptr;
lv_obj_t *g_rotation_row = nullptr;

void display_refresh()
{
    float lux = 0.0f;
    if (g_auto_row != nullptr && be().ambient_lux(lux)) {
        char text[24];
        std::snprintf(text, sizeof(text), "%ld lx, %u %%", std::lround(lux), power::backlight());
        t::line_text(t::row_value(g_auto_row), text);
    }
    if (g_rotation_row != nullptr) {
        // the name says the rotation, the value what a tap does
        char text[32];
        std::snprintf(text, sizeof(text), "Rotation %u\xc2\xb0", rotation::degrees());
        t::line_text(lv_obj_get_child(g_rotation_row, 0), text);
        t::line_text(t::row_value(g_rotation_row),
                     rotation::has_sensor() && !rotation::locked() ? "Lock" : "Rotate");
    }
}

// the rotation popup: Rotate turns the screen clockwise; with a sensor, "Use the sensor" hands
// the rotation back to it. It closes 5 s after the last tap, or with a tap outside.
lv_obj_t *g_popup_degrees = nullptr;

void popup_degrees_text()
{
    if (g_popup_degrees != nullptr) {
        lv_label_set_text_fmt(g_popup_degrees, "%u\xc2\xb0", rotation::degrees());
    }
}

void open_rotation_popup()
{
    lv_obj_t *card = settings::popup("Rotation", 5000, [] {
        g_popup_degrees = nullptr;
        reload();
    });
    // the degrees under the title, the square Rotate button at the exact center of the card, the
    // sensor button at the bottom
    g_popup_degrees = t::label(card, "", t::font_normal(), t::kTextValue);
    lv_obj_align(g_popup_degrees, LV_ALIGN_TOP_MID, 0, 26);
    popup_degrees_text();
    lv_obj_t *rotate = t::button(card, "", [](lv_event_t *) {
        rotation::rotate_clockwise();
        settings::popup_touch();
        popup_degrees_text();
    });
    lv_obj_set_size(rotate, 72, 72);
    lv_obj_set_style_min_width(rotate, 0, 0);
    lv_obj_set_style_text_align(lv_obj_get_child(rotate, 0), LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lv_obj_get_child(rotate, 0), LV_SYMBOL_REFRESH "\nRotate");
    lv_obj_align(rotate, LV_ALIGN_CENTER, 0, 0);
    if (rotation::has_sensor()) {
        lv_obj_t *sensor = t::button(card, "Use the sensor", [](lv_event_t *) {
            rotation::set_locked(false);
            settings::close_popup();
        });
        lv_obj_set_height(sensor, 32);
        lv_obj_align(sensor, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
}

Built display(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    const DisplaySettings s = display_settings();
    brightness_card(col);
    // only with a light sensor
    float lux = 0.0f;
    g_auto_row = nullptr;
    g_rotation_row = nullptr;
    if (be().ambient_lux(lux)) {
        g_auto_row = t::row(col, "Automatic", "", false, nullptr);
        t::toggle(g_auto_row, s.auto_brightness, [](lv_event_t *event) {
            DisplaySettings changed = display_settings();
            changed.auto_brightness = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
            store(changed);
        });
        text_block(col, "Automatic: the brightness follows the room light, the slider sets how bright it gets in "
                        "bright light.");
    }
    stepper_row(col, "Dim after", kDimTimes, s.dim_after_s, [](int32_t seconds) {
        DisplaySettings changed = display_settings();
        changed.dim_after_s = static_cast<uint16_t>(seconds);
        store(changed);
    });
    lv_obj_t *dim = slider_card(col, "Dimmed brightness", 1, 100, s.dim_brightness, [](lv_event_t *event) {
        DisplaySettings changed = display_settings();
        changed.dim_brightness = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
        be().set_display_settings(changed);
        power::preview(changed.dim_brightness); // shows the dimmed level while it is dragged
    });
    lv_obj_add_event_cb(dim, [](lv_event_t *) { power::settings_changed(); }, LV_EVENT_RELEASED, nullptr);
    stepper_row(col, "Screen off", kOffTimes, static_cast<int32_t>(s.off_after_s), [](int32_t seconds) {
        DisplaySettings changed = display_settings();
        changed.off_after_s = static_cast<uint32_t>(seconds);
        store(changed);
    });
    text_block(col, "A touch wakes the screen; when it was off, that touch only wakes it.");

    // Rotation: one row. With the sensor in charge it locks the rotation; locked or without a
    // sensor it opens the popup that turns the screen.
    g_rotation_row = t::row(col, "Rotation", "", true, [](lv_event_t *) {
        if (rotation::has_sensor() && !rotation::locked()) {
            rotation::set_locked(true);
            reload();
        } else {
            open_rotation_popup();
        }
    });
    display_refresh();
    DeviceInfo device{};
    be().device_info(device);
    char text[64];
    std::snprintf(text, sizeof(text), "%s, %d x %d", device.board, static_cast<int>(lv_disp_get_hor_res(nullptr)),
                  static_cast<int>(lv_disp_get_ver_res(nullptr)));
    text_block(col, text);
    return {"Display", display_refresh};
}

// ----------------------------------------------------------------------------- system page

lv_obj_t *g_system_uptime = nullptr;
lv_obj_t *g_system_memory = nullptr;

void system_refresh()
{
    DeviceInfo device{};
    be().device_info(device);
    char text[48];
    std::snprintf(text, sizeof(text), "%lu min", static_cast<unsigned long>(device.uptime_s / 60));
    t::line_text(t::row_value(g_system_uptime), text);
    std::snprintf(text, sizeof(text), "%lu KB + %lu KB PSRAM", static_cast<unsigned long>(device.heap_internal_kb),
                  static_cast<unsigned long>(device.heap_psram_kb));
    t::line_text(t::row_value(g_system_memory), text);
}

Built system(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    DeviceInfo device{};
    be().device_info(device);
    t::row(col, "Device", device.name, false, nullptr);
    t::row(col, "Board", device.board, false, nullptr);
    t::row(col, "Firmware", device.firmware, false, nullptr);
    g_system_uptime = t::row(col, "Uptime", "", false, nullptr);
    g_system_memory = t::row(col, "Free memory", "", false, nullptr);
    system_refresh();

    lv_obj_t *buttons = button_row(col);
    t::button(buttons, "Reset web UI login", [](lv_event_t *) {
        confirm("Reset the web UI login?", "The web UI is open until a new login is created.", "Reset", true,
                [](void *) { be().reset_web_login(); }, nullptr);
    });
    t::button(buttons, "Restart", [](lv_event_t *) {
        confirm("Restart?", "The device restarts now.", "Restart", false, [](void *) { be().restart(); }, nullptr);
    });
    t::button(
        buttons, "Factory reset",
        [](lv_event_t *) {
            confirm("Factory reset?", "All settings, networks and the web UI login are deleted.", "Reset", true,
                    [](void *) { be().factory_reset(); }, nullptr);
        },
        nullptr, true);
    return {"System", system_refresh};
}

} // namespace

Built build(Page page, lv_obj_t *content, const char *text, int index)
{
    // a page that is left forgets its list state
    if (page != Page::AddNetwork) {
        g_scan_built = -1;
    }
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    switch (page) {
    case Page::Quick:
        return quick(content);
    case Page::Wifi:
        return wifi(content);
    case Page::NetworkDetail:
        return network_detail(content, text, index);
    case Page::AddNetwork:
        return add_network(content);
    case Page::Password:
        return password(content, text);
    case Page::Hotspot:
        return hotspot(content);
    case Page::HomeAssistant:
        return home_assistant(content);
    case Page::Display:
        return display(content);
    case Page::System:
        return system(content);
    }
    return {"Settings", nullptr};
}

} // namespace hc::ui::settings::pages
