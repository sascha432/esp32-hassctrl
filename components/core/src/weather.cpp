#include "hc/core/weather.hpp"

#include <cmath>
#include <iterator>

namespace hc::core {

namespace {

struct Entry {
    Condition condition;
    std::string_view state;
    const char *words;
    const char *icon;
};

// homeassistant/components/weather/const.py, the frontend's words (en) and icons
// (src/data/weather.ts, weatherIcons)
constexpr Entry kConditions[] = {
    {Condition::ClearNight, "clear-night", "Clear, night", "weather-night"},
    {Condition::Cloudy, "cloudy", "Cloudy", "weather-cloudy"},
    {Condition::Exceptional, "exceptional", "Exceptional", "alert-circle-outline"},
    {Condition::Fog, "fog", "Fog", "weather-fog"},
    {Condition::Hail, "hail", "Hail", "weather-hail"},
    {Condition::Lightning, "lightning", "Lightning", "weather-lightning"},
    {Condition::LightningRainy, "lightning-rainy", "Lightning, rainy", "weather-lightning-rainy"},
    {Condition::PartlyCloudy, "partlycloudy", "Partly cloudy", "weather-partly-cloudy"},
    {Condition::Pouring, "pouring", "Pouring", "weather-pouring"},
    {Condition::Rainy, "rainy", "Rainy", "weather-rainy"},
    {Condition::Snowy, "snowy", "Snowy", "weather-snowy"},
    {Condition::SnowyRainy, "snowy-rainy", "Snowy, rainy", "weather-snowy-rainy"},
    {Condition::Sunny, "sunny", "Sunny", "weather-sunny"},
    {Condition::Windy, "windy", "Windy", "weather-windy"},
    {Condition::WindyVariant, "windy-variant", "Windy, cloudy", "weather-windy-variant"},
};

const Entry *entry_of(Condition condition)
{
    for (const Entry &entry : kConditions) {
        if (entry.condition == condition) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

Condition condition_of(std::string_view state)
{
    for (const Entry &entry : kConditions) {
        if (entry.state == state) {
            return entry.condition;
        }
    }
    return Condition::Unknown;
}

const char *condition_words(Condition condition)
{
    const Entry *entry = entry_of(condition);
    return entry != nullptr ? entry->words : "";
}

const char *condition_icon(Condition condition)
{
    const Entry *entry = entry_of(condition);
    return entry != nullptr ? entry->icon : "weather-partly-cloudy";
}

const char *compass(float degrees)
{
    static constexpr const char *kPoints[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                              "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    if (std::isnan(degrees)) {
        return "";
    }
    float turned = std::fmod(degrees, 360.0f);
    if (turned < 0.0f) {
        turned += 360.0f;
    }
    const auto index = static_cast<size_t>(std::lround(turned / 22.5f)) % std::size(kPoints);
    return kPoints[index];
}

} // namespace hc::core
