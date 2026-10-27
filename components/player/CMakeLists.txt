# The media player (docs/architecture.md section 9.6): the queue of what plays, independent of the
# screen. Without CONFIG_HC_UI_PLAYER the component has no sources. queue.cpp and tree.cpp are pure
# C++, built by the host tests as well.
set(srcs "")
if(CONFIG_HC_UI_PLAYER)
    list(APPEND srcs "src/player.cpp" "src/queue.cpp" "src/tree.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES audio core formats memory
                       PRIV_REQUIRES esp_hw_support hass platform sdcard)
