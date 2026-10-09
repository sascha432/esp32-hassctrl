# The Home Assistant connection (docs/architecture.md section 6).
# esp_websocket_client and cJSON come from idf_component.yml.
idf_component_register(SRCS "src/discover.cpp"
                            "src/hass.cpp"
                            "src/image_hw.cpp"
                            "src/media.cpp"
                            "src/new_jpeg.cpp"
                            "src/url.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES core memory
                       PRIV_REQUIRES esp-tls esp_driver_jpeg esp_driver_ppa esp_event esp_http_client esp_mm esp_netif esp_timer hass_protocol mbedtls platform)
