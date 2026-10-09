// The ES8311 of the JC4880P443C with esp_codec_dev's driver, control only: the codec is an I2S
// slave with the master clock of the ESP32-P4 (256 x the sample rate), and the audio component
// writes the samples to I2S itself. Its DAC is at kDacDb (start() sets it each time); the volume of
// the firmware is in software (hc::audio), as on the boards without a codec. The driver switches
// the amplifier (GPIO 11) with the codec's output.
// Pins: docs/devices/JC4880P443C/JC4880P443C.md, Audio.

#include "es8311.hpp"

#include "driver/gpio.h"
#include "es8311_codec.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

namespace hc::board::detail {

namespace {

constexpr const char *kTag = "board";
// 7 bit address 0x18; esp_codec_dev takes the 8 bit form
constexpr uint8_t kAddress = 0x18;
constexpr gpio_num_t kPinAmplifier = GPIO_NUM_11;
constexpr uint32_t kProbeTimeoutMs = 50;
// The DAC's level, the loudest the firmware plays (the alarm, 100 %). At the DAC's full scale the
// amplifier pulled the board's supply down and it restarted ("power on" reset, 2026-10-09, on USB
// from the PC): a 500 Hz tone at 70 % volume did, at 50 % (12 dB less) it did not.
constexpr float kDacDb = -12.0f;

const audio_codec_if_t *g_codec = nullptr;

esp_err_t start(uint32_t sample_rate)
{
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
    return ESP_OK;
}

void stop()
{
    g_codec->enable(g_codec, false);
}

constexpr AudioCodec kCodec{.start = start, .stop = stop};

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
    if (control == nullptr || gpio == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    es8311_codec_cfg_t config{};
    config.ctrl_if = control;
    config.gpio_if = gpio;
    config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
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
