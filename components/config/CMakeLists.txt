# The dashboard configuration: stored JSON, YAML import and export (docs/architecture.md section 5),
# and the backup file of the settings and the dashboard. Pure C++, built by the host tests as well.
# cJSON comes from idf_component.yml.
idf_component_register(SRCS "src/backup.cpp" "src/export.cpp" "src/import.cpp" "src/json.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES core formats memory)
