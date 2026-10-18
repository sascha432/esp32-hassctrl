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

    # The copy must carry the patch and, without it, equal the IDF's file: after an IDF update the
    # copy would otherwise replace a newer driver file unnoticed (README.md, "Keeping it").
    set(copy_file "${CMAKE_CURRENT_LIST_DIR}/ppa_srm.c")
    set(idf_file "${idf_ppa}/src/ppa_srm.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${copy_file}" "${idf_file}")
    file(READ "${copy_file}" copy_src)
    file(READ "${idf_file}" idf_src)
    string(REPLACE "\r\n" "\n" copy_src "${copy_src}")
    string(REPLACE "\r\n" "\n" idf_src "${idf_src}")
    set(patch_end "    bypass_mb_order = true;\n")
    string(FIND "${copy_src}" "    // hassctrl: always bypass." patch_start)
    if(patch_start GREATER_EQUAL 0)
        string(SUBSTRING "${copy_src}" ${patch_start} -1 patch_rest)
        string(FIND "${patch_rest}" "${patch_end}" patch_len)
    endif()
    if(patch_start LESS 0 OR patch_len LESS 0)
        message(FATAL_ERROR "${copy_file} is not patched: the line 'bypass_mb_order = true;' after "
                            "the comment '// hassctrl: always bypass.' is missing (see README.md "
                            "beside it). Without it the PPA hangs on the P4 (DIG-734).")
    endif()
    string(LENGTH "${patch_end}" end_len)
    math(EXPR after "${patch_start} + ${patch_len} + ${end_len}")
    string(SUBSTRING "${copy_src}" 0 ${patch_start} before_src)
    string(SUBSTRING "${copy_src}" ${after} -1 after_src)
    if(NOT "${before_src}${after_src}" STREQUAL "${idf_src}")
        message(FATAL_ERROR "${copy_file} differs from the IDF's ${idf_file} by more than the "
                            "hassctrl patch. IDF file edited: restore it (git checkout in the IDF), "
                            "the patch belongs only in the copy. IDF updated: copy the IDF's file "
                            "to ${copy_file} and add the four patch lines again. See README.md "
                            "beside it:\n  diff ${idf_file} ${copy_file}")
    endif()
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "${idf_ppa}/include"
                       PRIV_INCLUDE_DIRS "${idf_ppa}/src"
                       REQUIRES esp_hal_ppa
                       PRIV_REQUIRES esp_mm esp_pm esp_driver_dma efuse)
