#include "hc/platform/time_sync.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <mutex>
#include <string_view>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "hc/platform/settings.hpp"

namespace hc::platform {

namespace {

constexpr const char *kTag = "time";
constexpr const char *kNamespace = "time";
constexpr const char *kKeyZone = "zone";
// any time before this means the clock was never set (2025-01-01)
constexpr time_t kValidAfter = 1735689600;
// IANA names are at most about 30 characters ("America/Argentina/ComodRivadavia")
constexpr size_t kZoneSize = 48;

struct Zone {
    std::string_view name;
    unsigned char rule;
};

#include "timezones.inc"

std::mutex g_mutex;
char g_zone[kZoneSize] = {};
bool g_sntp_ready = false;

const char *rule_of(std::string_view name)
{
    const auto *end = std::end(kZones);
    const auto *found = std::lower_bound(std::begin(kZones), end, name,
                                         [](const Zone &zone, std::string_view key) { return zone.name < key; });
    return found != end && found->name == name ? kRules[found->rule] : nullptr;
}

void apply(const char *rule)
{
    setenv("TZ", rule, 1);
    tzset();
}

void on_sync(struct timeval *)
{
    ESP_LOGI(kTag, "clock set");
}

// every new address: ask now, not after the backoff of the requests that failed without one
void on_got_ip(void *, esp_event_base_t, int32_t, void *)
{
    if (g_sntp_ready) {
        esp_netif_sntp_start();
    }
}

} // namespace

esp_err_t start_time_sync()
{
    {
        Settings settings(kNamespace, Settings::Mode::Read);
        char stored[kZoneSize] = {};
        if (settings.ok() && settings.get_string(kKeyZone, stored, sizeof(stored)) == ESP_OK) {
            if (const char *rule = rule_of(stored)) {
                std::lock_guard lock(g_mutex);
                std::snprintf(g_zone, sizeof(g_zone), "%s", stored);
                apply(rule);
                ESP_LOGI(kTag, "time zone %s (%s)", stored, rule);
            }
        }
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org",
                                                                                              "time.cloudflare.com"));
    config.start = false;
    config.sync_cb = on_sync;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        return err;
    }
    g_sntp_ready = true;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, nullptr);
    // the station may have its address already
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    if (sta != nullptr && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0) {
        esp_netif_sntp_start();
    }
    return err;
}

bool time_synced()
{
    return std::time(nullptr) > kValidAfter;
}

bool set_time_zone(const char *iana_name)
{
    const char *rule = rule_of(iana_name);
    if (rule == nullptr) {
        ESP_LOGW(kTag, "unknown time zone '%s', the clock stays as it is", iana_name);
        return false;
    }
    {
        std::lock_guard lock(g_mutex);
        if (std::strcmp(g_zone, iana_name) == 0) {
            return true;
        }
        std::snprintf(g_zone, sizeof(g_zone), "%s", iana_name);
        apply(rule);
    }
    ESP_LOGI(kTag, "time zone %s (%s)", iana_name, rule);
    Settings settings(kNamespace, Settings::Mode::ReadWrite);
    if (!settings.ok() || settings.set_string(kKeyZone, iana_name) != ESP_OK || settings.commit() != ESP_OK) {
        ESP_LOGW(kTag, "the time zone could not be stored");
    }
    return true;
}

void time_zone(char *out, size_t size)
{
    std::lock_guard lock(g_mutex);
    std::snprintf(out, size, "%s", g_zone);
}

} // namespace hc::platform
