// The template of the visible page and the decoding of its results.

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>

#include "cJSON.h"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass_protocol {

using core::Field;
using core::FieldMask;
using core::TileKind;
using core::bit;

namespace {

enum class Kind : uint8_t {
    Text,   // a string; null is an empty one
    Number, // a number or a string of one; null is unknown
    List,   // a list of strings
    Pair,   // hs_color: two numbers
};

struct FieldSpec {
    Field field;
    const char *code;      // the key in the result
    const char *attribute; // nullptr: the state itself
    Kind kind;
};

// The one table of the fields (docs/architecture.md section 5, "Tile type descriptors").
constexpr FieldSpec kFields[] = {
    {Field::State, "s", nullptr, Kind::Text},
    {Field::Rounded, "r", nullptr, Kind::Text},
    {Field::Name, "n", "friendly_name", Kind::Text},
    {Field::Icon, "i", "icon", Kind::Text},
    {Field::DeviceClass, "d", "device_class", Kind::Text},
    {Field::Unit, "u", "unit_of_measurement", Kind::Text},
    {Field::Brightness, "b", "brightness", Kind::Number},
    {Field::ColorModes, "cs", "supported_color_modes", Kind::List},
    {Field::ColorMode, "cm", "color_mode", Kind::Text},
    {Field::Effect, "e", "effect", Kind::Text},
    {Field::EffectList, "el", "effect_list", Kind::List},
    {Field::ColorTemp, "k", "color_temp_kelvin", Kind::Number},
    {Field::MinColorTemp, "kl", "min_color_temp_kelvin", Kind::Number},
    {Field::MaxColorTemp, "kh", "max_color_temp_kelvin", Kind::Number},
    {Field::HsColor, "h", "hs_color", Kind::Pair},
    {Field::Target, "t", "temperature", Kind::Number},
    {Field::Current, "c", "current_temperature", Kind::Number},
    {Field::HvacAction, "a", "hvac_action", Kind::Text},
    {Field::MinTemp, "tl", "min_temp", Kind::Number},
    {Field::MaxTemp, "th", "max_temp", Kind::Number},
    {Field::TempStep, "ts", "target_temp_step", Kind::Number},
    {Field::HvacModes, "hm", "hvac_modes", Kind::List},
    {Field::PresetMode, "p", "preset_mode", Kind::Text},
    {Field::PresetModes, "pl", "preset_modes", Kind::List},
    {Field::FanMode, "f", "fan_mode", Kind::Text},
    {Field::FanModes, "fl", "fan_modes", Kind::List},
    {Field::Features, "sf", "supported_features", Kind::Number},
    {Field::Temperature, "wt", "temperature", Kind::Number},
    {Field::TemperatureUnit, "wtu", "temperature_unit", Kind::Text},
    {Field::ApparentTemperature, "wa", "apparent_temperature", Kind::Number},
    {Field::Humidity, "wh", "humidity", Kind::Number},
    {Field::Pressure, "wp", "pressure", Kind::Number},
    {Field::PressureUnit, "wpu", "pressure_unit", Kind::Text},
    {Field::WindSpeed, "ws", "wind_speed", Kind::Number},
    {Field::WindSpeedUnit, "wsu", "wind_speed_unit", Kind::Text},
    {Field::WindGust, "wg", "wind_gust_speed", Kind::Number},
    {Field::WindBearing, "wb", "wind_bearing", Kind::Number},
    {Field::Visibility, "wv", "visibility", Kind::Number},
    {Field::VisibilityUnit, "wvu", "visibility_unit", Kind::Text},
    {Field::CloudCoverage, "wc", "cloud_coverage", Kind::Number},
    {Field::DewPoint, "wd", "dew_point", Kind::Number},
    {Field::UvIndex, "wi", "uv_index", Kind::Number},
};
static_assert(std::size(kFields) == static_cast<size_t>(Field::Count));

constexpr bool table_in_field_order()
{
    for (size_t i = 0; i < std::size(kFields); i++) {
        if (static_cast<size_t>(kFields[i].field) != i) {
            return false;
        }
    }
    return true;
}
static_assert(table_in_field_order());

constexpr FieldMask kDimmer = bit(Field::Brightness) | bit(Field::ColorModes);
constexpr FieldMask kLightPanel = bit(Field::Brightness) | bit(Field::ColorModes) | bit(Field::ColorMode) |
                                  bit(Field::Effect) | bit(Field::EffectList) | bit(Field::ColorTemp) |
                                  bit(Field::MinColorTemp) | bit(Field::MaxColorTemp) | bit(Field::HsColor);
constexpr FieldMask kClimate = bit(Field::Target) | bit(Field::Current) | bit(Field::HvacAction) |
                               bit(Field::MinTemp) | bit(Field::MaxTemp) | bit(Field::TempStep);
constexpr FieldMask kClimatePanel = bit(Field::HvacModes) | bit(Field::PresetMode) | bit(Field::PresetModes) |
                                    bit(Field::FanMode) | bit(Field::FanModes);
constexpr FieldMask kWeather = bit(Field::Temperature) | bit(Field::TemperatureUnit) | bit(Field::Humidity);
// the details, and the forecasts the entity has (supported_features)
constexpr FieldMask kWeatherPanel = bit(Field::ApparentTemperature) | bit(Field::Pressure) | bit(Field::PressureUnit) |
                                    bit(Field::WindSpeed) | bit(Field::WindSpeedUnit) | bit(Field::WindGust) |
                                    bit(Field::WindBearing) | bit(Field::Visibility) | bit(Field::VisibilityUnit) |
                                    bit(Field::CloudCoverage) | bit(Field::DewPoint) | bit(Field::UvIndex) |
                                    bit(Field::Features);

void append(memory::String &out, std::string_view text)
{
    out.append(text.data(), text.size());
}

memory::String number_text(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.10g", value);
    return text;
}

memory::String text_of(const cJSON *item)
{
    if (cJSON_IsString(item)) {
        return item->valuestring;
    }
    if (cJSON_IsNumber(item)) {
        return number_text(item->valuedouble);
    }
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? "true" : "false";
    }
    return {};
}

float number_of(const cJSON *item)
{
    if (cJSON_IsNumber(item)) {
        return static_cast<float>(item->valuedouble);
    }
    if (cJSON_IsString(item)) {
        const char *begin = item->valuestring;
        const char *end = begin + std::strlen(begin);
        float value = 0;
        const auto [stop, error] = std::from_chars(begin, end, value);
        if (error == std::errc() && stop == end && begin != end) {
            return value;
        }
    }
    return core::kUnknown;
}

memory::Vector<memory::String> list_of(const cJSON *item)
{
    memory::Vector<memory::String> list;
    if (cJSON_IsArray(item)) {
        const cJSON *entry = nullptr;
        cJSON_ArrayForEach(entry, item)
        {
            list.push_back(text_of(entry));
        }
    }
    return list;
}

void decode_field(const FieldSpec &spec, const cJSON *item, core::EntityState &to)
{
    switch (spec.field) {
    case Field::State:
        to.state = text_of(item);
        break;
    case Field::Rounded:
        to.rounded = text_of(item);
        break;
    case Field::Name:
        to.name = text_of(item);
        break;
    case Field::Icon:
        to.icon = text_of(item);
        break;
    case Field::DeviceClass:
        to.device_class = text_of(item);
        break;
    case Field::Unit:
        to.unit = text_of(item);
        break;
    case Field::Brightness: {
        const float value = number_of(item);
        to.brightness = std::isnan(value) ? int16_t{-1} : static_cast<int16_t>(value);
        break;
    }
    case Field::ColorModes:
        to.color_modes = list_of(item);
        break;
    case Field::ColorMode:
        to.color_mode = text_of(item);
        break;
    case Field::Effect:
        to.effect = text_of(item);
        break;
    case Field::EffectList:
        to.effect_list = list_of(item);
        break;
    case Field::ColorTemp:
        to.color_temp = number_of(item);
        break;
    case Field::MinColorTemp:
        to.min_color_temp = number_of(item);
        break;
    case Field::MaxColorTemp:
        to.max_color_temp = number_of(item);
        break;
    case Field::HsColor:
        to.hue = cJSON_GetArraySize(item) == 2 ? number_of(cJSON_GetArrayItem(item, 0)) : core::kUnknown;
        to.saturation = cJSON_GetArraySize(item) == 2 ? number_of(cJSON_GetArrayItem(item, 1)) : core::kUnknown;
        break;
    case Field::Target:
        to.target = number_of(item);
        break;
    case Field::Current:
        to.current = number_of(item);
        break;
    case Field::HvacAction:
        to.hvac_action = text_of(item);
        break;
    case Field::MinTemp:
        to.min_temp = number_of(item);
        break;
    case Field::MaxTemp:
        to.max_temp = number_of(item);
        break;
    case Field::TempStep:
        to.temp_step = number_of(item);
        break;
    case Field::HvacModes:
        to.hvac_modes = list_of(item);
        break;
    case Field::PresetMode:
        to.preset_mode = text_of(item);
        break;
    case Field::PresetModes:
        to.preset_modes = list_of(item);
        break;
    case Field::FanMode:
        to.fan_mode = text_of(item);
        break;
    case Field::FanModes:
        to.fan_modes = list_of(item);
        break;
    case Field::Features: {
        const float value = number_of(item);
        to.features = std::isnan(value) ? 0 : static_cast<int32_t>(value);
        break;
    }
    case Field::Temperature:
        to.temperature = number_of(item);
        break;
    case Field::TemperatureUnit:
        to.temperature_unit = text_of(item);
        break;
    case Field::ApparentTemperature:
        to.apparent_temperature = number_of(item);
        break;
    case Field::Humidity:
        to.humidity = number_of(item);
        break;
    case Field::Pressure:
        to.pressure = number_of(item);
        break;
    case Field::PressureUnit:
        to.pressure_unit = text_of(item);
        break;
    case Field::WindSpeed:
        to.wind_speed = number_of(item);
        break;
    case Field::WindSpeedUnit:
        to.wind_speed_unit = text_of(item);
        break;
    case Field::WindGust:
        to.wind_gust = number_of(item);
        break;
    case Field::WindBearing:
        to.wind_bearing = number_of(item);
        break;
    case Field::Visibility:
        to.visibility = number_of(item);
        break;
    case Field::VisibilityUnit:
        to.visibility_unit = text_of(item);
        break;
    case Field::CloudCoverage:
        to.cloud_coverage = number_of(item);
        break;
    case Field::DewPoint:
        to.dew_point = number_of(item);
        break;
    case Field::UvIndex:
        to.uv_index = number_of(item);
        break;
    case Field::Count:
        break;
    }
}

struct JsonDeleter {
    void operator()(cJSON *json) const
    {
        cJSON_Delete(json);
    }
};

} // namespace

FieldMask fields_of(const core::Tile &tile, bool panel)
{
    if (tile.entity == core::kNone) {
        return 0;
    }
    FieldMask fields = bit(Field::State);
    if (tile.name.empty()) {
        fields |= bit(Field::Name);
    }
    if (tile.icon.empty()) {
        // the automatic icon: the icon attribute, else domain and device class
        fields |= bit(Field::Icon) | bit(Field::DeviceClass);
    }
    switch (tile.kind) {
    case TileKind::Sensor:
        // a binary sensor shows its state by device class ("open", "detected")
        fields |= bit(Field::DeviceClass);
        if (tile.unit.empty()) {
            fields |= bit(Field::Unit);
        }
        if (tile.decimals < 0) {
            fields |= bit(Field::Rounded);
        }
        break;
    case TileKind::Light:
        if (tile.dimmer) {
            fields |= kDimmer;
        }
        if (panel) {
            fields |= kLightPanel;
        }
        break;
    case TileKind::Climate:
        fields |= kClimate;
        if (panel) {
            fields |= kClimatePanel;
        }
        break;
    case TileKind::Lock:
        // the open button
        if (panel) {
            fields |= bit(Field::Features);
        }
        break;
    case TileKind::Weather:
        fields |= kWeather;
        if (panel) {
            fields |= kWeatherPanel;
        }
        break;
    default:
        break;
    }
    return fields;
}

PageSubscription build_page(const core::Layout &layout, core::PageIndex page, core::TileIndex panel)
{
    PageSubscription sub;
    if (page >= layout.pages.size()) {
        return sub;
    }
    // the entities in the order of their first tile; two tiles of one entity share an entry
    for (const core::TileIndex index : layout.pages[page].tiles) {
        const core::Tile &tile = layout.tiles[index];
        const FieldMask fields = fields_of(tile, index == panel);
        if (fields == 0) {
            continue;
        }
        size_t i = 0;
        while (i < sub.entities.size() && sub.entities[i] != tile.entity) {
            i++;
        }
        if (i == sub.entities.size()) {
            sub.entities.push_back(tile.entity);
            sub.fields.push_back(0);
        }
        sub.fields[i] |= fields;
    }
    if (sub.entities.empty()) {
        return sub;
    }

    // "0":{%set e='light.a'%}{"s":{{states(e)|tojson}},"b":{{state_attr(e,'brightness')|tojson}}}
    // The `set` never follows a `{`: Jinja would read "{{%" as the start of an expression. An
    // entity id has only [a-z0-9_.], so it needs no escaping in the quotes of Jinja.
    memory::String &out = sub.template_text;
    out.reserve(sub.entities.size() * 160);
    out += '{';
    for (size_t i = 0; i < sub.entities.size(); i++) {
        if (i > 0) {
            out += ',';
        }
        out += '"';
        append(out, std::to_string(i));
        out += "\":{%set e='";
        append(out, layout.entities[sub.entities[i]]);
        out += "'%}{";
        bool first = true;
        for (const FieldSpec &spec : kFields) {
            if ((sub.fields[i] & bit(spec.field)) == 0) {
                continue;
            }
            if (!first) {
                out += ',';
            }
            first = false;
            out += '"';
            out += spec.code;
            out += "\":{{";
            if (spec.field == Field::State) {
                out += "states(e)";
            } else if (spec.field == Field::Rounded) {
                out += "states(e,rounded=True)";
            } else {
                out += "state_attr(e,'";
                out += spec.attribute;
                out += "')";
            }
            out += "|tojson}}";
        }
        out += '}';
    }
    out += '}';
    return sub;
}

bool decode_page(const cJSON *result, const PageSubscription &subscription, memory::Vector<core::EntityUpdate> &out,
                 memory::String &error)
{
    out.clear();
    // Home Assistant parses a result that looks like JSON; older versions send the text
    std::unique_ptr<cJSON, JsonDeleter> parsed;
    if (cJSON_IsString(result)) {
        parsed.reset(cJSON_Parse(result->valuestring));
        result = parsed.get();
    }
    if (!cJSON_IsObject(result)) {
        error = "the template result is not an object";
        return false;
    }
    char key[12];
    for (size_t i = 0; i < subscription.entities.size(); i++) {
        std::snprintf(key, sizeof(key), "%u", static_cast<unsigned>(i));
        const cJSON *values = cJSON_GetObjectItemCaseSensitive(result, key);
        if (!cJSON_IsObject(values)) {
            continue;
        }
        core::EntityUpdate update;
        update.entity = subscription.entities[i];
        for (const FieldSpec &spec : kFields) {
            if ((subscription.fields[i] & bit(spec.field)) == 0) {
                continue;
            }
            const cJSON *item = cJSON_GetObjectItemCaseSensitive(values, spec.code);
            if (item == nullptr) {
                continue;
            }
            decode_field(spec, item, update.values);
            update.fields |= bit(spec.field);
        }
        out.push_back(std::move(update));
    }
    return true;
}

} // namespace hc::hass_protocol
