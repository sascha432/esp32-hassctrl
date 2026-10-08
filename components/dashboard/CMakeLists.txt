# The dashboard of the device: stored file and current layout (docs/architecture.md section 5).
idf_component_register(SRCS "src/dashboard.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES config core memory
                       PRIV_REQUIRES esp_common log platform)
