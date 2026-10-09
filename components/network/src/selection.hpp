#pragma once

// Which saved network to connect to, from the results of a scan. Plain C++ without ESP-IDF, so it
// can be tested on the PC (docs/architecture.md section 3, host tests).

#include <cstddef>
#include <cstdint>

#include "hc/network/network.hpp"

namespace hc::network::detail {

struct SavedNetwork {
    char ssid[kSsidSize];
    char password[kPasswordSize];
};

struct SeenAp {
    char ssid[kSsidSize];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
};

struct Candidate {
    uint8_t index; // into the saved networks
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
};

// `failed` has bit i set when the last attempt on saved network i failed in this round. Those are
// skipped while another saved network is in range. Of several access points with the same
// name, the strongest one is taken. False when no saved network is in range.
bool choose(const SavedNetwork *saved, size_t saved_count, const SeenAp *seen, size_t seen_count, Policy policy,
            uint32_t failed, Candidate &out);

} // namespace hc::network::detail
