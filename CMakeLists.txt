# esp32-hassctrl: one ESP-IDF project, two applications (docs/architecture.md section 9.5).
#
#   HC_APP=main      the panel firmware, top-level component "main", runs from an OTA partition
#   HC_APP=recovery  the recovery app, component "recovery", runs from the factory partition
#
# Both share every component in components/, the board configs and the partition tables.
# tools/build.py sets HC_APP, the board layers and one build directory per board and app.
cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED HC_APP)
    set(HC_APP main)
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/recovery")

# COMPONENTS limits the build to the application's top-level component and what it requires, so
# the recovery app does not compile the UI, LVGL or other components it never links.
if(HC_APP STREQUAL "main")
    set(COMPONENTS main)
    set(EXCLUDE_COMPONENTS recovery)
    set(HC_PROJECT_NAME hassctrl)
elseif(HC_APP STREQUAL "recovery")
    set(COMPONENTS recovery)
    set(EXCLUDE_COMPONENTS main)
    set(HC_PROJECT_NAME hassctrl-recovery)
else()
    message(FATAL_ERROR "HC_APP must be 'main' or 'recovery', not '${HC_APP}'")
endif()

set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${HC_PROJECT_NAME})
