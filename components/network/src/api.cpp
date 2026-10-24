// The calls of other tasks (hc/network/network.hpp): commands go into the queue of the network
// task, the status, the saved networks and the last scan are copies under a short lock.

#include <algorithm>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"

#include "task.hpp"

namespace hc::network {

using namespace detail;

namespace {

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

} // namespace

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
    // the flag is set when read: a network saved after the scan shows as saved without a rescan
    for (size_t i = 0; i < count; i++) {
        out[i].saved = false;
        for (size_t n = 0; n < g_saved_count; n++) {
            // one still waiting for its password is not saved yet: choosing it enters the password
            out[i].saved |= !g_saved[n].password_needed && std::strcmp(out[i].ssid, g_saved[n].ssid) == 0;
        }
    }
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

esp_err_t force_hotspot(bool on)
{
    return command(MsgType::ForceHotspot, 0, on ? 1 : 0);
}

esp_err_t set_hotspot(bool on)
{
    return command(MsgType::Hotspot, 0, on ? 1 : 0);
}

esp_err_t new_hotspot_password()
{
    return command(MsgType::NewHotspotPassword);
}

esp_err_t set_power_save(bool on)
{
    return command(MsgType::PowerSave, 0, on ? 1 : 0);
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
