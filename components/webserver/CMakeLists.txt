# Web server and captive portal (docs/ui/web/web-app.md, docs/architecture.md section 9.4).
# The SD card's routes only with CONFIG_HC_SDCARD (not in the recovery app, sdkconfig.recovery).
set(srcs "")
if(CONFIG_HC_SDCARD)
    list(APPEND srcs "src/api_sd.cpp")
endif()
idf_component_register(SRCS ${srcs}
                            "src/api_backup.cpp"
                            "src/api_camera.cpp"
                            "src/api_dashboard.cpp"
                            "src/api_files.cpp"
                            "src/api_hass.cpp"
                            "src/api_log.cpp"
                            "src/api_session.cpp"
                            "src/api_time.cpp"
                            "src/api_update.cpp"
                            "src/api_wifi.cpp"
                            "src/auth.cpp"
                            "src/builtin_page.cpp"
                            "src/captive_dns.cpp"
                            "src/http_util.cpp"
                            "src/webserver.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES memory
                       PRIV_REQUIRES core esp_app_format esp_http_server esp_netif esp_timer formats logbuf lwip mbedtls network ota platform sdcard)
