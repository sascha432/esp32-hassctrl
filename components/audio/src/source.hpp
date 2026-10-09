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

} // namespace hc::audio::detail
