#include "hc/sample/stub_backend.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "hc/ui/ui.hpp"
#include "lvgl.h"
#include "hc/sample/sample_hass.hpp"

namespace hc::sample {

namespace {

using hc::ui::AlarmSettings;
using hc::ui::CameraInfo;
using hc::ui::CameraPicture;
using hc::ui::CameraStream;
using hc::ui::DeviceInfo;
using hc::ui::DisplaySettings;
using hc::ui::HassInfo;
using hc::ui::kMaxSavedNetworks;
using hc::ui::kSsidSize;
using hc::ui::ScanItem;
using hc::ui::SoundSettings;
using hc::ui::WifiInfo;

constexpr uint32_t kConnectMs = 2000;
constexpr uint32_t kScanMs = 1500;
constexpr uint32_t kHassConnectMs = 1500;
constexpr uint32_t kHotspotMinutes = 15;
constexpr const char *kDeviceName = "hassctrl-3f9a";
constexpr const char *kIp = "192.168.0.206";

// set_start_time(): the epoch it set and LVGL's tick at that moment; 0: the real clock
int64_t g_start_time = 0;
uint32_t g_start_tick = 0;
bool g_verbose = true;

// the same defaults as main/ui_backend.cpp
constexpr DisplaySettings kDisplayDefaults{.brightness = 100,
                                           .dim_brightness = 20,
                                           .dim_after_s = 300,
                                           .off_after_s = 3600,
                                           .auto_brightness = true,
                                           .rotation = 0,
                                           .rotation_locked = false,
                                           .light_min_lux = 0.0f,
                                           .light_max_lux = 350.0f,
                                           .wake_on_motion = true,
                                           .motion_hold_s = 300};

enum class Sta : uint8_t { NoNetworks, Scanning, Connecting, Connected, WaitingToRetry };
enum class Hass : uint8_t { NotSetUp, Connected, TokenRejected, Disconnected };

struct Network {
    const char *ssid;
    int8_t rssi;
    bool secure;
};

// Names that test the layout: the longest SSID there is (32 bytes), a glyph the font lacks, an
// open network.
constexpr Network kAir[] = {
    {"Home-WLAN", -48, true},
    {"Office 5G", -61, true},
    {"FRITZ!Box 7590 KL", -67, true},
    {"Guest", -72, false},
    {"Telekom-Wohnzimmer-Mesh-Repeater", -75, true},
    {"Caf\xc3\xa9 Zentral", -80, false},
    {"iPhone von Sascha", -83, true},
    {"HP-Print-A2-LaserJet", -85, false},
    {"WLAN-4711", -89, true},
};

struct State {
    // Wi-Fi
    std::vector<std::string> saved;
    Sta sta = Sta::NoNetworks;
    std::string ssid;
    std::string error;
    int8_t rssi = -52;
    uint32_t sta_since = 0;
    // a Connecting that never ends (to look at the state)
    bool stuck = false;
    // every password is wrong
    bool reject_passwords = false;
    // the saved networks are not in the scan list
    bool saved_out_of_range = false;
    // saved by a backup import without their password
    std::vector<std::string> password_needed;
    bool ready = true;
    // hotspot
    bool hotspot_on = false;
    char hotspot_password[12] = "7k38d5192m";
    uint8_t hotspot_clients = 0;
    // lv_tick_get() when it turns itself off, 0: stays on
    uint32_t hotspot_off_at = 0;
    // scan
    uint32_t scan_done_at = 0;
    int scan_round = 0;
    // Home Assistant
    Hass hass = Hass::NotSetUp;
    uint32_t hass_connecting_until = 0;
    // other
    DisplaySettings display = kDisplayDefaults;
    // the Sound and Alarm pages as on a board with a speaker; the sounds are printed
    SoundSettings sound{.volume = 50, .tap_sound = true, .slider_sound = true};
    // and a vibration motor
    bool vibration = true;
    // the camera of the JC4880P443C, without a picture here
    CameraPicture camera_picture{.auto_exposure = true, .brightness = 0, .exposure_us = 10000, .gain = 16,
                                 .mirror = false, .flip = false, .mains_hz = 60};
    CameraStream camera_stream{.enabled = false, .half = false, .max_fps = 30, .quality = 50, .username = {},
                               .password = {}};
    AlarmSettings alarm{.on = true, .hour = 7, .minute = 0, .days = 0x3e, .snooze_min = 10, .sound = "Argon"};
    bool hint_seen = true;
    bool clock_set = true;
    uint32_t boot_tick = 0;
    uint32_t random = 12345;
};

State g;
size_t g_scenario = 0;
std::string g_board = "WT32-SC01";
bool g_has_sensor = false;
float g_lux = 100.0f;
bool g_has_motion = false;
uint8_t g_backlight = 100;

template <size_t N> void copy(char (&out)[N], const std::string &in)
{
    std::snprintf(out, N, "%.*s", static_cast<int>(N - 1), in.c_str());
}

bool in_range(const std::string &ssid)
{
    for (const Network &n : kAir) {
        if (ssid == n.ssid) {
            return !(g.saved_out_of_range && std::find(g.saved.begin(), g.saved.end(), ssid) != g.saved.end());
        }
    }
    return false;
}

int saved_index(const std::string &ssid)
{
    const auto it = std::find(g.saved.begin(), g.saved.end(), ssid);
    return it == g.saved.end() ? -1 : static_cast<int>(it - g.saved.begin());
}

void begin_connect(const std::string &ssid)
{
    g.ssid = ssid;
    g.error.clear();
    g.sta = Sta::Connecting;
    g.sta_since = lv_tick_get();
}

void finish_connect()
{
    if (g.reject_passwords) {
        g.sta = Sta::WaitingToRetry;
        g.error = "wrong Wi-Fi password";
    } else if (!in_range(g.ssid)) {
        g.sta = Sta::WaitingToRetry;
        g.error = "network not found";
    } else {
        g.sta = Sta::Connected;
        g.error.clear();
        for (const Network &n : kAir) {
            if (g.ssid == n.ssid) {
                g.rssi = n.rssi;
            }
        }
    }
    g.sta_since = lv_tick_get();
}

bool reached(uint32_t tick)
{
    return tick != 0 && static_cast<int32_t>(lv_tick_get() - tick) >= 0;
}

// the time-driven part of the state, run before every read
void update()
{
    if (g.sta == Sta::Connecting && !g.stuck && lv_tick_elaps(g.sta_since) >= kConnectMs) {
        finish_connect();
    }
    if (g.hotspot_on && reached(g.hotspot_off_at)) {
        g.hotspot_on = false;
        g.hotspot_off_at = 0;
    }
    if (reached(g.scan_done_at)) {
        g.scan_done_at = 0;
        g.scan_round++;
    }
    if (reached(g.hass_connecting_until)) {
        g.hass_connecting_until = 0;
    }
}

const char *sta_words()
{
    switch (g.sta) {
    case Sta::NoNetworks:
        return "Not set up";
    case Sta::Scanning:
        return "Searching for Wi-Fi";
    case Sta::Connecting:
        return "Connecting to";
    case Sta::Connected:
        return "Connected to";
    case Sta::WaitingToRetry:
        return "Retrying";
    }
    return "";
}

// 8 digits and 2 lowercase letters, like the firmware's hotspot password
void new_password()
{
    auto next = [] {
        g.random = g.random * 1664525u + 1013904223u;
        return g.random >> 16;
    };
    const size_t letter_a = next() % 10;
    size_t letter_b = next() % 10;
    if (letter_b == letter_a) {
        letter_b = (letter_b + 1) % 10;
    }
    for (size_t i = 0; i < 10; i++) {
        g.hotspot_password[i] = i == letter_a || i == letter_b ? static_cast<char>('a' + next() % 26)
                                                                : static_cast<char>('0' + next() % 10);
    }
    g.hotspot_password[10] = 0;
}

void base()
{
    g.saved = {"Home-WLAN", "Office 5G"};
    g.sta = Sta::Connected;
    g.ssid = "Home-WLAN";
    g.rssi = -48;
    g.hass = Hass::Connected;
}

// ------------------------------------------------------------------------------- scenarios

struct Scenario {
    ScenarioInfo info;
    void (*apply)();
};

const Scenario kScenarios[] = {
    {{"connected", "Wi-Fi and Home Assistant connected, the hint was seen"}, [] { base(); }},
    {{"hint", "connected, the one-time hint 'Swipe down for Settings' still shows"},
     [] {
         base();
         g.hint_seen = false;
     }},
    {{"first-setup", "no network saved, hotspot on: the first setup opens"},
     [] {
         g.hotspot_on = true;
         g.hint_seen = false;
     }},
    {{"first-setup-wrong-password", "the first setup where every password is refused (the failed view)"},
     [] {
         g.hotspot_on = true;
         g.hint_seen = false;
         g.reject_passwords = true;
     }},
    {{"connecting", "connecting to a saved network, never done"},
     [] {
         base();
         g.sta = Sta::Connecting;
         g.stuck = true;
         g.hass = Hass::Disconnected;
     }},
    {{"no-network", "saved networks are out of range"},
     [] {
         base();
         g.sta = Sta::WaitingToRetry;
         g.error = "network not found";
         g.saved_out_of_range = true;
     }},
    {{"wrong-password", "the router refuses the password (every new network fails the same way)"},
     [] {
         base();
         g.sta = Sta::WaitingToRetry;
         g.error = "wrong Wi-Fi password";
         g.reject_passwords = true;
     }},
    {{"hotspot", "connected, hotspot on with a phone joined, off in 15 minutes"},
     [] {
         base();
         g.hotspot_on = true;
         g.hotspot_clients = 1;
         g.hotspot_off_at = lv_tick_get() + kHotspotMinutes * 60 * 1000;
     }},
    {{"five-networks", "five saved networks: the list is full, no Add network"},
     [] {
         base();
         g.saved = {"Home-WLAN", "Office 5G", "FRITZ!Box 7590 KL", "Telekom-Wohnzimmer-Mesh-Repeater", "Guest"};
     }},
    {{"imported", "a backup import saved a network without its password: 'password needed'"},
     [] {
         base();
         g.saved = {"Holiday flat", "Home-WLAN", "Office 5G"};
         g.password_needed = {"Holiday flat"};
     }},
    {{"token-rejected", "Wi-Fi fine, Home Assistant refuses the token (the alert strip shows after 10 s)"},
     [] {
         base();
         g.hass = Hass::TokenRejected;
     }},
    {{"hass-unreachable", "Wi-Fi fine, Home Assistant does not answer"},
     [] {
         base();
         g.hass = Hass::Disconnected;
     }},
    {{"hass-not-set-up", "Wi-Fi fine, no Home Assistant address and token yet"},
     [] {
         base();
         g.hass = Hass::NotSetUp;
     }},
    {{"no-clock", "connected, the clock is not set (--:--)"},
     [] {
         base();
         g.clock_set = false;
     }},
};

void print(const char *what, const char *detail = "")
{
    say("[stub] %s%s%s\n", what, detail[0] != 0 ? " " : "", detail);
    std::fflush(stdout);
}

// -------------------------------------------------------------------------------- backend

void wifi_info(WifiInfo &out)
{
    update();
    out = {};
    out.connected = g.sta == Sta::Connected;
    out.connecting = g.sta == Sta::Scanning || g.sta == Sta::Connecting;
    out.has_networks = !g.saved.empty();
    std::snprintf(out.state, sizeof(out.state), "%s", sta_words());
    copy(out.ssid, g.sta == Sta::Scanning ? std::string() : g.ssid);
    copy(out.ip, g.sta == Sta::Connected ? std::string(kIp) : std::string());
    out.rssi = g.rssi;
    copy(out.error, g.error);
    out.hotspot_on = g.hotspot_on;
    copy(out.hotspot_ssid, std::string(kDeviceName));
    std::snprintf(out.hotspot_password, sizeof(out.hotspot_password), "%s", g.hotspot_password);
    out.hotspot_clients = g.hotspot_on ? g.hotspot_clients : 0;
    out.hotspot_off_in_s = g.hotspot_on && g.hotspot_off_at != 0
                               ? static_cast<uint32_t>(static_cast<int32_t>(g.hotspot_off_at - lv_tick_get()) / 1000)
                               : 0;
    out.ready = g.ready;
}

size_t saved_networks(char (*ssids)[kSsidSize], size_t max, uint8_t *password_needed)
{
    if (password_needed != nullptr) {
        *password_needed = 0;
    }
    const size_t count = std::min(g.saved.size(), max);
    for (size_t i = 0; i < count; i++) {
        copy(ssids[i], g.saved[i]);
        const bool needed = std::find(g.password_needed.begin(), g.password_needed.end(), g.saved[i]) !=
                            g.password_needed.end();
        if (needed && password_needed != nullptr) {
            *password_needed |= static_cast<uint8_t>(1u << i);
        }
    }
    return count;
}

size_t scan_results(ScanItem *out, size_t max)
{
    update();
    size_t count = 0;
    for (const Network &n : kAir) {
        if (count == max) {
            break;
        }
        if (g.saved_out_of_range && saved_index(n.ssid) >= 0) {
            continue;
        }
        out[count] = {};
        copy(out[count].ssid, std::string(n.ssid));
        // every finished scan moves the signals a little, like a real one
        const int jitter = (g.scan_round % 3 - 1) * 2 * static_cast<int>(count % 2 == 0 ? 1 : -1);
        out[count].rssi = static_cast<int8_t>(n.rssi + jitter);
        out[count].secure = n.secure;
        out[count].saved = saved_index(n.ssid) >= 0;
        count++;
    }
    return count;
}

void scan()
{
    print("scan");
    g.scan_done_at = lv_tick_get() + kScanMs;
    if (g.scan_done_at == 0) {
        g.scan_done_at = 1;
    }
}

void add_network(const char *ssid, const char *password)
{
    char detail[96];
    std::snprintf(detail, sizeof(detail), "\"%s\" (password of %u characters)", ssid,
                  static_cast<unsigned>(std::strlen(password)));
    print("add_network", detail);
    if (saved_index(ssid) < 0 && g.saved.size() < kMaxSavedNetworks) {
        g.saved.push_back(ssid);
    }
    // an empty password makes it an open network, as in the firmware
    std::erase(g.password_needed, std::string(ssid));
    begin_connect(ssid);
}

void remove_network(uint8_t index)
{
    print("remove_network", std::to_string(index).c_str());
    if (index < g.saved.size()) {
        const bool current = g.sta == Sta::Connected && g.ssid == g.saved[index];
        g.saved.erase(g.saved.begin() + index);
        if (current || g.saved.empty()) {
            g.sta = g.saved.empty() ? Sta::NoNetworks : Sta::WaitingToRetry;
            g.error.clear();
            g.ssid.clear();
        }
    }
}

void move_network(uint8_t index, int8_t delta)
{
    print("move_network", (std::to_string(index) + " by " + std::to_string(delta)).c_str());
    const int target = index + delta;
    if (index < g.saved.size() && target >= 0 && target < static_cast<int>(g.saved.size())) {
        std::swap(g.saved[index], g.saved[static_cast<size_t>(target)]);
    }
}

void reconnect()
{
    print("reconnect");
    if (!g.saved.empty()) {
        begin_connect(g.ssid.empty() ? g.saved[0] : g.ssid);
    }
}

void set_hotspot(bool on)
{
    print("set_hotspot", on ? "on" : "off");
    g.hotspot_on = on;
    // like the firmware: with a network it stays a while, in the first setup until it is turned off
    g.hotspot_off_at = on && !g.saved.empty() ? lv_tick_get() + kHotspotMinutes * 60 * 1000 : 0;
    if (!on) {
        g.hotspot_clients = 0;
    }
}

void new_hotspot_password()
{
    print("new_hotspot_password");
    new_password();
}

void display_settings(DisplaySettings &out)
{
    out = g.display;
}

void set_display_settings(const DisplaySettings &settings)
{
    g.display = settings;
}

bool sound_settings(SoundSettings &out)
{
    out = g.sound;
    return true;
}

void set_sound_settings(const SoundSettings &settings)
{
    g.sound = settings;
}

bool camera_info(CameraInfo &out)
{
    out = {.width = 1288, .height = 728, .max_exposure_us = 33161};
    return true;
}

void set_camera_stream(const CameraStream &stream)
{
    g.camera_stream = stream;
    // as the firmware: on only with a login
    g.camera_stream.enabled = stream.enabled && stream.username[0] != 0 && stream.password[0] != 0;
    char text[96];
    std::snprintf(text, sizeof(text), "%s, %s, %u fps, quality %u, login '%s'", g.camera_stream.enabled ? "on" : "off",
                  stream.half ? "half" : "full", stream.max_fps, stream.quality, stream.username);
    print("camera stream", text);
}

bool vibration(bool &on)
{
    on = g.vibration;
    return true;
}

void alarm_settings(AlarmSettings &out)
{
    out = g.alarm;
}

void set_alarm_settings(const AlarmSettings &settings)
{
    g.alarm = settings;
}

// the names of sounds/alarms/*.mp3
size_t alarm_sounds(char (*names)[hc::ui::kSoundNameSize], size_t max)
{
    static constexpr const char *kNames[] = {"Argon", "Carbon", "Helium", "Oxygen", "Platinum", "Timing"};
    size_t count = 0;
    for (const char *name : kNames) {
        if (count < max) {
            std::snprintf(names[count++], hc::ui::kSoundNameSize, "%s", name);
        }
    }
    return count;
}

void play_alarm(const char *sound, bool loop)
{
    print(loop ? "alarm rings:" : "alarm preview:", sound[0] != 0 ? sound : "Beeps");
}

void set_backlight(uint8_t percent)
{
    g_backlight = percent;
}

bool ambient_lux(float &lux)
{
    if (!g_has_sensor) {
        return false;
    }
    lux = g_lux;
    return true;
}

bool motion(bool &occupied)
{
    occupied = false;
    return g_has_motion;
}

void hass_info(HassInfo &out)
{
    update();
    out = {};
    const bool wifi = g.sta == Sta::Connected;
    out.configured = g.hass != Hass::NotSetUp;
    const char *state = "Not set up";
    if (g.hass != Hass::NotSetUp) {
        copy(out.url, std::string("http://homeassistant.local:8123"));
        if (!wifi) {
            state = "Waiting for Wi-Fi";
        } else if (g.hass_connecting_until != 0) {
            state = "Connecting";
        } else if (g.hass == Hass::Connected) {
            state = "Connected";
            out.connected = true;
            copy(out.version, std::string("2026.9.3"));
        } else if (g.hass == Hass::TokenRejected) {
            state = "Token rejected";
            out.failed = true;
            copy(out.error, std::string("Invalid access token or password"));
        } else {
            state = "Disconnected";
            out.failed = true;
            copy(out.error, std::string("the connection was refused: is the port right?"));
            out.retry_in_s = 30 - (lv_tick_get() / 1000) % 30;
        }
    }
    std::snprintf(out.state, sizeof(out.state), "%s", state);
}

void hass_reconnect()
{
    print("hass_reconnect");
    if (g.hass != Hass::NotSetUp) {
        g.hass_connecting_until = lv_tick_get() + kHassConnectMs;
    }
}

void device_info(DeviceInfo &out)
{
    out = {};
    copy(out.name, std::string(kDeviceName));
    copy(out.board, g_board);
    copy(out.firmware, std::string("0.1.0-sim"));
    out.uptime_s = (lv_tick_get() - g.boot_tick) / 1000;
    out.heap_internal_kb = 142;
    out.heap_psram_kb = 3012;
}

void local_time(int64_t epoch, std::tm &out)
{
    const auto t = static_cast<std::time_t>(epoch);
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
}

bool time_text(char *out, size_t size)
{
    if (!g.clock_set) {
        return false;
    }
    std::tm local{};
    local_time(now(), local);
    return std::strftime(out, size, "%H:%M", &local) != 0;
}

int64_t clock_now()
{
    return g.clock_set ? now() : 0;
}

void web_address(char *out, size_t size)
{
    update();
    std::snprintf(out, size, "%s", g.sta == Sta::Connected ? "http://192.168.0.206/" : "");
}

void reset_web_login()
{
    print("reset_web_login");
}

void restart()
{
    print("restart");
    // a restart starts the scenario again: the taps made so far are forgotten
    start_scenario(g_scenario);
}

void factory_reset()
{
    print("factory_reset");
    const uint8_t rotation = g.display.rotation;
    g = State{};
    g.display.rotation = rotation;
    g.boot_tick = lv_tick_get();
    // the boot after it opens the first setup
    hc::ui::show_setup();
}

bool hint_seen()
{
    return g.hint_seen;
}

void set_hint_seen()
{
    g.hint_seen = true;
}

const hc::ui::Backend kBackend{
    .wifi_info = wifi_info,
    .saved_networks = saved_networks,
    .scan_results = scan_results,
    .scan = scan,
    .add_network = add_network,
    .remove_network = remove_network,
    .move_network = move_network,
    .reconnect = reconnect,
    .set_hotspot = set_hotspot,
    .new_hotspot_password = new_hotspot_password,
    .display_settings = display_settings,
    .set_display_settings = set_display_settings,
    .set_backlight = set_backlight,
    .ambient_lux = ambient_lux,
    .motion = motion,
    .sound_settings = sound_settings,
    .set_sound_settings = set_sound_settings,
    .play_touch_sound = [] { print("touch sound"); },
    .play_slider_tick = [] { print("slider tick"); },
    .vibration = vibration,
    .set_vibration = [](bool on) { g.vibration = on; },
    .vibrate_click = [] { print("vibration pulse"); },
    .touch_fingers = nullptr,
    .alarm_settings = alarm_settings,
    .set_alarm_settings = set_alarm_settings,
    .alarm_sounds = alarm_sounds,
    .play_alarm = play_alarm,
    .stop_alarm = [] { print("alarm stops"); },
    .hass_info = hass_info,
    .hass_reconnect = hass_reconnect,
    .dashboard = dashboard,
    .save_dashboard = save_dashboard,
    .subscribe_page = subscribe_page,
    .call_service = call_service,
    .take_event = take_event,
    .fetch_history = fetch_history,
    .subscribe_forecast = subscribe_forecast,
    .find_entities = find_entities,
    .fetch_entity = fetch_entity,
    .fetch_image = fetch_image,
    .play_stream = play_stream,
    .stop_stream = stop_stream,
    .device_info = device_info,
    .time_text = time_text,
    .clock = clock_now,
    .local_time = local_time,
    .web_address = web_address,
    .reset_web_login = reset_web_login,
    .restart = restart,
    .factory_reset = factory_reset,
    .hint_seen = hint_seen,
    .set_hint_seen = set_hint_seen,
    .camera_info = camera_info,
    .camera_picture = [](CameraPicture &out) { out = g.camera_picture; },
    .set_camera_picture = [](const CameraPicture &picture) { g.camera_picture = picture; },
    .camera_stream = [](CameraStream &out) { out = g.camera_stream; },
    .set_camera_stream = set_camera_stream,
    .camera_preview_start = nullptr,
    .camera_preview_stop = nullptr,
};

} // namespace

bool hass_connected()
{
    update();
    return g.sta == Sta::Connected && g.hass == Hass::Connected && g.hass_connecting_until == 0;
}

size_t scenario_count()
{
    return sizeof(kScenarios) / sizeof(kScenarios[0]);
}

const ScenarioInfo &scenario_info(size_t index)
{
    return kScenarios[index].info;
}

int find_scenario(const char *name)
{
    for (size_t i = 0; i < scenario_count(); i++) {
        if (std::strcmp(kScenarios[i].info.name, name) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void start_scenario(size_t index)
{
    const DisplaySettings display = g.display;
    g_scenario = index < scenario_count() ? index : 0;
    g = State{};
    g.display = display;
    g.boot_tick = lv_tick_get();
    new_password();
    kScenarios[g_scenario].apply();
    say("[stub] scenario %s: %s\n", kScenarios[g_scenario].info.name, kScenarios[g_scenario].info.description);
    std::fflush(stdout);
}

size_t current_scenario()
{
    return g_scenario;
}

void set_rotation(uint8_t rotation)
{
    g.display.rotation = rotation & 3;
}

void set_board(const char *board_name)
{
    g_board = board_name;
}

void set_ambient_light(bool present, float lux)
{
    g_has_sensor = present;
    g_lux = lux;
}

void set_motion_sensor(bool present)
{
    g_has_motion = present;
}

uint8_t backlight_percent()
{
    return g_backlight;
}

const hc::ui::Backend &backend()
{
    return kBackend;
}

void set_verbose(bool on)
{
    g_verbose = on;
}

void say(const char *format, ...)
{
    if (!g_verbose) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
}

int64_t now()
{
    if (g_start_time == 0) {
        return static_cast<int64_t>(std::time(nullptr));
    }
    return g_start_time + static_cast<int64_t>(lv_tick_elaps(g_start_tick) / 1000);
}

void set_start_time(int64_t epoch)
{
    g_start_time = epoch;
    g_start_tick = lv_tick_get();
}

} // namespace hc::sample
