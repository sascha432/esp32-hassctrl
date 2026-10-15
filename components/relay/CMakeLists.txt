# GPIO relays switched by Home Assistant (docs/architecture.md section 9.7a).
idf_component_register(SRCS "src/relay.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_gpio)
