#include <algorithm>
#include <cmath>

#include "source.hpp"

namespace hc::audio::detail {

namespace {

constexpr float kTwoPi = 2.0f * static_cast<float>(M_PI);
// a tone fades in and out over 2 ms, which keeps its start and end from clicking
constexpr uint32_t kFade = kSampleRate * 2 / 1000;

constexpr uint32_t ms(uint32_t milliseconds)
{
    return kSampleRate * milliseconds / 1000;
}

// The beeps: 1760 Hz, where a small speaker is loud, 4 x (100 ms on, 80 ms off), then a pause
constexpr float kBeepHz = 1760.0f;
constexpr uint32_t kBeepOn = ms(100);
constexpr uint32_t kBeepPeriod = ms(180);
constexpr uint32_t kBeeps = 4;
constexpr uint32_t kBeepCycle = ms(1500);

// `fade` 0..1 at the edges of a tone of `length` samples
float fade(uint32_t n, uint32_t length)
{
    const uint32_t edge = std::min(n, length - 1 - n);
    return edge < kFade ? static_cast<float>(edge) / kFade : 1.0f;
}

int16_t sample(float phase, float level)
{
    return static_cast<int16_t>(std::sin(phase) * level * 0.7f * INT16_MAX);
}

void advance(float &phase, float hz)
{
    phase += kTwoPi * hz / kSampleRate;
    if (phase > kTwoPi) {
        phase -= kTwoPi;
    }
}

} // namespace

void MemorySource::open(const int16_t *samples, uint32_t count)
{
    samples_ = samples;
    count_ = count;
    position_ = 0;
}

size_t MemorySource::read(int16_t *out, size_t max)
{
    const size_t count = std::min<size_t>(max, count_ - position_);
    std::copy_n(samples_ + position_, count, out);
    position_ += count;
    return count;
}

bool MemorySource::restart()
{
    position_ = 0;
    return true;
}

void ToneSource::open(uint16_t hz, uint32_t count)
{
    hz_ = hz;
    count_ = count;
    position_ = 0;
    phase_ = 0.0f;
}

size_t ToneSource::read(int16_t *out, size_t max)
{
    const size_t count = std::min<size_t>(max, count_ - position_);
    for (size_t i = 0; i < count; i++) {
        out[i] = sample(phase_, fade(position_ + i, count_));
        advance(phase_, hz_);
    }
    position_ += count;
    return count;
}

bool ToneSource::restart()
{
    position_ = 0;
    return true;
}

void BeepsSource::open()
{
    position_ = 0;
    phase_ = 0.0f;
}

size_t BeepsSource::read(int16_t *out, size_t max)
{
    const size_t count = std::min<size_t>(max, kBeepCycle - position_);
    for (size_t i = 0; i < count; i++) {
        const uint32_t n = position_ + i;
        const uint32_t in_beep = n % kBeepPeriod;
        if (n < kBeeps * kBeepPeriod && in_beep < kBeepOn) {
            out[i] = sample(phase_, fade(in_beep, kBeepOn));
            advance(phase_, kBeepHz);
        } else {
            out[i] = 0;
            phase_ = 0.0f;
        }
    }
    position_ += count;
    return count;
}

bool BeepsSource::restart()
{
    position_ = 0;
    return true;
}

} // namespace hc::audio::detail
