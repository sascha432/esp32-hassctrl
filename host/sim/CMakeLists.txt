# The Windows version of the UI (docs/architecture.md section 1): components/ui on LVGL 8.4 with a
# Windows window, sample data instead of the firmware. A plain CMake project like host/; the build
# directory is build/sim and tools/sim.py runs it with MSVC.
cmake_minimum_required(VERSION 3.22)
project(hassctrl_sim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

set(HC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
# the same LVGL sources as the firmware (the managed component of the registry)
set(LVGL_DIR "${HC_ROOT}/managed_components/lvgl__lvgl")
if(NOT EXISTS "${LVGL_DIR}/src/lvgl.h")
    message(FATAL_ERROR "LVGL not found in ${LVGL_DIR}: build the firmware once (tools/build.py) so the "
                        "component manager downloads it")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "the simulator is for Windows with MSVC (tools/sim.py)")
endif()

# ------------------------------------------------------------------------------------- LVGL
# Every source of src/: the files of the drivers and libraries that lv_conf.h does not turn on
# are empty after the preprocessor.
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS "${LVGL_DIR}/src/*.c")
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC "${LVGL_DIR}" "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE _CRT_SECURE_NO_WARNINGS)
# not our code: no warnings
target_compile_options(lvgl PRIVATE /w /utf-8)

# ------------------------------------------------------------------------------------- the UI
# components/ui/src by glob, so a new screen is picked up without editing this file. Only its own
# public header directory is part of the component's interface; the simulator also reaches the
# private headers (settings.hpp, rotation.hpp) to open pages and turn the display.
# The icon fonts (tools/build_icons.py) are C files in src/fonts.
file(GLOB UI_SOURCES CONFIGURE_DEPENDS "${HC_ROOT}/components/ui/src/*.cpp" "${HC_ROOT}/components/ui/src/fonts/*.c")
add_library(hc_ui STATIC ${UI_SOURCES})
target_include_directories(hc_ui PUBLIC "${HC_ROOT}/components/ui/include" "${HC_ROOT}/components/ui/src")
target_link_libraries(hc_ui PUBLIC lvgl hc_dashboard)
# /utf-8: the sources are UTF-8; /permissive-: standard C++, like the firmware's compiler
target_compile_options(hc_ui PRIVATE /W4 /permissive- /utf-8 /EHsc)

# ------------------------------------------------------------------------------------- the sim
add_library(win32drv STATIC win32drv/win32drv.c)
target_include_directories(win32drv PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(win32drv PUBLIC lvgl user32 gdi32)
target_compile_options(win32drv PRIVATE /W3)

# The dashboard file (--dashboard): the pure components of the firmware, as the host tests build
# them. cJSON comes from the component registry.
set(HC_CJSON "${HC_ROOT}/managed_components/espressif__cjson/cJSON")
if(NOT EXISTS "${HC_CJSON}/cJSON.c")
    message(FATAL_ERROR "cJSON is missing: build the firmware once (python tools/build.py ttgo_t4)")
endif()
add_library(cjson STATIC ${HC_CJSON}/cJSON.c)
target_include_directories(cjson PUBLIC ${HC_CJSON})
add_library(hc_dashboard STATIC
    ${HC_ROOT}/components/config/src/export.cpp
    ${HC_ROOT}/components/config/src/import.cpp
    ${HC_ROOT}/components/config/src/json.cpp
    ${HC_ROOT}/components/core/src/alarm.cpp
    ${HC_ROOT}/components/core/src/entity.cpp
    ${HC_ROOT}/components/core/src/icons.cpp
    ${HC_ROOT}/components/core/src/image.cpp
    ${HC_ROOT}/components/core/src/layout.cpp
    ${HC_ROOT}/components/core/src/pending.cpp
    ${HC_ROOT}/components/core/src/timezone.cpp
    ${HC_ROOT}/components/core/src/weather.cpp
    ${HC_ROOT}/components/formats/src/json_writer.cpp
    ${HC_ROOT}/components/formats/src/yaml.cpp
    ${HC_ROOT}/components/memory/src/large.cpp
)
target_include_directories(hc_dashboard PUBLIC
    ${HC_ROOT}/components/config/include
    ${HC_ROOT}/components/core/include
    ${HC_ROOT}/components/formats/include
    ${HC_ROOT}/components/memory/include
)
target_link_libraries(hc_dashboard PUBLIC cjson)
target_compile_options(hc_dashboard PRIVATE /W4 /permissive- /utf-8 /EHsc)

# the sample data comes from the component the firmware's benchmark mode uses as well
set(HC_SAMPLE "${HC_ROOT}/components/sample_backend")
add_executable(hc_sim main.cpp png.cpp dashboard_file.cpp ${HC_SAMPLE}/src/stub_backend.cpp ${HC_SAMPLE}/src/sample_hass.cpp)
target_include_directories(hc_sim PRIVATE ${HC_SAMPLE}/include)
target_link_libraries(hc_sim PRIVATE hc_ui hc_dashboard win32drv winmm)
target_compile_options(hc_sim PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_compile_definitions(hc_sim PRIVATE HC_SOURCE_ROOT="${HC_ROOT}")
