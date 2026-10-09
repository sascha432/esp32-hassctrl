#include "hc/light_sensor/light_sensor.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/light_sensor/ldr.hpp"
#include "hc/platform/settings.hpp"

namespace hc::light_sensor {

namespace {

constexpr const char *kTag = "light";
constexpr uint8_t kAddresses[] = {0x23, 0x5c};
constexpr int kI2cTimeoutMs = 50;
constexpr uint32_t kI2cHz = 400000;

// BH1750 instructions (datasheet, "Instruction Set Architecture")
constexpr uint8_t kPowerOn = 0x01;
constexpr uint8_t kReset = 0x07;
constexpr uint8_t kContinuousHighRes = 0x10;
// counts per lux with the default measurement time
constexpr float kCountsPerLux = 1.2f;

constexpr uint32_t kPeriodMs = 250;
// a sensor that does not answer is tried this often, so the bus stays free for the touch
constexpr uint32_t kRetryMs = 2000;
constexpr int kAverageOf = 4; // one second
// single failed reads happen on the shared bus; only this many in a row mean the sensor is gone
constexpr int kFailuresGone = 3;

constexpr const char *kNamespace = "light";
constexpr const char *kScaleKey = "scale";

i2c_master_dev_handle_t g_device = nullptr;
float g_raw = 0.0f;
bool g_valid = false;
float g_scale = 1.0f;
float g_default_scale = 1.0f;
bool g_calibrated = false;
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t command(uint8_t instruction)
{
    return i2c_master_transmit(g_device, &instruction, 1, kI2cTimeoutMs);
}

esp_err_t set_up()
{
    esp_err_t err = command(kPowerOn);
    if (err == ESP_OK) {
        err = command(kContinuousHighRes);
    }
    return err;
}

void set_valid(bool valid)
{
    taskENTER_CRITICAL(&g_lock);
    g_valid = valid;
    taskEXIT_CRITICAL(&g_lock);
}

// The average of the last kAverageOf measurements, in sensor counts. A rolling average rather
// than a low-pass filter: the result is the light of the last second and nothing older (the
// brightness smooths it on its own, hc::ui power). The sum is kept, in integers so it never drifts.
struct Average {
    uint16_t samples[kAverageOf] = {};
    uint32_t sum = 0;
    int count = 0;
    int next = 0;

    void add(uint16_t counts)
    {
        if (count == kAverageOf) {
            sum -= samples[next];
        } else {
            count++;
        }
        samples[next] = counts;
        sum += counts;
        next = (next + 1) % kAverageOf;
    }

    float lux() const
    {
        return static_cast<float>(sum) / (static_cast<float>(count) * kCountsPerLux);
    }
};

void task(void *)
{
    Average average;
    bool answering = true;
    int failures = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(answering ? kPeriodMs : kRetryMs));
        if (!answering) {
            // one short transfer every 2 s until it answers again, then it measures again
            if (set_up() != ESP_OK) {
                continue;
            }
            ESP_LOGI(kTag, "BH1750 answers again");
            answering = true;
            average = {};
            continue;
        }
        uint8_t data[2];
        const esp_err_t err = i2c_master_receive(g_device, data, sizeof(data), kI2cTimeoutMs);
        if (err != ESP_OK) {
            if (++failures >= kFailuresGone) {
                ESP_LOGW(kTag, "BH1750 does not answer (%s), trying every %lu s", esp_err_to_name(err),
                         static_cast<unsigned long>(kRetryMs / 1000));
                answering = false;
                failures = 0;
                set_valid(false);
            }
            continue;
        }
        failures = 0;
        average.add(static_cast<uint16_t>((data[0] << 8) | data[1]));
        const float raw = average.lux();
        taskENTER_CRITICAL(&g_lock);
        g_raw = raw;
        g_valid = true;
        taskEXIT_CRITICAL(&g_lock);
    }
}

esp_err_t store_scale(float value, bool calibrated)
{
    platform::Settings settings(kNamespace, platform::Settings::Mode::ReadWrite);
    esp_err_t err = calibrated ? settings.set_blob(kScaleKey, &value, sizeof(value)) : settings.erase(kScaleKey);
    if (err == ESP_OK) {
        err = settings.commit();
    }
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&g_lock);
        g_scale = value;
        g_calibrated = calibrated;
        taskEXIT_CRITICAL(&g_lock);
    }
    return err;
}

void load_scale()
{
    float stored = 0.0f;
    size_t size = sizeof(stored);
    platform::Settings settings(kNamespace, platform::Settings::Mode::Read);
    if (settings.ok() && settings.get_blob(kScaleKey, &stored, size) == ESP_OK && size == sizeof(stored) &&
        stored > 0.0f) {
        g_scale = stored;
        g_calibrated = true;
    } else {
        g_scale = g_default_scale;
    }
}

} // namespace

esp_err_t start(i2c_master_bus_handle_t bus, float default_scale)
{
    uint8_t address = 0;
    for (uint8_t candidate : kAddresses) {
        if (i2c_master_probe(bus, candidate, kI2cTimeoutMs) == ESP_OK) {
            address = candidate;
            break;
        }
    }
    if (address == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    g_default_scale = default_scale > 0.0f ? default_scale : 1.0f;
    load_scale();

    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = address;
    device.scl_speed_hz = kI2cHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device, &g_device), kTag, "add device");
    esp_err_t err = command(kPowerOn);
    if (err == ESP_OK) {
        err = command(kReset);
    }
    if (err == ESP_OK) {
        err = command(kContinuousHighRes);
    }
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(g_device);
        g_device = nullptr;
        return err;
    }
    if (xTaskCreate(task, "light", 2560, nullptr, 3, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "BH1750 at 0x%02x, scale %.3f (%s)", address, static_cast<double>(g_scale),
             g_calibrated ? "calibrated" : "board default");
    return ESP_OK;
}

bool lux(float &out)
{
    if (ldr::active()) {
        return ldr::lux(out);
    }
    taskENTER_CRITICAL(&g_lock);
    const bool valid = g_valid;
    out = g_raw * g_scale;
    taskEXIT_CRITICAL(&g_lock);
    return valid;
}

bool raw_lux(float &out)
{
    if (ldr::active()) {
        return ldr::default_lux(out);
    }
    taskENTER_CRITICAL(&g_lock);
    const bool valid = g_valid;
    out = g_raw;
    taskEXIT_CRITICAL(&g_lock);
    return valid;
}

esp_err_t calibrate(float reference_lux)
{
    if (ldr::active()) {
        return ldr::add_point(reference_lux);
    }
    taskENTER_CRITICAL(&g_lock);
    const bool valid = g_valid;
    const float raw = g_raw;
    taskEXIT_CRITICAL(&g_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (raw < 1.0f || reference_lux <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    const float value = reference_lux / raw;
    ESP_LOGI(kTag, "calibrated: %.1f lx measured reads as %.1f lx, scale %.3f", static_cast<double>(raw),
             static_cast<double>(reference_lux), static_cast<double>(value));
    return store_scale(value, true);
}

esp_err_t reset_calibration()
{
    if (ldr::active()) {
        return ldr::clear_points();
    }
    return store_scale(g_default_scale, false);
}

float scale(bool *calibrated)
{
    if (ldr::active()) {
        // a table of points instead of a factor
        ldr::Point point;
        if (calibrated != nullptr) {
            *calibrated = ldr::points(&point, 1) > 0;
        }
        return 1.0f;
    }
    taskENTER_CRITICAL(&g_lock);
    const float value = g_scale;
    if (calibrated != nullptr) {
        *calibrated = g_calibrated;
    }
    taskEXIT_CRITICAL(&g_lock);
    return value;
}

} // namespace hc::light_sensor
