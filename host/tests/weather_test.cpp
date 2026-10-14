// components/core/src/weather.cpp: the conditions of a weather entity, their words and glyphs.

#include <cmath>
#include <string_view>

#include "check.hpp"
#include "hc/core/icons.hpp"
#include "hc/core/weather.hpp"

using namespace hc::core;

TEST(weather_reads_the_conditions)
{
    CHECK(condition_of("partlycloudy") == Condition::PartlyCloudy);
    CHECK(condition_of("clear-night") == Condition::ClearNight);
    CHECK(condition_of("windy-variant") == Condition::WindyVariant);
    CHECK(condition_of("unavailable") == Condition::Unknown);
    CHECK(condition_of("") == Condition::Unknown);
    CHECK_STR(condition_words(Condition::PartlyCloudy), "Partly cloudy");
    CHECK_STR(condition_words(Condition::LightningRainy), "Lightning, rainy");
    CHECK_STR(condition_words(Condition::Unknown), "");
    CHECK_STR(condition_icon(Condition::Cloudy), "weather-cloudy");
    CHECK_STR(condition_icon(Condition::Unknown), "weather-partly-cloudy");
}

TEST(weather_glyphs_are_in_the_icon_list)
{
    // every condition's glyph is in the fonts (tools/build_icons.py reads the icon list)
    for (int i = 0; i <= static_cast<int>(Condition::WindyVariant); i++) {
        const char *icon = condition_icon(static_cast<Condition>(i));
        if (!known_icon(icon)) {
            CHECK_STR(icon, "a name of the icon list");
        }
    }
}

TEST(weather_compass_points)
{
    CHECK_STR(compass(0), "N");
    CHECK_STR(compass(67), "ENE");
    CHECK_STR(compass(348.75f), "N");
    CHECK_STR(compass(348.7f), "NNW");
    CHECK_STR(compass(180), "S");
    CHECK_STR(compass(-90), "W");
    CHECK_STR(compass(720 + 90), "E");
    CHECK_STR(compass(NAN), "");
}
