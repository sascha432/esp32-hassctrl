# The heap for large data, PSRAM on the device (docs/architecture.md section 11). Pure C++.
idf_component_register(SRCS "src/large.cpp"
                       INCLUDE_DIRS "include")
