#include "store.hpp"

#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "hc/platform/settings.hpp"

namespace hc::network::detail {

namespace {

constexpr const char *kTag = "network";
constexpr const char *kNamespace = "network";
constexpr const char *kKeyNetworks = "networks";
constexpr const char *kKeyPolicy = "policy";
constexpr const char *kKeyHotspotPassword = "ap_password";
constexpr const char *kKeyHotspotEnabled = "ap_enabled";
constexpr const char *kKeyLastSeen = "last_seen";
constexpr const char *kKeyLastIndex = "last_index";

using hc::platform::Settings;

} // namespace

void generate_hotspot_password(char (&out)[12])
{
    // letters that cannot be mistaken for digits (no i, l, o)
    static constexpr char kLetters[] = "abcdefghjkmnpqrstuvwxyz";
    for (int i = 0; i < 8; i++) {
        out[i] = static_cast<char>('0' + esp_random() % 10);
    }
    for (int i = 8; i < 10; i++) {
        out[i] = kLetters[esp_random() % (sizeof(kLetters) - 1)];
    }
    out[10] = 0;
    out[11] = 0;
}

esp_err_t load(Stored &stored)
{
    stored = {};
    stored.policy = Policy::EnteredOrder;
    stored.last_index = kNoIndex;

    {
        Settings settings(kNamespace, Settings::Mode::Read);
        if (settings.ok()) {
            size_t size = sizeof(stored.networks);
            if (settings.get_blob(kKeyNetworks, stored.networks, size) == ESP_OK) {
                stored.count = static_cast<uint8_t>(size / sizeof(SavedNetwork));
            }
            uint8_t value = 0;
            if (settings.get_u8(kKeyPolicy, value) == ESP_OK && value <= static_cast<uint8_t>(Policy::StrongestSignal)) {
                stored.policy = static_cast<Policy>(value);
            }
            if (settings.get_u8(kKeyHotspotEnabled, value) == ESP_OK) {
                stored.hotspot_enabled = value != 0;
            }
            settings.get_string(kKeyHotspotPassword, stored.hotspot_password, sizeof(stored.hotspot_password));
            // only valid when it matches the saved networks
            size = sizeof(stored.last_seen);
            if (settings.get_blob(kKeyLastSeen, stored.last_seen, size) != ESP_OK ||
                size != stored.count * sizeof(LastSeen)) {
                std::memset(stored.last_seen, 0, sizeof(stored.last_seen));
            }
            if (settings.get_u8(kKeyLastIndex, value) == ESP_OK && value < stored.count) {
                stored.last_index = value;
            }
        }
    }

    if (std::strlen(stored.hotspot_password) != 10) {
        generate_hotspot_password(stored.hotspot_password);
        ESP_LOGI(kTag, "new hotspot password generated");
        ESP_RETURN_ON_ERROR(save_hotspot(stored), kTag, "save hotspot password");
    }
    return ESP_OK;
}

esp_err_t save_networks(const Stored &stored)
{
    Settings settings(kNamespace, Settings::Mode::ReadWrite);
    esp_err_t err = (stored.count == 0)
                        ? settings.erase(kKeyNetworks)
                        : settings.set_blob(kKeyNetworks, stored.networks, stored.count * sizeof(SavedNetwork));
    return (err == ESP_OK) ? settings.commit() : err;
}

esp_err_t save_last_seen(const Stored &stored)
{
    Settings settings(kNamespace, Settings::Mode::ReadWrite);
    esp_err_t err = (stored.count == 0)
                        ? settings.erase(kKeyLastSeen)
                        : settings.set_blob(kKeyLastSeen, stored.last_seen, stored.count * sizeof(LastSeen));
    if (err == ESP_OK) {
        err = settings.set_u8(kKeyLastIndex, stored.last_index);
    }
    return (err == ESP_OK) ? settings.commit() : err;
}

esp_err_t save_policy(const Stored &stored)
{
    Settings settings(kNamespace, Settings::Mode::ReadWrite);
    const esp_err_t err = settings.set_u8(kKeyPolicy, static_cast<uint8_t>(stored.policy));
    return (err == ESP_OK) ? settings.commit() : err;
}

esp_err_t save_hotspot(const Stored &stored)
{
    Settings settings(kNamespace, Settings::Mode::ReadWrite);
    esp_err_t err = settings.set_string(kKeyHotspotPassword, stored.hotspot_password);
    if (err == ESP_OK) {
        err = settings.set_u8(kKeyHotspotEnabled, stored.hotspot_enabled ? 1 : 0);
    }
    return (err == ESP_OK) ? settings.commit() : err;
}

} // namespace hc::network::detail
