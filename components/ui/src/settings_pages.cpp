// The pages of Settings (docs/ui/lvgl/lvgl-settings.md section 3): quick settings, Home Assistant,
// System and what the page files share. Only one page exists at a time, so each page keeps the
// objects its refresh needs in file-local pointers, set by its builder. Wi-Fi and Hotspot are in
// settings_wifi.cpp, Display in settings_display.cpp.

#include "settings_pages.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "alarm.hpp"
#include "sound.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

const Backend &be()
{
    return backend();
}

} // namespace

void *as_data(int value)
{
    return reinterpret_cast<void *>(static_cast<intptr_t>(value));
}

int from_data(lv_event_t *event)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
}

lv_obj_t *button_row(lv_obj_t *parent)
{
    lv_obj_t *obj = t::plain(parent);
    lv_obj_set_size(obj, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(obj, t::dim(8), 0);
    lv_obj_set_style_pad_row(obj, t::dim(8), 0);
    lv_obj_set_style_pad_top(obj, t::dim(6), 0);
    return obj;
}

namespace {

// ------------------------------------------------------------------------------ quick page

lv_obj_t *g_quick_wifi = nullptr;
lv_obj_t *g_quick_hotspot = nullptr;
lv_obj_t *g_quick_hotspot_switch = nullptr;
lv_obj_t *g_quick_ha = nullptr;
lv_obj_t *g_quick_alarm = nullptr;

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
    if (g_quick_alarm != nullptr) {
        alarm::summary(text, sizeof(text));
        t::line_text(t::row_value(g_quick_alarm), text);
    }
    if (w.hotspot_on != lv_obj_has_state(g_quick_hotspot_switch, LV_STATE_CHECKED)) {
        if (w.hotspot_on) {
            lv_obj_add_state(g_quick_hotspot_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(g_quick_hotspot_switch, LV_STATE_CHECKED);
        }
    }
}

} // namespace

void on_hotspot_switch(lv_event_t *event)
{
    be().set_hotspot(lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED));
}

namespace {

Built quick(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    brightness_card(col);
    if (sound::available()) {
        volume_card(col);
    }
    g_quick_wifi = t::row(col, "Wi-Fi", "", true, [](lv_event_t *) { push(Page::Wifi); });
    g_quick_hotspot = t::row(col, "Hotspot", "", false, [](lv_event_t *) { push(Page::Hotspot); });
    g_quick_hotspot_switch = t::toggle(g_quick_hotspot, false, on_hotspot_switch);
    g_quick_ha = t::row(col, "Home Assistant", "", true, [](lv_event_t *) { push(Page::HomeAssistant); });
    t::row(col, "Display", "", true, [](lv_event_t *) { push(Page::Display); });
    if (sound::page_available()) {
        t::row(col, "Sound", "", true, [](lv_event_t *) { push(Page::Sound); });
    }
    if (camera_available()) {
        t::row(col, "Camera", "", true, [](lv_event_t *) { push(Page::Camera); });
    }
    g_quick_alarm = nullptr;
    if (alarm::available()) {
        g_quick_alarm = t::row(col, "Alarm", "", true, [](lv_event_t *) { push(Page::Alarm); });
    }
    t::row(col, "System", "", true, [](lv_event_t *) { push(Page::System); });
    quick_refresh();
    return {"Settings", quick_refresh};
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
        lv_obj_set_style_pad_bottom(g_ha_retry, t::dim(4), 0);
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
        t::qr_card(wrap, address, t::dim(140));
    }
    return {"Home Assistant", ha_refresh};
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

// the keyboard and the fields of the form shown (set_form)
lv_obj_t *g_form_keyboard = nullptr;
lv_obj_t *g_form_areas[2] = {};
lv_event_cb_t g_form_ready = nullptr;

} // namespace

void set_form(lv_obj_t *keyboard, lv_obj_t *first, lv_obj_t *second, lv_event_cb_t on_ready)
{
    g_form_ready = on_ready;
    g_form_keyboard = keyboard;
    g_form_areas[0] = first;
    g_form_areas[1] = second;
}

void focus_area(lv_obj_t *area)
{
    for (lv_obj_t *other : g_form_areas) {
        if (other != nullptr && other != area) {
            lv_obj_clear_state(other, LV_STATE_FOCUSED);
        }
    }
    lv_obj_add_state(area, LV_STATE_FOCUSED);
    lv_event_send(area, LV_EVENT_FOCUSED, nullptr); // starts the cursor blinking
    lv_keyboard_set_textarea(g_form_keyboard, area);
}

lv_obj_t *text_area(lv_obj_t *parent, const char *placeholder, bool password)
{
    lv_obj_t *area = lv_textarea_create(parent);
    lv_textarea_set_one_line(area, true);
    lv_textarea_set_password_mode(area, password);
    lv_textarea_set_placeholder_text(area, placeholder);
    lv_obj_set_flex_grow(area, 1);
    lv_obj_set_style_text_font(area, &t::font_medium(), 0);
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
    lv_obj_add_event_cb(
        area,
        [](lv_event_t *event) {
            if (g_form_ready != nullptr) {
                g_form_ready(event);
            }
        },
        LV_EVENT_READY, nullptr);
    return area;
}

lv_obj_t *text_block(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *obj = t::label(parent, text, t::font_normal(), color);
    lv_obj_set_width(obj, lv_pct(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    return obj;
}

lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *obj = t::label(parent, text, t::font_small(), t::kTextLabel);
    lv_obj_set_style_pad_top(obj, t::dim(6), 0);
    return obj;
}

// past the slider in every direction: more than the card around it, which limits it
constexpr lv_coord_t kCardTouchReach = 200;

// the value label of a slider card; its user data is the card's ValueText
static void value_text(lv_obj_t *label, int32_t value)
{
    const auto format = reinterpret_cast<ValueText>(lv_obj_get_user_data(label));
    char text[24];
    if (format != nullptr) {
        format(value, text, sizeof(text));
    } else {
        std::snprintf(text, sizeof(text), "%d %%", static_cast<int>(value));
    }
    lv_label_set_text(label, text);
}

lv_obj_t *slider_card(lv_obj_t *parent, const char *name, int32_t min, int32_t max, int32_t value,
                      lv_event_cb_t on_change, ValueText format)
{
    lv_obj_t *card = t::plain(parent);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, t::color(t::kCard), 0);
    lv_obj_set_style_radius(card, t::radius(), 0);
    lv_obj_set_style_pad_hor(card, t::dim(12), 0);
    lv_obj_set_style_pad_top(card, t::dim(10), 0);
    lv_obj_set_style_pad_bottom(card, t::dim(6), 0);
    lv_obj_set_style_pad_row(card, t::dim(2), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *head = t::plain(card);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    t::label(head, name, t::font_medium(), t::kText);
    lv_obj_t *value_label = t::label(head, "", t::font_normal(), t::kTextValue);
    lv_obj_set_user_data(value_label, reinterpret_cast<void *>(format));
    value_text(value_label, value);

    // The knob reaches past the ends of the track by half its height plus its padding (theme).
    // The whole card is the slider's touch area: a strip around the slider was too thin for a
    // finger, also at 42 px on the P4's dense screen (2026-10-09). LVGL looks for a child only
    // inside its parent unless the parent lets it overflow, so the track does, and the slider's
    // touch area reaches past the card; the card's own edges cut it off (the card does not
    // overflow), so the rows next to it keep their touches.
    lv_obj_t *track = t::plain(card);
    lv_obj_set_size(track, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(track, t::dim(16), 0);
    lv_obj_set_style_pad_ver(track, t::dim(16), 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(track, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t *slider = t::slider(track, min, max, value);
    lv_obj_set_width(slider, lv_pct(100));
    lv_obj_set_ext_click_area(slider, kCardTouchReach);
    lv_obj_add_event_cb(
        slider,
        [](lv_event_t *event) {
            value_text(static_cast<lv_obj_t *>(lv_event_get_user_data(event)),
                       lv_slider_get_value(lv_event_get_target(event)));
        },
        LV_EVENT_VALUE_CHANGED, value_label);
    lv_obj_add_event_cb(slider, on_change, LV_EVENT_VALUE_CHANGED, nullptr);
    return slider;
}

Built build(Page page, lv_obj_t *content, const char *text, int index)
{
    // a page that is left forgets its list state
    if (page != Page::AddNetwork) {
        forget_scan();
    }
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    switch (page) {
    case Page::Quick:
        return quick(content);
    case Page::Wifi:
        return wifi_page(content);
    case Page::NetworkDetail:
        return network_detail_page(content, text, index);
    case Page::AddNetwork:
        return add_network_page(content);
    case Page::Password:
        return password_page(content, text);
    case Page::Hotspot:
        return hotspot_page(content);
    case Page::HomeAssistant:
        return home_assistant(content);
    case Page::Display:
        return display_page(content);
    case Page::Sound:
        return sound_page(content);
    case Page::Camera:
        return camera_page(content);
    case Page::CameraLogin:
        return camera_login_page(content);
    case Page::CameraPreview:
        return camera_preview_page(content);
    case Page::Alarm:
        return alarm_page(content);
    case Page::AlarmDetail:
        return alarm_detail_page(content, index);
    case Page::System:
        return system(content);
    }
    return {"Settings", nullptr};
}

} // namespace hc::ui::settings::pages
