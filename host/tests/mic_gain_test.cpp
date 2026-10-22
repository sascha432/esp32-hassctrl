// components/board/boards/jc4880p443c/es8311_gain.hpp: a microphone gain as the ES8311's stages.

#include "check.hpp"
#include "es8311_gain.hpp"

using hc::board::detail::es8311_gain;
using hc::board::detail::es8311_gain_db;

TEST(mic_gain_fills_the_pga_first)
{
    const auto zero = es8311_gain(0.0f);
    CHECK(zero.pga == 0 && zero.scale == 0 && zero.volume == 0xBF);
    const auto pga = es8311_gain(30.0f);
    CHECK(pga.pga == 10 && pga.scale == 0 && pga.volume == 0xBF);
    // 42 dB: the full PGA, two steps of scale
    const auto speech = es8311_gain(42.0f);
    CHECK(speech.pga == 10 && speech.scale == 2 && speech.volume == 0xBF);
}

TEST(mic_gain_half_steps_in_the_adc_volume)
{
    // 31.5 dB: 30 of PGA, 1.5 in the volume
    const auto gain = es8311_gain(31.5f);
    CHECK(gain.pga == 10 && gain.scale == 0 && gain.volume == 0xBF + 3);
    const auto small = es8311_gain(4.5f);
    CHECK(small.pga == 1 && small.scale == 0 && small.volume == 0xBF + 3);
}

TEST(mic_gain_every_half_db_comes_out_exact)
{
    for (int halves = 0; halves <= 120; halves++) {
        const float db = static_cast<float>(halves) / 2.0f;
        CHECK(es8311_gain_db(es8311_gain(db)) == db);
    }
}

TEST(mic_gain_clamps_to_its_range)
{
    CHECK(es8311_gain_db(es8311_gain(-5.0f)) == 0.0f);
    CHECK(es8311_gain_db(es8311_gain(80.0f)) == 60.0f);
    const auto top = es8311_gain(60.0f);
    CHECK(top.pga == 10 && top.scale == 5 && top.volume == 0xBF);
}
