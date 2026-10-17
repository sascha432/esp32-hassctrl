# Wi-Fi and hotspot (docs/architecture.md sections 9.1, 9.2, 9.4).
idf_component_register(SRCS "src/coprocessor.cpp"
                            "src/merge.cpp"
                            "src/api.cpp"
                            "src/commands.cpp"
                            "src/network.cpp"
                            "src/reasons.cpp"
                            "src/selection.cpp"
                            "src/station.cpp"
                            "src/store.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_event esp_http_client esp_netif esp_wifi platform)
