#include "icons.hpp"

#include <algorithm>
#include <iterator>

#include "hc/core/weather.hpp"

namespace hc::ui::icons {

namespace {

struct Glyph {
    std::string_view name;
    const char *utf8;
};

constexpr Glyph kGlyphs[] = {
#include "icon_glyphs.inc"
};

// the default of a domain and device class, by the HA frontend as far as the glyphs reach:
// {on, off}
struct Pair {
    const char *on;
    const char *off;
};

Pair binary_sensor(std::string_view device_class)
{
    if (device_class == "motion" || device_class == "occupancy" || device_class == "presence") {
        return {"motion-sensor", "motion-sensor-off"};
    }
    if (device_class == "door" || device_class == "garage_door") {
        return {"door-open", "door-closed"};
    }
    if (device_class == "window" || device_class == "opening") {
        return {"window-open", "window-closed"};
    }
    if (device_class == "plug" || device_class == "power") {
        return {"power-plug", "power-plug-off"};
    }
    return {"checkbox-marked-circle", "radiobox-blank"};
}

const char *sensor(std::string_view device_class)
{
    struct Entry {
        std::string_view device_class;
        const char *icon;
    };
    static constexpr Entry kSensors[] = {
        {"temperature", "thermometer"}, {"humidity", "water-percent"},   {"power", "flash"},
        {"energy", "lightning-bolt"},   {"carbon_dioxide", "molecule-co2"}, {"current", "current-ac"},
        {"voltage", "sine-wave"},       {"pressure", "gauge"},           {"atmospheric_pressure", "gauge"},
        {"illuminance", "brightness-6"},
    };
    for (const Entry &entry : kSensors) {
        if (entry.device_class == device_class) {
            return entry.icon;
        }
    }
    return "eye";
}

const char *by_domain(std::string_view domain, const core::EntityState &state, bool on)
{
    const auto pick = [on](Pair pair) { return on ? pair.on : pair.off; };
    const std::string_view device_class = state.device_class;
    if (domain == "light") {
        return pick({"lightbulb", "lightbulb-off"});
    }
    if (domain == "switch") {
        return device_class == "outlet" ? pick({"power-plug", "power-plug-off"})
                                        : pick({"toggle-switch", "toggle-switch-off"});
    }
    if (domain == "input_boolean") {
        return pick({"toggle-switch", "toggle-switch-off"});
    }
    if (domain == "fan") {
        return pick({"fan", "fan-off"});
    }
    if (domain == "automation") {
        return pick({"robot", "robot-off"});
    }
    if (domain == "button" || domain == "input_button") {
        return "gesture-tap-button";
    }
    if (domain == "scene") {
        return "palette";
    }
    if (domain == "script") {
        return "script-text";
    }
    if (domain == "climate") {
        return "thermostat";
    }
    if (domain == "camera") {
        return "camera";
    }
    if (domain == "lock") {
        const std::string_view s = state.state;
        return s == "jammed" ? "lock-alert" : s == "locked" || s == "locking" ? "lock" : "lock-open";
    }
    if (domain == "binary_sensor") {
        return pick(binary_sensor(device_class));
    }
    if (domain == "sensor") {
        return sensor(device_class);
    }
    if (domain == "weather") {
        return core::condition_icon(core::condition_of(state.state));
    }
    if (domain == "timer") {
        // as the HA frontend: running, paused, idle
        const std::string_view s = state.state;
        return s == "active" ? "timer-outline" : s == "paused" ? "timer-pause-outline" : "timer-off-outline";
    }
    return "home-assistant";
}

} // namespace

const char *glyph(std::string_view name)
{
    const auto *end = std::end(kGlyphs);
    const auto *found = std::lower_bound(std::begin(kGlyphs), end, name,
                                         [](const Glyph &glyph, std::string_view key) { return glyph.name < key; });
    return found != end && found->name == name ? found->utf8 : nullptr;
}

const char *for_tile(const core::Tile &tile, std::string_view entity_id, const core::EntityState &state, bool on)
{
    if (tile.icon == "none") {
        return nullptr;
    }
    if (!tile.icon.empty()) {
        return glyph(tile.icon);
    }
    if (tile.kind == core::TileKind::Area) {
        return glyph("folder");
    }
    if (tile.kind == core::TileKind::Media) {
        return glyph("music");
    }
    if (tile.kind == core::TileKind::Clock) {
        return nullptr; // the time is the picture
    }
    constexpr std::string_view kPrefix = "mdi:";
    const std::string_view attribute = state.icon;
    if (attribute.starts_with(kPrefix)) {
        if (const char *found = glyph(attribute.substr(kPrefix.size()))) {
            return found;
        }
    }
    return glyph(by_domain(entity_id.substr(0, entity_id.find('.')), state, on));
}

} // namespace hc::ui::icons
