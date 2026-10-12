#pragma once

// Wall clock time over SNTP and the time zone. The clock is needed for the expiry of login tokens
// and for the clock in the status bar (`--:--` until it is set, docs/ui/lvgl/lvgl-settings.md section 1).
//
// SNTP asks as soon as the station has an address, and again after every new address: started
// before the network is up, lwIP's retries back off to minutes. The time zone comes from Home
// Assistant (its configuration) and is kept in the settings, so the local time is right after a
// restart before Home Assistant answers.

#include <cstddef>

#include "esp_err.h"

namespace hc::platform {

// Sets the stored time zone and prepares SNTP (pool.ntp.org, time.cloudflare.com). Call after
// the network component.
esp_err_t start_time_sync();

// true once the clock has been set
bool time_synced();

// Sets the time zone by its IANA name ("Europe/Berlin") and keeps it for the next start; nothing
// is written when it did not change. False when the firmware does not know the name.
bool set_time_zone(const char *iana_name);

// the IANA name of the time zone in use, empty while there is none (UTC)
void time_zone(char *out, size_t size);

} // namespace hc::platform
