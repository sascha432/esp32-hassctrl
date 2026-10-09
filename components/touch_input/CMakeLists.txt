# The touch screen as LVGL's pointer, sampled by a task of its own (hc::touch_input).
# LVGL comes from idf_component.yml, esp_lcd_touch from the board component's dependencies.
idf_component_register(SRCS "src/touch_input.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES board
                       PRIV_REQUIRES freertos)
