#pragma once

namespace hc::platform {

// Logs the firmware version, chip, flash and PSRAM size, the running and the boot partition, the
// reset reason and the free memory. Both applications call it once at boot, so the first log of
// a board shows what the hardware really has.
void log_system_info();

} // namespace hc::platform
