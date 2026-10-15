# A motion sensor on a GPIO, published to Home Assistant (docs/architecture.md section 9.7b).
idf_component_register(SRCS "src/motion.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_gpio esp_timer)
