# A vibration motor on a GPIO (docs/architecture.md section 9.7). Without CONFIG_HC_VIBRATION the
# component has no sources: main and devconsole leave their calls out.
set(srcs "")
if(CONFIG_HC_VIBRATION)
    list(APPEND srcs "src/vibration.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_gpio esp_timer freertos)
