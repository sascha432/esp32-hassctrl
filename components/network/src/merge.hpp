#pragma once

// The saved networks after a settings import (see import_networks in hc/network/network.hpp).
// Plain C++ without ESP-IDF, tested on the PC.

#include <cstddef>
#include <cstdint>

#include "hc/network/network.hpp"
#include "selection.hpp"

namespace hc::network::detail {

constexpr uint8_t kNewNetwork = 0xff;

struct Merged {
    SavedNetwork networks[kMaxNetworks];
    // the index each one had among the saved networks, kNewNetwork for one the file added
    uint8_t from[kMaxNetworks];
    uint8_t count;
};

// `keep`: index of the saved network in use, -1 for none; it is never cut off. Entries with an
// empty name and repeated names are skipped.
void merge(const SavedNetwork *saved, size_t saved_count, int keep, const ImportedNetwork *imported,
           size_t imported_count, Merged &out, ImportResult &result);

} // namespace hc::network::detail
