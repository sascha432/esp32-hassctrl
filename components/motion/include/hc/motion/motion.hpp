#pragma once

// A motion sensor (PIR) on a GPIO (docs/architecture.md section 9.7b): the board names the pin and
// the level its output has while it sees motion (hc::board::Info::motion_gpio, motion_active_high).
// The pin is sampled every 100 ms; a level that holds for two samples is the state. main hands
// every change to hc::hass::publish_motion, which sets input_text.<device>_motion in Home
// Assistant to "Detected" or "Clear".

#include "esp_err.h"

namespace hc::motion {

// Called from the esp_timer task with every change of the state, and once with the state at start.
using Handler = void (*)(bool occupied);

// The sensor on `gpio`, `active_high`: its output is high while it sees motion. The pin gets the
// internal pull toward the other level, so an unplugged sensor reads clear.
esp_err_t start(int gpio, bool active_high, Handler on_change);

// A sensor is set up.
bool running();

// The state now (false without a sensor).
bool occupied();

// The GPIO of the sensor, -1 without one.
int gpio();

// Whether the sensor's output is high while it sees motion.
bool active_high();

} // namespace hc::motion
