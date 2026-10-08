#include "hc/platform/system_info.hpp"

#include <cinttypes>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

namespace hc::platform {

namespace {

constexpr const char *kTag = "system";

const char *chip_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:
        return "ESP32";
    case CHIP_ESP32S3:
        return "ESP32-S3";
    case CHIP_ESP32P4:
        return "ESP32-P4";
    default:
        return "unknown chip";
    }
}

const char *reset_reason_text(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power on";
    case ESP_RST_EXT:
        return "external pin";
    case ESP_RST_SW:
        return "software restart";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "other watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return "unknown";
    }
}

} // namespace

void log_system_info()
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(kTag, "%s %s, ESP-IDF %s", app->project_name, app->version, app->idf_ver);

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    ESP_LOGI(kTag, "chip %s rev v%d.%d, %d cores", chip_name(chip.model), chip.revision / 100, chip.revision % 100,
             chip.cores);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(nullptr, &flash_size) == ESP_OK) {
        ESP_LOGI(kTag, "flash %" PRIu32 " KB", flash_size / 1024);
    }

#if CONFIG_SPIRAM
    ESP_LOGI(kTag, "PSRAM %zu KB", esp_psram_get_size() / 1024);
#else
    ESP_LOGI(kTag, "PSRAM not enabled in this build");
#endif

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    ESP_LOGI(kTag, "running from '%s' at 0x%" PRIx32 ", boot partition '%s'", running ? running->label : "?",
             running ? running->address : 0, boot ? boot->label : "?");

    ESP_LOGI(kTag, "reset reason: %s", reset_reason_text(esp_reset_reason()));
    ESP_LOGI(kTag, "free heap: internal %zu KB, PSRAM %zu KB", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}

} // namespace hc::platform
