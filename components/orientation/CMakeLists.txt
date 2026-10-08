# Screen orientation from an MPU6050 accelerometer (docs/architecture.md section 8.1).
idf_component_register(SRCS "src/orientation.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES freertos)
