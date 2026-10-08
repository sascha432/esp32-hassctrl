#pragma once

// Ambient light from an LDR on an ADC1 pin, the other source of hc::light_sensor next to the
// BH1750. Wiring: LDR from 3V3 to the pin, a 10 kOhm resistor from the pin to GND (low side), so
// the voltage rises with the light.
//
// An LDR is not linear: its resistance falls with the light roughly as a power law, so the
// pin voltage is turned into the LDR's resistance first (the quantity that is a straight line
// against lux in a log-log plot) and the resistance into lux with a table of calibration points.
// Between two points the table is interpolated in log-log, beyond the outer points the nearest
// segment continues. Without points a datasheet-like curve for a GL5528 is used (about 15 kOhm
// at 10 lx, gamma 0.7), which is only good for "bright" and "dark".
//
// Points are made with a reference lux meter (console `light calibrate <lux>` adds one for the
// light of the moment) and kept in NVS for that device.

#include <cstddef>

#include "esp_err.h"

namespace hc::light_sensor::ldr {

struct Point {
    float ohms;
    float lux;
};

constexpr size_t kMaxPoints = 8;

// Starts the measuring task on `gpio` (an ADC1 pin: 32-39 on the ESP32). ESP_ERR_INVALID_ARG for
// any other pin.
esp_err_t start(int gpio);

// whether the LDR is the active light source
bool active();

// The average of the last second: the pin voltage and the LDR's resistance. False before the
// first average.
bool reading(float &millivolts, float &ohms);

// calibrated lux; with fewer than two points the table is partly the default curve
bool lux(float &out);

// lux of the default curve, for comparing it with the points
bool default_lux(float &out);

// Adds a point: the light of the moment is `reference_lux`. A point close to an existing
// resistance replaces it. ESP_ERR_NO_MEM when the table is full, ESP_ERR_INVALID_STATE without a
// reading.
esp_err_t add_point(float reference_lux);

// the same with a resistance given (a point written down from an earlier measurement)
esp_err_t add_point(float ohms, float reference_lux);

// back to the default curve (stored)
esp_err_t clear_points();

// copies the points sorted by resistance, returns how many there are
size_t points(Point *out, size_t max);

} // namespace hc::light_sensor::ldr
