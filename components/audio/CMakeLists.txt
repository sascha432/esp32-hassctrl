# Sound through the board's I2S amplifier (docs/architecture.md section 9.6). Without
# CONFIG_HC_AUDIO the component has no sources: main and devconsole leave their calls out, so
# nothing of it is in the firmware.
set(srcs "")
if(CONFIG_HC_AUDIO)
    list(APPEND srcs "src/audio.cpp" "src/sources.cpp")
    if(CONFIG_HC_AUDIO_TOUCH_SOUND)
        list(APPEND srcs "src/click.cpp")
    endif()
    if(CONFIG_HC_AUDIO_MP3)
        # the decoder comes from idf_component.yml, only with this option
        list(APPEND srcs "src/mp3_source.cpp")
    endif()
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_i2s freertos heap)
