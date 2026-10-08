#pragma once

#include <cstdint>

#include "esp_err.h"

namespace hc::webserver::detail {

// Answers every DNS question for an IPv4 address with `address` (network byte order); every other
// question gets an empty answer. Runs in a task of its own.
esp_err_t start_captive_dns(uint32_t address);

} // namespace hc::webserver::detail
