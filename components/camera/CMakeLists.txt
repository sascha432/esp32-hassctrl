# The MIPI-CSI camera (docs/devices/JC4880P443C): capture, its settings and /api/camera.jpg,
# /api/camera.mjpg. Without CONFIG_HC_BOARD_HAS_CAMERA it has no sources.
# ov02c10/ is the sensor driver from the vendor's demo (ov02c10/README.md).
set(srcs "")
set(priv_include_dirs "")
if(CONFIG_HC_BOARD_HAS_CAMERA)
    list(APPEND srcs "src/camera.cpp" "src/web.cpp" "ov02c10/ov02c10.c")
    list(APPEND priv_include_dirs "ov02c10/include" "ov02c10/private_include")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ${priv_include_dirs}
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES console core esp_http_server esp_driver_gpio esp_driver_jpeg esp_timer freertos mbedtls platform)

if(CONFIG_CAMERA_OV02C10_AUTO_DETECT_MIPI_INTERFACE_SENSOR)
    # esp_cam_sensor finds the detect function in a linker section; keep its object file
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ov02c10_detect")
endif()
