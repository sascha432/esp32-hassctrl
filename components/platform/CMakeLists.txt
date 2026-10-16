# Platform services (docs/architecture.md section 9.4): boot log, storage (both NVS partitions,
# LittleFS), the settings NVS, the device name. LittleFS comes from idf_component.yml.
idf_component_register(SRCS "src/device.cpp"
                            "src/settings.cpp"
                            "src/settings_later.cpp"
                            "src/storage.cpp"
                            "src/system_info.cpp"
                            "src/time_sync.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash
                       PRIV_REQUIRES core app_update esp_app_format esp_event esp_hw_support esp_netif esp_partition esp_psram esp_timer spi_flash)
