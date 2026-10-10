# Serial console for development (CONFIG_HC_DEV_CONSOLE).
idf_component_register(SRCS "src/devconsole.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES audio board console core dashboard esp_driver_uart esp_driver_usb_serial_jtag esp_system hass light_sensor logbuf network orientation platform touch_input ui uidebug vibration webserver)
