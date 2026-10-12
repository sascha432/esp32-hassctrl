#pragma once

// The log of the web UI (docs/ui/web/web-app.md section 4): the last messages of esp_log, kept in RAM
// (16 KB of PSRAM, the oldest lines go first).
//
// Errors and warnings are always kept; the debug mode (stored) keeps info messages as well. What
// the serial port shows is set apart from that (the console's `log` command), so a quiet console
// still fills the log: esp_log runs at the more detailed of the two levels and the output hook
// prints only what the console asked for.
//
// One line per message: `<unix time, 0 before the clock is set> <esp_log line>`, for example
// `1760092867 E (52113) wifi: wrong password`. Longer messages are cut at 200 characters.

#include <cstddef>

#include "esp_log.h"

namespace hc::logbuf {

// First thing in app_main: allocates the buffer and installs the output hook.
void start();

// after the storage is initialized: the stored debug mode
void restore();

bool debug();
void set_debug(bool on);

// The level of the serial output, for every tag and for LVGL's (the console keeps LVGL's info
// messages apart). Before the first call both are CONFIG_LOG_DEFAULT_LEVEL.
void set_console_level(esp_log_level_t level, esp_log_level_t lvgl_level);

// the size of the buffer, 0 when it could not be allocated
size_t capacity();

// Copies the log, oldest line first, into `out` (at least capacity() bytes) and returns its
// length.
size_t copy(char *out, size_t size);

} // namespace hc::logbuf
