#pragma once

#include <cstdint>

namespace hc::network::detail {

// A disconnect reason of the Wi-Fi driver in words, for the alert strip and the logs.
const char *reason_text(uint8_t reason);

// true when the reason means the network rejected the password
bool is_auth_failure(uint8_t reason);

// true when a failed attempt is worth one more try at the same network: a router with WPA3 and
// protected management frames still holds the connection of a device that lost power
bool may_pass_on_retry(uint8_t reason);

} // namespace hc::network::detail
