# Firmware and file system updates (docs/architecture.md section 9.5, docs/ui/web/web-app.md section 5).
idf_component_register(SRCS "src/ota.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_update board bootloader_support esp_app_format esp_http_client esp_partition
                                     esp_system esp_timer mbedtls platform)
