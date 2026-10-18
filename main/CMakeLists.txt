# The panel firmware: composition root only (docs/architecture.md section 3).
# esp_lvgl_port and LVGL come from idf_component.yml.
set(srcs "backup.cpp" "bench_mode.cpp" "main.cpp" "ui_backend.cpp" "ui_sound.cpp")
if(CONFIG_HC_BOARD_HAS_CAMERA)
    # the preview of Settings, Camera (components/camera)
    list(APPEND srcs "camera_preview.cpp")
endif()
# the benchmark mode's dashboard and the version of its data (docs/benchmark.md)
idf_component_register(SRCS ${srcs}
                       PRIV_REQUIRES app_update audio camera config console esp_mm dashboard esp_app_format esp_timer board hass heap memory light_sensor logbuf devconsole esp_event frame_display motion network orientation platform relay sample_backend touch_input ui uidebug vibration webserver
                       EMBED_TXTFILES "${PROJECT_DIR}/bench/dashboard/hass.yaml" "${PROJECT_DIR}/bench/VERSION")

# LVGL allocates from PSRAM (main/lvgl/hc_lv_mem.h). Its Kconfig only names the header, so the
# functions are set here, on the LVGL library alone.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/lvgl")
target_compile_definitions(${lvgl_lib} PRIVATE LV_MEM_CUSTOM_ALLOC=hc_lv_malloc LV_MEM_CUSTOM_FREE=hc_lv_free
                                               LV_MEM_CUSTOM_REALLOC=hc_lv_realloc)
# A failed LVGL assert (a failed allocation) halts in `while(1)` by default: the screen froze and
# only the task watchdog printed backtraces. abort() prints the panic and restarts the panel
# (`\;`: a plain ; separates CMake list items).
target_compile_definitions(${lvgl_lib} PRIVATE "LV_ASSERT_HANDLER_INCLUDE=<stdlib.h>" "LV_ASSERT_HANDLER=abort()\;")
