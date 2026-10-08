#include "hc/platform/storage.hpp"

#include <cstring>

#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "nvs_flash.h"

namespace hc::platform {

namespace {

constexpr const char *kTag = "storage";
constexpr const char *kSettingsPartition = "settings";

StorageStatus g_status{FsState::NoPartition, 0, 0};

// An NVS partition that is full or was written by a newer NVS version cannot be used as it is.
// That is the only case where it is erased: it holds no readable data in that state.
esp_err_t init_nvs(const char *label)
{
    esp_err_t err = nvs_flash_init_partition(label);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "NVS '%s' unusable (%s), erasing it", label, esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition(label), kTag, "erase %s", label);
        err = nvs_flash_init_partition(label);
    }
    return err;
}

// True when one of the two superblocks at the start of the partition carries the LittleFS magic.
// Each superblock begins with a revision count and a tag, followed by the name "littlefs".
bool holds_littlefs(const esp_partition_t *partition)
{
    constexpr size_t kBlockSize = 4096;
    constexpr char kMagic[] = "littlefs";
    for (size_t block = 0; block < 2; block++) {
        char head[64] = {};
        if (esp_partition_read(partition, block * kBlockSize, head, sizeof(head)) != ESP_OK) {
            continue;
        }
        for (size_t i = 0; i + sizeof(kMagic) - 1 <= sizeof(head); i++) {
            if (std::memcmp(head + i, kMagic, sizeof(kMagic) - 1) == 0) {
                return true;
            }
        }
    }
    return false;
}

esp_err_t register_fs(bool format)
{
    esp_vfs_littlefs_conf_t conf{};
    conf.base_path = kFsMountPoint;
    conf.partition_label = kFsPartition;
    conf.format_if_mount_failed = format;
    conf.dont_mount = false;
    return esp_vfs_littlefs_register(&conf);
}

// A LittleFS that fails to mount is damaged: it is never formatted automatically, so the files
// that can still be read survive until they are exported (recovery mode, docs/ui/web.md
// section 7). A partition without any LittleFS (new device, leftovers of another firmware) holds
// nothing to lose and is formatted.
void mount_fs()
{
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kFsPartition);
    if (partition == nullptr) {
        ESP_LOGW(kTag, "no '%s' partition, no file system", kFsPartition);
        g_status.fs = FsState::NoPartition;
        return;
    }

    const bool existing = holds_littlefs(partition);
    const esp_err_t err = register_fs(!existing);
    if (err != ESP_OK) {
        if (existing) {
            ESP_LOGE(kTag, "LittleFS on '%s' is damaged (%s), not formatting it", kFsPartition, esp_err_to_name(err));
        } else {
            ESP_LOGE(kTag, "'%s' cannot be formatted (%s)", kFsPartition, esp_err_to_name(err));
        }
        g_status.fs = FsState::MountFailed;
        return;
    }
    if (!existing) {
        ESP_LOGI(kTag, "'%s' held no LittleFS, formatted it", kFsPartition);
    }

    g_status.fs = FsState::Mounted;
    esp_littlefs_info(kFsPartition, &g_status.fs_total, &g_status.fs_used);
    ESP_LOGI(kTag, "LittleFS at %s: %zu of %zu KB used", kFsMountPoint, g_status.fs_used / 1024,
             g_status.fs_total / 1024);
}

} // namespace

esp_err_t init_storage()
{
    ESP_RETURN_ON_ERROR(init_nvs(NVS_DEFAULT_PART_NAME), kTag, "system NVS");
    ESP_RETURN_ON_ERROR(init_nvs(kSettingsPartition), kTag, "settings NVS");
    mount_fs();
    return ESP_OK;
}

const StorageStatus &storage_status()
{
    return g_status;
}

esp_err_t unmount_fs()
{
    if (g_status.fs != FsState::Mounted) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_vfs_littlefs_unregister(kFsPartition), kTag, "unmount");
    g_status = {FsState::MountFailed, 0, 0};
    return ESP_OK;
}

esp_err_t format_fs()
{
    ESP_RETURN_ON_ERROR(unmount_fs(), kTag, "unmount");
    ESP_RETURN_ON_ERROR(esp_littlefs_format(kFsPartition), kTag, "format");
    ESP_LOGW(kTag, "LittleFS on '%s' formatted on request", kFsPartition);
    return mount_fs_again() == FsState::Mounted ? ESP_OK : ESP_FAIL;
}

FsState mount_fs_again()
{
    if (g_status.fs == FsState::Mounted) {
        return g_status.fs;
    }
    if (register_fs(false) != ESP_OK) {
        ESP_LOGE(kTag, "LittleFS on '%s' does not mount", kFsPartition);
        g_status = {FsState::MountFailed, 0, 0};
        return g_status.fs;
    }
    g_status.fs = FsState::Mounted;
    esp_littlefs_info(kFsPartition, &g_status.fs_total, &g_status.fs_used);
    ESP_LOGI(kTag, "LittleFS mounted again: %zu of %zu KB used", g_status.fs_used / 1024, g_status.fs_total / 1024);
    return g_status.fs;
}

} // namespace hc::platform
