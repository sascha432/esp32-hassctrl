#include "backup.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_lvgl_port.h"
#include "hc/board/board.hpp"
#include "hc/config/backup.hpp"
#include "hc/dashboard/dashboard.hpp"
#include "hc/hass/hass.hpp"
#include "hc/network/network.hpp"
#include "hc/ui/ui.hpp"
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
    // the web server's task is the only caller; too large for its stack
    static net::ImportedNetwork imported[net::kMaxImport];
    static net::ImportResult result;

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
    bool vibration = false;
    UiLock lock;
    const bool has_sound = be.sound_settings != nullptr && be.sound_settings(s);
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
    if (has_vibration) {
        out.boolean("vibration", vibration);
    }
}

// fields of a device without audio or motor are named as ignored
void read_sound(const Node &value, bool apply, backup::Report &report)
{
    const hc::ui::Backend &be = ui_backend();
    UiLock lock;
    hc::ui::SoundSettings s{};
    bool vibration = false;
    const bool has_sound = be.sound_settings != nullptr && be.sound_settings(s);
    const bool has_vibration = be.vibration != nullptr && be.vibration(vibration);
    backup::Fields fields(value, "sound", report);
    if (has_sound) {
        fields.number("volume", 0, 100, s.volume);
        fields.boolean("tap_sound", s.tap_sound);
        fields.boolean("slider_sound", s.slider_sound);
    }
    const bool has_switch = has_vibration && fields.boolean("vibration", vibration);
    fields.finish();
    if (apply && has_sound) {
        be.set_sound_settings(s);
    }
    if (apply && has_switch) {
        be.set_vibration(vibration);
    }
}

// ------------------------------------------------------------------------------------ alarm

// bit n of AlarmSettings::days is weekday n of std::tm, 0 = Sunday; written Monday first
constexpr std::string_view kDays[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
constexpr uint8_t kDayOrder[] = {1, 2, 3, 4, 5, 6, 0};

bool has_alarm()
{
    return ui_backend().alarm_settings != nullptr;
}

void write_alarm(backup::Writer &out)
{
    const hc::ui::Backend &be = ui_backend();
    hc::ui::AlarmSettings s{};
    {
        UiLock lock;
        be.alarm_settings(s);
    }
    char time[8];
    std::snprintf(time, sizeof(time), "%02u:%02u", s.hour, s.minute);
    std::string_view days[7];
    size_t count = 0;
    for (const uint8_t day : kDayOrder) {
        if (s.days & (1u << day)) {
            days[count++] = kDays[day];
        }
    }
    out.section("alarm");
    out.boolean("on", s.on);
    out.text("time", time);
    out.comment("no days: once, then the alarm turns itself off");
    out.flow("days", std::span(days, count));
    out.number("snooze_min", s.snooze_min);
    out.text("sound", s.sound);
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

void read_alarm(const Node &value, bool apply, backup::Report &report)
{
    const hc::ui::Backend &be = ui_backend();
    UiLock lock;
    hc::ui::AlarmSettings s{};
    be.alarm_settings(s);
    backup::Fields fields(value, "alarm", report);
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
            report.note("alarm", text_of("the sound %s is not on this device, the alarm beeps", sound.c_str()));
            sound.clear();
        }
        std::snprintf(s.sound, sizeof(s.sound), "%s", sound.c_str());
    }
    fields.finish();
    if (apply) {
        be.set_alarm_settings(s);
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
    {"hass", write_hass, read_hass},
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
