#pragma once

// Storage of the device (docs/architecture.md section 9.3):
//
//   "nvs"       system NVS of the Wi-Fi driver and the PHY calibration
//   "settings"  NVS of the firmware: settings, secrets, calibration (hc::platform::Settings)
//   "storage"   LittleFS at /fs: web UI files and the dashboard configuration
//
// A LittleFS that cannot be mounted is reported and never formatted automatically, so files that
// can still be read survive until they are exported (docs/ui/web.md section 7). A partition that
// holds no LittleFS at all (new device, leftovers of another firmware) is formatted.

#include <cstddef>

#include "esp_err.h"

namespace hc::platform {

constexpr const char *kFsMountPoint = "/fs";
constexpr const char *kFsPartition = "storage";

enum class FsState : unsigned char {
    Mounted,
    // the partition holds a LittleFS that cannot be mounted: recovery mode
    MountFailed,
    // no "storage" partition in the partition table
    NoPartition,
};

struct StorageStatus {
    FsState fs;
    size_t fs_total;
    size_t fs_used;
};

// Initializes both NVS partitions and mounts LittleFS. Fails only when an NVS partition cannot be
// used; a failing file system is reported in storage_status().
esp_err_t init_storage();

const StorageStatus &storage_status();

// For the file system update (hc::ota), which writes the whole partition: unmount before, mount
// after. mount_fs_again() never formats; the state tells whether the new image mounted.
esp_err_t unmount_fs();
FsState mount_fs_again();

// Erases the file system and mounts the empty one (the recovery page, after the readable files
// were exported). The web files are gone afterwards: the built-in page is served.
esp_err_t format_fs();

} // namespace hc::platform
