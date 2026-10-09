// The touch sounds (CONFIG_HC_AUDIO_TOUCH_SOUND): computed once instead of stored as files, they are
// 96 and 48 samples.

#include <array>
#include <cmath>

#include "hc/audio/audio.hpp"

namespace hc::audio {

namespace {

constexpr size_t kClickSamples = kSampleRate * 6 / 1000;
constexpr size_t kTickSamples = kSampleRate * 3 / 1000;

// a tone of `hz` that dies away with the time constant `decay_s`, at `level` of full scale
template <size_t N> std::array<int16_t, N> make_tick(float hz, float decay_s, float level)
{
    std::array<int16_t, N> out{};
    for (size_t i = 0; i < out.size(); i++) {
        const float t = static_cast<float>(i) / kSampleRate;
        const float value = std::sin(2.0f * static_cast<float>(M_PI) * hz * t) * std::exp(-t / decay_s);
        out[i] = static_cast<int16_t>(value * level * INT16_MAX);
    }
    return out;
}

} // namespace

void play_click()
{
    // a 3 kHz tick that dies away within a few milliseconds
    static const auto click = make_tick<kClickSamples>(3000.0f, 0.0012f, 0.5f);
    play(click.data(), click.size());
}

void play_tick()
{
    // higher, shorter and softer, so a slider that moves fast does not rattle
    static const auto tick = make_tick<kTickSamples>(4500.0f, 0.0006f, 0.3f);
    play(tick.data(), tick.size());
}

} // namespace hc::audio
