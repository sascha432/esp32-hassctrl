#pragma once

// Wi-Fi (station) and hotspot (access point) of the device, docs/architecture.md sections 9.1
// and 9.2. On screen and in the web UI the two roles are "Wi-Fi" and "Hotspot".
//
// One task owns the Wi-Fi driver and every piece of network state. The functions below only post
// commands to it and return at once; the task reports changes through the listener and keeps a
// snapshot that status() copies. Passwords never leave the component.
//
// Behavior:
//   * Up to 5 saved networks, chosen by the order they were entered or by the strongest signal.
//   * Fast reconnect: each network remembers the access point and channel of its last
//     connection. Waking from deep sleep, the device goes straight to the network of the last
//     connection, without a scan; only when that fails it scans. A normal boot and a lost
//     connection always scan, so the network order is followed.
//   * On connect with a scan: the first usable network (entered order) or the strongest one.
//     A failed attempt is retried with a growing delay from 1 s; a network that rejected the
//     password is skipped for the next attempts while other saved networks are in range.
//   * While connected: no switching while the signal is good. When it stays weak for 30 s, a
//     background scan looks for an access point that is clearly better and switches to it.
//   * Hotspot: on while no network is saved (first setup), for 15 minutes after the first
//     successful connection of the setup, and whenever the user turned it on. It never opens by
//     itself when the Wi-Fi fails (touch is a requirement, Settings is the recovery path).
//   * The hotspot password is generated on the first boot: 8 digits and 2 lowercase letters.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace hc::network {

constexpr size_t kMaxNetworks = 5;
constexpr size_t kSsidSize = 33;     // 32 characters and the terminator
constexpr size_t kPasswordSize = 65; // 64 characters and the terminator
constexpr size_t kMinPassword = 8;   // WPA; an open network has none

// starting values, to be adjusted by measuring throughput against signal strength
constexpr int8_t kUsableRssi = -75;   // "entered order" skips networks weaker than this
constexpr int8_t kWeakRssi = -70;     // roaming starts below this (Apple's trigger)
constexpr uint32_t kWeakForMs = 30000;
constexpr int8_t kRoamMarginDb = 8;   // a new access point has to be this much stronger

enum class Policy : uint8_t {
    EnteredOrder,
    StrongestSignal,
};

enum class StaState : uint8_t {
    NoNetworks,
    Scanning,
    Connecting,
    Connected,
    WaitingToRetry,
};

struct Status {
    StaState state;
    Policy policy;
    uint8_t saved_count;
    // network in use or being tried
    char ssid[kSsidSize];
    char ip[16];
    int8_t rssi;
    uint8_t channel;
    // the last problem in words ("wrong Wi-Fi password"), empty while there is none
    char error[64];
    // time from the start of the last connection to the IP address, and whether it skipped the scan
    uint32_t connect_ms;
    bool connect_fast;
    bool hotspot_on;
    char hotspot_ssid[kSsidSize];
    char hotspot_password[12];
    uint8_t hotspot_clients;
    // seconds until the hotspot of the setup turns off, 0 while no such timer runs
    uint32_t hotspot_off_in_s;
    // the user wants the hotspot on (stored)
    bool hotspot_enabled;
    // the task runs and the saved networks are loaded (before: saved_count says nothing)
    bool ready;
};

struct ScanEntry {
    char ssid[kSsidSize];
    int8_t rssi;
    uint8_t channel;
    bool secure;
    bool saved;
};

// Called from the network task after every change of the status.
using Listener = void (*)(const Status &status, void *context);

// Starts the task and the Wi-Fi driver. NVS and the default event loop have to be ready.
esp_err_t start(const char *device_name, Listener listener, void *context);

Status status();

// saved networks in their order, SSIDs only
size_t saved_networks(char (*ssids)[kSsidSize], size_t max);

// the results of the last scan, strongest first
size_t last_scan(ScanEntry *out, size_t max);

// Commands. They return ESP_ERR_TIMEOUT when the task's queue is full.
// Saves a network (updates the password of a saved one; an empty password keeps it). `connect_now` (the screen's Connect):
// connects to it right away, once, which also proves the password; when that fails, or after a
// restart, the network order applies as usual. ESP_ERR_INVALID_ARG for a password of 1 to 7
// characters.
esp_err_t add_network(const char *ssid, const char *password, bool connect_now = false);
esp_err_t remove_network(uint8_t index);
esp_err_t move_network(uint8_t index, int8_t delta);
esp_err_t set_policy(Policy policy);
esp_err_t reconnect();
esp_err_t scan();
esp_err_t set_hotspot(bool on);
esp_err_t new_hotspot_password();

const char *state_name(StaState state);

} // namespace hc::network
