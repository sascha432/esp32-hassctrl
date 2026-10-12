# The IPA tuning of the OV02C10 (auto exposure, white balance ...) for esp_ipa, as esp_cam_sensor
# does for its own sensors in its project_include.cmake.
if(CONFIG_CAMERA_OV02C10)
    idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${COMPONENT_PATH}/ov02c10/cfg/ov02c10_default.json" APPEND)
endif()
