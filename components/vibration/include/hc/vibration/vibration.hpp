#pragma once

// A vibration motor for tactile feedback (docs/architecture.md section 9.7), compiled in with
// CONFIG_HC_VIBRATION. The motor hangs on a GPIO through a transistor (a GPIO cannot drive it
// alone): high turns it on. A pulse turns it on for a few milliseconds; a newer pulse replaces the
// one that runs. The alarm pattern (on, pause, on ...) comes first: pulses are dropped while it
// runs, and only stop_alarm() ends it. An esp_timer turns the motor off, so nothing waits.

#include <cstdint>

#include "esp_err.h"

namespace hc::vibration {

// Sets up the motor's GPIO, the motor off; `click_ms` is the pulse of click() (both from
// hc::board::Info::vibration_*, wired in main). Call once.
esp_err_t start(int gpio, uint16_t click_ms);

// start() succeeded
bool running();

// the level the pin drives now, read back from the pin (console `vibrate status`)
bool motor_on();

// The motor on for `ms` (up to 2 s). From any task; returns at once, like everything below.
void pulse(uint16_t ms);

// The pulse for a tap, along with the click sound.
void click();

// The alarm pattern, repeated until stop_alarm(). With `active`, it also ends by itself, after
// the pulse that runs, once active() returns false (the sound it goes with has ended).
void start_alarm(bool (*active)() = nullptr);
void stop_alarm();

} // namespace hc::vibration
