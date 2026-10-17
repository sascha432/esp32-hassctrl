// The commands of other tasks that change the saved networks (docs/architecture.md 9.3), run by
// the network task: add, remove, move, import, reconnect.

#include <algorithm>
#include <cstring>

#include "esp_log.h"

#include "merge.hpp"
#include "task.hpp"

namespace hc::network::detail {

namespace {

void reset_round()
{
    t.failed = 0;
    t.retried = 0;
    t.fast_failed = false;
    t.retry_delay_ms = kRetryMinMs;
}

} // namespace

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
    t.setup_failed = false;
    t.setup_failures = 0;
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

} // namespace hc::network::detail
