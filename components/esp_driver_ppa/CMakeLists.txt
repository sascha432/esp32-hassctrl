# Overrides ESP-IDF's esp_driver_ppa: a project component with the name of an IDF component
# replaces it. The sources are the IDF's own, except ppa_srm.c, the copy beside this file with one
# change (README.md: the PPA hung on the P4, silicon bug DIG-734). Same CMake as the IDF's.
file(TO_CMAKE_PATH "$ENV{IDF_PATH}" idf_path) # backslashes of a Windows path are escapes to CMake
set(idf_ppa "${idf_path}/components/esp_driver_ppa")
idf_build_get_property(target IDF_TARGET)

set(srcs)
if(CONFIG_SOC_PPA_SUPPORTED)
    list(APPEND srcs "${idf_ppa}/src/ppa_core.c"
                     "ppa_srm.c"
                     "${idf_ppa}/src/ppa_blend.c"
                     "${idf_ppa}/src/ppa_fill.c")
    if(CONFIG_SOC_PAU_SUPPORTED)
        list(APPEND srcs "${idf_ppa}/${target}/ppa_retention.c")
    endif()
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "${idf_ppa}/include"
                       PRIV_INCLUDE_DIRS "${idf_ppa}/src"
                       REQUIRES esp_hal_ppa
                       PRIV_REQUIRES esp_mm esp_pm esp_driver_dma efuse)
