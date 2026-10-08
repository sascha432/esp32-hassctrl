#pragma once

// What the screens need from the rest of the firmware (docs/architecture.md section 3: the UI
// never talks to the network itself). `main` fills this table with the functions of the network,
// board and web server components. Every function is called from the LVGL task and returns at
// once; actions that take time (scans, connecting) report their progress through wifi_info().

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string_view>

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"

namespace hc::ui {

constexpr size_t kSsidSize = 33;
// WPA needs at least 8 characters; an open network has no password
constexpr size_t kMinPassword = 8;
constexpr size_t kMaxSavedNetworks = 5;

struct WifiInfo {
    bool connected;
    bool connecting;
    bool has_networks;
    // "connected", "scanning", ... in words
    char state[24];
    char ssid[kSsidSize];
    char ip[16];
    int8_t rssi;
    // the last problem in words, empty while there is none
    char error[64];
    bool hotspot_on;
    char hotspot_ssid[kSsidSize];
    char hotspot_password[12];
    uint8_t hotspot_clients;
    uint32_t hotspot_off_in_s;
    // the network has loaded its saved networks: has_networks is meaningful
    bool ready;
};

struct ScanItem {
    char ssid[kSsidSize];
    int8_t rssi;
    bool secure;
    bool saved;
};

struct DeviceInfo {
    char name[24];
    char board[24];
    char firmware[32];
    uint32_t uptime_s;
    uint32_t heap_internal_kb;
    uint32_t heap_psram_kb;
};

// The Home Assistant connection. Address and token are set in the web UI only.
struct HassInfo {
    bool configured; // address and token are stored
    bool connected;
    // the token was rejected, or the connection is down with a problem
    bool failed;
    char state[24]; // "connected", "token rejected", ... in words
    char url[128];
    char version[24];
    char error[96];
    uint32_t retry_in_s; // seconds until the next try while disconnected
};

// The screen's power settings (Settings, Display). The UI applies them; the backend stores them.
struct DisplaySettings {
    // percent, 1 to 100; with the automatic brightness the brightness in a bright room
    uint8_t brightness;
    uint8_t dim_brightness; // percent while dimmed, 1 to 100
    uint16_t dim_after_s;   // idle time before dimming, 0: never (up to 1 h)
    uint32_t off_after_s;   // idle time before the screen turns off, 0: never (up to 24 h)
    // follow the ambient light (only with a light sensor)
    bool auto_brightness;
    // display rotation in 90 degree steps (LVGL's), used when there is no orientation sensor or
    // it is locked
    uint8_t rotation;
    bool rotation_locked;
};

struct Backend {
    void (*wifi_info)(WifiInfo &out);
    size_t (*saved_networks)(char (*ssids)[kSsidSize], size_t max);
    size_t (*scan_results)(ScanItem *out, size_t max);
    void (*scan)();
    // saves the network and connects to it right away
    void (*add_network)(const char *ssid, const char *password);
    void (*remove_network)(uint8_t index);
    void (*move_network)(uint8_t index, int8_t delta);
    void (*reconnect)();
    void (*set_hotspot)(bool on);
    void (*new_hotspot_password)();

    void (*display_settings)(DisplaySettings &out);
    // keeps the settings and stores them a few seconds after the last change
    void (*set_display_settings)(const DisplaySettings &settings);
    // the backlight in percent of the perceived brightness, 0 turns it off
    void (*set_backlight)(uint8_t percent);
    // the light of the room in lux (calibrated, without the screen's own light); false without a
    // light sensor
    bool (*ambient_lux)(float &lux);

    void (*hass_info)(HassInfo &out);
    // tries at once, also after a rejected token
    void (*hass_reconnect)();

    // The dashboard (docs/architecture.md section 4): the current layout, null without one. A
    // new pointer is a new dashboard (an import or an edit); the screen builds itself again.
    std::shared_ptr<const core::Layout> (*dashboard)();
    // the values of the tiles of `page` (and the panel of the tile `panel`) are wanted from now
    // on, stamped with `generation`
    void (*subscribe_page)(std::shared_ptr<const core::Layout> layout, core::PageIndex page, core::TileIndex panel,
                           uint32_t generation);
    void (*call_service)(const core::Intent &intent, std::string_view entity_id);
    // the next values or failure from Home Assistant, false when there is none
    bool (*take_event)(core::HassEvent &out);
    // The history of `entity_id` over the last `hours` for the graph of a sensor panel; it comes
    // back through take_event as a core::History stamped with `generation`.
    void (*fetch_history)(std::string_view entity_id, core::EntityIndex entity, core::HistoryKind kind, uint32_t hours,
                          uint32_t generation);
    // The camera image of `entity_id` as a `width` x `height` frame in the display's pixel format
    // (lv_color_t); it comes back through take_event as a core::Image stamped with `generation`.
    // `stream`: the tile's MJPEG stream, whose first frame is the image; empty: Home Assistant's.
    void (*fetch_image)(std::string_view entity_id, std::string_view stream, core::EntityIndex entity, uint16_t width,
                        uint16_t height, uint32_t generation);
    // Plays the MJPEG `stream` as `width` x `height` frames (core::Image with `live`, stamped with
    // `generation`) until stop_stream() or the next play_stream(); one plays at a time.
    void (*play_stream)(std::string_view entity_id, std::string_view stream, core::EntityIndex entity, uint16_t width,
                        uint16_t height, uint32_t generation);
    void (*stop_stream)();

    void (*device_info)(DeviceInfo &out);
    // "HH:MM", false while the clock is not set
    bool (*time_text)(char *out, size_t size);
    // the time in epoch seconds, 0 while the clock is not set
    int64_t (*clock)();
    // epoch seconds in the local time of the device (its time zone comes from Home Assistant)
    void (*local_time)(int64_t epoch, std::tm &out);
    // the address of the web UI ("http://192.168.0.206/"), empty without a network
    void (*web_address)(char *out, size_t size);

    void (*reset_web_login)();
    void (*restart)();
    void (*factory_reset)();

    // the one-time hint "Swipe down from the top for Settings" was used (stored)
    bool (*hint_seen)();
    void (*set_hint_seen)();
};

} // namespace hc::ui
