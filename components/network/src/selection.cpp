#include "selection.hpp"

#include <cstring>

namespace hc::network::detail {

namespace {

// the strongest access point with the name of saved network `index`, nullptr if none is in range
const SeenAp *strongest_of(const SavedNetwork &network, const SeenAp *seen, size_t seen_count)
{
    const SeenAp *best = nullptr;
    for (size_t i = 0; i < seen_count; i++) {
        if (std::strcmp(seen[i].ssid, network.ssid) == 0 && (best == nullptr || seen[i].rssi > best->rssi)) {
            best = &seen[i];
        }
    }
    return best;
}

void fill(Candidate &out, size_t index, const SeenAp &ap)
{
    out.index = static_cast<uint8_t>(index);
    std::memcpy(out.bssid, ap.bssid, sizeof(out.bssid));
    out.rssi = ap.rssi;
    out.channel = ap.channel;
}

bool pass(const SavedNetwork *saved, size_t saved_count, const SeenAp *seen, size_t seen_count, Policy policy,
          uint32_t skip, Candidate &out)
{
    if (policy == Policy::EnteredOrder) {
        for (size_t i = 0; i < saved_count; i++) {
            if (skip & (1u << i)) {
                continue;
            }
            const SeenAp *ap = strongest_of(saved[i], seen, seen_count);
            if (ap != nullptr && ap->rssi >= kUsableRssi) {
                fill(out, i, *ap);
                return true;
            }
        }
    }
    // the strongest signal, also the fallback of the entered order when none is usable
    const SeenAp *best = nullptr;
    size_t best_index = 0;
    for (size_t i = 0; i < saved_count; i++) {
        if (skip & (1u << i)) {
            continue;
        }
        const SeenAp *ap = strongest_of(saved[i], seen, seen_count);
        if (ap != nullptr && (best == nullptr || ap->rssi > best->rssi)) {
            best = ap;
            best_index = i;
        }
    }
    if (best == nullptr) {
        return false;
    }
    fill(out, best_index, *best);
    return true;
}

} // namespace

bool choose(const SavedNetwork *saved, size_t saved_count, const SeenAp *seen, size_t seen_count, Policy policy,
            uint32_t failed, Candidate &out)
{
    if (pass(saved, saved_count, seen, seen_count, policy, failed, out)) {
        return true;
    }
    // only networks that rejected the password are in range: try them again
    return failed != 0 && pass(saved, saved_count, seen, seen_count, policy, 0, out);
}

} // namespace hc::network::detail
