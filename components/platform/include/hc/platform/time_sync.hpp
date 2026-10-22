#pragma once

// Wall clock time over SNTP and the time zone. The clock is needed for the expiry of login tokens
// and for the clock in the status bar (`--:--` until it is set, docs/ui/lvgl/lvgl-settings.md section 1).
//
// SNTP asks as soon as the station has an address, and again after every new address: started
// before the network is up, lwIP's retries back off to minutes. Then lwIP asks again every hour
// (CONFIG_LWIP_SNTP_UPDATE_DELAY). The servers are set on the web UI's Time page or by a backup.
//
// The time zone comes from Home Assistant (its configuration) unless one is fixed on the Time page
// or by a backup. Both are kept in the settings (namespace `time`), so the local time is right
// after a restart before Home Assistant answers.

#include <cstddef>
#include <ctime>

#include "esp_err.h"

namespace hc::platform {

// up to two servers (CONFIG_LWIP_SNTP_MAX_SERVERS), a host name or an address of up to 63 bytes
constexpr size_t kTimeServers = 2;
constexpr size_t kTimeServerSize = 64;
// the servers while none is set
constexpr const char *kDefaultTimeServers[kTimeServers] = {"pool.ntp.org", "time.cloudflare.com"};

// the stored servers, an empty name is none; all empty means the defaults
struct TimeServers {
    char names[kTimeServers][kTimeServerSize] = {};
};

// Sets the stored time zone and servers and prepares SNTP. Call after the network component.
esp_err_t start_time_sync();

// true once the clock has been set
bool time_synced();

// when the last answer of a server set the clock, 0 before the first one
std::time_t last_time_sync();

// Home Assistant's time zone by its IANA name ("Europe/Berlin"), kept for the next start; nothing
// is written when it did not change. In use unless a zone is fixed. False when the firmware does
// not know the name.
bool set_hass_time_zone(const char *iana_name);

// Fixes the time zone (written at once); an empty name follows Home Assistant's again. False when
// the firmware does not know the name.
bool set_fixed_time_zone(const char *iana_name);

// the IANA name of the time zone in use, empty while there is none (UTC)
void time_zone(char *out, size_t size);
// the fixed zone, empty when Home Assistant's is followed
void fixed_time_zone(char *out, size_t size);
// Home Assistant's zone, empty before it first answered
void hass_time_zone(char *out, size_t size);

// the stored servers (empty names: the defaults are used)
TimeServers time_servers();

// False with the problem in `error` when a name is neither a host name nor an address.
bool check_time_servers(const TimeServers &servers, char *error, size_t error_size);

// Checks, stores (at once) and uses the servers: SNTP starts again with them. False with the
// problem in `error`; nothing changes then.
bool set_time_servers(const TimeServers &servers, char *error, size_t error_size);

} // namespace hc::platform
