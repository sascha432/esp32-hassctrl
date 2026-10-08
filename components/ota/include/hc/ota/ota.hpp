#pragma once

// Firmware and file system updates (docs/architecture.md section 9.5, docs/ui/web.md section 8).
//
// Firmware: written to the next OTA partition. On a 16 MB board that is the other one and the
// main firmware updates itself. On a 4 MB board the only OTA partition is the running one, so the
// main firmware restarts into the recovery app (restart_into_recovery()), which writes ota_0.
// The image is checked with its first bytes: an ESP32 app image for this chip whose project is
// "hassctrl" (not the recovery app, not another firmware), built for this board (its board tag,
// hc::board::ImageTag). After finish() the bootloader starts
// it with the next restart; a firmware that does not confirm itself is rolled back.
//
// File system: a LittleFS image of the "storage" partition (tools/build.py builds it later). The
// file system is unmounted, the partition written and mounted again in finish().
//
// One update at a time. begin/write/finish come from one task (an upload handler); a download
// runs in a task of its own. status() can be read from any task.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace hc::ota {

enum class Target : uint8_t {
    Firmware,
    FileSystem,
};

struct Info {
    // this is the recovery app (factory partition)
    bool recovery_app;
    // this app can write the main firmware: the recovery app, or the main firmware on a board
    // with two OTA partitions
    bool firmware_direct;
    // ota_0 holds a complete, valid firmware image (what the recovery app returns to); an
    // interrupted update leaves none
    bool firmware_present;
    // that firmware crashed or restarted before it confirmed itself and was rolled back
    bool firmware_failed;
    // the running app is confirmed (or is not an update): a restart keeps it. A new firmware is
    // unconfirmed for its first seconds; a reset then rolls it back.
    bool confirmed;
    char firmware_version[32];
    size_t firmware_max;
    size_t filesystem_max;
};

Info info();

enum class State : uint8_t {
    Idle,
    Receiving,
    Downloading,
    Done,
    Failed,
};

struct Status {
    State state;
    Target target;
    size_t done;
    size_t total; // 0 while unknown (a download without a length)
    // the reason of the last failure in words
    char error[80];
};

Status status();

esp_err_t begin(Target target, size_t size);
esp_err_t write(const void *data, size_t size);
esp_err_t finish();
void abort();

// Downloads the image from an http:// or https:// URL in a task of its own (public
// certificates). ESP_ERR_INVALID_STATE while another update runs.
esp_err_t start_download(Target target, const char *url);

// The main firmware on a 4 MB board before a firmware update: the next start runs the recovery
// app, which starts the main firmware again when asked or after an update.
esp_err_t restart_into_recovery(uint32_t delay_ms);
// The recovery app: back to the main firmware in ota_0.
esp_err_t restart_into_firmware(uint32_t delay_ms);
// a restart after `delay_ms`, so the answer to the request still goes out
void restart_later(uint32_t delay_ms);

const char *target_name(Target target);
const char *state_name(State state);

} // namespace hc::ota
