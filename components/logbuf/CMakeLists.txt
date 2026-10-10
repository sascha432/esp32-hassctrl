# The log of the web UI (docs/ui/web.md section 6): the last messages of esp_log in RAM.
idf_component_register(SRCS "src/logbuf.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES log
                       PRIV_REQUIRES esp_hw_support freertos heap platform)
