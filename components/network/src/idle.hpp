#pragma once

// The time since the last activity for the Wi-Fi's modem sleep (docs/architecture.md section 9.2).
// Plain C++ without ESP-IDF, so it can be tested on the PC (host/tests/network_test.cpp).

#include <atomic>
#include <cstdint>

namespace hc::network::detail {

// Ticks since the activity stamp `at` (other tasks write it), at most `delay`. A stamp older than
// `delay` is moved up to `now - delay`: the difference of two tick counts turns negative after 2^31
// ticks (24.8 days at 1 kHz), and a device without activity for that long would never sleep again.
// A stamp another task wrote meanwhile stays. Called at least once a second (the network task).
uint32_t idle_ticks(std::atomic<uint32_t> &at, uint32_t now, uint32_t delay);

} // namespace hc::network::detail
