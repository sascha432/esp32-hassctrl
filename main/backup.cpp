#include "backup.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_lvgl_port.h"
#include "hc/board/board.hpp"
#include "hc/config/backup.hpp"
#include "hc/dashboard/dashboard.hpp"
#include "hc/hass/hass.hpp"
#include "hc/motion/motion.hpp"
#include "hc/network/network.hpp"
#include "hc/platform/time_sync.hpp"
#include "hc/ui/ui.hpp"
#include "hc/webserver/webserver.hpp"
#include "ui_backend.hpp"

namespace {

namespace backup = hc::config::backup;
namespace net = hc::network;
using hc::formats::yaml::Kind;
using hc::formats::yaml::Node;
using hc::memory::String;

bool g_ui_started = false;

// The settings of the screens are read and written in the LVGL task; the web server's task holds
// the lock while it uses them. Before the UI runs there is no LVGL task to collide with.
class UiLock {
public:
    UiLock() : _locked(g_ui_started && lvgl_port_lock(0))
    {
    }
    ~UiLock()
    {
        if (_locked) {
            lvgl_port_unlock();
        }
    }
    UiLock(const UiLock &) = delete;
    UiLock &operator=(const UiLock &) = delete;

private:
    bool _locked;
};

String text_of(const char *format, const char *a, const char *b = "")
{
    char text[160];
    std::snprintf(text, sizeof(text), format, a, b);
    return text;
}

// ---------------------------------------------------------------------------------- network

constexpr std::string_view kPolicies[] = {"order", "strongest"};

void write_network(backup::Writer &out)
{
    net::SavedNetworkInfo saved[net::kMaxNetworks];
    const size_t count = net::saved_networks(saved, net::kMaxNetworks);
    const net::Status status = net::status();
    out.section("network");
    out.text("policy", kPolicies[status.policy == net::Policy::StrongestSignal ? 1 : 0]);
    out.boolean("hotspot_on", status.hotspot_enabled);
    out.comment("passwords are not exported: add \"password: ...\" under a name by hand, or enter them on the device");
    out.open("networks");
    for (size_t i = 0; i < count; i++) {
        out.item("name", saved[i].ssid);
        if (saved[i].open) {
            out.boolean("open", true, 3);
        }
    }
}

// one network of the file; false when it has a problem (already in the report)
bool read_one_network(const Node &item, size_t number, net::ImportedNetwork &out, backup::Report &report)
{
    char path[32];
    std::snprintf(path, sizeof(path), "network.networks[%u]", static_cast<unsigned>(number));
    const size_t problems = report.problems.size();
    backup::Fields fields(item, path, report);
    String name, password;
    bool open = false;
    fields.text("name", net::kSsidSize - 1, name);
    const bool has_password = fields.text("password", net::kPasswordSize - 1, password);
    fields.boolean("open", open);
    fields.finish();
    if (report.problems.size() != problems) {
        return false;
    }
    if (name.empty()) {
        fields.problem("name", "the name of the network is missing");
        return false;
    }
    if (has_password && !password.empty() && password.size() < net::kMinPassword) {
        fields.problem("password", "at least 8 characters (none for an open network)");
        return false;
    }
    if (open && has_password && !password.empty()) {
        fields.problem("open", "an open network has no password");
        return false;
    }
    out = {};
    std::snprintf(out.ssid, sizeof(out.ssid), "%s", name.c_str());
    std::snprintf(out.password, sizeof(out.password), "%s", password.c_str());
    // open: saved without a password, nothing to enter later
    out.has_password = has_password || open;
    return true;
}

void read_network(const Node &value, bool apply, backup::Report &report)
{
    // the web server's task is the only caller; too large for its stack, in PSRAM (an import is rare)
    static EXT_RAM_BSS_ATTR net::ImportedNetwork imported[net::kMaxImport];
    static EXT_RAM_BSS_ATTR net::ImportResult result;

    const net::Status status = net::status();
    const size_t problems = report.problems.size();
    backup::Fields fields(value, "network", report);
    size_t policy = status.policy == net::Policy::StrongestSignal ? 1 : 0;
    const bool has_policy = fields.choice("policy", kPolicies, policy);
    bool hotspot = status.hotspot_enabled;
    const bool has_hotspot = fields.boolean("hotspot_on", hotspot);
    size_t count = 0;
    const Node *list = fields.node("networks");
    if (list != nullptr && list->kind != Kind::List) {
        fields.problem("networks", "a list of networks expected (- name: ...)");
    } else if (list != nullptr && list->list.size() > net::kMaxImport) {
        char message[48];
        std::snprintf(message, sizeof(message), "at most %u networks", static_cast<unsigned>(net::kMaxImport));
        fields.problem("networks", message);
    } else if (list != nullptr) {
        for (size_t i = 0; i < list->list.size(); i++) {
            if (!read_one_network(list->list[i], i + 1, imported[count], report)) {
                continue;
            }
            const bool twice = std::any_of(imported, imported + count, [&](const net::ImportedNetwork &other) {
                return std::strcmp(other.ssid, imported[count].ssid) == 0;
            });
            if (twice) {
                fields.problem("networks", text_of("'%s' is in the list twice", imported[count].ssid));
                continue;
            }
            count++;
        }
    }
    fields.finish();
    if (report.problems.size() != problems) {
        return;
    }

    if (list != nullptr) {
        // with `apply` false the network task only says what it would do
        if (net::import_networks(imported, count, apply, result) != ESP_OK) {
            report.problem("network", "the network task did not answer");
            return;
        }
        for (size_t i = 0; i < result.need_password_count; i++) {
            report.note("network", text_of("%s needs its password (Wi-Fi page)", result.need_password[i]));
        }
        for (size_t i = 0; i < result.dropped_count; i++) {
            report.note("network", text_of("%s is not saved, at most 5 networks", result.dropped[i]));
        }
    }
    if (apply && has_policy) {
        net::set_policy(policy == 1 ? net::Policy::StrongestSignal : net::Policy::EnteredOrder);
    }
    if (apply && has_hotspot) {
        net::set_hotspot(hotspot);
    }
}

// ---------------------------------------------------------------------------------- display

constexpr std::string_view kRotations[] = {"0", "90", "180", "270"};

// LVGL's rotation turns the content counterclockwise; the file has degrees clockwise, as the
// screen's Rotate button counts them
uint8_t clockwise_steps(uint8_t lvgl_rotation)
{
    return static_cast<uint8_t>((4 - lvgl_rotation) & 3);
}

void write_display(backup::Writer &out)
{
    hc::ui::DisplaySettings s{};
    {
        UiLock lock;
        ui_backend().display_settings(s);
    }
    out.section("display");
    out.number("brightness", s.brightness);
    out.number("dim_brightness", s.dim_brightness);
    out.number("dim_after_s", s.dim_after_s);
    out.number("off_after_s", static_cast<long>(s.off_after_s));
    out.boolean("automatic", s.auto_brightness);
    out.number("rotation", clockwise_steps(s.rotation) * 90);
    out.boolean("rotation_locked", s.rotation_locked);
    out.number("light_min_lx", std::lround(s.light_min_lux));
    out.number("light_max_lx", std::lround(s.light_max_lux));
    out.boolean("wake_on_motion", s.wake_on_motion);
    out.number("motion_hold_s", static_cast<long>(s.motion_hold_s));
}

void read_display(const Node &value, bool apply, backup::Report &report)
{
    UiLock lock;
    hc::ui::DisplaySettings s{};
    ui_backend().display_settings(s);
    backup::Fields fields(value, "display", report);
    fields.number("brightness", 1, 100, s.brightness);
    fields.number("dim_brightness", 1, 100, s.dim_brightness);
    fields.number("dim_after_s", 0, 3600, s.dim_after_s);
    fields.number("off_after_s", 0, 24 * 3600, s.off_after_s);
    fields.boolean("automatic", s.auto_brightness);
    size_t steps = clockwise_steps(s.rotation);
    if (fields.choice("rotation", kRotations, steps)) {
        s.rotation = clockwise_steps(static_cast<uint8_t>(steps));
    }
    fields.boolean("rotation_locked", s.rotation_locked);
    long light_min = std::lround(s.light_min_lux);
    long light_max = std::lround(s.light_max_lux);
    const bool has_min = fields.number("light_min_lx", 0, 100000, light_min);
    const bool has_max = fields.number("light_max_lx", 1, 100000, light_max);
    if ((has_min || has_max) && light_min >= light_max) {
        fields.problem("light_min_lx", "must be less than light_max_lx");
    } else {
        s.light_min_lux = static_cast<float>(light_min);
        s.light_max_lux = static_cast<float>(light_max);
    }
    fields.boolean("wake_on_motion", s.wake_on_motion);
    fields.number("motion_hold_s", hc::motion::kMinHoldS, hc::motion::kMaxHoldS, s.motion_hold_s);
    fields.finish();
    if (apply) {
        ui_backend().set_display_settings(s);
    }
}

// ------------------------------------------------------------------------------------ sound

void write_sound(backup::Writer &out)
{
    const hc::ui::Backend &be = ui_backend();
    hc::ui::SoundSettings s{};
    hc::ui::MicSettings mic{};
    bool vibration = false;
    UiLock lock;
    const bool has_sound = be.sound_settings != nullptr && be.sound_settings(s);
    const bool has_mic = be.mic_settings != nullptr && be.mic_settings(mic);
    const bool has_vibration = be.vibration != nullptr && be.vibration(vibration);
    if (!has_sound && !has_vibration) {
        return;
    }
    out.section("sound");
    if (has_sound) {
        out.number("volume", s.volume);
        out.boolean("tap_sound", s.tap_sound);
        out.boolean("slider_sound", s.slider_sound);
    }
    if (has_mic) {
        out.number("mic_gain_db", mic.gain_db);
        out.boolean("mic_auto_gain", mic.auto_gain);
    }
    if (has_vibration) {
        out.boolean("vibration", vibration);
    }
}

// fields of a device without audio, microphone or motor are named as ignored
void read_sound(const Node &value, bool apply, backup::Report &report)
{
    const hc::ui::Backend &be = ui_backend();
    UiLock lock;
    hc::ui::SoundSettings s{};
    hc::ui::MicSettings mic{};
    bool vibration = false;
    const bool has_sound = be.sound_settings != nullptr && be.sound_settings(s);
    const bool has_mic = be.mic_settings != nullptr && be.mic_settings(mic);
    const bool has_vibration = be.vibration != nullptr && be.vibration(vibration);
    backup::Fields fields(value, "sound", report);
    if (has_sound) {
        fields.number("volume", 0, 100, s.volume);
        fields.boolean("tap_sound", s.tap_sound);
        fields.boolean("slider_sound", s.slider_sound);
    }
    if (has_mic) {
        fields.number("mic_gain_db", 0, hc::ui::kMicMaxGainDb, mic.gain_db);
        fields.boolean("mic_auto_gain", mic.auto_gain);
    }
    const bool has_switch = has_vibration && fields.boolean("vibration", vibration);
    fields.finish();
    if (apply && has_sound) {
        be.set_sound_settings(s);
    }
    if (apply && has_mic) {
        be.set_mic_settings(mic);
    }
    if (apply && has_switch) {
        be.set_vibration(vibration);
    }
}

// ------------------------------------------------------------------------------------ alarm

// bit n of AlarmSettings::days is weekday n of std::tm, 0 = Sunday; written Monday first
constexpr std::string_view kDays[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
constexpr uint8_t kDayOrder[] = {1, 2, 3, 4, 5, 6, 0};

// a field of an item that the file leaves out: the alarm "Add alarm" makes
constexpr hc::ui::AlarmSettings kImportedAlarm{
    .on = true, .hour = 7, .minute = 0, .days = 0x3e, .snooze_min = 10, .sound = {}, .vibrate = true};

bool has_alarm()
{
    return ui_backend().alarms != nullptr;
}

void write_alarm(backup::Writer &out)
{
    const hc::ui::Backend &be = ui_backend();
    hc::ui::Alarms alarms{};
    {
        UiLock lock;
        be.alarms(alarms);
    }
    out.section("alarm");
    out.comment("up to 5; no days: once, then the alarm turns itself off");
    out.open("alarms");
    for (size_t i = 0; i < alarms.count; i++) {
        const hc::ui::AlarmSettings &s = alarms.list[i];
        char time[8];
        std::snprintf(time, sizeof(time), "%02u:%02u", s.hour, s.minute);
        std::string_view days[7];
        size_t count = 0;
        for (const uint8_t day : kDayOrder) {
            if (s.days & (1u << day)) {
                days[count++] = kDays[day];
            }
        }
        out.item("time", time);
        out.boolean("on", s.on, 3);
        out.flow("days", std::span(days, count), 3);
        out.number("snooze_min", s.snooze_min, 3);
        out.text("sound", s.sound, 3);
        out.boolean("vibrate", s.vibrate, 3);
    }
}

// "07:30"
bool parse_time(const String &text, uint8_t &hour, uint8_t &minute)
{
    unsigned h = 0;
    unsigned m = 0;
    char rest = 0;
    if (std::sscanf(text.c_str(), "%2u:%2u%c", &h, &m, &rest) != 2 || h > 23 || m > 59) {
        return false;
    }
    hour = static_cast<uint8_t>(h);
    minute = static_cast<uint8_t>(m);
    return true;
}

// the fields of one alarm into `s` (a field that is not there keeps its value)
void read_alarm_fields(backup::Fields &fields, const char *where, hc::ui::AlarmSettings &s, backup::Report &report)
{
    const hc::ui::Backend &be = ui_backend();
    fields.boolean("on", s.on);
    String time;
    if (fields.text("time", 5, time) && !parse_time(time, s.hour, s.minute)) {
        fields.problem("time", "hours and minutes, 00:00 to 23:59");
    }
    if (const Node *days = fields.node("days"); days != nullptr) {
        uint8_t mask = 0;
        bool ok = days->kind == Kind::List;
        for (size_t i = 0; ok && i < days->list.size(); i++) {
            const Node &day = days->list[i];
            const auto found = std::find(std::begin(kDays), std::end(kDays), std::string_view(day.text));
            ok = day.kind == Kind::Scalar && found != std::end(kDays);
            mask |= ok ? static_cast<uint8_t>(1u << (found - std::begin(kDays))) : 0;
        }
        if (ok) {
            s.days = mask;
        } else {
            fields.problem("days", "a list of mon, tue, wed, thu, fri, sat, sun ([] for once)");
        }
    }
    fields.number("snooze_min", 1, 60, s.snooze_min);
    String sound;
    if (fields.text("sound", hc::ui::kSoundNameSize - 1, sound)) {
        char names[16][hc::ui::kSoundNameSize];
        const size_t count = be.alarm_sounds(names, 16);
        const bool known = sound.empty() || std::any_of(names, names + count, [&](const char *name) {
                               return sound == name;
                           });
        if (!known) {
            report.note(where, text_of("the sound %s is not on this device, the alarm beeps", sound.c_str()));
            sound.clear();
        }
        std::snprintf(s.sound, sizeof(s.sound), "%s", sound.c_str());
    }
    fields.boolean("vibrate", s.vibrate);
    fields.finish();
}

void read_alarm(const Node &value, bool apply, backup::Report &report)
{
    const hc::ui::Backend &be = ui_backend();
    UiLock lock;
    hc::ui::Alarms alarms{};
    be.alarms(alarms);
    bool changed = false;
    if (value.kind == Kind::Map && value.find("alarms") == nullptr && value.find("time") != nullptr) {
        // a file from before 2026-10-21 with the single alarm: it becomes the only one
        hc::ui::AlarmSettings s = alarms.count > 0 ? alarms.list[0] : kImportedAlarm;
        backup::Fields fields(value, "alarm", report);
        read_alarm_fields(fields, "alarm", s, report);
        alarms.list[0] = s;
        alarms.count = 1;
        changed = true;
    } else {
        backup::Fields fields(value, "alarm", report);
        const Node *list = fields.node("alarms");
        if (list != nullptr && list->kind != Kind::List) {
            fields.problem("alarms", "a list of alarms expected (- time: ...)");
        } else if (list != nullptr && list->list.size() > hc::ui::kMaxAlarms) {
            char message[24];
            std::snprintf(message, sizeof(message), "at most %u alarms", static_cast<unsigned>(hc::ui::kMaxAlarms));
            fields.problem("alarms", message);
        } else if (list != nullptr) {
            alarms.count = static_cast<uint8_t>(list->list.size());
            for (size_t i = 0; i < list->list.size(); i++) {
                char path[24];
                std::snprintf(path, sizeof(path), "alarm.alarms[%u]", static_cast<unsigned>(i + 1));
                alarms.list[i] = kImportedAlarm;
                backup::Fields item(list->list[i], path, report);
                read_alarm_fields(item, path, alarms.list[i], report);
            }
            changed = true;
        }
        fields.finish();
    }
    if (apply && changed) {
        be.set_alarms(alarms);
    }
}

// ------------------------------------------------------------------------- Home Assistant

void write_hass(backup::Writer &out)
{
    const hc::hass::Status status = hc::hass::status();
    if (status.url[0] == 0) {
        return;
    }
    out.section("hass");
    out.text("url", status.url);
    if (status.certificate[0] != 0) {
        out.comment("the SHA-256 of Home Assistant's self-signed certificate, trusted on the device");
        out.text("certificate", status.certificate);
    }
    out.comment("the token is not exported: add \"token: ...\" by hand, or enter it on the device");
}

void read_hass(const Node &value, bool apply, backup::Report &report)
{
    const hc::hass::Status status = hc::hass::status();
    backup::Fields fields(value, "hass", report);
    String url = status.url;
    String token;
    String certificate;
    const bool has_url = fields.text("url", hc::hass::kUrlSize - 1, url);
    const bool has_token = fields.text("token", hc::hass::kTokenSize - 1, token) && !token.empty();
    // colons allowed (95 characters)
    const bool has_certificate = fields.text("certificate", 127, certificate) && !certificate.empty();
    fields.finish();
    if (!has_url && !has_token) {
        return;
    }
    char certificate_error[64] = {};
    if (has_certificate &&
        hc::hass::check_certificate(certificate.c_str(), certificate_error, sizeof(certificate_error)) != ESP_OK) {
        fields.problem("certificate", certificate_error);
        return;
    }
    if (url.empty() && has_token) {
        // an empty URL removes the connection, token and all
        fields.problem("url", "a token needs the address of Home Assistant");
        return;
    }
    char error[96] = {};
    if (hc::hass::check_config(url.c_str(), has_token ? token.c_str() : nullptr, error, sizeof(error)) != ESP_OK) {
        report.problem("hass", error);
        return;
    }
    if (!url.empty() && !has_token && !status.token_set) {
        report.note("hass", "no token yet: enter it on the Home Assistant page");
    }
    if (!apply) {
        return;
    }
    if (hc::hass::set_config(url.c_str(), has_token ? token.c_str() : nullptr, error, sizeof(error)) != ESP_OK) {
        report.problem("hass", error);
        return;
    }
    // after the address: a new one forgets the certificate of the old
    if (has_certificate && hc::hass::trust_certificate(certificate.c_str(), error, sizeof(error)) != ESP_OK) {
        report.problem("hass", error);
    }
}

// ------------------------------------------------------------------------------------- time

constexpr std::string_view kServerKeys[hc::platform::kTimeServers] = {"server_1", "server_2"};

// The Time page (docs/ui/web/web-app.md section 4): the fixed time zone (empty: Home Assistant's)
// and the SNTP servers, empty for none (both empty: the defaults)
void write_time(backup::Writer &out)
{
    char zone[hc::platform::kTimeZoneSize] = {};
    hc::platform::fixed_time_zone(zone, sizeof(zone));
    const hc::platform::TimeServers servers = hc::platform::time_servers();
    out.section("time");
    out.comment("an IANA name or a POSIX rule, \"\": the time zone of Home Assistant");
    out.text("zone", zone);
    for (size_t i = 0; i < hc::platform::kTimeServers; ++i) {
        out.text(kServerKeys[i], servers.names[i]);
    }
}

void read_time(const Node &value, bool apply, backup::Report &report)
{
    const size_t problems = report.problems.size();
    char current[hc::platform::kTimeZoneSize] = {};
    hc::platform::fixed_time_zone(current, sizeof(current));
    hc::platform::TimeServers servers = hc::platform::time_servers();
    backup::Fields fields(value, "time", report);
    String zone(current);
    if (fields.text("zone", hc::platform::kTimeZoneSize - 1, zone) && !zone.empty() &&
        !hc::platform::valid_time_zone(zone.c_str())) {
        fields.problem("zone", "neither a time zone this firmware knows nor a POSIX rule");
    }
    for (size_t i = 0; i < hc::platform::kTimeServers; ++i) {
        String name(servers.names[i]);
        if (fields.text(kServerKeys[i], hc::platform::kTimeServerSize - 1, name)) {
            std::snprintf(servers.names[i], sizeof(servers.names[i]), "%s", name.c_str());
        }
    }
    fields.finish();
    char error[96] = {};
    if (report.problems.size() == problems && !hc::platform::check_time_servers(servers, error, sizeof(error))) {
        report.problem("time", error);
    }
    if (report.problems.size() != problems || !apply) {
        return;
    }
    hc::platform::set_fixed_time_zone(zone.c_str());
    if (!hc::platform::set_time_servers(servers, error, sizeof(error))) {
        report.problem("time", error);
    }
}

// ------------------------------------------------------------------------------------ webui

// The web UI login (docs/ui/web/web-app.md section 3): import only. The export writes a hint in
// comments, never the section: the device stores a hash, the password is not there to write.
void write_webui(backup::Writer &out)
{
    out.comment("the web UI login is not exported; added by hand, an import sets it (and ends every session):", 0);
    out.comment("webui:", 0);
    out.comment("  user: admin", 0);
    out.comment("  password: 8-to-64-characters", 0);
}

void read_webui(const Node &value, bool apply, backup::Report &report)
{
    backup::Fields fields(value, "webui", report);
    String user;
    String password;
    const bool has_user = fields.text("user", 32, user);
    const bool has_password = fields.text("password", 64, password);
    fields.finish();
    if (!has_user && !has_password) {
        return;
    }
    if (!has_user || !has_password) {
        fields.problem(has_user ? "password" : "user", "user and password go together");
        return;
    }
    char error[64] = {};
    if (!hc::webserver::import_login(user.c_str(), password.c_str(), apply, error, sizeof(error))) {
        // "user: ..." and "password: ..." name their field
        const std::string_view text = error;
        const size_t colon = text.find(": ");
        const std::string_view key = colon == std::string_view::npos ? "" : text.substr(0, colon);
        if (key == "user" || key == "password") {
            fields.problem(key, text.substr(colon + 2));
        } else {
            report.problem("webui", text);
        }
        return;
    }
    report.note("webui", apply ? "new login, sign in again" : "the login will be replaced, sign in again afterwards");
}

// ----------------------------------------------------------------------------------- camera

// Settings, Camera (docs/architecture.md section 9.8). Only on a device with a camera: the
// section of another device's file is ignored there (backup::Section::available).
constexpr std::string_view kMains[] = {"50", "60"};
constexpr std::string_view kSizes[] = {"full", "half"};
constexpr std::string_view kFrameRates[] = {"5", "10", "15", "30"};

bool has_camera()
{
    hc::ui::CameraInfo info{};
    return ui_backend().camera_info != nullptr && ui_backend().camera_info(info);
}

void write_camera(backup::Writer &out)
{
    const hc::ui::Backend &be = ui_backend();
    hc::ui::CameraPicture p{};
    hc::ui::CameraStream s{};
    be.camera_picture(p);
    be.camera_stream(s);
    out.section("camera");
    out.number("mains_hz", p.mains_hz);
    out.boolean("auto_exposure", p.auto_exposure);
    out.number("brightness", p.brightness);
    out.comment("by hand (auto_exposure: false); the gain in 1/16, 16 = 1x");
    out.number("exposure_us", p.exposure_us);
    out.number("gain", p.gain);
    out.boolean("mirror", p.mirror);
    out.boolean("flip", p.flip);
    out.boolean("stream", s.enabled);
    out.text("stream_size", kSizes[s.half ? 1 : 0]);
    out.number("stream_fps", s.max_fps);
    out.number("stream_quality", s.quality);
    out.comment("the password is not exported: add \"stream_password: ...\" by hand, or enter it on the device");
    out.text("stream_user", s.username);
}

void read_camera(const Node &value, bool apply, backup::Report &report)
{
    const hc::ui::Backend &be = ui_backend();
    hc::ui::CameraInfo info{};
    hc::ui::CameraPicture p{};
    hc::ui::CameraStream s{};
    be.camera_info(info);
    be.camera_picture(p);
    be.camera_stream(s);
    backup::Fields fields(value, "camera", report);
    size_t index = 0;
    if (fields.choice("mains_hz", kMains, index)) {
        p.mains_hz = index == 0 ? 50 : 60;
    }
    fields.boolean("auto_exposure", p.auto_exposure);
    fields.number("brightness", -50, 50, p.brightness);
    fields.number("exposure_us", 100, info.max_exposure_us, p.exposure_us);
    fields.number("gain", 16, 248, p.gain);
    fields.boolean("mirror", p.mirror);
    fields.boolean("flip", p.flip);
    fields.boolean("stream", s.enabled);
    if (fields.choice("stream_size", kSizes, index)) {
        s.half = index == 1;
    }
    if (fields.choice("stream_fps", kFrameRates, index)) {
        constexpr uint8_t kRates[] = {5, 10, 15, 30};
        s.max_fps = kRates[index];
    }
    fields.number("stream_quality", 30, 95, s.quality);
    String user;
    String password;
    if (fields.text("stream_user", hc::ui::kCameraLoginSize - 1, user)) {
        // another name drops the stored password: it belonged to the old name
        if (user != s.username) {
            s.password[0] = 0;
        }
        std::snprintf(s.username, sizeof(s.username), "%s", user.c_str());
    }
    if (fields.text("stream_password", hc::ui::kCameraLoginSize - 1, password) && !password.empty()) {
        std::snprintf(s.password, sizeof(s.password), "%s", password.c_str());
    }
    fields.finish();
    if (s.enabled && (s.username[0] == 0 || s.password[0] == 0)) {
        report.note("camera", "the stream stays off until it has a login (Settings, Camera)");
    }
    if (apply) {
        be.set_camera_picture(p);
        be.set_camera_stream(s);
    }
}

// --------------------------------------------------------------------------------- dashboard

String write_dashboard()
{
    size_t length = 0;
    char *text = hc::dashboard::export_yaml(length);
    if (text == nullptr) {
        return {};
    }
    String out(text, length);
    hc::dashboard::release(text);
    return out;
}

bool read_dashboard(std::string_view text, bool apply, String &error)
{
    // the backup is one file: the main file of a dashboard import, without includes
    const hc::config::File files[] = {{"hass.yaml", text}};
    hc::dashboard::ImportReport report{};
    char problem[200] = {};
    const auto result = hc::dashboard::import_yaml(files, !apply, report, problem, sizeof(problem));
    if (result != hc::dashboard::Result::Ok) {
        error = problem;
        return false;
    }
    return true;
}

// in this order: the Home Assistant URL may need the network, the dashboard comes last
const backup::Section kSections[] = {
    {"network", write_network, read_network}, {"display", write_display, read_display},
    {"sound", write_sound, read_sound},       {"alarm", write_alarm, read_alarm, has_alarm},
    {"camera", write_camera, read_camera, has_camera}, {"hass", write_hass, read_hass},
    {"time", write_time, read_time},          {"webui", write_webui, read_webui},
};

const backup::Dashboard kDashboard{write_dashboard, read_dashboard};

const hc::webserver::BackupAccess kBackupAccess{
    .export_file =
        [](bool settings, bool dashboard) {
            const backup::Device device{hc::board::info().name, esp_app_get_description()->version};
            return backup::write(device, kSections, &kDashboard, settings, dashboard);
        },
    .import_file =
        [](std::string_view text, bool check_only) {
            backup::Report report = backup::read(text, kSections, &kDashboard, !check_only);
            if (!check_only && report.problems.empty() && g_ui_started) {
                UiLock lock;
                hc::ui::settings_imported();
            }
            hc::webserver::BackupReport out;
            out.problems = std::move(report.problems);
            out.ignored = std::move(report.ignored);
            out.notes = std::move(report.notes);
            out.sections = std::move(report.sections);
            return out;
        },
};

} // namespace

const hc::webserver::BackupAccess &backup_access()
{
    return kBackupAccess;
}

void backup_ui_started()
{
    g_ui_started = true;
}

backup::Report import_dashboard_settings(std::string_view text, bool apply)
{
    backup::Report report = backup::read_settings(text, kSections, apply);
    if (apply && report.problems.empty() && !report.sections.empty() && g_ui_started) {
        UiLock lock;
        hc::ui::settings_imported();
    }
    return report;
}
