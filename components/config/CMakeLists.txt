# The dashboard configuration: stored JSON, YAML import and export (docs/architecture.md section 5).
# Pure C++, built by the host tests as well. cJSON comes from idf_component.yml.
idf_component_register(SRCS "src/export.cpp" "src/import.cpp" "src/json.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES core memory
                       PRIV_REQUIRES formats)
