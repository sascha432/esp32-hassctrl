#include "mp3_source.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"

namespace hc::audio::detail {

namespace {

constexpr const char *kTag = "audio";
constexpr size_t kInSize = 2048;
// one MP3 frame: 1152 samples of 2 channels; the decoder asks for more when it needs it
constexpr size_t kFrameSize = 1152 * 2 * sizeof(int16_t);

// buffers that are only used while a file plays go to PSRAM where there is some
void *allocate(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

} // namespace

} // namespace hc::audio::detail

// esp_audio_codec allocates through these; its own definitions are weak and call malloc, which
// keeps every block under 16 KB in internal RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL): the MP3
// decoder took 25 KB of the 33 KB left on the JC3248W535. Here they prefer PSRAM. The signatures
// are those of ESP-ADF's media_lib_sal, checked in the library's disassembly (version 2.6.2).
extern "C" void *media_lib_module_malloc(const char *, size_t size)
{
    return hc::audio::detail::allocate(size);
}

extern "C" void *media_lib_module_calloc(const char *, size_t count, size_t size)
{
    return heap_caps_calloc_prefer(count, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

extern "C" void *media_lib_module_realloc(const char *, void *buffer, size_t size)
{
    return heap_caps_realloc_prefer(buffer, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

extern "C" void media_lib_free(void *buffer)
{
    heap_caps_free(buffer);
}

namespace hc::audio::detail {

Mp3Source::~Mp3Source()
{
    close();
}

void Mp3Source::init()
{
    esp_mp3_dec_register();
}

bool Mp3Source::open(const char *path)
{
    close();
    file_ = ::open(path, O_RDONLY);
    if (file_ < 0) {
        ESP_LOGW(kTag, "%s: cannot open", path);
        return false;
    }
    struct stat info {};
    file_size_ = fstat(file_, &info) == 0 ? static_cast<uint32_t>(info.st_size) : 0;
    in_ = static_cast<uint8_t *>(allocate(kInSize));
    if (in_ == nullptr || !open_decoder()) {
        ESP_LOGW(kTag, "%s: no memory for the decoder", path);
        close();
        return false;
    }
    return true;
}

bool Mp3Source::open(uint8_t *data, size_t size)
{
    close();
    // the whole MP3 is the input: nothing more to read
    in_ = data;
    in_end_ = size;
    end_of_file_ = true;
    if (data == nullptr || size == 0 || !open_decoder()) {
        ESP_LOGW(kTag, "no memory for the decoder");
        close();
        return false;
    }
    return true;
}

bool Mp3Source::open_decoder()
{
    esp_audio_simple_dec_cfg_t config{};
    config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    frame_ = static_cast<uint8_t *>(allocate(kFrameSize));
    frame_size_ = kFrameSize;
    return frame_ != nullptr && esp_audio_simple_dec_open(&config, &decoder_) == ESP_AUDIO_ERR_OK;
}

void Mp3Source::close()
{
    if (decoder_ != nullptr) {
        esp_audio_simple_dec_close(decoder_);
        decoder_ = nullptr;
    }
    if (file_ >= 0) {
        ::close(file_);
        file_ = -1;
    }
    heap_caps_free(in_);
    heap_caps_free(frame_);
    in_ = frame_ = nullptr;
    in_start_ = in_end_ = 0;
    end_of_file_ = false;
    pcm_count_ = pcm_position_ = 0;
    bitrate_ = file_size_ = 0;
}

bool Mp3Source::restart()
{
    if (decoder_ == nullptr) {
        return false;
    }
    if (file_ >= 0) {
        if (::lseek(file_, 0, SEEK_SET) != 0) {
            return false;
        }
        in_end_ = 0;
        end_of_file_ = false;
    }
    esp_audio_simple_dec_reset(decoder_);
    in_start_ = 0;
    pcm_count_ = pcm_position_ = 0;
    return true;
}

// what is left of the input to the front, the rest from the file
bool Mp3Source::fill()
{
    if (end_of_file_) {
        return false;
    }
    std::memmove(in_, in_ + in_start_, in_end_ - in_start_);
    in_end_ -= in_start_;
    in_start_ = 0;
    const ssize_t got = ::read(file_, in_ + in_end_, kInSize - in_end_);
    in_end_ += got > 0 ? static_cast<size_t>(got) : 0;
    end_of_file_ = got <= 0;
    return true;
}

bool Mp3Source::decode()
{
    for (;;) {
        if (in_start_ == in_end_ && !fill()) {
            return false;
        }
        esp_audio_simple_dec_raw_t raw{};
        raw.buffer = in_ + in_start_;
        raw.len = static_cast<uint32_t>(in_end_ - in_start_);
        raw.eos = end_of_file_;
        esp_audio_simple_dec_out_t out{};
        out.buffer = frame_;
        out.len = static_cast<uint32_t>(frame_size_);
        const esp_audio_err_t err = esp_audio_simple_dec_process(decoder_, &raw, &out);
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            auto *larger = static_cast<uint8_t *>(allocate(out.needed_size));
            if (larger == nullptr) {
                return false;
            }
            heap_caps_free(frame_);
            frame_ = larger;
            frame_size_ = out.needed_size;
            continue;
        }
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(kTag, "MP3 decoder: %d", static_cast<int>(err));
            return false;
        }
        in_start_ += raw.consumed;
        if (out.decoded_size == 0) {
            // a frame does not fit in what was read: read more, or stop at the end of the file
            if (raw.consumed == 0 && !fill()) {
                return false;
            }
            continue;
        }
        esp_audio_simple_dec_info_t info{};
        esp_audio_simple_dec_get_info(decoder_, &info);
        if (info.bits_per_sample != 16 || info.channel == 0) {
            ESP_LOGW(kTag, "MP3 with %u bit, %u channels", info.bits_per_sample, info.channel);
            return false;
        }
        sample_rate_ = info.sample_rate;
        if (bitrate_ == 0) {
            // the MP3 decoder says kbit/s (256 for 256 kbit/s, 2026-10-23), against its header's
            // "bits per second"; an MP3's bitrates are 8 to 320 kbit/s
            bitrate_ = info.bitrate < 1000 ? info.bitrate * 1000 : info.bitrate;
        }
        // mono: the channels of a sample averaged, in place
        auto *pcm = reinterpret_cast<int16_t *>(frame_);
        const size_t frames = out.decoded_size / sizeof(int16_t) / info.channel;
        for (size_t i = 0; i < frames; i++) {
            int32_t sum = 0;
            for (uint8_t c = 0; c < info.channel; c++) {
                sum += pcm[i * info.channel + c];
            }
            pcm[i] = static_cast<int16_t>(sum / info.channel);
        }
        pcm_count_ = frames;
        pcm_position_ = 0;
        return true;
    }
}

uint32_t Mp3Source::duration_ms() const
{
    return bitrate_ > 0 ? static_cast<uint32_t>(uint64_t{file_size_} * 8000 / bitrate_) : 0;
}

bool Mp3Source::seek(uint32_t ms)
{
    if (decoder_ == nullptr || file_ < 0 || bitrate_ == 0) {
        return false;
    }
    const uint64_t offset = std::min<uint64_t>(uint64_t{ms} * bitrate_ / 8000, file_size_);
    if (::lseek(file_, static_cast<off_t>(offset), SEEK_SET) < 0) {
        return false;
    }
    esp_audio_simple_dec_reset(decoder_);
    in_start_ = in_end_ = 0;
    end_of_file_ = false;
    pcm_count_ = pcm_position_ = 0;
    return true;
}

size_t Mp3Source::read(int16_t *out, size_t max)
{
    if (decoder_ == nullptr || (pcm_position_ == pcm_count_ && !decode())) {
        return 0;
    }
    const size_t count = std::min(max, pcm_count_ - pcm_position_);
    std::memcpy(out, reinterpret_cast<const int16_t *>(frame_) + pcm_position_, count * sizeof(int16_t));
    pcm_position_ += count;
    return count;
}

} // namespace hc::audio::detail
