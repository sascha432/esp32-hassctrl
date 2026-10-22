#include "hc/platform/time_sync.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "hc/core/timezone.hpp"
#include "hc/platform/settings.hpp"

namespace hc::platform {

namespace {

constexpr const char *kTag = "time";
constexpr const char *kNamespace = "time";
// Home Assistant's zone and the fixed one
constexpr const char *kKeyZone = "zone";
constexpr const char *kKeyFixed = "fixed";
constexpr const char *kKeyServers[kTimeServers] = {"server1", "server2"};
// any time before this means the clock was never set (2025-01-01)
constexpr time_t kValidAfter = 1735689600;
constexpr size_t kZoneSize = kTimeZoneSize;

// g_mutex guards the zones, the servers and the start and restart of SNTP
std::mutex g_mutex;
char g_hass_zone[kZoneSize] = {};
char g_fixed_zone[kZoneSize] = {};
// the zone in use: the fixed one, else Home Assistant's; g_rule its POSIX rule
char g_zone[kZoneSize] = {};
char g_rule[kZoneSize] = "UTC0";
// lwIP keeps pointers to the names: they stay here while SNTP runs
TimeServers g_servers;
bool g_sntp_ready = false;
std::atomic<time_t> g_last_sync{0};

// the rule of a valid zone: the table's for a name, else the zone is a rule itself
const char *rule_of(const char *zone)
{
    if (zone[0] == 0) {
        return "UTC0";
    }
    const char *rule = core::time_zone_rule(zone);
    return rule != nullptr ? rule : zone;
}

// The zone in use after a change of either, with g_mutex held. Every zone here is valid.
void apply_zone()
{
    const char *name = g_fixed_zone[0] != 0 ? g_fixed_zone : g_hass_zone;
    if (std::strcmp(name, g_zone) == 0) {
        return;
    }
    std::snprintf(g_zone, sizeof(g_zone), "%s", name);
    const char *rule = rule_of(name);
    std::snprintf(g_rule, sizeof(g_rule), "%s", rule);
    setenv("TZ", rule, 1);
    tzset();
    ESP_LOGI(kTag, "time zone %s (%s)%s", name[0] != 0 ? name : "UTC", rule, g_fixed_zone[0] != 0 ? ", fixed" : "");
}

// a name of the table (or with `rules` a POSIX rule as well), else empty
void load_zone(Settings &settings, const char *key, bool rules, char *out)
{
    if (settings.get_string(key, out, kZoneSize) != ESP_OK ||
        !(rules ? valid_time_zone(out) : core::time_zone_rule(out) != nullptr)) {
        out[0] = 0;
    }
}

// writes `value`, an empty one removes the key; with commit
esp_err_t store(const char *key, const char *value)
{
    Settings settings(kNamespace, Settings::Mode::ReadWrite);
    if (!settings.ok()) {
        return ESP_FAIL;
    }
    esp_err_t err = value[0] != 0 ? settings.set_string(key, value) : settings.erase(key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    return err == ESP_OK ? settings.commit() : err;
}

void on_sync(struct timeval *tv)
{
    g_last_sync = tv->tv_sec;
    ESP_LOGI(kTag, "clock set");
}

bool station_has_address()
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    return sta != nullptr && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0;
}

// SNTP with g_servers (or the defaults), asking at once when the station has an address. With
// g_mutex held.
esp_err_t init_sntp()
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(kDefaultTimeServers[0]);
    config.num_of_servers = 0;
    for (const char *name : g_servers.names) {
        if (name[0] != 0) {
            config.servers[config.num_of_servers++] = name;
        }
    }
    if (config.num_of_servers == 0) {
        for (const char *name : kDefaultTimeServers) {
            config.servers[config.num_of_servers++] = name;
        }
    }
    config.start = false;
    config.sync_cb = on_sync;
    const esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "SNTP did not start: %s", esp_err_to_name(err));
        return err;
    }
    g_sntp_ready = true;
    if (station_has_address()) {
        esp_netif_sntp_start();
    }
    return ESP_OK;
}

// every new address: ask now, not after the backoff of the requests that failed without one
void on_got_ip(void *, esp_event_base_t, int32_t, void *)
{
    std::lock_guard lock(g_mutex);
    if (g_sntp_ready) {
        esp_netif_sntp_start();
    }
}

// a host name or an IPv4 or IPv6 address: letters, digits, '.', '-' and ':'
bool valid_server(const char *name)
{
    if (name[0] == '-' || name[0] == '.') {
        return false;
    }
    for (const char *c = name; *c != 0; ++c) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                        *c == '.' || *c == '-' || *c == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

esp_err_t start_time_sync()
{
    std::lock_guard lock(g_mutex);
    {
        Settings settings(kNamespace, Settings::Mode::Read);
        if (settings.ok()) {
            load_zone(settings, kKeyZone, false, g_hass_zone);
            load_zone(settings, kKeyFixed, true, g_fixed_zone);
            for (size_t i = 0; i < kTimeServers; ++i) {
                if (settings.get_string(kKeyServers[i], g_servers.names[i], kTimeServerSize) != ESP_OK ||
                    !valid_server(g_servers.names[i])) {
                    g_servers.names[i][0] = 0;
                }
            }
        }
        apply_zone();
    }
    const esp_err_t err = init_sntp();
    if (err != ESP_OK) {
        return err;
    }
    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, nullptr);
}

bool time_synced()
{
    return std::time(nullptr) > kValidAfter;
}

std::time_t last_time_sync()
{
    return g_last_sync;
}

bool set_hass_time_zone(const char *iana_name)
{
    if (core::time_zone_rule(iana_name) == nullptr) {
        ESP_LOGW(kTag, "unknown time zone '%s', the clock stays as it is", iana_name);
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (std::strcmp(g_hass_zone, iana_name) == 0) {
        return true;
    }
    std::snprintf(g_hass_zone, sizeof(g_hass_zone), "%s", iana_name);
    apply_zone();
    if (store(kKeyZone, iana_name) != ESP_OK) {
        ESP_LOGW(kTag, "the time zone could not be stored");
    }
    return true;
}

bool valid_time_zone(const char *zone)
{
    core::TzRule rule;
    return strnlen(zone, kZoneSize) < kZoneSize &&
           (core::time_zone_rule(zone) != nullptr || core::parse_tz_rule(zone, rule));
}

bool set_fixed_time_zone(const char *zone)
{
    if (zone[0] != 0 && !valid_time_zone(zone)) {
        ESP_LOGW(kTag, "unknown time zone '%s', the clock stays as it is", zone);
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (std::strcmp(g_fixed_zone, zone) == 0) {
        return true;
    }
    std::snprintf(g_fixed_zone, sizeof(g_fixed_zone), "%s", zone);
    apply_zone();
    if (store(kKeyFixed, zone) != ESP_OK) {
        ESP_LOGW(kTag, "the fixed time zone could not be stored");
    }
    return true;
}

void time_zone(char *out, size_t size)
{
    std::lock_guard lock(g_mutex);
    std::snprintf(out, size, "%s", g_zone);
}

void time_zone_rule(char *out, size_t size)
{
    std::lock_guard lock(g_mutex);
    std::snprintf(out, size, "%s", g_rule);
}

void fixed_time_zone(char *out, size_t size)
{
    std::lock_guard lock(g_mutex);
    std::snprintf(out, size, "%s", g_fixed_zone);
}

void hass_time_zone(char *out, size_t size)
{
    std::lock_guard lock(g_mutex);
    std::snprintf(out, size, "%s", g_hass_zone);
}

TimeServers time_servers()
{
    std::lock_guard lock(g_mutex);
    return g_servers;
}

bool check_time_servers(const TimeServers &servers, char *error, size_t error_size)
{
    for (size_t i = 0; i < kTimeServers; ++i) {
        const char *name = servers.names[i];
        if (strnlen(name, kTimeServerSize) == kTimeServerSize) {
            std::snprintf(error, error_size, "server %u: at most %u characters", static_cast<unsigned>(i + 1),
                          static_cast<unsigned>(kTimeServerSize - 1));
            return false;
        }
        if (!valid_server(name)) {
            std::snprintf(error, error_size, "server %u: a host name or an address (letters, digits, '.', '-', ':')",
                          static_cast<unsigned>(i + 1));
            return false;
        }
    }
    return true;
}

bool set_time_servers(const TimeServers &servers, char *error, size_t error_size)
{
    if (!check_time_servers(servers, error, error_size)) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    if (std::memcmp(&servers, &g_servers, sizeof(servers)) == 0) {
        return true;
    }
    Settings settings(kNamespace, Settings::Mode::ReadWrite);
    esp_err_t err = settings.ok() ? ESP_OK : ESP_FAIL;
    for (size_t i = 0; i < kTimeServers && err == ESP_OK; ++i) {
        err = servers.names[i][0] != 0 ? settings.set_string(kKeyServers[i], servers.names[i])
                                       : settings.erase(kKeyServers[i]);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = settings.commit();
    }
    if (err != ESP_OK) {
        std::snprintf(error, error_size, "the servers could not be stored: %s", esp_err_to_name(err));
        return false;
    }
    // lwIP reads the names while it runs: stopped before they change
    if (g_sntp_ready) {
        esp_netif_sntp_deinit();
        g_sntp_ready = false;
    }
    g_servers = servers;
    ESP_LOGI(kTag, "servers '%s' '%s'", g_servers.names[0], g_servers.names[1]);
    // the clock keeps its time until a server answers
    if (init_sntp() != ESP_OK) {
        std::snprintf(error, error_size, "stored, but SNTP did not start again");
        return false;
    }
    return true;
}

} // namespace hc::platform
