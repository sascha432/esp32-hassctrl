# Board support (docs/architecture.md section 8): one directory per board in boards/, compiled
# only when its Kconfig option is selected. No LVGL in this component, so the recovery app can
# use it too.
set(srcs "src/backlight.cpp")
if(CONFIG_HC_BOARD_WT32_SC01)
    list(APPEND srcs "boards/wt32_sc01/wt32_sc01.cpp")
endif()
if(CONFIG_HC_BOARD_TTGO_T4)
    list(APPEND srcs "boards/ttgo_t4/ttgo_t4.cpp")
endif()
if(CONFIG_HC_BOARD_JC3248W535)
    list(APPEND srcs "boards/jc3248w535/jc3248w535.cpp")
endif()
if(CONFIG_HC_BOARD_JC4880P443C)
    list(APPEND srcs "boards/jc4880p443c/jc4880p443c.cpp")
endif()

# panel and touch drivers from the component registry: idf_component.yml
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_lcd esp_driver_i2c
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_spi)

# the board tag is never referenced by code: keep it in the image (docs: app_image_format)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u hc_image_tag")
