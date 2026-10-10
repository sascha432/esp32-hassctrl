// The network task (see hc/network/network.hpp). Commands from other tasks and the events of the
// Wi-Fi driver arrive in one queue; only this task touches the driver and the state. Readers get
// copies of the status and the scan results under a short lock.

#include "hc/network/network.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"

#include "merge.hpp"
#include "reasons.hpp"
#include "selection.hpp"
#include "store.hpp"

namespace hc::network {

namespace {

using namespace detail;

constexpr const char *kTag = "network";

constexpr size_t kMaxScan = 20;
// short first: a router that still holds the connection of a reset device (WPA3 with PMF) lets
// it back in after a few seconds
constexpr uint32_t kRetryMinMs = 1000;
constexpr uint32_t kRetryMaxMs = 60000;
constexpr uint32_t kRssiCheckMs = 5000;
// the hotspot of the first setup stays on this long after the first connection
constexpr uint32_t kSetupHotspotMs = 15 * 60 * 1000;
constexpr uint8_t kHotspotMaxClients = 4;

enum class MsgType : uint8_t {
    Add,
    Remove,
    Move,
    SetPolicy,
    Reconnect,
    Scan,
    Hotspot,
    NewHotspotPassword,
    Import,
    WifiEvent,
    GotIp,
};

struct Msg {
    MsgType type;
    int32_t event;
    uint8_t index;
    int8_t value;
    uint8_t reason;
    uint32_t ip;
    char ssid[kSsidSize];
    char password[kPasswordSize];
    // Import: the caller's ImportJob, which waits for it
    void *data;
};

struct ImportJob {
    const ImportedNetwork *networks;
    size_t count;
    bool apply;
    ImportResult *result;
    SemaphoreHandle_t done;
};

enum class ScanPurpose : uint8_t {
    None,
    Connect,
    Roam,
    User,
};

// owned by the network task
struct Task {
    Stored stored{};
    Status status{};
    esp_netif_t *sta = nullptr;
    esp_netif_t *ap = nullptr;
    const char *device_name = "";
    ScanPurpose scan = ScanPurpose::None;
    uint32_t retry_delay_ms = kRetryMinMs;
    TickType_t retry_at = 0;
    TickType_t rssi_check_at = 0;
    TickType_t weak_since = 0;
    // the hotspot of the first setup: on until this tick, 0 = no timer
    TickType_t setup_hotspot_until = 0;
    // the hotspot is on because no network was saved when the setup started
    bool setup_hotspot = false;
    // saved networks whose connection attempt failed in this round, for any reason (bit per
    // index): the next attempt takes another saved network in range. A round ends with a
    // connection, or when every saved network in range has failed.
    uint32_t failed = 0;
    // saved networks that already had their second try in this round (see may_pass_on_retry)
    uint32_t retried = 0;
    // index and access point of the network in use or being tried
    int current = -1;
    uint8_t current_bssid[6] = {};
    // a disconnect that was asked for: no error, the next step follows
    bool expect_disconnect = false;
    // roaming: connect to this one as soon as the old connection is gone
    bool switch_pending = false;
    // the running attempt goes straight to the last access point, without a scan
    bool fast_attempt = false;
    // the fast attempt failed in this round: scan instead
    bool fast_failed = false;
    // Only the first connection after waking from deep sleep may skip the scan. A normal boot and
    // a lost connection scan, so the network order is followed (the user's decision).
    bool fast_allowed = false;
    // start of the running connection attempt, including its scan
    TickType_t connect_started = 0;
    // the next connection scan tries this saved network first, once (add_network connect_now)
    int prefer = -1;
    Candidate switch_to{};
    wifi_ap_record_t records[kMaxScan];
};

Task t;

QueueHandle_t g_queue = nullptr;
// set by the shutdown handler: the disconnect of a restart is not a lost connection
volatile bool g_restarting = false;
SemaphoreHandle_t g_lock = nullptr;
Listener g_listener = nullptr;
void *g_listener_context = nullptr;

// copies for other tasks, guarded by g_lock
Status g_status{};
TickType_t g_setup_hotspot_until = 0;
SavedNetworkInfo g_saved[kMaxNetworks] = {};
size_t g_saved_count = 0;
ScanEntry g_scan[kMaxScan] = {};
size_t g_scan_count = 0;

TickType_t now()
{
    return xTaskGetTickCount();
}

bool reached(TickType_t deadline)
{
    return static_cast<int32_t>(now() - deadline) >= 0;
}

// copies at most size - 1 characters and always terminates (SSIDs from the driver are not
// terminated when they use all 32 bytes)
void copy_string(char *out, size_t size, const char *in)
{
    const size_t length = strnlen(in, size - 1);
    std::memcpy(out, in, length);
    out[length] = 0;
}

// Fills a fixed-size field of the Wi-Fi driver. Those need no terminator: an SSID may use all 32
// bytes and a raw key all 64.
template <size_t N>
void copy_field(uint8_t (&field)[N], const char *in, size_t in_size)
{
    const size_t length = strnlen(in, std::min(N, in_size));
    std::memcpy(field, in, length);
    std::memset(field + length, 0, N - length);
}

void publish()
{
    t.status.saved_count = t.stored.count;
    t.status.policy = t.stored.policy;
    t.status.hotspot_enabled = t.stored.hotspot_enabled;
    copy_string(t.status.hotspot_password, sizeof(t.status.hotspot_password), t.stored.hotspot_password);

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_status = t.status;
    g_setup_hotspot_until = t.setup_hotspot_until;
    g_saved_count = t.stored.count;
    for (size_t i = 0; i < t.stored.count; i++) {
        const SavedNetwork &network = t.stored.networks[i];
        copy_string(g_saved[i].ssid, kSsidSize, network.ssid);
        g_saved[i].open = network.password[0] == 0 && !network.password_needed;
        g_saved[i].password_needed = network.password_needed;
    }
    xSemaphoreGive(g_lock);

    if (g_listener != nullptr) {
        g_listener(t.status, g_listener_context);
    }
}

void set_error(const char *text)
{
    copy_string(t.status.error, sizeof(t.status.error), text);
}

// ----------------------------------------------------------------------------------- hotspot

esp_err_t configure_hotspot()
{
    wifi_config_t config{};
    copy_field(config.ap.ssid, t.device_name, kSsidSize);
    config.ap.ssid_len = static_cast<uint8_t>(strnlen(t.device_name, sizeof(config.ap.ssid)));
    copy_field(config.ap.password, t.stored.hotspot_password, sizeof(t.stored.hotspot_password));
    config.ap.max_connection = kHotspotMaxClients;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    // the station decides the channel once it is connected
    config.ap.channel = 1;
    return esp_wifi_set_config(WIFI_IF_AP, &config);
}

// saved networks that may be tried (not waiting for their password)
size_t usable_count()
{
    size_t count = 0;
    for (size_t i = 0; i < t.stored.count; i++) {
        count += t.stored.networks[i].password_needed ? 0 : 1;
    }
    return count;
}

// without a network to try, the hotspot is the way to set one up, as at the first setup
bool hotspot_wanted()
{
    return usable_count() == 0 || t.stored.hotspot_enabled || t.setup_hotspot_until != 0;
}

void update_hotspot()
{
    const bool wanted = hotspot_wanted();
    if (wanted == t.status.hotspot_on) {
        return;
    }
    if (wanted) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        configure_hotspot();
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        t.status.hotspot_clients = 0;
    }
    t.status.hotspot_on = wanted;
    ESP_LOGI(kTag, "hotspot %s", wanted ? "on" : "off");
}

// --------------------------------------------------------------------------------- station

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
        entry.saved = false;
        for (size_t n = 0; n < t.stored.count; n++) {
            entry.saved |= std::strcmp(entry.ssid, t.stored.networks[n].ssid) == 0;
        }
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
    }
    schedule_retry();
}

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

// ---------------------------------------------------------------------------------- commands

void reset_round()
{
    t.failed = 0;
    t.retried = 0;
    t.fast_failed = false;
    t.retry_delay_ms = kRetryMinMs;
}

void cmd_add(const Msg &msg)
{
    size_t index = t.stored.count;
    for (size_t i = 0; i < t.stored.count; i++) {
        if (std::strcmp(t.stored.networks[i].ssid, msg.ssid) == 0) {
            index = i;
        }
    }
    if (index == kMaxNetworks) {
        ESP_LOGW(kTag, "already %u networks saved", static_cast<unsigned>(kMaxNetworks));
        return;
    }
    // A saved network chosen again with an empty password field keeps its password: the setup
    // pages offer the saved networks in their list, and "connect" there wiped the password. One
    // that needs its password has none to keep: empty means an open network.
    const bool keep_password =
        index < t.stored.count && msg.password[0] == 0 && !t.stored.networks[index].password_needed;
    if (index == t.stored.count) {
        t.stored.count++;
    }
    copy_string(t.stored.networks[index].ssid, kSsidSize, msg.ssid);
    if (!keep_password) {
        copy_string(t.stored.networks[index].password, kPasswordSize, msg.password);
        t.stored.networks[index].password_needed = false;
    }
    save_networks(t.stored);
    ESP_LOGI(kTag, "network '%s' saved as %u%s", msg.ssid, static_cast<unsigned>(index + 1),
             keep_password ? ", password kept" : "");

    if (msg.value != 0) {
        // connect to it now: it goes first in the next round, whatever the order says
        reset_round();
        t.prefer = static_cast<int>(index);
        if (t.status.state == StaState::Connected || t.status.state == StaState::Connecting) {
            disconnect_on_purpose();
        } else if (t.scan != ScanPurpose::Connect) {
            start_connect();
        }
        return;
    }
    if (t.status.state != StaState::Connected && t.status.state != StaState::Connecting) {
        reset_round();
        start_connect();
    }
}

void cmd_remove(uint8_t index)
{
    if (index >= t.stored.count) {
        return;
    }
    t.prefer = -1;
    const bool in_use = (index == t.current) &&
                        (t.status.state == StaState::Connected || t.status.state == StaState::Connecting);
    for (size_t i = index; i + 1 < t.stored.count; i++) {
        t.stored.networks[i] = t.stored.networks[i + 1];
        t.stored.last_seen[i] = t.stored.last_seen[i + 1];
    }
    t.stored.count--;
    t.stored.networks[t.stored.count] = {};
    t.stored.last_seen[t.stored.count] = {};
    if (t.stored.last_index == index) {
        t.stored.last_index = kNoIndex;
    } else if (t.stored.last_index != kNoIndex && t.stored.last_index > index) {
        t.stored.last_index--;
    }
    save_networks(t.stored);
    save_last_seen(t.stored);
    reset_round();
    if (t.current == index) {
        t.current = -1;
    } else if (t.current > index) {
        t.current--;
    }
    if (usable_count() == 0) {
        // back to the first setup: the hotspot comes on
        t.setup_hotspot = true;
    }
    if (in_use) {
        disconnect_on_purpose();
    } else if (t.stored.count == 0) {
        start_connect();
    }
}

void cmd_move(uint8_t index, int8_t delta)
{
    const int target = index + delta;
    if (index >= t.stored.count || target < 0 || target >= t.stored.count) {
        return;
    }
    t.prefer = -1;
    std::swap(t.stored.networks[index], t.stored.networks[target]);
    std::swap(t.stored.last_seen[index], t.stored.last_seen[target]);
    if (t.stored.last_index == index) {
        t.stored.last_index = static_cast<uint8_t>(target);
    } else if (t.stored.last_index == target) {
        t.stored.last_index = index;
    }
    if (t.current == index) {
        t.current = target;
    } else if (t.current == target) {
        t.current = index;
    }
    save_networks(t.stored);
    save_last_seen(t.stored);
}

void cmd_import(const ImportJob &job)
{
    const bool in_use = t.status.state == StaState::Connected || t.status.state == StaState::Connecting;
    static Merged merged;
    merge(t.stored.networks, t.stored.count, in_use ? t.current : -1, job.networks, job.count, merged, *job.result);
    if (!job.apply) {
        return;
    }

    // the access points, the network of the last connection and the one in use move along
    LastSeen last_seen[kMaxNetworks] = {};
    uint8_t last_index = kNoIndex;
    int current = -1;
    for (size_t i = 0; i < merged.count; i++) {
        const uint8_t from = merged.from[i];
        if (from == kNewNetwork) {
            continue;
        }
        last_seen[i] = t.stored.last_seen[from];
        last_index = (t.stored.last_index == from) ? static_cast<uint8_t>(i) : last_index;
        current = (t.current == from) ? static_cast<int>(i) : current;
    }
    for (size_t i = 0; i < kMaxNetworks; i++) {
        t.stored.networks[i] = (i < merged.count) ? merged.networks[i] : SavedNetwork{};
    }
    t.stored.count = merged.count;
    std::memcpy(t.stored.last_seen, last_seen, sizeof(last_seen));
    t.stored.last_index = last_index;
    t.current = current;
    t.prefer = -1;
    save_networks(t.stored);
    save_last_seen(t.stored);
    ESP_LOGI(kTag, "import: %u networks saved, %u need their password, %u cut off", merged.count,
             job.result->need_password_count, job.result->dropped_count);

    reset_round();
    if (usable_count() == 0) {
        t.setup_hotspot = true;
    }
    if (!in_use) {
        start_connect();
    }
}

void cmd_reconnect()
{
    reset_round();
    if (t.status.state == StaState::Connected || t.status.state == StaState::Connecting) {
        disconnect_on_purpose();
    } else {
        start_connect();
    }
}

void handle_wifi_event(int32_t event, uint8_t reason)
{
    switch (event) {
    case WIFI_EVENT_STA_START:
        start_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        on_disconnected(reason);
        break;
    case WIFI_EVENT_SCAN_DONE:
        on_scan_done();
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        t.status.hotspot_clients++;
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        if (t.status.hotspot_clients > 0) {
            t.status.hotspot_clients--;
        }
        break;
    default:
        return;
    }
}

void handle(const Msg &msg)
{
    switch (msg.type) {
    case MsgType::Add:
        cmd_add(msg);
        break;
    case MsgType::Remove:
        cmd_remove(msg.index);
        break;
    case MsgType::Move:
        cmd_move(msg.index, msg.value);
        break;
    case MsgType::SetPolicy:
        t.stored.policy = static_cast<Policy>(msg.value);
        save_policy(t.stored);
        break;
    case MsgType::Reconnect:
        cmd_reconnect();
        break;
    case MsgType::Scan:
        start_scan(ScanPurpose::User);
        break;
    case MsgType::Hotspot:
        t.stored.hotspot_enabled = msg.value != 0;
        if (!t.stored.hotspot_enabled) {
            // off means off, also during the time after the setup
            t.setup_hotspot_until = 0;
        }
        save_hotspot(t.stored);
        break;
    case MsgType::NewHotspotPassword:
        generate_hotspot_password(t.stored.hotspot_password);
        save_hotspot(t.stored);
        if (t.status.hotspot_on) {
            configure_hotspot();
        }
        ESP_LOGI(kTag, "new hotspot password");
        break;
    case MsgType::Import: {
        const ImportJob &job = *static_cast<const ImportJob *>(msg.data);
        cmd_import(job);
        xSemaphoreGive(job.done);
        break;
    }
    case MsgType::WifiEvent:
        handle_wifi_event(msg.event, msg.reason);
        break;
    case MsgType::GotIp:
        on_got_ip(msg.ip);
        break;
    }
}

void handle_timers()
{
    if (t.status.state == StaState::WaitingToRetry && reached(t.retry_at)) {
        start_connect();
    }
    if (t.status.state == StaState::Connected && reached(t.rssi_check_at)) {
        check_signal();
    }
    if (t.setup_hotspot_until != 0 && reached(t.setup_hotspot_until)) {
        t.setup_hotspot_until = 0;
        ESP_LOGI(kTag, "setup hotspot time is over");
    }
    update_hotspot();
}

// the time until the next timer, at most a second (keeps the countdown in the status current)
TickType_t next_wait()
{
    TickType_t wait = pdMS_TO_TICKS(1000);
    auto consider = [&wait](TickType_t deadline) {
        const int32_t left = static_cast<int32_t>(deadline - now());
        wait = std::min<TickType_t>(wait, left > 0 ? static_cast<TickType_t>(left) : 0);
    };
    if (t.status.state == StaState::WaitingToRetry) {
        consider(t.retry_at);
    }
    if (t.status.state == StaState::Connected) {
        consider(t.rssi_check_at);
    }
    return wait;
}

void task(void *)
{
    Msg msg;
    for (;;) {
        if (xQueueReceive(g_queue, &msg, next_wait()) == pdTRUE) {
            handle(msg);
        }
        handle_timers();
        publish();
    }
}

// ------------------------------------------------------------------------- driver events

bool post(const Msg &msg)
{
    return g_queue != nullptr && xQueueSend(g_queue, &msg, 0) == pdTRUE;
}

void on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    Msg msg{};
    if (base == WIFI_EVENT) {
        msg.type = MsgType::WifiEvent;
        msg.event = id;
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            msg.reason = static_cast<uint8_t>(static_cast<wifi_event_sta_disconnected_t *>(data)->reason);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        msg.type = MsgType::GotIp;
        msg.ip = static_cast<ip_event_got_ip_t *>(data)->ip_info.ip.addr;
    } else {
        return;
    }
    if (!post(msg)) {
        ESP_LOGW(kTag, "event %ld dropped, queue full", static_cast<long>(id));
    }
}

esp_err_t command(MsgType type, uint8_t index = 0, int8_t value = 0, const char *ssid = nullptr,
                  const char *password = nullptr)
{
    Msg msg{};
    msg.type = type;
    msg.index = index;
    msg.value = value;
    if (ssid != nullptr) {
        copy_string(msg.ssid, sizeof(msg.ssid), ssid);
    }
    if (password != nullptr) {
        copy_string(msg.password, sizeof(msg.password), password);
    }
    return (g_queue != nullptr && xQueueSend(g_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK
                                                                                           : ESP_ERR_TIMEOUT;
}

// The device answers as <device name>.local on the Wi-Fi and the hotspot, and announces its web
// server, so the address on the screen works without knowing the IP. Without it the device is
// still reachable by its address.
void start_mdns(const char *device_name)
{
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        err = mdns_hostname_set(device_name);
    }
    if (err == ESP_OK) {
        err = mdns_instance_name_set(device_name);
    }
    if (err == ESP_OK) {
        err = mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "mDNS did not start (%s), the device is reachable by its address only", esp_err_to_name(err));
    }
}

} // namespace

esp_err_t start(const char *device_name, Listener listener, void *context)
{
    ESP_RETURN_ON_FALSE(g_queue == nullptr, ESP_ERR_INVALID_STATE, kTag, "already started");
    g_lock = xSemaphoreCreateMutex();
    g_queue = xQueueCreate(16, sizeof(Msg));
    ESP_RETURN_ON_FALSE(g_lock != nullptr && g_queue != nullptr, ESP_ERR_NO_MEM, kTag, "queue");
    g_listener = listener;
    g_listener_context = context;

    t.device_name = device_name;
    ESP_RETURN_ON_ERROR(load(t.stored), kTag, "settings");
    copy_string(t.status.hotspot_ssid, sizeof(t.status.hotspot_ssid), device_name);
    t.status.state = StaState::NoNetworks;
    t.setup_hotspot = usable_count() == 0;
    t.fast_allowed = esp_reset_reason() == ESP_RST_DEEPSLEEP;

    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "netif");
    t.sta = esp_netif_create_default_wifi_sta();
    t.ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(t.sta, device_name);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), kTag, "wifi init");
    // Shutdown handlers run in reverse order: this one runs before the Wi-Fi driver stops.
    // Leaving the network on purpose sends the deauthentication the router waits for; without it
    // the router still holds the old connection after the restart and turns the first attempt away.
    esp_register_shutdown_handler([] {
        g_restarting = true;
        if (esp_wifi_disconnect() == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    });
    // the network component is the only owner of the stored networks (docs/architecture.md 9.3)
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), kTag, "storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr), kTag, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr), kTag, "events");
    start_mdns(device_name);

    t.status.hotspot_on = hotspot_wanted();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(t.status.hotspot_on ? WIFI_MODE_APSTA : WIFI_MODE_STA), kTag, "mode");
    if (t.status.hotspot_on) {
        ESP_RETURN_ON_ERROR(configure_hotspot(), kTag, "hotspot");
    }

    t.status.ready = true;
    publish();
    ESP_RETURN_ON_FALSE(xTaskCreate(task, "network", 4096, nullptr, 5, nullptr) == pdPASS, ESP_ERR_NO_MEM, kTag,
                        "task");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi start");
    // No modem sleep: with it every packet waits for the next beacon (ping 18 to 200 ms, single
    // web requests up to seconds on the T4). The panels run on mains power; a battery remote
    // will need its own choice here.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(kTag, "%u saved networks, hotspot '%s' %s", t.stored.count, device_name,
             t.status.hotspot_on ? "on" : "off");
    return ESP_OK;
}

Status status()
{
    Status copy{};
    if (g_lock == nullptr) {
        return copy;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    copy = g_status;
    const TickType_t until = g_setup_hotspot_until;
    xSemaphoreGive(g_lock);
    if (until != 0) {
        const int32_t left = static_cast<int32_t>(until - xTaskGetTickCount());
        copy.hotspot_off_in_s = left > 0 ? static_cast<uint32_t>(left) * portTICK_PERIOD_MS / 1000 : 0;
    }
    return copy;
}

size_t saved_networks(SavedNetworkInfo *out, size_t max)
{
    if (g_lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const size_t count = std::min(max, g_saved_count);
    std::copy(g_saved, g_saved + count, out);
    xSemaphoreGive(g_lock);
    return count;
}

size_t last_scan(ScanEntry *out, size_t max)
{
    if (g_lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    const size_t count = std::min(max, g_scan_count);
    std::copy(g_scan, g_scan + count, out);
    xSemaphoreGive(g_lock);
    return count;
}

esp_err_t add_network(const char *ssid, const char *password, bool connect_now)
{
    ESP_RETURN_ON_FALSE(ssid != nullptr && ssid[0] != 0 && std::strlen(ssid) < kSsidSize, ESP_ERR_INVALID_ARG, kTag,
                        "SSID");
    // WPA needs 8 to 63 characters (64 = the key in hex); none = an open network
    ESP_RETURN_ON_FALSE(password != nullptr && std::strlen(password) < kPasswordSize &&
                            (password[0] == 0 || std::strlen(password) >= kMinPassword),
                        ESP_ERR_INVALID_ARG, kTag, "password");
    return command(MsgType::Add, 0, connect_now ? 1 : 0, ssid, password);
}

esp_err_t import_networks(const ImportedNetwork *networks, size_t count, bool apply, ImportResult &result)
{
    ESP_RETURN_ON_FALSE(networks != nullptr || count == 0, ESP_ERR_INVALID_ARG, kTag, "networks");
    ESP_RETURN_ON_FALSE(count <= kMaxImport, ESP_ERR_INVALID_SIZE, kTag, "more than %u networks",
                        static_cast<unsigned>(kMaxImport));
    for (size_t i = 0; i < count; i++) {
        const ImportedNetwork &network = networks[i];
        const size_t length = strnlen(network.password, kPasswordSize);
        ESP_RETURN_ON_FALSE(network.ssid[0] != 0 && strnlen(network.ssid, kSsidSize) < kSsidSize, ESP_ERR_INVALID_ARG,
                            kTag, "SSID");
        ESP_RETURN_ON_FALSE(!network.has_password || (length < kPasswordSize && (length == 0 || length >= kMinPassword)),
                            ESP_ERR_INVALID_ARG, kTag, "password of '%s'", network.ssid);
    }
    ESP_RETURN_ON_FALSE(g_queue != nullptr, ESP_ERR_INVALID_STATE, kTag, "not started");
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(done != nullptr, ESP_ERR_NO_MEM, kTag, "semaphore");
    ImportJob job{networks, count, apply, &result, done};
    Msg msg{};
    msg.type = MsgType::Import;
    msg.data = &job;
    const bool posted = xQueueSend(g_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE;
    if (posted) {
        // the task uses `job` until it gives the semaphore
        xSemaphoreTake(done, portMAX_DELAY);
    }
    vSemaphoreDelete(done);
    return posted ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t remove_network(uint8_t index)
{
    return command(MsgType::Remove, index);
}

esp_err_t move_network(uint8_t index, int8_t delta)
{
    return command(MsgType::Move, index, delta);
}

esp_err_t set_policy(Policy policy)
{
    return command(MsgType::SetPolicy, 0, static_cast<int8_t>(policy));
}

esp_err_t reconnect()
{
    return command(MsgType::Reconnect);
}

esp_err_t scan()
{
    return command(MsgType::Scan);
}

esp_err_t set_hotspot(bool on)
{
    return command(MsgType::Hotspot, 0, on ? 1 : 0);
}

esp_err_t new_hotspot_password()
{
    return command(MsgType::NewHotspotPassword);
}

const char *state_name(StaState state)
{
    switch (state) {
    case StaState::NoNetworks:
        return "no networks";
    case StaState::Scanning:
        return "scanning";
    case StaState::Connecting:
        return "connecting";
    case StaState::Connected:
        return "connected";
    case StaState::WaitingToRetry:
        return "waiting to retry";
    }
    return "?";
}

} // namespace hc::network
