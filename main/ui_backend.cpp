#include "ui_backend.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hc/board/board.hpp"
#include "hc/dashboard/dashboard.hpp"
#include "hc/hass/hass.hpp"
#include "hc/light_sensor/light_sensor.hpp"
#include "hc/network/network.hpp"
#include "hc/platform/device.hpp"
#include "hc/platform/settings.hpp"
#include "hc/platform/time_sync.hpp"
#include "hc/webserver/webserver.hpp"
#include "lvgl.h"
#include "ui_sound.hpp"

namespace {

namespace net = hc::network;
using hc::ui::kSsidSize;

static_assert(hc::ui::kSsidSize == net::kSsidSize);
static_assert(hc::ui::kMaxSavedNetworks == net::kMaxNetworks);

constexpr const char *kUiNamespace = "ui";
constexpr const char *kHintKey = "hint";

constexpr const char *kDisplayKey = "display";
// dims after a minute, never turns off
constexpr hc::ui::DisplaySettings kDisplayDefaults{.brightness = 100,
                                                   .dim_brightness = 20,
                                                   .dim_after_s = 60,
                                                   .off_after_s = 0,
                                                   .auto_brightness = true,
                                                   .rotation = 0,
                                                   .rotation_locked = false};
// The layout of the second version (screen off in 16 bit seconds); the first version was its
// first 8 bytes, without the rotation. The current one is DisplaySettings itself: the sizes tell
// them apart.
struct DisplaySettingsV2 {
    uint8_t brightness;
    uint8_t dim_brightness;
    uint16_t dim_after_s;
    uint16_t off_after_s;
    bool auto_brightness;
    uint8_t rotation;
    bool rotation_locked;
};
constexpr size_t kDisplaySettingsV1 = 8;
static_assert(sizeof(DisplaySettingsV2) != sizeof(hc::ui::DisplaySettings));

// a stored value of any version into the current settings; false when it is none of them
bool convert_display_settings(const uint8_t *data, size_t size, hc::ui::DisplaySettings &out)
{
    if (size == sizeof(hc::ui::DisplaySettings)) {
        std::memcpy(&out, data, size);
        return true;
    }
    if (size != sizeof(DisplaySettingsV2) && size != kDisplaySettingsV1) {
        return false;
    }
    // what the first version did not have keeps the defaults
    DisplaySettingsV2 old{.brightness = out.brightness,
                          .dim_brightness = out.dim_brightness,
                          .dim_after_s = out.dim_after_s,
                          .off_after_s = 0,
                          .auto_brightness = out.auto_brightness,
                          .rotation = out.rotation,
                          .rotation_locked = out.rotation_locked};
    std::memcpy(&old, data, size);
    out.brightness = old.brightness;
    out.dim_brightness = old.dim_brightness;
    out.dim_after_s = old.dim_after_s;
    out.off_after_s = old.off_after_s;
    out.auto_brightness = old.auto_brightness;
    out.rotation = old.rotation;
    out.rotation_locked = old.rotation_locked;
    return true;
}

hc::ui::DisplaySettings g_display = kDisplayDefaults;
bool g_display_loaded = false;
// -1: not read yet
int g_hint_seen = -1;

// copies and cuts to the field, which snprintf cannot do without a truncation warning
template <size_t N> void copy(char (&out)[N], const char *in)
{
    std::snprintf(out, N, "%.*s", static_cast<int>(N - 1), in);
}

const char *state_words(net::StaState state)
{
    switch (state) {
    case net::StaState::NoNetworks:
        return "Not set up";
    case net::StaState::Scanning:
        return "Searching for Wi-Fi";
    case net::StaState::Connecting:
        return "Connecting to";
    case net::StaState::Connected:
        return "Connected to";
    case net::StaState::WaitingToRetry:
        return "Retrying";
    }
    return "";
}

void wifi_info(hc::ui::WifiInfo &out)
{
    const net::Status s = net::status();
    out = {};
    out.connected = s.state == net::StaState::Connected;
    out.connecting = s.state == net::StaState::Scanning || s.state == net::StaState::Connecting;
    out.has_networks = s.saved_count > 0;
    copy(out.state, state_words(s.state));
    // while searching no network is chosen yet
    copy(out.ssid, s.state == net::StaState::Scanning ? "" : s.ssid);
    copy(out.ip, out.connected ? s.ip : "");
    out.rssi = s.rssi;
    copy(out.error, s.error);
    out.hotspot_on = s.hotspot_on;
    copy(out.hotspot_ssid, s.hotspot_ssid);
    copy(out.hotspot_password, s.hotspot_password);
    out.hotspot_clients = s.hotspot_clients;
    out.hotspot_off_in_s = s.hotspot_off_in_s;
    out.ready = s.ready;
}

size_t scan_results(hc::ui::ScanItem *out, size_t max)
{
    net::ScanEntry entries[20];
    const size_t count = net::last_scan(entries, max < 20 ? max : 20);
    for (size_t i = 0; i < count; i++) {
        out[i] = {};
        copy(out[i].ssid, entries[i].ssid);
        out[i].rssi = entries[i].rssi;
        out[i].secure = entries[i].secure;
        out[i].saved = entries[i].saved;
    }
    return count;
}

void device_info(hc::ui::DeviceInfo &out)
{
    out = {};
    copy(out.name, hc::platform::device_name());
    copy(out.board, hc::board::info().name);
    copy(out.firmware, esp_app_get_description()->version);
    out.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    out.heap_internal_kb = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    out.heap_psram_kb = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

// local time is UTC until the time zone comes from Home Assistant
bool time_text(char *out, size_t size)
{
    if (!hc::platform::time_synced()) {
        return false;
    }
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    return strftime(out, size, "%H:%M", &local) != 0;
}

// the frames in lv_color_t's byte order
void fetch_image(std::string_view entity_id, std::string_view stream, hc::core::EntityIndex entity, uint16_t width,
                 uint16_t height, uint32_t generation)
{
    hc::hass::fetch_image(entity_id, stream, entity, width, height, LV_COLOR_16_SWAP != 0, generation);
}

void play_stream(std::string_view entity_id, std::string_view stream, hc::core::EntityIndex entity, uint16_t width,
                 uint16_t height, uint32_t generation)
{
    hc::hass::play_stream(entity_id, stream, entity, width, height, LV_COLOR_16_SWAP != 0, generation);
}

int64_t clock_now()
{
    return hc::platform::time_synced() ? static_cast<int64_t>(time(nullptr)) : 0;
}

void local_time(int64_t epoch, tm &out)
{
    const auto t = static_cast<time_t>(epoch);
    localtime_r(&t, &out);
}

void web_address(char *out, size_t size)
{
    const net::Status s = net::status();
    if (s.state == net::StaState::Connected) {
        std::snprintf(out, size, "http://%s/", s.ip);
    } else if (size > 0) {
        out[0] = 0;
    }
}

void display_settings(hc::ui::DisplaySettings &out)
{
    if (!g_display_loaded) {
        g_display_loaded = true;
        hc::ui::DisplaySettings stored = kDisplayDefaults;
        uint8_t data[sizeof(hc::ui::DisplaySettings)] = {};
        size_t size = sizeof(data);
        hc::platform::Settings settings(kUiNamespace, hc::platform::Settings::Mode::Read);
        if (settings.ok() && settings.get_blob(kDisplayKey, data, size) == ESP_OK &&
            convert_display_settings(data, size, stored) && stored.brightness >= 1 && stored.brightness <= 100 &&
            stored.dim_brightness >= 1 && stored.dim_brightness <= 100) {
            g_display = stored;
        }
    }
    out = g_display;
}

void set_display_settings(const hc::ui::DisplaySettings &settings)
{
    g_display = settings;
    g_display_loaded = true;
    hc::platform::set_blob_later(kUiNamespace, kDisplayKey, &g_display, sizeof(g_display));
}

// The sensor also sees the screen: the board's share of it at full brightness, scaled by the
// backlight of the moment, is taken off.
bool ambient_lux(float &out)
{
    if (!hc::light_sensor::lux(out)) {
        return false;
    }
    out -= hc::board::info().light_sensor_backlight_lux * hc::board::backlight_level();
    if (out < 0.0f) {
        out = 0.0f;
    }
    return true;
}

bool hint_seen()
{
    if (g_hint_seen < 0) {
        uint8_t value = 0;
        hc::platform::Settings settings(kUiNamespace, hc::platform::Settings::Mode::Read);
        g_hint_seen = settings.ok() && settings.get_u8(kHintKey, value) == ESP_OK && value != 0 ? 1 : 0;
    }
    return g_hint_seen == 1;
}

void set_hint_seen()
{
    if (hint_seen()) {
        return;
    }
    g_hint_seen = 1;
    hc::platform::Settings settings(kUiNamespace, hc::platform::Settings::Mode::ReadWrite);
    if (settings.ok() && settings.set_u8(kHintKey, 1) == ESP_OK) {
        settings.commit();
    }
}

void hass_info(hc::ui::HassInfo &out)
{
    using hc::hass::State;
    const hc::hass::Status s = hc::hass::status();
    out.configured = s.state != State::NotSetUp;
    out.connected = s.state == State::Connected;
    // also while it tries again: the problem lasts until the connection works
    out.failed = s.state != State::Connected && s.state != State::NotSetUp && s.error[0] != 0;
    // a state on its own line or in a row: "Token rejected"
    std::snprintf(out.state, sizeof(out.state), "%s", hc::hass::state_name(s.state));
    out.state[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out.state[0])));
    std::snprintf(out.url, sizeof(out.url), "%s", s.url);
    std::snprintf(out.version, sizeof(out.version), "%s", s.version);
    std::snprintf(out.error, sizeof(out.error), "%s", s.error);
    out.retry_in_s = s.retry_in_s;
}

const hc::ui::Backend g_backend{
    .wifi_info = wifi_info,
    .saved_networks =
        [](char (*ssids)[kSsidSize], size_t max, uint8_t *password_needed) {
            net::SavedNetworkInfo saved[net::kMaxNetworks];
            const size_t count = net::saved_networks(saved, std::min(max, net::kMaxNetworks));
            uint8_t needed = 0;
            for (size_t i = 0; i < count; i++) {
                copy(ssids[i], saved[i].ssid);
                needed |= saved[i].password_needed ? (1u << i) : 0;
            }
            if (password_needed != nullptr) {
                *password_needed = needed;
            }
            return count;
        },
    .scan_results = scan_results,
    .scan = [] { net::scan(); },
    .add_network = [](const char *ssid, const char *password) { net::add_network(ssid, password, true); },
    .remove_network = [](uint8_t index) { net::remove_network(index); },
    .move_network = [](uint8_t index, int8_t delta) { net::move_network(index, delta); },
    .reconnect = [] { net::reconnect(); },
    .set_hotspot = [](bool on) { net::set_hotspot(on); },
    .new_hotspot_password = [] { net::new_hotspot_password(); },
    .display_settings = display_settings,
    .set_display_settings = set_display_settings,
    .set_backlight = [](uint8_t percent) { hc::board::set_backlight(percent); },
    .ambient_lux = ambient_lux,
    // nullptr without the options (ui_sound.cpp)
    .sound_settings = ui_sound::sound_settings,
    .set_sound_settings = ui_sound::set_sound_settings,
    .play_touch_sound = ui_sound::play_touch_sound,
    .play_slider_tick = ui_sound::play_slider_tick,
    .vibration = ui_sound::vibration,
    .set_vibration = ui_sound::set_vibration_setting,
    .vibrate_click = ui_sound::vibrate_click,
    .alarm_settings = ui_sound::alarm_settings,
    .set_alarm_settings = ui_sound::set_alarm_settings,
    .alarm_sounds = ui_sound::alarm_sounds,
    .play_alarm = ui_sound::play_alarm,
    .stop_alarm = ui_sound::stop_alarm,
    .hass_info = hass_info,
    .hass_reconnect = [] { hc::hass::reconnect(); },
    .dashboard = hc::dashboard::current,
    .save_dashboard =
        [](const hc::core::Layout &layout, uint32_t revision, char *error, size_t error_size) {
            uint32_t stored = 0;
            return hc::dashboard::save_layout(layout, revision, stored, error, error_size) == hc::dashboard::Result::Ok;
        },
    .subscribe_page = hc::hass::subscribe_page,
    .call_service = hc::hass::call_service,
    .take_event = hc::hass::take_event,
    .fetch_history = hc::hass::fetch_history,
    .fetch_image = fetch_image,
    .play_stream = play_stream,
    .stop_stream = hc::hass::stop_stream,
    .device_info = device_info,
    .time_text = time_text,
    .clock = clock_now,
    .local_time = local_time,
    .web_address = web_address,
    .reset_web_login = [] { hc::webserver::reset_login(); },
    .restart = [] { esp_restart(); },
    .factory_reset =
        [] {
            hc::platform::erase_all_settings();
            esp_restart();
        },
    .hint_seen = hint_seen,
    .set_hint_seen = set_hint_seen,
};

} // namespace

const hc::ui::Backend &ui_backend()
{
    return g_backend;
}
