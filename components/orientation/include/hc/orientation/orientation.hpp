#pragma once

// Screen orientation from an MPU6050 on the board's I2C bus (docs/architecture.md section 8.1).
//
// A task samples the accelerometer every 100 ms. The direction of gravity in the panel plane gives
// the orientation, also while the panel leans back on a desk; only while it lies (almost) flat or
// gravity points near a diagonal, the last orientation is kept. A new orientation is only taken
// over when the accelerometer reading is steady (the gyroscope is not used) and the orientation held
// for 500 ms. The defaults are those of the old firmware (kfc_fw Sensor_MPU6050), which needed no
// calibration.
//
// A module that is mounted differently (not mirrored on the back of the panel, rotated, at an
// angle) is measured once by the developer when the board is added (docs/adding-a-board.md,
// console `orient`): the panel upright in the base orientation, then turned 90 degrees clockwise,
// each time exactly upright so gravity lies in the panel plane. The two gravity directions in the
// module's own axes go into the board info (`imu_axes`); the orientation is then the one of +g0,
// +g1, -g0, -g1 that gravity points along. Without them the fixed mapping applies. Nothing is
// stored on the device.

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace hc::orientation {

// display rotation in 90 degree steps, the same steps as LVGL's rotation
enum class Rotation : uint8_t {
    R0 = 0,
    R90 = 1,
    R180 = 2,
    R270 = 3,
};

// Called from the orientation task: once with the orientation at start (as soon as the device is
// not lying flat) and after every change.
using Callback = void (*)(Rotation rotation, void *context);

// Starts the task. `axes` are the six floats of the board's `imu_axes` (what the module reads upright
// in the base orientation, then turned 90 degrees clockwise), nullptr or all zero for the fixed
// mapping. `offset` are the three floats of `imu_offset`, taken off every reading (nullptr = none). Fails when no MPU6050 answers at the address.
esp_err_t start(i2c_master_bus_handle_t bus, uint8_t address, const float *axes, const float *offset,
                Callback callback, void *context);

// Gravity in the module's axes (in g), the average of the last second. False without a fresh
// reading; `at_rest` tells whether the device held still during that second.
struct Gravity {
    float x, y, z;
    bool at_rest;
    float cx, cy, cz; // without the offset of the board info
    float motion_g;   // largest change of the reading between two samples in the last second, about 0.01 at rest
};
bool gravity(Gravity &out);

// reads of the module that failed since boot (each one is also logged as a warning)
uint32_t failed_reads();
// reads that needed a retry (the timeouts the retry hid)
uint32_t retried_reads();

// The developer's measurement (console `orient capture`). Five poses, each held still: the panel
// exactly upright in the base orientation (0: the way up the screen text reads at rotation 0, which
// is not always the way you would call landscape), turned 90 degrees clockwise (90), 180 and 270, all
// standing on a flat surface, and lying flat on the table with the screen up. Opposite poses cancel
// the offset of the module, so a module that reads wrong is measured as well.
enum class Pose : uint8_t { Up0, Up90, Up180, Up270, Flat };
struct Measurement {
    float length;          // g of this capture
    uint8_t captured;      // bit per Pose
    bool complete;         // all five captured and consistent
    float axes[6];         // for the board's `imu_axes`
    float offset[3];       // for the board's `imu_offset`
    float gain;            // g the module reads for 1 g (about 1 for a healthy module)
    float angle_deg;       // between pose 0 and 90, 90 when the panel stood exactly upright
    float tilt_deg;        // of the module against the panel plane (0 = flat on the back)
    float offset_mismatch; // g between the offsets of the two pairs, near 0 when all poses were still
};
// ESP_ERR_INVALID_STATE: no fresh reading or the device is moving; ESP_ERR_INVALID_RESPONSE: the
// poses do not fit together, the reason is logged. Captures stay until the next restart.
esp_err_t capture(Pose pose, Measurement &out);
esp_err_t clear_captures();

// The module's registers and one raw sample, for finding out why a reading is off (console
// `orient raw`). Reads the bus directly.
struct Raw {
    uint8_t who_am_i, power_management, config, accel_config;
    int16_t accel[3];
    int16_t gyro[3];
};
esp_err_t read_raw(Raw &out);

} // namespace hc::orientation
