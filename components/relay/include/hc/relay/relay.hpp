#pragma once

// Relays on GPIOs (docs/architecture.md section 9.7a): relay n (1 to kMaxRelays) is a GPIO, high =
// on, through a transistor or a relay board. Home Assistant switches them (hc::hass::set_relay_handler),
// the console `relay` too. Every pin is low before it becomes an output, so a relay is off while
// the board starts; the state follows Home Assistant once it is connected.

#include <cstddef>

#include "esp_err.h"

namespace hc::relay {

constexpr size_t kMaxRelays = 4;

// Relay n+1 on `gpios[n]` for n < `count`; a negative entry: no relay n+1.
esp_err_t start(const signed char *gpios, size_t count);

// At least one relay is set up.
bool running();

// Relay `relay` (1-based) on or off; false when there is no such relay. From any task.
bool set(unsigned relay, bool on);

// What the pin of relay `relay` drives; false for a relay that is not there.
bool is_on(unsigned relay);

// The GPIO of relay `relay`, -1 when there is none.
int gpio_of(unsigned relay);

} // namespace hc::relay
