# Ambient light from a BH1750 or an LDR for the automatic brightness.
idf_component_register(SRCS "src/light_sensor.cpp" "src/ldr.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES esp_adc freertos platform)
