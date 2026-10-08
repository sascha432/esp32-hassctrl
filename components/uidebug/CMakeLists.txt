# Screenshots and touch injection for development (docs/architecture.md section 9.4).
# LVGL and esp_lvgl_port come from idf_component.yml.
idf_component_register(SRCS "src/uidebug.cpp"
                       INCLUDE_DIRS "include")
