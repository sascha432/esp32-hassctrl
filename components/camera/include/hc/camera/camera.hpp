#pragma once

// The MIPI-CSI camera of the JC4880P443C (docs/devices/JC4880P443C, "Camera"), compiled in with
// CONFIG_HC_BOARD_HAS_CAMERA. esp_video finds the sensor on the I2C bus of the board (SCCB) and
// runs the CSI controller and the ISP, which gives RGB565 frames in the sensor's default size.
// Frames are captured only while someone uses them (acquire / release): the preview of Settings,
// Camera and the web routes /api/camera.jpg and /api/camera.mjpg (HTTP Basic auth with the login
// of the stream settings; both answer 404 while the stream is off).
//
// The settings live in the "camera" namespace of the settings NVS.

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace hc::camera {

// The picture: applied to the sensor and the exposure control at once.
struct Picture {
    bool auto_exposure = true;
    int8_t brightness = 0;      // auto exposure: the target brightness moved by this, -50 to 50
    uint16_t exposure_us = 10000; // manual: the exposure time (kMinExposureUs to max_exposure_us())
    uint8_t gain = 16;          // manual: the analog gain in 1/16, kMinGain to kMaxGain
    bool mirror = false;
    bool flip = false;          // upside down
    uint8_t mains_hz = 60;      // 50 or 60: the exposure avoids the flicker of the lights
};

constexpr uint16_t kMinExposureUs = 100;
constexpr uint8_t kMinGain = 16;  // 1x
constexpr uint8_t kMaxGain = 248; // 15.5x
constexpr int8_t kMaxBrightness = 50;

constexpr size_t kLoginSize = 33; // username and password, terminated

// The web stream.
struct Stream {
    bool enabled = false;
    bool half = false;     // half the sensor's width and height
    uint8_t max_fps = 30;  // 5, 10, 15 or 30
    uint8_t quality = 70;  // JPEG, 30 to 95
    char username[kLoginSize] = {};
    char password[kLoginSize] = {};
};

// Probes the sensor on `bus`, sets up the capture and applies the stored picture settings; logs
// the sensor and its format. An error when no sensor answers: everything below then does nothing.
esp_err_t start(i2c_master_bus_handle_t bus);

// start() found a sensor
bool available();

// the sensor's frame
uint16_t width();
uint16_t height();
// the longest exposure time of the sensor's frame rate
uint16_t max_exposure_us();

Picture picture();
// applies at once, stores a few seconds after the last change (a slider)
void set_picture(const Picture &picture);

Stream stream();
// the stream can only be on with a username and a password; stored at once
void set_stream(const Stream &stream);

// The capture runs while at least one user holds it.
void acquire();
void release();

// Calls `use` with the newest frame (RGB565, the CPU's byte order) once it is newer than
// `generation`, waiting up to `timeout_ms` for one; `generation` is set to the frame's. The
// frame stays valid only during the call, which holds up the capture: keep it short. false when
// no newer frame came.
using FrameUse = void (*)(const uint16_t *pixels, uint16_t width, uint16_t height, void *context);
bool use_frame(uint32_t &generation, uint32_t timeout_ms, FrameUse use, void *context);

// Registers /api/camera.jpg and /api/camera.mjpg on the web server (an httpd_handle_t).
void register_routes(void *server);

// The development console's `camreg <reg> [<value>]` (hex): reads or writes a sensor register,
// for finding the sensor's settings. Call once the console runs.
void register_console();

} // namespace hc::camera
