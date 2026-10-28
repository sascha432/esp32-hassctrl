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

# The FPUs of the ESP32, S3 and P4 are single precision; double math runs in software. A float that
# becomes a double without a cast is an error in our own components (printf arguments take the
# cast). esp_driver_ppa is a copy of the IDF's driver and stays as it is.
idf_build_get_property(hc_components BUILD_COMPONENTS)
string(TOLOWER "${CMAKE_CURRENT_LIST_DIR}" hc_root)
foreach(name IN LISTS hc_components)
    idf_component_get_property(dir ${name} COMPONENT_DIR)
    idf_component_get_property(lib ${name} COMPONENT_LIB)
    string(TOLOWER "${dir}" dir)
    get_target_property(type ${lib} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY" OR name STREQUAL "esp_driver_ppa")
        continue()
    endif()
    if(dir MATCHES "^${hc_root}/(components|main|recovery)(/|$)")
        target_compile_options(${lib} PRIVATE -Werror=double-promotion)
    endif()
endforeach()

# The app descriptor (esp_app_desc_t) carries __DATE__ and __TIME__, but the IDF only recompiles it
# when one of its dependencies changes, so an incremental build would keep the stamp of an older
# build. The command below has an output that is never created, so it runs with every build and
# touches the stamp; esp_app_desc.c depends on the stamp and is compiled again, and the date and time
# in the image, in the boot log and on the System pages are those of the build.
set(hc_app_desc_stamp "${CMAKE_BINARY_DIR}/app_desc.stamp")
add_custom_command(OUTPUT "${hc_app_desc_stamp}" "${CMAKE_BINARY_DIR}/app_desc.always"
    COMMAND ${CMAKE_COMMAND} -E touch "${hc_app_desc_stamp}"
    COMMENT "Stamping the app descriptor with the build time"
    VERBATIM)
add_custom_target(hc_app_desc_stamp DEPENDS "${hc_app_desc_stamp}")
add_dependencies(__idf_esp_app_format hc_app_desc_stamp)
set_source_files_properties("$ENV{IDF_PATH}/components/esp_app_format/esp_app_desc.c"
    TARGET_DIRECTORY __idf_esp_app_format PROPERTIES OBJECT_DEPENDS "${hc_app_desc_stamp}")
