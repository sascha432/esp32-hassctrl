#include "hc/platform/settings.hpp"

#include "nvs_flash.h"

namespace hc::platform {

namespace {
constexpr const char *kPartition = "settings";
}

Settings::Settings(const char *name_space, Mode mode)
{
    const nvs_open_mode_t open_mode = (mode == Mode::ReadWrite) ? NVS_READWRITE : NVS_READONLY;
    if (nvs_open_from_partition(kPartition, name_space, open_mode, &_handle) != ESP_OK) {
        _handle = 0;
    }
}

Settings::~Settings()
{
    if (_handle != 0) {
        nvs_close(_handle);
    }
}

esp_err_t Settings::get_blob(const char *key, void *out, size_t &size)
{
    return ok() ? nvs_get_blob(_handle, key, out, &size) : ESP_ERR_INVALID_STATE;
}

esp_err_t Settings::set_blob(const char *key, const void *data, size_t size)
{
    return ok() ? nvs_set_blob(_handle, key, data, size) : ESP_ERR_INVALID_STATE;
}

esp_err_t Settings::get_string(const char *key, char *out, size_t size)
{
    return ok() ? nvs_get_str(_handle, key, out, &size) : ESP_ERR_INVALID_STATE;
}

esp_err_t Settings::set_string(const char *key, const char *value)
{
    return ok() ? nvs_set_str(_handle, key, value) : ESP_ERR_INVALID_STATE;
}

esp_err_t Settings::get_u8(const char *key, uint8_t &out)
{
    return ok() ? nvs_get_u8(_handle, key, &out) : ESP_ERR_INVALID_STATE;
}

esp_err_t Settings::set_u8(const char *key, uint8_t value)
{
    return ok() ? nvs_set_u8(_handle, key, value) : ESP_ERR_INVALID_STATE;
}

esp_err_t Settings::get_u32(const char *key, uint32_t &out)
{
    return ok() ? nvs_get_u32(_handle, key, &out) : ESP_ERR_INVALID_STATE;
}

esp_err_t Settings::set_u32(const char *key, uint32_t value)
{
    return ok() ? nvs_set_u32(_handle, key, value) : ESP_ERR_INVALID_STATE;
}

esp_err_t Settings::erase(const char *key)
{
    if (!ok()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = nvs_erase_key(_handle, key);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

esp_err_t Settings::commit()
{
    return ok() ? nvs_commit(_handle) : ESP_ERR_INVALID_STATE;
}

esp_err_t erase_all_settings()
{
    nvs_flash_deinit_partition(kPartition);
    const esp_err_t err = nvs_flash_erase_partition(kPartition);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_flash_init_partition(kPartition);
}

} // namespace hc::platform
