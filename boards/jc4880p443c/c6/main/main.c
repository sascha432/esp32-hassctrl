// The esp_hosted coprocessor starts by itself (its components hook into the startup); the app only
// provides NVS (the Wi-Fi driver stores its calibration there) and the default event loop.

#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI("hassctrl-c6", "coprocessor running");
}
