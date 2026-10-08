// The recovery app: runs from the factory partition when the main firmware is being updated on a
// 4 MB board, is broken, or the user asks for it (docs/architecture.md section 9.5).
//
// Same storage, Wi-Fi, hotspot and web server (with its login) as the main firmware, from the
// same NVS: the web page offers the firmware and file system update and "Start the firmware".
// No LVGL. The screen stays dark for now; status lines and a QR code come later.

#include "esp_event.h"
#include "esp_log.h"
#include "hc/board/board.hpp"
#include "hc/network/network.hpp"
#include "hc/ota/ota.hpp"
#include "hc/platform/device.hpp"
#include "hc/platform/storage.hpp"
#include "hc/platform/system_info.hpp"
#include "hc/webserver/webserver.hpp"

namespace {
constexpr const char *kTag = "recovery";
}

extern "C" void app_main()
{
    hc::platform::log_system_info();
    const hc::ota::Info ota = hc::ota::info();
    ESP_LOGI(kTag, "recovery app on %s, firmware in flash: %s%s", hc::board::info().name,
             ota.firmware_present ? ota.firmware_version : "none",
             ota.firmware_failed ? " (did not start, rolled back)" : "");

    ESP_ERROR_CHECK(hc::platform::init_storage());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (hc::network::start(hc::platform::device_name(), nullptr, nullptr) != ESP_OK) {
        ESP_LOGE(kTag, "network did not start");
        return;
    }
    if (hc::webserver::start({.device_name = hc::platform::device_name()}) != ESP_OK) {
        ESP_LOGE(kTag, "web server did not start");
    }
}
