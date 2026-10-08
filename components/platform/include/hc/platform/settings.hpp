#pragma once

// Access to the "settings" NVS partition (docs/architecture.md section 9.3), one namespace per
// component. RAII: the handle is closed when the object goes out of scope. Writes become durable
// with commit().

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "nvs.h"

namespace hc::platform {

class Settings {
public:
    enum class Mode : unsigned char {
        Read,
        ReadWrite,
    };

    Settings(const char *name_space, Mode mode);
    ~Settings();
    Settings(const Settings &) = delete;
    Settings &operator=(const Settings &) = delete;

    // false when the namespace could not be opened (for Read: it does not exist yet)
    bool ok() const
    {
        return _handle != 0;
    }

    // `size` is the buffer size on input and the stored size on output
    esp_err_t get_blob(const char *key, void *out, size_t &size);
    esp_err_t set_blob(const char *key, const void *data, size_t size);
    esp_err_t get_string(const char *key, char *out, size_t size);
    esp_err_t set_string(const char *key, const char *value);
    esp_err_t get_u8(const char *key, uint8_t &out);
    esp_err_t set_u8(const char *key, uint8_t value);
    esp_err_t get_u32(const char *key, uint32_t &out);
    esp_err_t set_u32(const char *key, uint32_t value);
    esp_err_t erase(const char *key);
    esp_err_t commit();

private:
    nvs_handle_t _handle = 0;
};

// Erases every setting of the firmware (factory reset). The system NVS is left alone.
esp_err_t erase_all_settings();

// Delayed writes for values that change often, like a slider: the value is kept in RAM and all
// pending values are written together kWriteLaterMs after the last call, so dragging a slider
// writes the flash once. Pending values are also written when the firmware restarts
// (esp_restart). Up to kMaxPendingWrites keys, values up to kMaxPendingSize bytes. Any task.
constexpr uint32_t kWriteLaterMs = 5000;
constexpr size_t kMaxPendingWrites = 4;
constexpr size_t kMaxPendingSize = 32;

esp_err_t set_blob_later(const char *name_space, const char *key, const void *data, size_t size);

// writes the pending values now
void write_pending_settings();

} // namespace hc::platform
