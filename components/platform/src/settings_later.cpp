// Delayed settings writes (hc/platform/settings.hpp). The values wait in a small table; an
// esp_timer, restarted by every call, writes them from the esp_timer task.

#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hc/platform/settings.hpp"

namespace hc::platform {

namespace {

constexpr const char *kTag = "settings";
constexpr size_t kNameSize = 16; // NVS namespace and key names have up to 15 characters

struct Pending {
    char name_space[kNameSize];
    char key[kNameSize];
    uint8_t data[kMaxPendingSize];
    size_t size;
};

Pending g_pending[kMaxPendingWrites];
size_t g_count = 0;
SemaphoreHandle_t g_lock = nullptr;
esp_timer_handle_t g_timer = nullptr;

void write_all()
{
    Pending pending[kMaxPendingWrites];
    size_t count;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    count = g_count;
    std::memcpy(pending, g_pending, sizeof(Pending) * count);
    g_count = 0;
    xSemaphoreGive(g_lock);

    for (size_t i = 0; i < count; i++) {
        Settings settings(pending[i].name_space, Settings::Mode::ReadWrite);
        esp_err_t err = settings.set_blob(pending[i].key, pending[i].data, pending[i].size);
        if (err == ESP_OK) {
            err = settings.commit();
        }
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "%s/%s not stored: %s", pending[i].name_space, pending[i].key, esp_err_to_name(err));
        }
    }
}

esp_err_t init()
{
    g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_timer_create_args_t args{};
    args.callback = [](void *) { write_all(); };
    args.name = "settings_later";
    esp_err_t err = esp_timer_create(&args, &g_timer);
    if (err == ESP_OK) {
        err = esp_register_shutdown_handler(write_pending_settings);
    }
    return err;
}

} // namespace

esp_err_t set_blob_later(const char *name_space, const char *key, const void *data, size_t size)
{
    if (size > kMaxPendingSize || std::strlen(name_space) >= kNameSize || std::strlen(key) >= kNameSize) {
        return ESP_ERR_INVALID_ARG;
    }
    // created on the first call, from whichever task makes it
    static const esp_err_t init_result = init();
    if (init_result != ESP_OK) {
        return init_result;
    }

    xSemaphoreTake(g_lock, portMAX_DELAY);
    Pending *slot = nullptr;
    for (size_t i = 0; i < g_count; i++) {
        if (std::strcmp(g_pending[i].name_space, name_space) == 0 && std::strcmp(g_pending[i].key, key) == 0) {
            slot = &g_pending[i];
        }
    }
    if (slot == nullptr && g_count < kMaxPendingWrites) {
        slot = &g_pending[g_count++];
        std::strcpy(slot->name_space, name_space);
        std::strcpy(slot->key, key);
    }
    if (slot != nullptr) {
        std::memcpy(slot->data, data, size);
        slot->size = size;
    }
    xSemaphoreGive(g_lock);
    if (slot == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_timer_stop(g_timer); // not running is fine
    return esp_timer_start_once(g_timer, static_cast<uint64_t>(kWriteLaterMs) * 1000);
}

void write_pending_settings()
{
    if (g_lock == nullptr) {
        return;
    }
    esp_timer_stop(g_timer);
    write_all();
}

} // namespace hc::platform
