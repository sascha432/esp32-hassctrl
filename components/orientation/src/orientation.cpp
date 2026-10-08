#include "hc/orientation/orientation.hpp"

#include <atomic>
#include <cmath>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace hc::orientation {

namespace {

constexpr const char *kTag = "orientation";

// MPU6050 registers
constexpr uint8_t kRegSampleRateDivider = 0x19;
constexpr uint8_t kRegConfig = 0x1a;
constexpr uint8_t kRegGyroConfig = 0x1b;
constexpr uint8_t kRegAccelConfig = 0x1c;
constexpr uint8_t kRegAccelX = 0x3b; // 14 bytes: accel x/y/z, temperature, gyro x/y/z
constexpr uint8_t kRegPowerManagement = 0x6b;
constexpr uint8_t kRegWhoAmI = 0x75;
constexpr uint8_t kWhoAmI = 0x68;

constexpr uint32_t kI2cHz = 400000;
constexpr int kI2cTimeoutMs = 50;
// A module that does not answer (no supply, loose wire) is tried this often, so the shared bus
// stays free for the touch controller. After it answers again it is set up again: an MPU6050
// starts asleep after a power loss and reads zero.
constexpr uint32_t kRetryMs = 2000;
// single failed reads happen on the shared bus (seen every few seconds on the WT32-SC01); only
// this many in a row mean the module is gone
constexpr int kFailuresGone = 5;
// a read that timed out is repeated this often before it counts as failed
constexpr int kRetries = 2;

// +-2 g, the power-on range
constexpr float kAccelLsbPerG = 16384.0f;

constexpr uint32_t kSamplePeriodMs = 100;
// Above LVGL (4), hass (4) and network (5): at priority 2 the task was starved for up to a second
// while the screen redrew, so samples went stale and `orient capture` found no fresh reading. It
// only runs for a few milliseconds every 100 ms.
constexpr UBaseType_t kTaskPriority = 6;
constexpr uint32_t kHoldMs = 500;
// The device counts as at rest while the reading changes less than this between two samples (the
// noise is a few mg; 0.05 g per 100 ms is a turn of about 30 degrees per second).
constexpr float kRestDeltaG = 0.05f;
// shorter gravity vectors are not a valid sample (the first one after the reset reads zero)
constexpr float kMinGravity = 0.1f;

// The calibration from the board info (measured by the developer, console `orient capture`):
// two orthonormal axes in the module frame, e0 = what the accelerometer reads with the panel upright
// in the base orientation, e1 = with the panel turned 90 degrees clockwise, and the offset of the
// module (a module can read several tenths of a g too much in every pose).
// the window of samples a calibration step averages: one second at 10 samples per second
constexpr int kWindow = 10;
constexpr int kMinSamples = 8;
constexpr TickType_t kFreshTicks = pdMS_TO_TICKS(500);
// a measurement reading should look like 1 g (only a warning), and the two captures have to be a
// quarter turn apart, which they are only when the panel stood exactly upright both times
constexpr float kGravityTolerance = 0.15f;
// samples of one capture window that differ more than this in length are not a resting module
constexpr float kMaxLengthSpread = 0.10f;
constexpr float kMinAngleDeg = 85.0f;
constexpr float kMaxAngleDeg = 95.0f;

struct Vec3 {
    float x, y, z;
};

Vec3 sub(const Vec3 &a, const Vec3 &b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 add(const Vec3 &a, const Vec3 &b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 scale(const Vec3 &a, float f)
{
    return {a.x * f, a.y * f, a.z * f};
}

Vec3 cross(const Vec3 &a, const Vec3 &b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(const Vec3 &a, const Vec3 &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float norm(const Vec3 &a)
{
    return std::sqrt(dot(a, a));
}

struct Window {
    Vec3 samples[kWindow] = {};
    bool rest[kWindow] = {};
    float motion[kWindow] = {}; // g the reading changed since the sample before
    int count = 0;
    int next = 0;
    TickType_t last = 0;
};

struct Calibration {
    bool valid = false;
    Vec3 e0 = {};
    Vec3 e1 = {};
    Vec3 offset = {}; // subtracted from every reading, also with the fixed mapping
};

portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
Window g_window;
Calibration g_calibration;
std::atomic<uint32_t> g_failed_reads{0};
std::atomic<uint32_t> g_retried_reads{0};
// the five poses of a measurement, by Pose; the mask tells which are captured
Vec3 g_poses[5] = {};
uint8_t g_pose_mask = 0;

struct Context {
    i2c_master_dev_handle_t device = nullptr;
    Callback callback = nullptr;
    void *user = nullptr;
};

Context g;

esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(g.device, data, sizeof(data), kI2cTimeoutMs);
}

esp_err_t read_registers(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(g.device, &reg, 1, data, length, kI2cTimeoutMs);
}

// the register setup of the old firmware
esp_err_t set_up()
{
    esp_err_t err = write_register(kRegPowerManagement, 0x80); // reset
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    const uint8_t setup[][2] = {
        {kRegPowerManagement, 0x01},   // wake up, PLL with gyro X
        {kRegSampleRateDivider, 0x04}, // 200 Hz
        {kRegConfig, 0x03},            // ~44 Hz low pass
        {kRegAccelConfig, 0x00},       // +-2 g
        {kRegGyroConfig, 0x00},        // +-250 deg/s
    };
    for (const auto &step : setup) {
        err = write_register(step[0], step[1]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

int16_t be16(const uint8_t *p)
{
    return static_cast<int16_t>((p[0] << 8) | p[1]);
}

// Orientation of the module from the gravity direction, false while it is undecided. The steps
// count clockwise from the module's -X axis pointing down: +Y up is 90, +X up is 180. The module
// sits mirrored on the back of the panel, so the display turns the other way round (as in the old
// firmware: display step = (360 - module step) / 90).
//
// Only the part of gravity in the panel plane (X, Y) decides. The old firmware called the device
// flat as soon as Z was the largest part, so a panel leaning back on a desk (more than 45 degrees
// from upright) got no orientation at all and started in the base orientation, upside down if it
// stood the other way round. Now it is undecided only when it really lies flat or the plane part
// points close to a diagonal.
bool rotation_from_gravity(float ux, float uy, float /* uz */, Rotation &out)
{
    // about 15 degrees away from lying flat
    constexpr float kMinPlane = 0.26f;
    // the larger axis has to win clearly, so a panel standing near 45 degrees does not flip
    constexpr float kAxisMargin = 1.3f;

    const float ax = std::fabs(ux);
    const float ay = std::fabs(uy);
    if (std::sqrt(ux * ux + uy * uy) < kMinPlane) {
        return false;
    }
    if (std::fmax(ax, ay) < kAxisMargin * std::fmin(ax, ay)) {
        return false;
    }
    int module_deg;
    if (ax >= ay) {
        module_deg = (ux >= 0) ? 180 : 0;
    } else {
        module_deg = (uy >= 0) ? 90 : 270;
    }
    out = static_cast<Rotation>(((360 - module_deg) % 360) / 90);
    return true;
}

// The same decision in the calibrated axes: a = gravity along e0 (base orientation), b = along e1.
bool rotation_from_calibration(const Calibration &cal, const Vec3 &u, Rotation &out)
{
    constexpr float kMinPlane = 0.26f;
    constexpr float kAxisMargin = 1.3f;

    const float a = dot(u, cal.e0);
    const float b = dot(u, cal.e1);
    const float ax = std::fabs(a);
    const float ay = std::fabs(b);
    if (std::sqrt(a * a + b * b) < kMinPlane) {
        return false;
    }
    if (std::fmax(ax, ay) < kAxisMargin * std::fmin(ax, ay)) {
        return false;
    }
    // Reading along e1 (the panel turned a quarter clockwise from the base orientation) needs rotation
    // 90 on the WT32-SC01: checked on the device 2026-10-07, where the opposite assignment showed
    // portrait upside down while landscape was right.
    if (ax >= ay) {
        out = a >= 0 ? Rotation::R0 : Rotation::R180;
    } else {
        out = b >= 0 ? Rotation::R90 : Rotation::R270;
    }
    return true;
}

void add_sample(const Vec3 &g, float motion, bool at_rest)
{
    taskENTER_CRITICAL(&g_lock);
    g_window.samples[g_window.next] = g;
    g_window.motion[g_window.next] = motion;
    g_window.rest[g_window.next] = at_rest;
    g_window.next = (g_window.next + 1) % kWindow;
    g_window.count = g_window.count < kWindow ? g_window.count + 1 : kWindow;
    g_window.last = xTaskGetTickCount();
    taskEXIT_CRITICAL(&g_lock);
}

// the mean of the window; false when it is stale or too short
bool window_mean(Vec3 &mean, bool &at_rest, float *motion = nullptr)
{
    Window w;
    taskENTER_CRITICAL(&g_lock);
    w = g_window;
    taskEXIT_CRITICAL(&g_lock);
    if (w.count < kMinSamples || xTaskGetTickCount() - w.last > kFreshTicks) {
        return false;
    }
    mean = {};
    float largest = 0.0f;
    at_rest = true;
    for (int i = 0; i < w.count; i++) {
        largest = std::fmax(largest, w.motion[i]);
        mean.x += w.samples[i].x;
        mean.y += w.samples[i].y;
        mean.z += w.samples[i].z;
        at_rest = at_rest && w.rest[i];
    }
    mean.x /= static_cast<float>(w.count);
    mean.y /= static_cast<float>(w.count);
    mean.z /= static_cast<float>(w.count);
    if (motion != nullptr) {
        *motion = largest;
    }
    return true;
}

// how much the length of the single samples in the window differs; a module at rest reads the same
// length every time
float window_length_spread()
{
    Window w;
    taskENTER_CRITICAL(&g_lock);
    w = g_window;
    taskEXIT_CRITICAL(&g_lock);
    float lowest = 1e9f;
    float highest = 0.0f;
    for (int i = 0; i < w.count; i++) {
        const float length = norm(w.samples[i]);
        lowest = std::fmin(lowest, length);
        highest = std::fmax(highest, length);
    }
    return w.count == 0 ? 0.0f : highest - lowest;
}

// The board's axes (six floats) and offset (three); the axes are ignored when all zero or no two
// orthonormal axes, then the fixed mapping applies, still with the offset.
void set_calibration(const float *axes, const float *offset)
{
    Calibration cal;
    if (offset != nullptr) {
        cal.offset = {offset[0], offset[1], offset[2]};
    }
    if (axes != nullptr) {
        const Vec3 e0 = {axes[0], axes[1], axes[2]};
        const Vec3 e1 = {axes[3], axes[4], axes[5]};
        if (norm(e0) != 0.0f || norm(e1) != 0.0f) {
            if (std::fabs(norm(e0) - 1.0f) > 0.01f || std::fabs(norm(e1) - 1.0f) > 0.01f ||
                std::fabs(dot(e0, e1)) > 0.01f) {
                ESP_LOGW(kTag, "imu_axes of the board are not two orthonormal axes, using the fixed mapping");
            } else {
                cal.e0 = e0;
                cal.e1 = e1;
                cal.valid = true;
            }
        }
    }
    g_calibration = cal;
}

void task(void *)
{
    bool have_rotation = false;
    Rotation current = Rotation::R0;
    Rotation pending = Rotation::R0;
    TickType_t pending_since = 0;

    TickType_t wake = xTaskGetTickCount();
    bool answering = true;
    int zero_samples = 0;
    Vec3 previous = {};
    bool have_previous = false;
    int failures = 0;
    TickType_t last_ok = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(answering ? kSamplePeriodMs : kRetryMs));

        if (!answering) {
            if (set_up() != ESP_OK) {
                continue;
            }
            ESP_LOGI(kTag, "MPU6050 answers again");
            answering = true;
            wake = xTaskGetTickCount();
            continue;
        }
        uint8_t raw[14];
        const TickType_t before = xTaskGetTickCount();
        esp_err_t err = read_registers(kRegAccelX, raw, sizeof(raw));
        // Single timeouts come at random on the shared bus of the WT32-SC01 (about one in four
        // seconds, also untouched; the next read works), so a read is tried again at once. Only a
        // read that fails every time counts as a failure.
        for (int retry = 0; err != ESP_OK && retry < kRetries; retry++) {
            g_retried_reads++;
            err = read_registers(kRegAccelX, raw, sizeof(raw));
        }
        const TickType_t took = xTaskGetTickCount() - before;
        if (took * portTICK_PERIOD_MS > 150 || before - last_ok > pdMS_TO_TICKS(400)) {
            ESP_LOGW(kTag, "slow: the read took %u ms, %u ms since the last sample (err %s)",
                     static_cast<unsigned>(took * portTICK_PERIOD_MS),
                     static_cast<unsigned>((before - last_ok) * portTICK_PERIOD_MS), esp_err_to_name(err));
        }
        if (err != ESP_OK) {
            g_failed_reads++;
            ESP_LOGW(kTag, "read failed after %d tries: %s (%lu since boot)", kRetries + 1, esp_err_to_name(err),
                     static_cast<unsigned long>(g_failed_reads.load()));
            if (++failures >= kFailuresGone) {
                ESP_LOGW(kTag, "MPU6050 does not answer (%s), trying every %lu s", esp_err_to_name(err),
                         static_cast<unsigned long>(kRetryMs / 1000));
                answering = false;
                failures = 0;
            }
            continue;
        }
        failures = 0;
        last_ok = xTaskGetTickCount();
        const float ax = be16(raw + 0) / kAccelLsbPerG;
        const float ay = be16(raw + 2) / kAccelLsbPerG;
        const float az = be16(raw + 4) / kAccelLsbPerG;

        const float length = std::sqrt(ax * ax + ay * ay + az * az);
        if (length < kMinGravity) {
            // a second of zeros: the module lost its supply in between and woke up asleep
            if (++zero_samples >= 10) {
                zero_samples = 0;
                ESP_LOGW(kTag, "MPU6050 reads zero, setting it up again");
                set_up();
            }
            continue;
        }
        zero_samples = 0;
        Calibration cal;
        taskENTER_CRITICAL(&g_lock);
        cal = g_calibration;
        taskEXIT_CRITICAL(&g_lock);
        // the direction of gravity without the offset of the module
        const Vec3 corrected = sub({ax, ay, az}, cal.offset);
        const float corrected_length = norm(corrected);
        const TickType_t now = xTaskGetTickCount();
        // At rest = the accelerometer reading hardly changed since the sample before. The gyroscope
        // is not used: this module reads 12 deg/s at rest, close to the limit, and noise on it kept
        // resetting the hold time, so a new orientation came late and at random.
        const Vec3 now_reading = {ax, ay, az};
        const float motion = have_previous ? norm(sub(now_reading, previous)) : 1.0f;
        previous = now_reading;
        have_previous = true;
        const bool at_rest = motion <= kRestDeltaG;
        add_sample(now_reading, motion, at_rest);
        if (corrected_length < kMinGravity) {
            pending = current;
            pending_since = now;
            continue;
        }
        const Vec3 unit = scale(corrected, 1.0f / corrected_length);
        if (!have_rotation && pending_since == 0) {
            // how the device stands at boot, for checking the orientation logic
            ESP_LOGI(kTag, "gravity at start: x %.2f, y %.2f, z %.2f", static_cast<double>(unit.x),
                     static_cast<double>(unit.y), static_cast<double>(unit.z));
        }
        Rotation candidate;
        const bool decided = cal.valid ? rotation_from_calibration(cal, unit, candidate)
                                       : rotation_from_gravity(unit.x, unit.y, unit.z, candidate);
        if (!at_rest || !decided) {
            pending = current;
            pending_since = now;
            continue;
        }

        if (have_rotation && candidate == current) {
            pending = current;
            continue;
        }
        if (candidate != pending) {
            pending = candidate;
            pending_since = now;
            continue;
        }
        if ((now - pending_since) * portTICK_PERIOD_MS >= kHoldMs) {
            current = candidate;
            have_rotation = true;
            ESP_LOGI(kTag, "rotation %d deg", static_cast<int>(current) * 90);
            g.callback(current, g.user);
        }
    }
}

} // namespace

esp_err_t start(i2c_master_bus_handle_t bus, uint8_t address, const float *axes, const float *offset,
                Callback callback, void *context)
{
    ESP_RETURN_ON_FALSE(bus != nullptr && callback != nullptr, ESP_ERR_INVALID_ARG, kTag, "no bus or callback");

    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = address;
    device.scl_speed_hz = kI2cHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device, &g.device), kTag, "add device");

    uint8_t who_am_i = 0;
    if (read_registers(kRegWhoAmI, &who_am_i, 1) != ESP_OK || who_am_i != kWhoAmI) {
        ESP_LOGW(kTag, "no MPU6050 at 0x%02x (WHO_AM_I 0x%02x)", address, who_am_i);
        i2c_master_bus_rm_device(g.device);
        g.device = nullptr;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(set_up(), kTag, "setup");
    set_calibration(axes, offset);

    g.callback = callback;
    g.user = context;
    ESP_RETURN_ON_FALSE(xTaskCreate(task, "orientation", 3072, nullptr, kTaskPriority, nullptr) == pdPASS, ESP_ERR_NO_MEM, kTag,
                        "task");
    ESP_LOGI(kTag, "MPU6050 at 0x%02x, %s", address, g_calibration.valid ? "axes from the board" : "fixed mapping");
    if (norm(g_calibration.offset) != 0.0f) {
        ESP_LOGI(kTag, "offset of the module (%.2f, %.2f, %.2f) g taken off every reading",
                 static_cast<double>(g_calibration.offset.x), static_cast<double>(g_calibration.offset.y),
                 static_cast<double>(g_calibration.offset.z));
    }
    return ESP_OK;
}

uint32_t failed_reads()
{
    return g_failed_reads.load();
}

uint32_t retried_reads()
{
    return g_retried_reads.load();
}

bool gravity(Gravity &out)
{
    Vec3 mean;
    float motion = 0.0f;
    bool at_rest = false;
    if (!window_mean(mean, at_rest, &motion)) {
        return false;
    }
    Calibration cal;
    taskENTER_CRITICAL(&g_lock);
    cal = g_calibration;
    taskEXIT_CRITICAL(&g_lock);
    const Vec3 c = sub(mean, cal.offset);
    out = {mean.x, mean.y, mean.z, at_rest, c.x, c.y, c.z, motion};
    return true;
}

esp_err_t read_raw(Raw &out)
{
    ESP_RETURN_ON_FALSE(g.device != nullptr, ESP_ERR_INVALID_STATE, kTag, "no module");
    ESP_RETURN_ON_ERROR(read_registers(kRegWhoAmI, &out.who_am_i, 1), kTag, "who am i");
    ESP_RETURN_ON_ERROR(read_registers(kRegPowerManagement, &out.power_management, 1), kTag, "power");
    ESP_RETURN_ON_ERROR(read_registers(kRegConfig, &out.config, 1), kTag, "config");
    ESP_RETURN_ON_ERROR(read_registers(kRegAccelConfig, &out.accel_config, 1), kTag, "accel config");
    uint8_t raw[14];
    ESP_RETURN_ON_ERROR(read_registers(kRegAccelX, raw, sizeof(raw)), kTag, "sample");
    for (int i = 0; i < 3; i++) {
        out.accel[i] = be16(raw + 2 * i);
        out.gyro[i] = be16(raw + 8 + 2 * i);
    }
    return ESP_OK;
}

esp_err_t capture(Pose pose, Measurement &out)
{
    ESP_RETURN_ON_FALSE(pose <= Pose::Flat, ESP_ERR_INVALID_ARG, kTag, "pose");
    Vec3 mean;
    bool at_rest = false;
    if (!window_mean(mean, at_rest)) {
        ESP_LOGW(kTag, "no fresh reading");
        return ESP_ERR_INVALID_STATE;
    }
    if (!at_rest) {
        ESP_LOGW(kTag, "the device moved during the last second (the reading changed by more than %.2f g between "
                       "two samples), hold it still",
                 static_cast<double>(kRestDeltaG));
        return ESP_ERR_INVALID_STATE;
    }
    const float spread = window_length_spread();
    if (spread > kMaxLengthSpread) {
        ESP_LOGW(kTag, "the readings of the last second vary by %.2f g: the device was moved or pushed (let go of it "
                       "and wait a second), or the bus returns wrong data (check the wiring of the module)",
                 static_cast<double>(spread));
        return ESP_ERR_INVALID_STATE;
    }
    if (norm(mean) < kMinGravity) {
        ESP_LOGW(kTag, "no gravity in the reading");
        return ESP_ERR_INVALID_STATE;
    }
    out = {};
    out.length = norm(mean);
    taskENTER_CRITICAL(&g_lock);
    g_poses[static_cast<int>(pose)] = mean;
    g_pose_mask |= 1u << static_cast<int>(pose);
    const uint8_t mask = g_pose_mask;
    Vec3 poses[5];
    for (int i = 0; i < 5; i++) {
        poses[i] = g_poses[i];
    }
    taskEXIT_CRITICAL(&g_lock);
    out.captured = mask;
    if (mask != 0x1f) {
        return ESP_OK;
    }

    // Opposite poses cancel the offset of the module: (p0 - p180) / 2 is the reading of exactly 1 g
    // along the panel's up direction, (p90 - p270) / 2 the same turned a quarter.
    const Vec3 d0 = scale(sub(poses[0], poses[2]), 0.5f);
    const Vec3 d90 = scale(sub(poses[1], poses[3]), 0.5f);
    const float gain0 = norm(d0);
    const float gain90 = norm(d90);
    if (gain0 < kMinGravity || gain90 < kMinGravity) {
        ESP_LOGW(kTag, "opposite poses read the same: the panel was not turned between the captures");
        return ESP_ERR_INVALID_RESPONSE;
    }
    out.gain = 0.5f * (gain0 + gain90);
    if (std::fabs(gain0 - 1.0f) > kGravityTolerance || std::fabs(gain90 - 1.0f) > kGravityTolerance) {
        ESP_LOGW(kTag, "the module reads %.2f g and %.2f g where it should read 1 g: its scale is off, the "
                       "directions are used anyway",
                 static_cast<double>(gain0), static_cast<double>(gain90));
    }
    const Vec3 e0 = scale(d0, 1.0f / gain0);
    const Vec3 f1 = scale(d90, 1.0f / gain90);
    const float cosine = std::fmax(-1.0f, std::fmin(1.0f, dot(e0, f1)));
    out.angle_deg = std::acos(cosine) * 57.29578f;
    if (out.angle_deg < kMinAngleDeg || out.angle_deg > kMaxAngleDeg) {
        ESP_LOGW(kTag, "0 and 90 are %.1f degrees apart, 90 (%.0f to %.0f accepted): the panel has to stand exactly "
                       "upright in every pose and be turned a quarter each time",
                 static_cast<double>(out.angle_deg), static_cast<double>(kMinAngleDeg),
                 static_cast<double>(kMaxAngleDeg));
        return ESP_ERR_INVALID_RESPONSE;
    }
    // the second axis made exactly perpendicular to the first
    const Vec3 rest = sub(f1, scale(e0, cosine));
    const Vec3 e1 = scale(rest, 1.0f / norm(rest));
    // the panel normal (out of the screen); the module lies flat on the panel when it is its Z axis
    const Vec3 normal = cross(e0, e1);
    out.tilt_deg = std::acos(std::fmin(1.0f, std::fabs(normal.z))) * 57.29578f;

    // offset in the panel plane from the two pairs, which have to agree
    const Vec3 pair_a = scale(add(poses[0], poses[2]), 0.5f);
    const Vec3 pair_b = scale(add(poses[1], poses[3]), 0.5f);
    const Vec3 pairs = scale(add(pair_a, pair_b), 0.5f);
    out.offset_mismatch = std::fmax(std::fabs(pair_a.x - pair_b.x), std::fabs(pair_a.y - pair_b.y));
    if (out.offset_mismatch > 0.15f) {
        ESP_LOGW(kTag, "the two pairs give offsets %.2f g apart: a pose was not upright or moved",
                 static_cast<double>(out.offset_mismatch));
    }
    // Flat on the table, screen up, the module reads its offset plus 1 g along the normal. That is
    // the only pose that gives the Z offset (the upright poses carry the lean of the panel).
    const Vec3 offset = sub(poses[4], scale(normal, out.gain));
    if (std::fabs(offset.z - pairs.z) > 0.6f) {
        ESP_LOGW(kTag, "the flat pose does not fit the upright ones (Z offset %.2f against %.2f): it has to lie "
                       "flat with the screen up",
                 static_cast<double>(offset.z), static_cast<double>(pairs.z));
        return ESP_ERR_INVALID_RESPONSE;
    }
    out.axes[0] = e0.x;
    out.axes[1] = e0.y;
    out.axes[2] = e0.z;
    out.axes[3] = e1.x;
    out.axes[4] = e1.y;
    out.axes[5] = e1.z;
    // X and Y from the pairs (they do not depend on the lean), Z from the flat pose
    out.offset[0] = pairs.x;
    out.offset[1] = pairs.y;
    out.offset[2] = offset.z;
    out.complete = true;
    return ESP_OK;
}

esp_err_t clear_captures()
{
    taskENTER_CRITICAL(&g_lock);
    g_pose_mask = 0;
    taskEXIT_CRITICAL(&g_lock);
    return ESP_OK;
}

} // namespace hc::orientation
