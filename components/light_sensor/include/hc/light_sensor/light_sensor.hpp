#pragma once

// Ambient light from a BH1750 on the board's I2C bus, for the automatic brightness (Settings,
// Display). The sensor is optional per device, not per board: start() looks for it at both of
// its addresses, 0x23 (ADDR low) and 0x5C (ADDR high). A floating ADDR pin can end up as either.
//
// A task measures continuously in high resolution mode (1 lx steps, 120 ms per measurement) and
// keeps the average of the last second. A sensor that stops answering (loose wire, no supply) is
// tried again every 2 s only, so the shared bus stays free for the touch controller.
//
// Calibration is a step of adding a board (docs/adding-a-board.md, console `light`), not a user
// setting: lux() is the measurement times the board's scale (hc::board::Info::light_sensor_scale).
// calibrate() finds the scale for a known light and keeps it in NVS for that device while the
// board is set up; the result goes into the board info.

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace hc::light_sensor {

// Starts the task. ESP_ERR_NOT_FOUND when no BH1750 answers.
esp_err_t start(i2c_master_bus_handle_t bus, float default_scale);

// The ambient light in calibrated lux; false without a sensor or while it does not answer.
bool lux(float &out);

// what the sensor measures, without the scale
bool raw_lux(float &out);

// Makes the current light read as `reference_lux` (stored). ESP_ERR_INVALID_STATE without a
// reading, ESP_ERR_INVALID_ARG when it is too dark to calibrate (below 1 lx measured).
esp_err_t calibrate(float reference_lux);

// back to the board's default scale (stored)
esp_err_t reset_calibration();

// the scale in use and whether it is the device's own
float scale(bool *calibrated = nullptr);

} // namespace hc::light_sensor
