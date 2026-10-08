# The recovery app (docs/architecture.md section 9.5): built with HC_APP=recovery, flashed to the
# factory partition. Small, no LVGL, no Home Assistant.
idf_component_register(SRCS "recovery.cpp"
                       PRIV_REQUIRES board esp_event network ota platform webserver)
