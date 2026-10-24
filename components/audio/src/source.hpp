#pragma once

// What the audio task plays: a source of 16 bit mono samples. The task asks for the next samples
// until a source has no more; a looping sound starts over with restart().

#include <cstddef>
#include <cstdint>

#include "hc/audio/audio.hpp"

namespace hc::audio::detail {

class Source {
public:
    virtual ~Source() = default;
    // up to `max` samples into `out`; 0 at the end (or after an error)
    virtual size_t read(int16_t *out, size_t max) = 0;
    // valid once read() returned samples
    virtual uint32_t sample_rate() const
    {
        return kSampleRate;
    }
    // back to the start for a loop; false when it cannot
    virtual bool restart() = 0;
    // a file of the player (play_media): its length, 0 while it is not known; to `ms` from the
    // start, false when it cannot
    virtual uint32_t duration_ms() const
    {
        return 0;
    }
    virtual bool seek(uint32_t ms)
    {
        (void)ms;
        return false;
    }
    // A download that has nothing to play right now: read() gave 0, but more comes.
    virtual bool waiting() const
    {
        return false;
    }
    // it ended because reading failed (a download that broke for good)
    virtual bool failed() const
    {
        return false;
    }
};

// samples in memory (the click)
class MemorySource final : public Source {
public:
    void open(const int16_t *samples, uint32_t count);
    size_t read(int16_t *out, size_t max) override;
    bool restart() override;

private:
    const int16_t *samples_ = nullptr;
    uint32_t count_ = 0;
    uint32_t position_ = 0;
};

// a sine tone that fades in and out (console `sound tone`)
class ToneSource final : public Source {
public:
    void open(uint16_t hz, uint32_t count);
    size_t read(int16_t *out, size_t max) override;
    bool restart() override;

private:
    uint16_t hz_ = 0;
    uint32_t count_ = 0;
    uint32_t position_ = 0;
    float phase_ = 0.0f;
};

// The built-in alarm: four short beeps and a pause, 1.5 s
class BeepsSource final : public Source {
public:
    void open();
    size_t read(int16_t *out, size_t max) override;
    bool restart() override;

private:
    uint32_t position_ = 0;
    float phase_ = 0.0f;
};

// A WAV file of the player: 16 bit PCM, mono or stereo (mixed to mono), at its own rate. Read 4 KB
// at a time into a buffer in PSRAM where there is some.
class WavSource final : public Source {
public:
    ~WavSource() override;
    bool open(const char *path);
    void close();
    size_t read(int16_t *out, size_t max) override;
    uint32_t sample_rate() const override
    {
        return sample_rate_;
    }
    bool restart() override;
    uint32_t duration_ms() const override;
    bool seek(uint32_t ms) override;

private:
    // a file descriptor, as Mp3Source: no stdio buffer in internal RAM
    int file_ = -1;
    int16_t *in_ = nullptr;
    // samples (all channels) in in_, and the next one
    size_t in_count_ = 0;
    size_t in_position_ = 0;
    uint32_t data_start_ = 0;
    uint32_t data_size_ = 0;
    // bytes of the data read so far
    uint32_t data_read_ = 0;
    uint32_t sample_rate_ = kSampleRate;
    uint8_t channels_ = 1;
};

} // namespace hc::audio::detail
