# LVGL display for panels that only take whole frames (hc::board::Info::full_frames).
# LVGL comes from idf_component.yml.
idf_component_register(SRCS "src/frame_display.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd
                       PRIV_REQUIRES heap)
