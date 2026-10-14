#pragma once

// The words and icons of a weather entity (docs/ui/lvgl/lvgl-dashboard.md section 3, the weather
// tile; docs/ui/lvgl/lvgl-panels.md, the weather panel). Plain C++, host tested.

#include <cstdint>
#include <string_view>

namespace hc::core {

// The state of a weather entity and the condition of a forecast entry: HA's conditions
// (homeassistant/components/weather/const.py).
enum class Condition : uint8_t {
    Unknown, // not one of them, "unknown" or "unavailable"
    ClearNight,
    Cloudy,
    Exceptional,
    Fog,
    Hail,
    Lightning,
    LightningRainy,
    PartlyCloudy,
    Pouring,
    Rainy,
    Snowy,
    SnowyRainy,
    Sunny,
    Windy,
    WindyVariant,
};

// "partlycloudy" -> PartlyCloudy
Condition condition_of(std::string_view state);

// the condition in the words of the HA frontend ("Partly cloudy"); "" for Unknown
const char *condition_words(Condition condition);

// the MDI name of the condition's glyph, as the HA frontend picks it ("weather-partly-cloudy");
// Unknown: "weather-partly-cloudy" as well, the glyph of the domain
const char *condition_icon(Condition condition);

// the point of the compass of a wind bearing in degrees, 16 points ("ENE"); "" for NaN
const char *compass(float degrees);

} // namespace hc::core
