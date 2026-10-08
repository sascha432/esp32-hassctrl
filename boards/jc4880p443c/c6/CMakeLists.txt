# Wi-Fi firmware of the ESP32-C6 on the JC4880P443C (docs/devices/JC4880P443C): the esp_hosted
# coprocessor, same version as the host side in components/network. A separate ESP-IDF project for
# the C6, sent to it from the P4 over SDIO with the console command `c6 update <url>`.
#
#   idf.py -C boards/jc4880p443c/c6 -B build/jc4880p443c/c6 set-target esp32c6 build
cmake_minimum_required(VERSION 3.22)

# the generated sdkconfig goes to the build directory, like the apps of tools/build.py
set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/../../../build/jc4880p443c/c6/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hassctrl-c6)
