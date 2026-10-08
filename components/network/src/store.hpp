#pragma once

// The stored network settings, namespace "network" of the settings NVS. Only the network task
// uses this.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "selection.hpp"

namespace hc::network::detail {

// Where a saved network was connected last: a reconnect goes straight there without a scan.
// channel 0 means unknown.
struct LastSeen {
    uint8_t bssid[6];
    uint8_t channel;
};

constexpr uint8_t kNoIndex = 0xff;

struct Stored {
    SavedNetwork networks[kMaxNetworks];
    uint8_t count;
    LastSeen last_seen[kMaxNetworks];
    // the network of the last connection, kNoIndex for none
    uint8_t last_index;
    Policy policy;
    // 8 digits and 2 lowercase letters
    char hotspot_password[12];
    // the user wants the hotspot on
    bool hotspot_enabled;
};

// Loads everything; creates the hotspot password on the first boot.
esp_err_t load(Stored &stored);

esp_err_t save_networks(const Stored &stored);
// separate from the networks, so a connection does not rewrite the passwords
esp_err_t save_last_seen(const Stored &stored);
esp_err_t save_policy(const Stored &stored);
esp_err_t save_hotspot(const Stored &stored);

// a new random hotspot password, not saved
void generate_hotspot_password(char (&out)[12]);

} // namespace hc::network::detail
