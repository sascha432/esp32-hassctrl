// A WAV file of 16 bit mono samples (the microphone's recording): the 44 byte RIFF header, then
// the samples as they are (little endian, as the chip stores them).

#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "hc/sdcard/sdcard.hpp"

namespace hc::sdcard {

namespace {

constexpr const char *kTag = "sdcard";

void put16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t *p, uint32_t v)
{
    put16(p, static_cast<uint16_t>(v));
    put16(p + 2, static_cast<uint16_t>(v >> 16));
}

} // namespace

esp_err_t save_wav(const char *path, const int16_t *samples, size_t count, uint32_t sample_rate)
{
    const uint32_t data_bytes = static_cast<uint32_t>(count * sizeof(int16_t));
    uint8_t header[44];
    std::memcpy(header, "RIFF", 4);
    put32(header + 4, 36 + data_bytes);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    put32(header + 16, 16);              // the fmt chunk's size
    put16(header + 20, 1);               // PCM
    put16(header + 22, 1);               // mono
    put32(header + 24, sample_rate);
    put32(header + 28, sample_rate * 2); // bytes per second
    put16(header + 32, 2);               // bytes per frame
    put16(header + 34, 16);              // bits per sample
    std::memcpy(header + 36, "data", 4);
    put32(header + 40, data_bytes);

    FILE *file = std::fopen(path, "wb");
    if (file == nullptr) {
        ESP_LOGW(kTag, "cannot create %s", path);
        return ESP_FAIL;
    }
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
              std::fwrite(samples, sizeof(int16_t), count, file) == count;
    ok = std::fclose(file) == 0 && ok;
    if (!ok) {
        ESP_LOGW(kTag, "writing %s failed", path);
        std::remove(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t save_recording(const int16_t *samples, size_t count, uint32_t sample_rate, char *path, size_t size)
{
    if (status().state != State::Mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    std::snprintf(path, size, "%s/recordings", kMountPoint);
    mkdir(path, 0755);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    const size_t length = std::strlen(path);
    if (local.tm_year + 1900 >= 2025) {
        std::strftime(path + length, size - length, "/rec-%Y%m%d-%H%M%S.wav", &local);
    } else {
        std::snprintf(path + length, size - length, "/rec-%lld.wav", esp_timer_get_time() / 1000);
    }
    const int64_t started = esp_timer_get_time();
    const esp_err_t err = save_wav(path, samples, count, sample_rate);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "recording saved as %s (%zu samples, %lld ms)", path, count,
                 (esp_timer_get_time() - started) / 1000);
    }
    return err;
}

} // namespace hc::sdcard
