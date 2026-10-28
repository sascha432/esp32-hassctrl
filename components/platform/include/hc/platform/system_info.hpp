#pragma once

#include <cstddef>

namespace hc::platform {

// The build time of the running application as "2026-10-27 12:34" (local time of the building
// machine, from the app descriptor's __DATE__ and __TIME__; the root CMakeLists.txt recompiles the
// descriptor with every build). `size` of 24 holds it. Returns false and the raw text when the
// descriptor has no usable date, as in a reproducible build (CONFIG_APP_REPRODUCIBLE_BUILD).
bool build_time(char *out, size_t size);

// Logs the firmware version, chip, flash and PSRAM size, the running and the boot partition, the
// reset reason and the free memory. Both applications call it once at boot, so the first log of
// a board shows what the hardware really has.
void log_system_info();

} // namespace hc::platform
