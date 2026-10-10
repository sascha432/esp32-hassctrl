#include "merge.hpp"

#include <cstring>

namespace hc::network::detail {

namespace {

struct Entry {
    SavedNetwork network;
    uint8_t from;
};

// the sources are fields of the same size, terminated
template <size_t N>
void copy_text(char (&out)[N], const char (&in)[N])
{
    const size_t length = strnlen(in, N - 1);
    std::memcpy(out, in, length);
    out[length] = 0;
}

int find(const SavedNetwork *networks, size_t count, const char *ssid)
{
    for (size_t i = 0; i < count; i++) {
        if (std::strcmp(networks[i].ssid, ssid) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool listed(const Entry *entries, size_t count, const char *ssid)
{
    for (size_t i = 0; i < count; i++) {
        if (std::strcmp(entries[i].network.ssid, ssid) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

void merge(const SavedNetwork *saved, size_t saved_count, int keep, const ImportedNetwork *imported,
           size_t imported_count, Merged &out, ImportResult &result)
{
    out = {};
    result = {};
    // about 2 KB, too much for the network task's stack; that task is the only caller
    static Entry entries[kMaxImport + kMaxNetworks];
    size_t count = 0;

    // the file's networks in its order; a saved one keeps its password unless the file has one
    for (size_t i = 0; i < imported_count && i < kMaxImport; i++) {
        const ImportedNetwork &in = imported[i];
        if (in.ssid[0] == 0 || listed(entries, count, in.ssid)) {
            continue;
        }
        Entry &entry = entries[count++];
        entry = {};
        const int index = find(saved, saved_count, in.ssid);
        entry.from = (index < 0) ? kNewNetwork : static_cast<uint8_t>(index);
        if (index >= 0) {
            entry.network = saved[index];
        } else {
            copy_text(entry.network.ssid, in.ssid);
            entry.network.password_needed = true;
        }
        if (in.has_password) {
            copy_text(entry.network.password, in.password);
            entry.network.password_needed = false;
        }
    }
    // then the saved networks the file does not name, in their order
    for (size_t i = 0; i < saved_count; i++) {
        if (!listed(entries, count, saved[i].ssid)) {
            entries[count++] = {saved[i], static_cast<uint8_t>(i)};
        }
    }

    // too many: cut off from the end, never the network in use
    while (count > kMaxNetworks) {
        size_t cut = count - 1;
        if (keep >= 0 && entries[cut].from == keep) {
            cut--;
        }
        copy_text(result.dropped[result.dropped_count++], entries[cut].network.ssid);
        for (size_t i = cut; i + 1 < count; i++) {
            entries[i] = entries[i + 1];
        }
        count--;
    }
    // they were cut from the end: name them in the file's order
    for (size_t i = 0, j = result.dropped_count; i < j / 2; i++) {
        char swap[kSsidSize];
        std::memcpy(swap, result.dropped[i], kSsidSize);
        std::memcpy(result.dropped[i], result.dropped[j - 1 - i], kSsidSize);
        std::memcpy(result.dropped[j - 1 - i], swap, kSsidSize);
    }

    for (size_t i = 0; i < count; i++) {
        out.networks[i] = entries[i].network;
        out.from[i] = entries[i].from;
        if (entries[i].network.password_needed) {
            copy_text(result.need_password[result.need_password_count++], entries[i].network.ssid);
        }
    }
    out.count = static_cast<uint8_t>(count);
    result.count = out.count;
}

} // namespace hc::network::detail
