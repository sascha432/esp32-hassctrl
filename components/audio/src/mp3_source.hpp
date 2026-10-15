#pragma once

// An MP3 decoded while it plays (CONFIG_HC_AUDIO_MP3): esp_audio_codec's simple decoder finds the
// frames in what is read from a file, 2 KB at a time, or in a buffer that holds the whole MP3 (a
// sound from Home Assistant). The decoder and the buffers exist only while one is open, in PSRAM;
// stereo is mixed to mono.

#include "esp_audio_simple_dec.h"
#include "source.hpp"

namespace hc::audio::detail {

class Mp3Source final : public Source {
public:
    ~Mp3Source() override;
    // registers the MP3 decoder; once, before the first open()
    static void init();
    bool open(const char *path);
    // Takes `data` (heap_caps_malloc), also when it fails: freed with close().
    bool open(uint8_t *data, size_t size);
    void close();
    size_t read(int16_t *out, size_t max) override;
    uint32_t sample_rate() const override
    {
        return sample_rate_;
    }
    bool restart() override;

private:
    // decodes the next frame into pcm_; false at the end or after an error
    bool decode();
    bool fill();
    bool open_decoder();

    // a file descriptor, not a FILE: stdio's buffer would take internal RAM, the 2 KB pieces are
    // read into in_ (PSRAM) anyway; -1 for a buffer
    int file_ = -1;
    esp_audio_simple_dec_handle_t decoder_ = nullptr;
    // 2 KB of the file, or the whole buffer (in_end_ its size)
    uint8_t *in_ = nullptr;
    size_t in_start_ = 0;
    size_t in_end_ = 0;
    bool end_of_file_ = false;
    uint8_t *frame_ = nullptr;
    size_t frame_size_ = 0;
    // the mono samples of the last frame, in frame_
    size_t pcm_count_ = 0;
    size_t pcm_position_ = 0;
    uint32_t sample_rate_ = kSampleRate;
};

} // namespace hc::audio::detail
