# The Home Assistant connection (docs/architecture.md section 6).
# esp_websocket_client and cJSON come from idf_component.yml.
idf_component_register(SRCS "src/config.cpp"
                            "src/discover.cpp"
                            "src/hass.cpp"
                            "src/image_hw.cpp"
                            "src/media.cpp"
                            "src/media_audio.cpp"
                            "src/media_decode.cpp"
                            "src/media_http.cpp"
                            "src/new_jpeg.cpp"
                            "src/say.cpp"
                            "src/tls.cpp"
                            "src/url.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES core memory
                       PRIV_REQUIRES esp-tls esp_driver_jpeg esp_driver_ppa esp_event esp_http_client esp_mm esp_netif esp_timer hass_protocol mbedtls platform)

# esp_new_jpeg's own buffers go through hot_alloc (new_jpeg.cpp); without the library (ESP32,
# P4) nothing calls them and the options do nothing
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=jpeg_calloc_align_inner" "-Wl,--wrap=jpeg_calloc_inner")
