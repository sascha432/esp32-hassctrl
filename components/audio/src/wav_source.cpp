#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "source.hpp"

namespace hc::audio::detail {

namespace {

constexpr const char *kTag = "audio";
constexpr size_t kInSamples = 2048;
constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatExtensible = 0xfffe;

uint16_t u16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}

uint32_t u32(const uint8_t *p)
{
    return u16(p) | static_cast<uint32_t>(u16(p + 2)) << 16;
}

bool read_all(int file, void *out, size_t size)
{
    return ::read(file, out, size) == static_cast<ssize_t>(size);
}

} // namespace

WavSource::~WavSource()
{
    close();
}

// The RIFF chunks up to "data": "fmt " says the format, the others are skipped.
bool WavSource::open(const char *path)
{
    close();
    file_ = ::open(path, O_RDONLY);
    if (file_ < 0) {
        ESP_LOGW(kTag, "%s: cannot open", path);
        return false;
    }
    uint8_t header[12];
    bool format = false;
    if (read_all(file_, header, sizeof(header)) && std::memcmp(header, "RIFF", 4) == 0 &&
        std::memcmp(header + 8, "WAVE", 4) == 0) {
        uint32_t position = sizeof(header);
        uint8_t chunk[8];
        while (read_all(file_, chunk, sizeof(chunk))) {
            const uint32_t size = u32(chunk + 4);
            position += sizeof(chunk);
            if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
                uint8_t fmt[16];
                if (!read_all(file_, fmt, sizeof(fmt))) {
                    break;
                }
                const uint16_t tag = u16(fmt);
                channels_ = static_cast<uint8_t>(u16(fmt + 2));
                sample_rate_ = u32(fmt + 4);
                format = (tag == kFormatPcm || tag == kFormatExtensible) && u16(fmt + 14) == 16 &&
                         (channels_ == 1 || channels_ == 2) && sample_rate_ >= 8000 && sample_rate_ <= 48000;
                if (!format) {
                    break;
                }
                position += 16;
                // the rest of the chunk (and its pad byte) is skipped below
                const uint32_t rest = size - 16 + (size & 1);
                position += rest;
                if (::lseek(file_, position, SEEK_SET) < 0) {
                    format = false;
                    break;
                }
                continue;
            }
            if (std::memcmp(chunk, "data", 4) == 0) {
                data_start_ = position;
                data_size_ = size;
                break;
            }
            position += size + (size & 1);
            if (::lseek(file_, position, SEEK_SET) < 0) {
                break;
            }
        }
    }
    if (!format || data_start_ == 0) {
        ESP_LOGW(kTag, "%s: not a 16 bit PCM WAV file", path);
        close();
        return false;
    }
    in_ = static_cast<int16_t *>(heap_caps_malloc_prefer(kInSamples * sizeof(int16_t), 2,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT));
    if (in_ == nullptr) {
        close();
        return false;
    }
    return true;
}

void WavSource::close()
{
    if (file_ >= 0) {
        ::close(file_);
        file_ = -1;
    }
    heap_caps_free(in_);
    in_ = nullptr;
    in_count_ = in_position_ = 0;
    data_start_ = data_size_ = data_read_ = 0;
    channels_ = 1;
}

size_t WavSource::read(int16_t *out, size_t max)
{
    if (file_ < 0) {
        return 0;
    }
    if (in_position_ == in_count_) {
        // whole frames of all channels
        const size_t frame_bytes = channels_ * sizeof(int16_t);
        size_t want = std::min<size_t>(kInSamples * sizeof(int16_t), data_size_ - data_read_);
        want -= want % frame_bytes;
        const ssize_t got = want > 0 ? ::read(file_, in_, want) : 0;
        if (got <= 0) {
            return 0;
        }
        data_read_ += static_cast<uint32_t>(got);
        in_count_ = static_cast<size_t>(got) / sizeof(int16_t);
        in_count_ -= in_count_ % channels_;
        in_position_ = 0;
    }
    const size_t frames = std::min(max, (in_count_ - in_position_) / channels_);
    for (size_t i = 0; i < frames; i++) {
        const int16_t *frame = in_ + in_position_ + i * channels_;
        out[i] = channels_ == 2 ? static_cast<int16_t>((frame[0] + frame[1]) / 2) : frame[0];
    }
    in_position_ += frames * channels_;
    return frames;
}

bool WavSource::restart()
{
    return seek(0);
}

uint32_t WavSource::duration_ms() const
{
    const uint32_t bytes_per_second = sample_rate_ * channels_ * sizeof(int16_t);
    return bytes_per_second > 0 ? static_cast<uint32_t>(uint64_t{data_size_} * 1000 / bytes_per_second) : 0;
}

bool WavSource::seek(uint32_t ms)
{
    if (file_ < 0) {
        return false;
    }
    const uint32_t frame_bytes = channels_ * sizeof(int16_t);
    const uint64_t frame = uint64_t{ms} * sample_rate_ / 1000;
    const uint32_t offset = static_cast<uint32_t>(std::min<uint64_t>(frame * frame_bytes, data_size_));
    if (::lseek(file_, data_start_ + offset, SEEK_SET) < 0) {
        return false;
    }
    data_read_ = offset;
    in_count_ = in_position_ = 0;
    return true;
}

} // namespace hc::audio::detail
