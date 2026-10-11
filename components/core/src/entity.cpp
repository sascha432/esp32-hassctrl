#include "hc/core/entity.hpp"

#include <charconv>
#include <cstdio>

namespace hc::core {

namespace {

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
