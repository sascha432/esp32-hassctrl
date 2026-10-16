# The dashboard model (docs/architecture.md section 5). Pure C++, built by the host tests as well.
idf_component_register(SRCS "src/alarm.cpp" "src/entity.cpp" "src/icons.cpp" "src/image.cpp" "src/layout.cpp" "src/pending.cpp" "src/timezone.cpp" "src/weather.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES memory)
