// The ES8311 of the JC4880P443C with esp_codec_dev's driver, control only: the codec is an I2S
// slave with the master clock of the ESP32-P4 (256 x the sample rate), and the audio component
// writes the samples to I2S itself. Its DAC is at kDacDb (start() sets it each time); the volume of
// the firmware is in software (hc::audio), as on the boards without a codec. The driver switches
// the amplifier (GPIO 11) with the codec's output. With CONFIG_HC_MIC the ADC runs too: the
// microphone on MIC1P/N, its samples on I2S DIN (the left slot; the right one is the DAC's
// signal, REG44), its gain or the codec's ALC set by set_mic().
// Pins: docs/devices/JC4880P443C/JC4880P443C.md, Audio.

#include "es8311.hpp"

#include <cstdio>

#include "driver/gpio.h"
#include "es8311_codec.h"
#include "es8311_gain.hpp"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace hc::board::detail {

namespace {

constexpr const char *kTag = "board";
// 7 bit address 0x18; esp_codec_dev takes the 8 bit form
constexpr uint8_t kAddress = 0x18;
constexpr gpio_num_t kPinAmplifier = GPIO_NUM_11;
constexpr uint32_t kProbeTimeoutMs = 50;
// The DAC's level, the loudest the firmware plays (the alarm, 100 %)
constexpr float kDacDb = 0.0f;

const audio_codec_if_t *g_codec = nullptr;
// start() and stop() on the audio task, set_mic() from the UI's: one at a time on the codec
SemaphoreHandle_t g_lock = nullptr;

class Lock {
public:
    Lock()
    {
        xSemaphoreTake(g_lock, portMAX_DELAY);
    }
    ~Lock()
    {
        xSemaphoreGive(g_lock);
    }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};

#if CONFIG_HC_MIC
constexpr int kRegPga = 0x14;    // [3:0] PGA gain, [6] digital mic, [5:4] input select
constexpr int kRegScale = 0x16;  // [2:0] ADC scale
constexpr int kRegVolume = 0x17; // ADC volume
constexpr int kRegAlc = 0x18;    // [7] ALC on, [6] automute on, [3:0] ALC window
constexpr int kRegLevels = 0x19; // [7:4] ALC max level, [3:0] ALC min level
// The ALC holds the ADC's output between these levels (REG19, the steps of Linux's
// es8311_level_tlv): 3 = -18.1 dBFS, 5 = -14.5 dBFS, speech for a voice assistant or an intercom.
constexpr uint8_t kAlcMinLevel = 3;
constexpr uint8_t kAlcMaxLevel = 5;
// REG18[3:0]: 5 = 0.25 dB per 64 LRCK, about 60 dB/s at 16 kHz: 7 (15 dB/s) let the first second
// of a sound clip at 0 dBFS while it came down from kAlcGainDb (2026-10-22)
constexpr uint8_t kAlcWindow = 5;
// The gain the ALC starts from: it only turns down from there (measured 2026-10-22 with the
// speaker's tone: over 30 dB, -26 dBFS stayed -26; over 54 dB, -2 dBFS came down to -15). 54 dB
// lifts a quiet room to about -46 dBFS.
constexpr float kAlcGainDb = 54.0f;

float g_mic_gain_db = 0.0f;
bool g_mic_auto = false;
// set_mic() was called: start() writes the microphone's registers again after the driver's
bool g_mic_set = false;

bool update_bits(int reg, int mask, int value)
{
    int current = 0;
    if (g_codec->get_reg(g_codec, reg, &current) != ESP_CODEC_DEV_OK) {
        return false;
    }
    return g_codec->set_reg(g_codec, reg, (current & ~mask) | (value & mask)) == ESP_CODEC_DEV_OK;
}

// under the lock
esp_err_t apply_mic()
{
    if (!g_mic_set) {
        return ESP_OK;
    }
    const Es8311Gain gain = es8311_gain(g_mic_auto ? kAlcGainDb : g_mic_gain_db);
    const bool ok = update_bits(kRegPga, 0x0F, gain.pga) && update_bits(kRegScale, 0x07, gain.scale) &&
                    g_codec->set_reg(g_codec, kRegVolume, gain.volume) == ESP_CODEC_DEV_OK &&
                    g_codec->set_reg(g_codec, kRegLevels, kAlcMaxLevel << 4 | kAlcMinLevel) == ESP_CODEC_DEV_OK &&
                    update_bits(kRegAlc, 0x8F, (g_mic_auto ? 0x80 : 0x00) | kAlcWindow);
    if (!ok) {
        ESP_LOGW(kTag, "ES8311: microphone gain not set");
    }
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t set_mic(float gain_db, bool auto_gain)
{
    Lock lock;
    g_mic_gain_db = gain_db;
    g_mic_auto = auto_gain;
    g_mic_set = true;
    return apply_mic();
}

void log_mic()
{
    Lock lock;
    char line[96];
    int length = 0;
    for (int reg = 0x14; reg <= 0x1C; reg++) {
        int value = 0;
        g_codec->get_reg(g_codec, reg, &value);
        length += std::snprintf(line + length, sizeof(line) - length, " %02X=%02X", reg, value & 0xFF);
    }
    ESP_LOGI(kTag, "ES8311 ADC registers:%s (%.1f dB%s)", line, static_cast<double>(g_mic_gain_db),
             g_mic_auto ? ", ALC" : "");
}
#endif

esp_err_t start(uint32_t sample_rate)
{
    Lock lock;
    esp_codec_dev_sample_info_t format{};
    format.bits_per_sample = 16;
    format.channel = 2;
    format.sample_rate = sample_rate;
    format.mclk_multiple = 256;
    if (g_codec->set_fs(g_codec, &format) != ESP_CODEC_DEV_OK || g_codec->enable(g_codec, true) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(kTag, "ES8311: no output at %lu Hz", static_cast<unsigned long>(sample_rate));
        return ESP_FAIL;
    }
    // Every time: the driver's stop (es8311_suspend) writes 0 to the DAC volume (REG32) and its
    // start leaves it there, so only the first sound after a restart played (2026-10-09).
    if (g_codec->set_vol(g_codec, kDacDb) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(kTag, "ES8311: volume not set");
        return ESP_FAIL;
    }
#if CONFIG_HC_MIC
    // the driver's start writes REG14 and REG17 (30 dB PGA, ADC volume 0 dB)
    return apply_mic();
#else
    return ESP_OK;
#endif
}

void stop()
{
    Lock lock;
    g_codec->enable(g_codec, false);
}

#if CONFIG_HC_MIC
constexpr AudioCodec kCodec{.start = start, .stop = stop, .set_mic = set_mic, .log_mic = log_mic};
#else
constexpr AudioCodec kCodec{.start = start, .stop = stop, .set_mic = nullptr, .log_mic = nullptr};
#endif

} // namespace

esp_err_t es8311_init(i2c_master_bus_handle_t bus)
{
    if (bus == nullptr || i2c_master_probe(bus, kAddress, kProbeTimeoutMs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    audio_codec_i2c_cfg_t i2c{};
    i2c.port = 0;
    i2c.addr = kAddress << 1;
    i2c.bus_handle = bus;
    const audio_codec_ctrl_if_t *control = audio_codec_new_i2c_ctrl(&i2c);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    g_lock = xSemaphoreCreateMutex();
    if (control == nullptr || gpio == nullptr || g_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    es8311_codec_cfg_t config{};
    config.ctrl_if = control;
    config.gpio_if = gpio;
#if CONFIG_HC_MIC
    config.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
#else
    config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
#endif
    config.pa_pin = kPinAmplifier;
    config.use_mclk = true;
    config.mclk_div = 256;
    // The driver takes off 20 log(DAC voltage / amplifier voltage) from every volume, 3.3 V and
    // 5 V by default: set_vol(0) was +3.6 dB on the DAC. Equal voltages make set_vol() the DAC's
    // level itself.
    config.hw_gain.pa_voltage = 3.3f;
    config.hw_gain.codec_dac_voltage = 3.3f;
    // es8311_codec_new() opens the codec and leaves the amplifier off
    g_codec = es8311_codec_new(&config);
    if (g_codec == nullptr) {
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "ES8311 audio codec at 0x%02x, amplifier on GPIO %d", kAddress, kPinAmplifier);
    return ESP_OK;
}

const AudioCodec *es8311()
{
    return g_codec != nullptr ? &kCodec : nullptr;
}

} // namespace hc::board::detail
