#include "hc/core/icons.hpp"

namespace hc::core {

namespace {

// the glyphs of the old firmware by their MDI names, their off states, and the HA defaults of the
// domains and device classes of the tiles (docs/architecture.md section 5, "Icon order"); sorted.
// tools/build_icons.py reads this list for the icon fonts of ui and web/icons.svg; run it after a change.
constexpr std::string_view kIcons[] = {
    "alert-circle-outline",    "brightness-6",            "camera",                  "checkbox-marked-circle",
    "clock-outline",           "current-ac",              "door-closed",             "door-open",               "eye",
    "fan",                     "fan-off",                 "flash",                   "flash-off",
    "folder",                  "gauge",                   "gesture-tap-button",      "home",
    "home-assistant",          "lightbulb",               "lightbulb-off",           "lightbulb-on",
    "lightning-bolt",          "lock",                    "lock-alert",              "lock-open",
    "molecule-co2",            "motion-sensor",           "motion-sensor-off",       "palette",
    "power",                   "power-plug",              "power-plug-off",          "radiator",
    "radiobox-blank",          "remote",                  "robot",                   "robot-off",
    "script-text",             "sine-wave",               "thermometer",             "thermostat",
    "toggle-switch",           "toggle-switch-off",       "water-percent",           "weather-cloudy",
    "weather-fog",             "weather-hail",            "weather-lightning",       "weather-lightning-rainy",
    "weather-night",           "weather-partly-cloudy",   "weather-pouring",         "weather-rainy",
    "weather-snowy",           "weather-snowy-rainy",     "weather-sunny",           "weather-windy",
    "weather-windy-variant",   "window-closed",           "window-open",
};

} // namespace

std::span<const std::string_view> icon_names()
{
    return kIcons;
}

bool known_icon(std::string_view name)
{
    for (const auto icon : kIcons) {
        if (icon == name) {
            return true;
        }
    }
    return false;
}

} // namespace hc::core
