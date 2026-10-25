# The microSD card (docs/architecture.md section 9.3). Without CONFIG_HC_SDCARD the component has
# no sources: main, the webserver and the UI leave their calls out. The requirements cannot depend
# on Kconfig (ESP-IDF reads them before the configuration); with no sources nothing of fatfs is
# linked.
set(srcs "")
if(CONFIG_HC_SDCARD)
    list(APPEND srcs "src/sdcard.cpp" "src/console.cpp" "src/fetch.cpp" "src/files.cpp" "src/wav.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES board esp_common
                       PRIV_REQUIRES console esp_driver_gpio esp_driver_sdmmc esp_http_client esp_hw_support esp_timer fatfs freertos mbedtls sdmmc)
