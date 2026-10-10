# The recovery app (docs/architecture.md section 9.5): built with HC_APP=recovery, flashed to the
# factory partition. Small, no LVGL, no Home Assistant. Its screen is drawn by hand: font8x8.inc
# (tools/gen_recovery_font.py) and qrcodegen.c (Project Nayuki, MIT).
idf_component_register(SRCS "recovery.cpp" "qrcodegen.c"
                       PRIV_REQUIRES board esp_event esp_lcd logbuf network ota platform webserver)
