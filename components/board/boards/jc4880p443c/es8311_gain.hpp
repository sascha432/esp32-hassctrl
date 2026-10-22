#pragma once

// A gain of the ES8311's microphone input as its three stages, in register values: the analog PGA
// first (REG14[3:0], 0 to 30 dB in 3 dB steps, the least noise), then the digital ADC scale
// (REG16[2:0], 0 to 42 dB in 6 dB steps), the rest in the ADC volume (REG17, 0.5 dB steps, 0xBF =
// 0 dB). The steps are those of Linux's es8311 driver (DECLARE_TLV_DB_SCALE of the three
// controls). Header only: the host tests check it.

#include <algorithm>
#include <cstdint>

namespace hc::board::detail {

struct Es8311Gain {
    uint8_t pga;    // REG14[3:0], 3 dB a step
    uint8_t scale;  // REG16[2:0], 6 dB a step
    uint8_t volume; // REG17, 0xBF + 0.5 dB a step
};

// 30 dB of PGA and 30 dB of scale; the ADC volume only fills in the half steps
constexpr float kEs8311MaxGainDb = 60.0f;
constexpr uint8_t kEs8311VolumeZeroDb = 0xBF;

// `db` rounded to 0.5 dB, 0 to kEs8311MaxGainDb
constexpr Es8311Gain es8311_gain(float db)
{
    const float clamped = std::clamp(db, 0.0f, kEs8311MaxGainDb);
    int halves = static_cast<int>(clamped * 2.0f + 0.5f);
    const int pga = std::min(halves / 6, 10);
    halves -= pga * 6;
    const int scale = std::min(halves / 12, 7);
    halves -= scale * 12;
    return {static_cast<uint8_t>(pga), static_cast<uint8_t>(scale),
            static_cast<uint8_t>(kEs8311VolumeZeroDb + halves)};
}

// what the registers give, dB
constexpr float es8311_gain_db(const Es8311Gain &gain)
{
    return gain.pga * 3.0f + gain.scale * 6.0f + (gain.volume - kEs8311VolumeZeroDb) * 0.5f;
}

} // namespace hc::board::detail
