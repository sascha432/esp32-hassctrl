# Sample data instead of the network and Home Assistant (hc/sample/stub_backend.hpp): the Windows
# version of the UI runs on it (host/sim builds these files itself), the firmware in benchmark mode
# (docs/benchmark.md). LVGL comes through ui, whose manifest makes it public.
idf_component_register(SRCS "src/stub_backend.cpp" "src/sample_hass.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ui config core)
