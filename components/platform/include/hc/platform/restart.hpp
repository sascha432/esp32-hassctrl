#pragma once

namespace hc::platform {

// esp_restart() with a deadline: the RTC watchdog resets the chip kRestartDeadlineMs from now,
// should a shutdown handler (backlight, pending settings, Wi-Fi) or esp_restart itself wait for
// good. Seen once on the JC3248W535 with 10 KB RAM free: Wi-Fi stopped, the board stayed up. The
// next start then reports a watchdog reset instead of a software reset. Every restart of the
// firmware goes through here.
constexpr unsigned kRestartDeadlineMs = 5000;

[[noreturn]] void restart();

} // namespace hc::platform
