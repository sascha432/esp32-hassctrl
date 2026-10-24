# Text formats of the API and the files: JSON writer, YAML reader (docs/architecture.md section 5).
# Pure C++, built by the host tests as well.
idf_component_register(SRCS "src/json_writer.cpp" "src/m3u.cpp" "src/mp3_tags.cpp" "src/multipart.cpp" "src/yaml.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES memory)
