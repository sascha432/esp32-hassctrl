#pragma once

// The Wi-Fi coprocessor of chips without a radio: the ESP32-C6 next to the ESP32-P4, reached with
// esp_hosted over SDIO (docs/devices/JC4880P443C). On every other target there is none, and the
// functions return ESP_ERR_NOT_SUPPORTED.

#include <cstddef>

#include "esp_err.h"

namespace hc::network {

bool has_coprocessor();

// esp_hosted firmware versions of the coprocessor and of this side, "3.0.9"
esp_err_t coprocessor_version(char *text, size_t size);
const char *host_hosted_version();

// Called while an update runs: bytes sent to the coprocessor, size of the image (0 = unknown)
using CoprocessorProgress = void (*)(size_t done, size_t total);

// Downloads a coprocessor firmware image over HTTP(S) and writes it into the coprocessor's other
// OTA slot. The coprocessor checks the image at the end; a broken one leaves it on the running
// firmware. On success it boots the new one at its next reset: the caller restarts this chip,
// which resets the coprocessor (esp_hosted resets it at every start). Blocks for the whole
// download, use it from the console only.
esp_err_t update_coprocessor(const char *url, CoprocessorProgress progress);

// The same without the network, for a coprocessor whose firmware cannot hold Wi-Fi (the vendor's
// esp_hosted 2.3.2): the image was written over USB into this chip's free app slot (the OTA slot
// coprocessor_slot() names, "ota_1" after a serial flash). Wi-Fi is stopped while it runs.
const char *coprocessor_slot();
esp_err_t update_coprocessor_from_slot(CoprocessorProgress progress);

} // namespace hc::network
