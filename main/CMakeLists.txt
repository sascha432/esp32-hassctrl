# The panel firmware: composition root only (docs/architecture.md section 3).
# esp_lvgl_port and LVGL come from idf_component.yml.
idf_component_register(SRCS "main.cpp" "ui_backend.cpp"
                       PRIV_REQUIRES app_update config dashboard esp_app_format esp_timer board hass heap memory light_sensor devconsole esp_event frame_display network orientation platform ui uidebug webserver)

# LVGL allocates from PSRAM (main/lvgl/hc_lv_mem.h). Its Kconfig only names the header, so the
# functions are set here, on the LVGL library alone.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/lvgl")
target_compile_definitions(${lvgl_lib} PRIVATE LV_MEM_CUSTOM_ALLOC=hc_lv_malloc LV_MEM_CUSTOM_FREE=hc_lv_free
                                               LV_MEM_CUSTOM_REALLOC=hc_lv_realloc)
