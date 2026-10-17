// The station of the network task (docs/architecture.md 9.1): scans, the choice of a saved
// network, connecting, the retries and roaming to a better access point.

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"

#include "reasons.hpp"
#include "task.hpp"

namespace hc::network::detail {

namespace {

constexpr uint32_t kRetryMaxMs = 60000;
constexpr uint32_t kRssiCheckMs = 5000;
// the hotspot of the first setup stays on this long after the first connection
constexpr uint32_t kSetupHotspotMs = 15 * 60 * 1000;
// failed attempts of the first setup (not a wrong password) before the hotspot comes back
constexpr uint8_t kSetupFailures = 3;

} // namespace

void start_scan(ScanPurpose purpose)
{
    if (t.scan != ScanPurpose::None) {
        // a scan is running, its results serve this purpose as well where it matters
        if (purpose == ScanPurpose::Connect) {
            t.scan = ScanPurpose::Connect;
        }
        return;
    }
    wifi_scan_config_t config{};
    config.show_hidden = false;
    const esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "scan not started: %s", esp_err_to_name(err));
        if (purpose == ScanPurpose::Connect) {
            t.status.state = StaState::WaitingToRetry;
            t.retry_at = now() + pdMS_TO_TICKS(t.retry_delay_ms);
        }
        return;
    }
    t.scan = purpose;
    if (purpose == ScanPurpose::Connect) {
        t.status.state = StaState::Scanning;
    }
}

namespace {

void connect_to(const Candidate &candidate);

// The network of the last connection, at its last access point and channel, when that is known
// and has not failed in this round.
bool fast_candidate(Candidate &out)
{
    const uint8_t index = t.stored.last_index;
    if (!t.fast_allowed || t.fast_failed || index >= t.stored.count || (t.failed & (1u << index)) ||
        t.stored.networks[index].password_needed) {
        return false;
    }
    const LastSeen &seen = t.stored.last_seen[index];
    if (seen.channel == 0) {
        return false;
    }
    out = {};
    out.index = index;
    std::memcpy(out.bssid, seen.bssid, sizeof(out.bssid));
    out.channel = seen.channel;
    return true;
}

} // namespace

void start_connect()
{
    if (usable_count() == 0) {
        t.status.state = StaState::NoNetworks;
        t.status.ssid[0] = 0;
        std::snprintf(t.status.error, sizeof(t.status.error), "%s",
                      t.stored.count == 0 ? "" : "every saved network needs its password");
        return;
    }
    t.connect_started = now();
    Candidate candidate{};
    if (t.prefer < 0 && fast_candidate(candidate)) {
        t.fast_attempt = true;
        connect_to(candidate);
        return;
    }
    start_scan(ScanPurpose::Connect);
}

namespace {

// A failed attempt during the first setup: the hotspot comes back, at once for a wrong password
void setup_attempt_failed(bool wrong_password)
{
    if (t.setup_hotspot && !t.setup_failed && (wrong_password || ++t.setup_failures >= kSetupFailures)) {
        t.setup_failed = true;
        ESP_LOGI(kTag, "first setup: the network failed, the hotspot comes back");
    }
}

void schedule_retry()
{
    t.status.state = StaState::WaitingToRetry;
    t.status.ip[0] = 0;
    t.retry_at = now() + pdMS_TO_TICKS(t.retry_delay_ms);
    ESP_LOGI(kTag, "retry in %lu s", static_cast<unsigned long>(t.retry_delay_ms / 1000));
    t.retry_delay_ms = std::min(t.retry_delay_ms * 2, kRetryMaxMs);
}

void connect_to(const Candidate &candidate)
{
    const SavedNetwork &network = t.stored.networks[candidate.index];
    wifi_config_t config{};
    copy_field(config.sta.ssid, network.ssid, sizeof(network.ssid));
    copy_field(config.sta.password, network.password, sizeof(network.password));
    // straight to the access point the scan found: no second scan
    config.sta.bssid_set = true;
    std::memcpy(config.sta.bssid, candidate.bssid, sizeof(config.sta.bssid));
    config.sta.channel = candidate.channel;
    // a network with a password needs at least WPA2 (WPA3 qualifies); open only without one
    config.sta.threshold.authmode = (strnlen(network.password, sizeof(network.password)) >= 8) ? WIFI_AUTH_WPA2_PSK
                                                                                            : WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    t.current = candidate.index;
    std::memcpy(t.current_bssid, candidate.bssid, sizeof(t.current_bssid));
    copy_string(t.status.ssid, sizeof(t.status.ssid), network.ssid);
    t.status.rssi = candidate.rssi;
    t.status.channel = candidate.channel;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "connect to '%s' failed: %s", network.ssid, esp_err_to_name(err));
        set_error("connection failed");
        setup_attempt_failed(false);
        schedule_retry();
        return;
    }
    t.status.state = StaState::Connecting;
    if (t.fast_attempt) {
        ESP_LOGI(kTag, "connecting to '%s' at its last access point (channel %u), no scan", network.ssid,
                 candidate.channel);
    } else {
        ESP_LOGI(kTag, "connecting to '%s' (%d dBm, channel %u)", network.ssid, candidate.rssi, candidate.channel);
    }
}

} // namespace

// Disconnects on purpose; the disconnect event then starts the next connection.
void disconnect_on_purpose()
{
    t.expect_disconnect = true;
    if (esp_wifi_disconnect() != ESP_OK) {
        t.expect_disconnect = false;
        start_connect();
    }
}

void on_scan_done()
{
    uint16_t count = kMaxScan;
    if (esp_wifi_scan_get_ap_records(&count, t.records) != ESP_OK) {
        count = 0;
    }
    std::sort(t.records, t.records + count,
              [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) { return a.rssi > b.rssi; });

    SeenAp seen[kMaxScan];
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_scan_count = count;
    for (size_t i = 0; i < count; i++) {
        const auto &record = t.records[i];
        // record.ssid has room for the terminator (33 bytes)
        copy_string(seen[i].ssid, kSsidSize, reinterpret_cast<const char *>(record.ssid));
        std::memcpy(seen[i].bssid, record.bssid, sizeof(seen[i].bssid));
        seen[i].rssi = record.rssi;
        seen[i].channel = record.primary;
        ESP_LOGD(kTag, "  '%s' " MACSTR " %d dBm ch %u auth %d pairwise %d group %d", seen[i].ssid,
                 MAC2STR(record.bssid), record.rssi, record.primary, record.authmode, record.pairwise_cipher,
                 record.group_cipher);

        ScanEntry &entry = g_scan[i];
        copy_string(entry.ssid, kSsidSize, seen[i].ssid);
        entry.rssi = record.rssi;
        entry.channel = record.primary;
        entry.secure = record.authmode != WIFI_AUTH_OPEN;
        entry.saved = false; // set when the results are read, see last_scan()
    }
    xSemaphoreGive(g_lock);

    const ScanPurpose purpose = t.scan;
    t.scan = ScanPurpose::None;
    ESP_LOGI(kTag, "scan: %u access points", count);

    if (purpose == ScanPurpose::Connect && t.prefer >= 0) {
        // the network the user just chose: its strongest access point, else the usual choice
        const char *ssid = t.stored.networks[t.prefer].ssid;
        const int prefer = t.prefer;
        t.prefer = -1;
        for (size_t i = 0; i < count; i++) {
            if (std::strcmp(seen[i].ssid, ssid) == 0) {
                Candidate candidate{};
                candidate.index = static_cast<uint8_t>(prefer);
                std::memcpy(candidate.bssid, seen[i].bssid, sizeof(candidate.bssid));
                candidate.rssi = seen[i].rssi;
                candidate.channel = seen[i].channel;
                connect_to(candidate);
                return;
            }
        }
        ESP_LOGW(kTag, "'%s' is not in range", ssid);
    }
    if (purpose == ScanPurpose::Connect) {
        Candidate candidate{};
        if (choose(t.stored.networks, t.stored.count, seen, count, t.stored.policy, t.failed, candidate)) {
            if (t.failed & (1u << candidate.index)) {
                // every saved network in range failed: a new round in the configured order
                ESP_LOGI(kTag, "every saved network in range failed, starting over");
                t.failed = 0;
                t.retried = 0;
                choose(t.stored.networks, t.stored.count, seen, count, t.stored.policy, 0, candidate);
            }
            connect_to(candidate);
        } else {
            set_error("no saved network in range");
            setup_attempt_failed(false);
            schedule_retry();
        }
    } else if (purpose == ScanPurpose::Roam && t.status.state == StaState::Connected) {
        Candidate candidate{};
        if (choose(t.stored.networks, t.stored.count, seen, count, t.stored.policy, 0, candidate) &&
            std::memcmp(candidate.bssid, t.current_bssid, sizeof(t.current_bssid)) != 0 &&
            candidate.rssi >= t.status.rssi + kRoamMarginDb) {
            ESP_LOGI(kTag, "roaming to '%s' (%d dBm, now %d dBm)", t.stored.networks[candidate.index].ssid,
                     candidate.rssi, t.status.rssi);
            t.switch_pending = true;
            t.switch_to = candidate;
            t.expect_disconnect = true;
            esp_wifi_disconnect();
        }
    }
}

void on_disconnected(uint8_t reason)
{
    t.status.ip[0] = 0;
    if (g_restarting) {
        return;
    }
    if (t.switch_pending) {
        t.switch_pending = false;
        t.expect_disconnect = false;
        connect_to(t.switch_to);
        return;
    }
    if (t.expect_disconnect) {
        t.expect_disconnect = false;
        start_connect();
        return;
    }
    if (t.status.state != StaState::Connected && t.status.state != StaState::Connecting) {
        return;
    }
    if (t.fast_attempt && t.status.state == StaState::Connecting) {
        // the access point moved or is gone: look for it with a scan right away
        ESP_LOGI(kTag, "fast reconnect to '%s' failed (%s), scanning", t.status.ssid, reason_text(reason));
        t.fast_attempt = false;
        t.fast_failed = true;
        t.fast_allowed = false;
        start_scan(ScanPurpose::Connect);
        return;
    }
    // A failed attempt (wrong password, "network security not supported", no answer) moves on to
    // the next saved network; a connection that was up and broke may be tried again first. A
    // router that still holds the old connection of this device gets one more try first, else
    // a power cut moves the device to the next network for good. That first try is expected
    // after a reset and is no error for the display.
    bool expected = false;
    if (t.status.state == StaState::Connecting && t.current >= 0) {
        const uint32_t bit = 1u << t.current;
        if (may_pass_on_retry(reason) && !(t.retried & bit)) {
            t.retried |= bit;
            t.prefer = t.current;
            expected = true;
        } else {
            t.failed |= bit;
        }
    }
    if (expected) {
        ESP_LOGI(kTag, "'%s': %s (reason %u), trying again", t.status.ssid, reason_text(reason), reason);
    } else {
        // saved without a password: the device looked for an open network, the network has security
        const bool no_password = reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY && t.current >= 0 &&
                                 t.stored.networks[t.current].password[0] == 0;
        set_error(no_password ? "the network needs a password, add it again" : reason_text(reason));
        ESP_LOGW(kTag, "'%s': %s (reason %u)", t.status.ssid, t.status.error, reason);
        setup_attempt_failed(is_auth_failure(reason));
    }
    schedule_retry();
}

namespace {

// Stores the access point and channel of the connection for the next fast reconnect, only when
// they changed.
void remember_access_point()
{
    wifi_ap_record_t ap{};
    if (t.current < 0 || t.current >= t.stored.count || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return;
    }
    LastSeen &seen = t.stored.last_seen[t.current];
    t.status.channel = ap.primary;
    if (t.stored.last_index == t.current && seen.channel == ap.primary &&
        std::memcmp(seen.bssid, ap.bssid, sizeof(seen.bssid)) == 0) {
        return;
    }
    std::memcpy(seen.bssid, ap.bssid, sizeof(seen.bssid));
    seen.channel = ap.primary;
    t.stored.last_index = static_cast<uint8_t>(t.current);
    save_last_seen(t.stored);
}

} // namespace

void on_got_ip(uint32_t ip)
{
    esp_ip4_addr_t address{.addr = ip};
    snprintf(t.status.ip, sizeof(t.status.ip), IPSTR, IP2STR(&address));
    t.status.state = StaState::Connected;
    t.status.error[0] = 0;
    t.status.connect_ms = (now() - t.connect_started) * portTICK_PERIOD_MS;
    t.status.connect_fast = t.fast_attempt;
    t.retry_delay_ms = kRetryMinMs;
    t.failed = 0;
    t.retried = 0;
    t.setup_failed = false;
    t.setup_failures = 0;
    t.fast_attempt = false;
    t.fast_failed = false;
    t.fast_allowed = false;
    t.weak_since = 0;
    t.rssi_check_at = now() + pdMS_TO_TICKS(kRssiCheckMs);
    ESP_LOGI(kTag, "connected to '%s' as %s in %lu ms (%s)", t.status.ssid, t.status.ip,
             static_cast<unsigned long>(t.status.connect_ms), t.status.connect_fast ? "fast" : "with scan");
    remember_access_point();

    // the setup is done: its hotspot stays on for a while, so the phone can see the result
    if (t.setup_hotspot) {
        t.setup_hotspot = false;
        t.setup_hotspot_until = now() + pdMS_TO_TICKS(kSetupHotspotMs);
        ESP_LOGI(kTag, "setup hotspot stays on for %lu minutes", static_cast<unsigned long>(kSetupHotspotMs / 60000));
    }
}

void check_signal()
{
    t.rssi_check_at = now() + pdMS_TO_TICKS(kRssiCheckMs);
    int rssi = 0;
    if (esp_wifi_sta_get_rssi(&rssi) != ESP_OK) {
        return;
    }
    t.status.rssi = static_cast<int8_t>(rssi);
    if (rssi >= kWeakRssi) {
        t.weak_since = 0;
        return;
    }
    if (t.weak_since == 0) {
        t.weak_since = now();
    } else if (now() - t.weak_since >= pdMS_TO_TICKS(kWeakForMs)) {
        ESP_LOGI(kTag, "signal weak (%d dBm) for %lu s, looking for a better access point", rssi,
                 static_cast<unsigned long>(kWeakForMs / 1000));
        t.weak_since = now();
        start_scan(ScanPurpose::Roam);
    }
}

} // namespace hc::network::detail
