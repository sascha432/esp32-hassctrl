#pragma once

// What the files of the network task share (network.cpp: the state, the task and start; station.cpp:
// scan, connect, roam; commands.cpp: the saved networks; api.cpp: the calls of other tasks). Only
// the network task touches `t`; other tasks read the copies under g_lock.

#include <algorithm>
#include <cstring>

#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "hc/network/network.hpp"

#include "selection.hpp"
#include "store.hpp"

namespace hc::network::detail {

constexpr const char *kTag = "network";

constexpr size_t kMaxScan = 20;
// short first: a router that still holds the connection of a reset device (WPA3 with PMF) lets
// it back in after a few seconds
constexpr uint32_t kRetryMinMs = 1000;

enum class MsgType : uint8_t {
    Add,
    Remove,
    Move,
    SetPolicy,
    Reconnect,
    Scan,
    Hotspot,
    NewHotspotPassword,
    ForceHotspot,
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
    // during the first setup: the network just entered failed (wrong password at once, other
    // errors after kSetupFailures attempts), so the hotspot comes back for the next try
    bool setup_failed = false;
    // the hotspot is on whatever the settings say; not stored (the recovery app)
    bool force_hotspot = false;
    uint8_t setup_failures = 0;
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

extern Task t;

extern QueueHandle_t g_queue;
// set by the shutdown handler: the disconnect of a restart is not a lost connection
extern volatile bool g_restarting;
extern SemaphoreHandle_t g_lock;

// copies for other tasks, guarded by g_lock
extern Status g_status;
extern TickType_t g_setup_hotspot_until;
extern SavedNetworkInfo g_saved[kMaxNetworks];
extern size_t g_saved_count;
extern ScanEntry g_scan[kMaxScan];
extern size_t g_scan_count;

TickType_t now();
bool reached(TickType_t deadline);
// copies at most size - 1 characters and always terminates (SSIDs from the driver are not
// terminated when they use all 32 bytes)
void copy_string(char *out, size_t size, const char *in);

// Fills a fixed-size field of the Wi-Fi driver. Those need no terminator: an SSID may use all 32
// bytes and a raw key all 64.
template <size_t N>
void copy_field(uint8_t (&field)[N], const char *in, size_t in_size)
{
    const size_t length = strnlen(in, std::min(N, in_size));
    std::memcpy(field, in, length);
    std::memset(field + length, 0, N - length);
}

void set_error(const char *text);
// saved networks that may be tried (not waiting for their password)
size_t usable_count();

// ----------------------------------------------------------------------- station.cpp

void start_scan(ScanPurpose purpose);
// a connection: the last access point without a scan when allowed, else a scan
void start_connect();
// Disconnects on purpose; the disconnect event then starts the next connection.
void disconnect_on_purpose();
void on_scan_done();
void on_disconnected(uint8_t reason);
void on_got_ip(uint32_t ip);
// roaming: a weak signal for a while scans for a better access point
void check_signal();

// ---------------------------------------------------------------------- commands.cpp

void cmd_add(const Msg &msg);
void cmd_remove(uint8_t index);
void cmd_move(uint8_t index, int8_t delta);
void cmd_import(const ImportJob &job);
void cmd_reconnect();

} // namespace hc::network::detail
