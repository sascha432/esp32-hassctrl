#include "mp3_source.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"

namespace hc::audio::detail {

namespace {

constexpr const char *kTag = "audio";
constexpr size_t kInSize = 2048;
#if CONFIG_HC_UI_PLAYER
// A download plays from this many bytes on, and again after it ran dry: a second at 256 kbit/s.
constexpr size_t kStartBytes = 32 * 1024;
#endif
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
    reader_.reset(new (std::nothrow) formats::Mp3Reader());
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

#if CONFIG_HC_UI_PLAYER
bool Mp3Source::open(std::shared_ptr<core::ByteRing> ring)
{
    close();
    ring_ = std::move(ring);
    offset_ = ring_->position();
    buffering_ = true;
    in_ = static_cast<uint8_t *>(allocate(kInSize));
    reader_.reset(new (std::nothrow) formats::Mp3Reader());
    if (in_ == nullptr || !open_decoder()) {
        ESP_LOGW(kTag, "no memory for the decoder");
        close();
        return false;
    }
    return true;
}
#endif

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
#if CONFIG_HC_UI_PLAYER
    if (ring_) {
        // the download ends with it
        ring_->cancel();
        ring_.reset();
    }
    buffering_ = false;
#endif
    reader_.reset();
    heap_caps_free(in_);
    heap_caps_free(frame_);
    in_ = frame_ = nullptr;
    reset_input();
    offset_ = 0;
    bitrate_ = file_size_ = 0;
}

void Mp3Source::reset_input()
{
    in_start_ = in_end_ = 0;
    end_of_file_ = false;
    pcm_count_ = pcm_position_ = 0;
    waiting_ = false;
}

uint32_t Mp3Source::input_size() const
{
#if CONFIG_HC_UI_PLAYER
    if (ring_) {
        return ring_->size();
    }
#endif
    return file_size_;
}

bool Mp3Source::failed() const
{
#if CONFIG_HC_UI_PLAYER
    return ring_ && ring_->failed();
#else
    return false;
#endif
}

bool Mp3Source::restart()
{
    // a loop: only a file (an alarm sound)
    if (decoder_ == nullptr || file_ < 0 || ::lseek(file_, 0, SEEK_SET) != 0) {
        return false;
    }
    esp_audio_simple_dec_reset(decoder_);
    reset_input();
    offset_ = 0;
    return true;
}

size_t Mp3Source::read_input(uint8_t *out, size_t max, Fill &state)
{
    state = Fill::Got;
#if CONFIG_HC_UI_PLAYER
    if (ring_) {
        // a download plays once a part is there, and after it ran dry once it has a part again
        if (buffering_ && ring_->available() < kStartBytes && !ring_->finished()) {
            state = Fill::Waiting;
            return 0;
        }
        buffering_ = false;
        const size_t got = ring_->read(out, max);
        if (got == 0 && !ring_->ended()) {
            buffering_ = true;
            state = Fill::Waiting;
        } else if (got == 0) {
            state = Fill::End;
        }
        return got;
    }
#endif
    const ssize_t got = ::read(file_, out, max);
    if (got <= 0) {
        state = Fill::End;
        return 0;
    }
    return static_cast<size_t>(got);
}

// what is left of the input to the front, the next bytes after it; the tag's bytes are read past
Mp3Source::Fill Mp3Source::fill()
{
    if (end_of_file_) {
        return Fill::End;
    }
    std::memmove(in_, in_ + in_start_, in_end_ - in_start_);
    in_end_ -= in_start_;
    in_start_ = 0;
    for (;;) {
        Fill state = Fill::Got;
        const uint32_t at = offset_;
        const size_t got = read_input(in_ + in_end_, kInSize - in_end_, state);
        if (got == 0) {
            if (state == Fill::Waiting) {
                return Fill::Waiting;
            }
            // the decoder gets what is left with the end of the stream
            end_of_file_ = true;
            if (reader_ != nullptr && !reader_->done()) {
                reader_->end();
            }
            return Fill::Got;
        }
        offset_ += static_cast<uint32_t>(got);
        size_t skip = 0;
        if (reader_ != nullptr) {
            if (!reader_->done()) {
                reader_->feed(in_ + in_end_, got);
            }
            if (reader_->tag_size_known() && at < reader_->tag_size()) {
                skip = std::min<size_t>(got, reader_->tag_size() - at);
            }
        }
        std::memmove(in_ + in_end_, in_ + in_end_ + skip, got - skip);
        in_end_ += got - skip;
        if (got > skip) {
            return Fill::Got;
        }
    }
}

bool Mp3Source::decode()
{
    waiting_ = false;
    for (;;) {
        if (in_start_ == in_end_) {
            const Fill state = fill();
            if (state != Fill::Got) {
                waiting_ = state == Fill::Waiting;
                return false;
            }
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
            if (raw.consumed == 0) {
                const Fill state = fill();
                if (state != Fill::Got) {
                    waiting_ = state == Fill::Waiting;
                    return false;
                }
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
    if (reader_ != nullptr && reader_->duration_ms() > 0) {
        return reader_->duration_ms();
    }
    const uint32_t size = input_size();
    const uint32_t tag = reader_ != nullptr ? reader_->tag_size() : 0;
    return bitrate_ > 0 && size > tag ? static_cast<uint32_t>(uint64_t{size - tag} * 8000 / bitrate_) : 0;
}

bool Mp3Source::seek(uint32_t ms)
{
#if CONFIG_HC_UI_PLAYER
    const bool seekable = file_ >= 0 || ring_;
#else
    const bool seekable = file_ >= 0;
#endif
    if (decoder_ == nullptr || !seekable) {
        return false;
    }
    const uint32_t size = input_size();
    const uint32_t tag = reader_ != nullptr ? reader_->tag_size() : 0;
    uint32_t at = 0;
    if (reader_ == nullptr || !reader_->offset_of(ms, size > tag ? size - tag : 0, at)) {
        if (bitrate_ == 0) {
            return false;
        }
        at = static_cast<uint32_t>(uint64_t{ms} * bitrate_ / 8000);
    }
    uint32_t offset = tag + at;
    if (size > 0) {
        offset = std::min(offset, size);
    }
#if CONFIG_HC_UI_PLAYER
    if (ring_) {
        ring_->seek(offset);
        buffering_ = true;
    } else
#endif
        if (::lseek(file_, static_cast<off_t>(offset), SEEK_SET) < 0) {
        return false;
    }
    // the tag is not read on from another place
    if (reader_ != nullptr && !reader_->done()) {
        reader_->end();
    }
    esp_audio_simple_dec_reset(decoder_);
    reset_input();
    offset_ = offset;
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
