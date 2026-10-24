#pragma once

// An MP3 decoded while it plays (CONFIG_HC_AUDIO_MP3): esp_audio_codec's simple decoder finds the
// frames in what is read, 2 KB at a time, from a file, from a download on its way (a ByteRing,
// CONFIG_HC_UI_PLAYER), or in a buffer that holds the whole MP3 (a sound from Home Assistant).
// The decoder and the buffers exist only while one is open, in PSRAM; stereo is mixed to mono.
//
// The ID3v2 tag at the start of a file or a download is read by formats::Mp3Reader (title,
// artist, album, cover) and not handed to the decoder: a cover of 70 KB held bytes it took for
// frames. Its Xing/Info header gives the exact length of a file with a variable bitrate.

#include <memory>

#include "esp_audio_simple_dec.h"
#include "hc/formats/mp3_tags.hpp"
#include "source.hpp"

#if CONFIG_HC_UI_PLAYER
#include "hc/core/byte_ring.hpp"
#endif

namespace hc::audio::detail {

class Mp3Source final : public Source {
public:
    ~Mp3Source() override;
    // registers the MP3 decoder; once, before the first open()
    static void init();
    bool open(const char *path);
    // Takes `data` (heap_caps_malloc), also when it fails: freed with close().
    bool open(uint8_t *data, size_t size);
#if CONFIG_HC_UI_PLAYER
    // A download that is on its way: it plays once a part of it is there.
    bool open(std::shared_ptr<core::ByteRing> ring);
#endif
    void close();
    size_t read(int16_t *out, size_t max) override;
    uint32_t sample_rate() const override
    {
        return sample_rate_;
    }
    bool restart() override;
    // From the Xing/Info header, or the size and the first frame's bitrate: exact for a constant
    // bitrate, about right for a variable one without a header. A download: once its size is known.
    uint32_t duration_ms() const override;
    // to the byte the header's table or the bitrate gives; the decoder finds the next frame there
    bool seek(uint32_t ms) override;
    // a download that has nothing to play right now (read() gave 0 but it is not the end)
    bool waiting() const override
    {
        return waiting_;
    }
    bool failed() const override;
    // the tag, once read (or once it is clear there is none)
    bool tags_ready() const
    {
        return reader_ != nullptr && reader_->done();
    }
    const formats::Mp3Tags *tags() const
    {
        return reader_ != nullptr ? &reader_->tags() : nullptr;
    }

private:
    enum class Fill : uint8_t { Got, Waiting, End };

    // decodes the next frame into pcm_; false at the end, after an error or while waiting
    bool decode();
    Fill fill();
    // the next bytes from the file or the download into in_ + in_end_; 0 at the end (or waiting)
    size_t read_input(uint8_t *out, size_t max, Fill &state);
    bool open_decoder();
    void reset_input();
    // the input's whole size, 0 unknown
    uint32_t input_size() const;

    // a file descriptor, not a FILE: stdio's buffer would take internal RAM, the 2 KB pieces are
    // read into in_ (PSRAM) anyway; -1 for a buffer or a download
    int file_ = -1;
#if CONFIG_HC_UI_PLAYER
    std::shared_ptr<core::ByteRing> ring_;
    // a download plays from this many bytes on, and again after it ran dry
    bool buffering_ = false;
#endif
    bool waiting_ = false;
    esp_audio_simple_dec_handle_t decoder_ = nullptr;
    // 2 KB of the input, or the whole buffer (in_end_ its size)
    uint8_t *in_ = nullptr;
    size_t in_start_ = 0;
    size_t in_end_ = 0;
    bool end_of_file_ = false;
    // the input's offset of the next byte read
    uint32_t offset_ = 0;
    // the tag; null for a buffer
    std::unique_ptr<formats::Mp3Reader> reader_;
    uint8_t *frame_ = nullptr;
    size_t frame_size_ = 0;
    // the mono samples of the last frame, in frame_
    size_t pcm_count_ = 0;
    size_t pcm_position_ = 0;
    uint32_t sample_rate_ = kSampleRate;
    // bits per second of the first frame, 0 before it; the file's size (0 for a buffer)
    uint32_t bitrate_ = 0;
    uint32_t file_size_ = 0;
};

} // namespace hc::audio::detail
