# What goes over the WebSocket of Home Assistant (docs/architecture.md section 6). Pure C++ with
# cJSON, built by the host tests as well. cJSON comes from idf_component.yml.
idf_component_register(SRCS "src/messages.cpp" "src/page.cpp" "src/history.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES core memory
                       PRIV_REQUIRES formats)
