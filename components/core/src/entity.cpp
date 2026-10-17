#include "hc/core/entity.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace hc::core {

namespace {

constexpr OnOffWords kOnOffWords[] = {{"On", "Off"}, {"Detected", "Clear"}, {"Occupied", "Clear"}, {"Open", "Closed"}};

bool same_word(std::string_view a, const char *b)
{
    for (const char c : a) {
        if (*b == 0 || std::tolower(static_cast<unsigned char>(c)) != std::tolower(static_cast<unsigned char>(*b++))) {
            return false;
        }
    }
    return *b == 0;
}

template <class T>
bool assign(T &to, const T &from)
{
    if (to == from) {
        return false;
    }
    to = from;
    return true;
}

// an unknown value (NaN) equals another unknown one
bool assign(float &to, float from)
{
    if (to == from || (std::isnan(to) && std::isnan(from))) {
        return false;
    }
    to = from;
    return true;
}


bool copy(EntityState &to, const EntityState &from, Field field)
{
    switch (field) {
    case Field::State:
        return assign(to.state, from.state);
    case Field::Rounded:
        return assign(to.rounded, from.rounded);
    case Field::Name:
        return assign(to.name, from.name);
    case Field::Icon:
        return assign(to.icon, from.icon);
    case Field::DeviceClass:
        return assign(to.device_class, from.device_class);
    case Field::Unit:
        return assign(to.unit, from.unit);
    case Field::Brightness:
        return assign(to.brightness, from.brightness);
    case Field::ColorModes:
        return assign(to.color_modes, from.color_modes);
    case Field::ColorMode:
        return assign(to.color_mode, from.color_mode);
    case Field::Effect:
        return assign(to.effect, from.effect);
    case Field::EffectList:
        return assign(to.effect_list, from.effect_list);
    case Field::ColorTemp:
        return assign(to.color_temp, from.color_temp);
    case Field::MinColorTemp:
        return assign(to.min_color_temp, from.min_color_temp);
    case Field::MaxColorTemp:
        return assign(to.max_color_temp, from.max_color_temp);
    case Field::HsColor:
        return assign(to.hue, from.hue) | assign(to.saturation, from.saturation);
    case Field::Target:
        return assign(to.target, from.target);
    case Field::Current:
        return assign(to.current, from.current);
    case Field::HvacAction:
        return assign(to.hvac_action, from.hvac_action);
    case Field::MinTemp:
        return assign(to.min_temp, from.min_temp);
    case Field::MaxTemp:
        return assign(to.max_temp, from.max_temp);
    case Field::TempStep:
        return assign(to.temp_step, from.temp_step);
    case Field::TargetLow:
        return assign(to.target_low, from.target_low);
    case Field::TargetHigh:
        return assign(to.target_high, from.target_high);
    case Field::HvacModes:
        return assign(to.hvac_modes, from.hvac_modes);
    case Field::PresetMode:
        return assign(to.preset_mode, from.preset_mode);
    case Field::PresetModes:
        return assign(to.preset_modes, from.preset_modes);
    case Field::FanMode:
        return assign(to.fan_mode, from.fan_mode);
    case Field::FanModes:
        return assign(to.fan_modes, from.fan_modes);
    case Field::Features:
        return assign(to.features, from.features);
    case Field::Temperature:
        return assign(to.temperature, from.temperature);
    case Field::TemperatureUnit:
        return assign(to.temperature_unit, from.temperature_unit);
    case Field::ApparentTemperature:
        return assign(to.apparent_temperature, from.apparent_temperature);
    case Field::Humidity:
        return assign(to.humidity, from.humidity);
    case Field::Pressure:
        return assign(to.pressure, from.pressure);
    case Field::PressureUnit:
        return assign(to.pressure_unit, from.pressure_unit);
    case Field::WindSpeed:
        return assign(to.wind_speed, from.wind_speed);
    case Field::WindSpeedUnit:
        return assign(to.wind_speed_unit, from.wind_speed_unit);
    case Field::WindGust:
        return assign(to.wind_gust, from.wind_gust);
    case Field::WindBearing:
        return assign(to.wind_bearing, from.wind_bearing);
    case Field::Visibility:
        return assign(to.visibility, from.visibility);
    case Field::VisibilityUnit:
        return assign(to.visibility_unit, from.visibility_unit);
    case Field::CloudCoverage:
        return assign(to.cloud_coverage, from.cloud_coverage);
    case Field::DewPoint:
        return assign(to.dew_point, from.dew_point);
    case Field::UvIndex:
        return assign(to.uv_index, from.uv_index);
    case Field::Duration:
        return assign(to.duration, from.duration);
    case Field::Remaining:
        return assign(to.remaining, from.remaining);
    case Field::FinishesAt:
        return assign(to.finishes_at, from.finishes_at);
    case Field::LastTriggered:
        return assign(to.last_triggered, from.last_triggered);
    case Field::Count:
        break;
    }
    return false;
}

} // namespace

EntityStore::EntityStore(size_t entities) : _states(entities)
{
}

bool EntityStore::apply(const EntityUpdate &update)
{
    if (update.entity >= _states.size()) {
        return false;
    }
    EntityState &state = _states[update.entity];
    bool changed = false;
    for (int i = 0; i < static_cast<int>(Field::Count); i++) {
        const auto field = static_cast<Field>(i);
        if ((update.fields & bit(field)) != 0) {
            changed |= copy(state, update.values, field);
        }
    }
    if (state.brightness >= 0) {
        state.last_brightness = state.brightness;
    }
    // the first value of a field is a change even when it equals the default
    changed |= (update.fields & ~state.known) != 0;
    state.known |= update.fields;
    return changed;
}

const OnOffWords *on_off_words(std::string_view state, bool *on)
{
    for (const OnOffWords &pair : kOnOffWords) {
        const bool is_on = same_word(state, pair.on);
        if (is_on || same_word(state, pair.off)) {
            if (on != nullptr) {
                *on = is_on;
            }
            return &pair;
        }
    }
    return nullptr;
}

bool EntityState::is_on() const
{
    bool on = false;
    return on_off_words(state, &on) != nullptr && on;
}

memory::String display_value(const EntityState &state, const Tile &tile)
{
    if (tile.decimals >= 0) {
        const char *begin = state.state.data();
        double value = 0;
        const auto [end, error] = std::from_chars(begin, begin + state.state.size(), value);
        if (error == std::errc() && end == begin + state.state.size() && !state.state.empty()) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.*f", tile.decimals, value);
            return text;
        }
    }
    if (state.has(Field::Rounded) && !state.rounded.empty()) {
        return state.rounded;
    }
    return state.state;
}

std::string_view display_unit(const EntityState &state, const Tile &tile)
{
    return tile.unit.empty() ? std::string_view(state.unit) : std::string_view(tile.unit);
}

float duration_seconds(std::string_view text)
{
    // "[D day[s], ]H:MM:SS[.ffffff]"
    float days = 0;
    const size_t comma = text.find(',');
    if (comma != std::string_view::npos) {
        const std::string_view head = text.substr(0, comma);
        const auto [end, error] = std::from_chars(head.data(), head.data() + head.size(), days);
        if (error != std::errc() || end == head.data()) {
            return kUnknown;
        }
        text = text.substr(comma + 1);
        while (!text.empty() && text.front() == ' ') {
            text.remove_prefix(1);
        }
    }
    float parts[3] = {0, 0, 0};
    int count = 0;
    while (count < 3) {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parts[count]);
        if (error != std::errc() || end == text.data()) {
            return kUnknown;
        }
        count++;
        text.remove_prefix(static_cast<size_t>(end - text.data()));
        if (text.empty()) {
            break;
        }
        if (text.front() != ':') {
            return kUnknown;
        }
        text.remove_prefix(1);
    }
    if (count != 3 || !text.empty()) {
        return kUnknown;
    }
    return days * 86400 + parts[0] * 3600 + parts[1] * 60 + parts[2];
}

memory::String duration_text(float seconds)
{
    const auto total = static_cast<long>(std::lround(std::fmax(seconds, 0.0f)));
    char text[24];
    std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", total / 3600, total / 60 % 60, total % 60);
    return text;
}

std::string_view display_name(const EntityState &state, const Tile &tile, std::string_view entity_id)
{
    if (!tile.name.empty()) {
        return tile.name;
    }
    if (!state.name.empty()) {
        return state.name;
    }
    return entity_id;
}

} // namespace hc::core
