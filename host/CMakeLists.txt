# Host build: the pure components compiled for the PC and tested there (docs/architecture.md
# section 2). Plain CMake, not the ESP-IDF build system; tools/host_test.py runs it with MSVC.
cmake_minimum_required(VERSION 3.22)
project(hassctrl_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HC_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

if(MSVC)
    # /utf-8: the sources are UTF-8 (°C); /permissive-: standard C++; _CRT_SECURE_NO_WARNINGS:
    # the code uses snprintf and strncpy deliberately, not the _s variants
    add_compile_options(/permissive- /utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    set(HC_WARNINGS /W4)
else()
    set(HC_WARNINGS -Wall -Wextra)
endif()

# cJSON as the firmware gets it from the component registry (tools/build.py downloads it into
# managed_components/ with the first firmware build)
set(HC_CJSON "${HC_ROOT}/managed_components/espressif__cjson/cJSON")
if(NOT EXISTS "${HC_CJSON}/cJSON.c")
    message(FATAL_ERROR "cJSON is missing: build the firmware once (python tools/build.py ttgo_t4)")
endif()
add_library(cjson STATIC ${HC_CJSON}/cJSON.c)
target_include_directories(cjson PUBLIC ${HC_CJSON})

add_executable(hc_tests
    tests/alarm_test.cpp
    tests/backup_test.cpp
    tests/check.cpp
    tests/config_test.cpp
    tests/layout_test.cpp
    tests/multipart_test.cpp
    tests/network_test.cpp
    tests/protocol_test.cpp
    tests/url_test.cpp
    tests/weather_test.cpp
    tests/yaml_test.cpp
    ${HC_ROOT}/components/config/src/backup.cpp
    ${HC_ROOT}/components/config/src/export.cpp
    ${HC_ROOT}/components/config/src/import.cpp
    ${HC_ROOT}/components/config/src/json.cpp
    ${HC_ROOT}/components/core/src/alarm.cpp
    ${HC_ROOT}/components/core/src/entity.cpp
    ${HC_ROOT}/components/core/src/icons.cpp
    ${HC_ROOT}/components/core/src/image.cpp
    ${HC_ROOT}/components/core/src/layout.cpp
    ${HC_ROOT}/components/core/src/pending.cpp
    ${HC_ROOT}/components/core/src/weather.cpp
    ${HC_ROOT}/components/formats/src/json_writer.cpp
    ${HC_ROOT}/components/formats/src/multipart.cpp
    ${HC_ROOT}/components/formats/src/yaml.cpp
    ${HC_ROOT}/components/hass/src/url.cpp
    ${HC_ROOT}/components/hass_protocol/src/forecast.cpp
    ${HC_ROOT}/components/hass_protocol/src/messages.cpp
    ${HC_ROOT}/components/hass_protocol/src/page.cpp
    ${HC_ROOT}/components/hass_protocol/src/history.cpp
    ${HC_ROOT}/components/hass_protocol/src/picker.cpp
    ${HC_ROOT}/components/memory/src/large.cpp
    ${HC_ROOT}/components/network/src/merge.cpp
    ${HC_ROOT}/components/network/src/selection.cpp
)
target_include_directories(hc_tests PRIVATE
    tests
    ${HC_ROOT}/components/config/include
    ${HC_ROOT}/components/core/include
    ${HC_ROOT}/components/formats/include
    ${HC_ROOT}/components/hass/src
    ${HC_ROOT}/components/hass_protocol/include
    ${HC_ROOT}/components/memory/include
    ${HC_ROOT}/components/network/include
    ${HC_ROOT}/components/network/src
    # esp_err.h for the network header
    tests/stubs
)
target_link_libraries(hc_tests PRIVATE cjson)
target_compile_options(hc_tests PRIVATE ${HC_WARNINGS})
# the sample files of the tests, and the developer's converted dashboard when it is there
target_compile_definitions(hc_tests PRIVATE HC_SOURCE_ROOT="${HC_ROOT}")
