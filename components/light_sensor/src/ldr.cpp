#include "hc/light_sensor/ldr.hpp"

#include <algorithm>
#include <cmath>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/platform/settings.hpp"

namespace hc::light_sensor::ldr {

namespace {

constexpr const char *kTag = "ldr";

// the divider: LDR to 3V3, this resistor to GND
constexpr float kSupplyMv = 3300.0f;
constexpr float kSeriesOhms = 10000.0f;

constexpr adc_atten_t kAtten = ADC_ATTEN_DB_12;
constexpr int kOversample = 16;
constexpr uint32_t kPeriodMs = 100;
constexpr int kAverageOf = 10; // one second
// the ends of the ADC range are not usable: below this the resistance is only a guess
constexpr float kMinMv = 20.0f;
constexpr float kMaxMv = kSupplyMv - 20.0f;

// default curve: GL5528 class LDR, R = kOhmsAt10Lux * (10 lx / lux) ^ kGamma
constexpr float kOhmsAt10Lux = 15000.0f;
constexpr float kGamma = 0.7f;

// a new point this close (in resistance) to an old one replaces it
constexpr float kSameRatio = 0.02f;

constexpr const char *kNamespace = "light";
constexpr const char *kPointsKey = "ldr_points";

adc_oneshot_unit_handle_t g_adc = nullptr;
adc_cali_handle_t g_cali = nullptr;
adc_channel_t g_channel{};
bool g_active = false;
bool g_valid = false;
float g_mv = 0.0f;
Point g_points[kMaxPoints] = {};
size_t g_count = 0;
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

float ohms_of(float millivolts)
{
    const float mv = std::clamp(millivolts, kMinMv, kMaxMv);
    return kSeriesOhms * (kSupplyMv - mv) / mv;
}

float default_lux_of(float ohms)
{
    return 10.0f * std::pow(kOhmsAt10Lux / ohms, 1.0f / kGamma);
}

// ohms -> lux through sorted points
float lux_of(float ohms, const Point *points, size_t count)
{
    if (count == 0) {
        return default_lux_of(ohms);
    }
    if (count == 1) {
        // one point: the default exponent through it
        return points[0].lux * std::pow(points[0].ohms / ohms, 1.0f / kGamma);
    }
    size_t i = 0;
    while (i + 2 < count && ohms > points[i + 1].ohms) {
        i++;
    }
    const float x0 = std::log(points[i].ohms);
    const float x1 = std::log(points[i + 1].ohms);
    const float y0 = std::log(points[i].lux);
    const float y1 = std::log(points[i + 1].lux);
    return std::exp(y0 + (y1 - y0) / (x1 - x0) * (std::log(ohms) - x0));
}

void snapshot(float &millivolts, Point *points, size_t &count, bool &valid)
{
    taskENTER_CRITICAL(&g_lock);
    millivolts = g_mv;
    valid = g_valid;
    count = std::min(g_count, kMaxPoints);
    std::copy(g_points, g_points + g_count, points);
    taskEXIT_CRITICAL(&g_lock);
}

esp_err_t store_points(const Point *points, size_t count)
{
    platform::Settings settings(kNamespace, platform::Settings::Mode::ReadWrite);
    esp_err_t err = count > 0 ? settings.set_blob(kPointsKey, points, count * sizeof(Point)) : settings.erase(kPointsKey);
    if (err == ESP_OK) {
        err = settings.commit();
    }
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&g_lock);
        std::copy(points, points + count, g_points);
        g_count = count;
        taskEXIT_CRITICAL(&g_lock);
    }
    return err;
}

void load_points()
{
    Point stored[kMaxPoints] = {};
    size_t size = sizeof(stored);
    platform::Settings settings(kNamespace, platform::Settings::Mode::Read);
    if (!settings.ok() || settings.get_blob(kPointsKey, stored, size) != ESP_OK || size % sizeof(Point) != 0) {
        return;
    }
    const size_t count = std::min(size / sizeof(Point), kMaxPoints);
    // ignore a table that is not usable rather than compute with it
    for (size_t i = 0; i < count; i++) {
        if (!(stored[i].ohms > 0.0f) || !(stored[i].lux > 0.0f)) {
            return;
        }
    }
    std::sort(stored, stored + count, [](const Point &a, const Point &b) { return a.ohms < b.ohms; });
    g_count = count;
    std::copy(stored, stored + count, g_points);
}

float read_mv()
{
    int sum = 0;
    int taken = 0;
    for (int i = 0; i < kOversample; i++) {
        int raw = 0;
        if (adc_oneshot_read(g_adc, g_channel, &raw) == ESP_OK) {
            sum += raw;
            taken++;
        }
    }
    if (taken == 0) {
        return -1.0f;
    }
    const int mean = sum / taken;
    int mv = 0;
    if (g_cali != nullptr && adc_cali_raw_to_voltage(g_cali, mean, &mv) == ESP_OK) {
        return static_cast<float>(mv);
    }
    // without the chip's calibration data: the nominal full scale of 12 dB attenuation
    return static_cast<float>(mean) * 3100.0f / 4095.0f;
}

void task(void *)
{
    float samples[kAverageOf] = {};
    int count = 0;
    int next = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
        const float mv = read_mv();
        if (mv < 0.0f) {
            continue;
        }
        samples[next] = mv;
        next = (next + 1) % kAverageOf;
        count = std::min(count + 1, kAverageOf);
        float sum = 0.0f;
        for (int i = 0; i < count; i++) {
            sum += samples[i];
        }
        taskENTER_CRITICAL(&g_lock);
        g_mv = sum / static_cast<float>(count);
        g_valid = true;
        taskEXIT_CRITICAL(&g_lock);
    }
}

} // namespace

esp_err_t start(int gpio)
{
    adc_unit_t unit{};
    if (adc_oneshot_io_to_channel(gpio, &unit, &g_channel) != ESP_OK || unit != ADC_UNIT_1) {
        return ESP_ERR_INVALID_ARG;
    }
    adc_oneshot_unit_init_cfg_t unit_config{};
    unit_config.unit_id = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_new_unit(&unit_config, &g_adc);
    if (err != ESP_OK) {
        return err;
    }
    adc_oneshot_chan_cfg_t channel_config{};
    channel_config.atten = kAtten;
    channel_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    err = adc_oneshot_config_channel(g_adc, g_channel, &channel_config);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(g_adc);
        g_adc = nullptr;
        return err;
    }
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config{};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.atten = kAtten;
    cali_config.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_line_fitting(&cali_config, &g_cali) != ESP_OK) {
        g_cali = nullptr;
        ESP_LOGW(kTag, "no ADC calibration data in this chip, using the nominal scale");
    }
#endif
    load_points();
    if (xTaskCreate(task, "ldr", 3072, nullptr, 3, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    g_active = true;
    ESP_LOGI(kTag, "LDR on GPIO %d (ADC1 channel %d), %u calibration points", gpio, static_cast<int>(g_channel),
             static_cast<unsigned>(g_count));
    return ESP_OK;
}

bool active()
{
    return g_active;
}

bool reading(float &millivolts, float &ohms)
{
    Point points[kMaxPoints];
    size_t count = 0;
    bool valid = false;
    snapshot(millivolts, points, count, valid);
    ohms = ohms_of(millivolts);
    return valid;
}

bool lux(float &out)
{
    Point points[kMaxPoints];
    size_t count = 0;
    bool valid = false;
    float mv = 0.0f;
    snapshot(mv, points, count, valid);
    out = lux_of(ohms_of(mv), points, count);
    return valid;
}

bool default_lux(float &out)
{
    Point points[kMaxPoints];
    size_t count = 0;
    bool valid = false;
    float mv = 0.0f;
    snapshot(mv, points, count, valid);
    out = default_lux_of(ohms_of(mv));
    return valid;
}

esp_err_t add_point(float reference_lux)
{
    float mv = 0.0f;
    float ohms = 0.0f;
    if (!reading(mv, ohms)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mv < kMinMv || mv > kMaxMv) {
        return ESP_ERR_INVALID_ARG;
    }
    return add_point(ohms, reference_lux);
}

esp_err_t add_point(float ohms, float reference_lux)
{
    Point points[kMaxPoints];
    size_t count = 0;
    bool valid = false;
    float mv = 0.0f;
    snapshot(mv, points, count, valid);
    if (!(ohms > 0.0f) || !(reference_lux > 0.0f)) {
        return ESP_ERR_INVALID_ARG;
    }
    bool replaced = false;
    for (size_t i = 0; i < count; i++) {
        if (std::fabs(points[i].ohms - ohms) < kSameRatio * ohms) {
            points[i] = {ohms, reference_lux};
            replaced = true;
        }
    }
    if (!replaced) {
        if (count >= kMaxPoints) {
            return ESP_ERR_NO_MEM;
        }
        points[count++] = {ohms, reference_lux};
    }
    count = std::min(count, kMaxPoints); // GCC cannot see that the table never grows past it
    std::sort(points, points + count, [](const Point &a, const Point &b) { return a.ohms < b.ohms; });
    ESP_LOGI(kTag, "point: %.0f Ohm = %.1f lx (%u points)", static_cast<double>(ohms),
             static_cast<double>(reference_lux), static_cast<unsigned>(count));
    return store_points(points, count);
}

esp_err_t clear_points()
{
    return store_points(nullptr, 0);
}

size_t points(Point *out, size_t max)
{
    Point copy[kMaxPoints];
    float mv = 0.0f;
    size_t count = 0;
    bool valid = false;
    snapshot(mv, copy, count, valid);
    count = std::min(count, max);
    std::copy(copy, copy + count, out);
    return count;
}

} // namespace hc::light_sensor::ldr
