#pragma once

// The live state of the entities of a layout and what the UI asks of them (docs/architecture.md
// section 5). Plain C++, host tested.
//
// The store belongs to the UI task: the hass task decodes the template results into
// EntityUpdates and hands them over, the UI applies them and redraws the tiles of the entities
// that changed. One EntityState per interned entity of the layout, so two tiles of one entity
// share it. The states live on the large heap; the flags the UI reads on every redraw stay in
// internal RAM.

#include <cmath>
#include <cstdint>
#include <string_view>
#include <variant>

#include "hc/core/image.hpp"
#include "hc/core/layout.hpp"
#include "hc/core/weather.hpp"
#include "hc/memory/large.hpp"

namespace hc::core {

// The values a tile can show. Which ones are subscribed follows from the tiles of the page
// (hass_protocol), so an entity only carries what its tiles need.
enum class Field : uint8_t {
    State,        // the state as Home Assistant has it ("on", "21.53", "unavailable")
    Rounded,      // the state rounded to the display precision set in Home Assistant
    Name,         // friendly_name
    Icon,         // the icon attribute ("mdi:lightbulb")
    DeviceClass,
    Unit,         // unit_of_measurement
    Brightness,   // 0 to 255
    ColorModes,   // supported_color_modes
    ColorMode,
    Effect,
    EffectList,
    ColorTemp,    // kelvin
    MinColorTemp,
    MaxColorTemp,
    HsColor,      // hue and saturation
    Target,       // the target temperature
    Current,      // the current temperature
    HvacAction,
    MinTemp,
    MaxTemp,
    TempStep,     // target_temp_step, only there when the integration sets it
    HvacModes,
    PresetMode,
    PresetModes,
    FanMode,
    FanModes,
    Features,     // supported_features
    // a weather entity (the state is its condition)
    Temperature,  // temperature: the current one (a climate's `temperature` is its setpoint, Target)
    TemperatureUnit,
    ApparentTemperature,
    Humidity,
    Pressure,
    PressureUnit,
    WindSpeed,
    WindSpeedUnit,
    WindGust,
    WindBearing,  // degrees
    Visibility,
    VisibilityUnit,
    CloudCoverage,
    DewPoint,
    UvIndex,
    // a timer entity (the state is active, paused or idle)
    Duration,     // the duration it runs for, seconds ("0:30:00" in Home Assistant)
    Remaining,    // the seconds left when it was paused (or started: counts down from there)
    FinishesAt,   // the epoch second it ends at while it runs (an integer)
    // an automation or a script
    LastTriggered, // the epoch second of its last run (an integer; 0: never)
    Count,
};

using FieldMask = uint64_t;
static_assert(static_cast<int>(Field::Count) <= 64);

constexpr FieldMask bit(Field field)
{
    return FieldMask{1} << static_cast<int>(field);
}

constexpr float kUnknown = NAN;

struct EntityState {
    FieldMask known = 0; // the fields a result has carried
    memory::String state;
    memory::String rounded;
    memory::String name;
    memory::String icon;
    memory::String device_class;
    memory::String unit;
    int16_t brightness = -1; // -1: off or unknown
    // the last brightness reported while on, kept while the light is off: HA turns it on at that
    // level, so a pending "on" shows it (-1: none seen yet). Set by EntityStore::apply().
    int16_t last_brightness = -1;
    memory::Vector<memory::String> color_modes;
    memory::String color_mode;
    memory::String effect;
    memory::Vector<memory::String> effect_list;
    float color_temp = kUnknown;
    float min_color_temp = kUnknown;
    float max_color_temp = kUnknown;
    float hue = kUnknown;
    float saturation = kUnknown;
    float target = kUnknown;
    float current = kUnknown;
    memory::String hvac_action;
    float min_temp = kUnknown;
    float max_temp = kUnknown;
    float temp_step = kUnknown;
    memory::Vector<memory::String> hvac_modes;
    memory::String preset_mode;
    memory::Vector<memory::String> preset_modes;
    memory::String fan_mode;
    memory::Vector<memory::String> fan_modes;
    int32_t features = 0; // supported_features, 0 until it arrived
    // a weather entity, in its units
    float temperature = kUnknown;
    memory::String temperature_unit;
    float apparent_temperature = kUnknown;
    float humidity = kUnknown; // percent
    float pressure = kUnknown;
    memory::String pressure_unit;
    float wind_speed = kUnknown;
    memory::String wind_speed_unit;
    float wind_gust = kUnknown;
    float wind_bearing = kUnknown;
    float visibility = kUnknown;
    memory::String visibility_unit;
    float cloud_coverage = kUnknown; // percent
    float dew_point = kUnknown;
    float uv_index = kUnknown;
    // a timer entity: seconds (duration, remaining) and the epoch second it ends at while it
    // runs (0: not running or unknown; whole seconds, like Backend::clock)
    float duration = kUnknown;
    float remaining = kUnknown;
    int64_t finishes_at = 0;
    // an automation or a script: the epoch second of its last run, 0 while it never ran
    int64_t last_triggered = 0;

    bool has(Field field) const
    {
        return (known & bit(field)) != 0;
    }
    // "unavailable" or "unknown": the entity exists but has no state, or it does not exist
    bool unavailable() const
    {
        return state == "unavailable" || state == "unknown";
    }
    // the state is the "on" word of a pair (on_off_words): on, Detected, Occupied, Open
    bool is_on() const;
};

// A state that is one of two words, in any case: on/off, Detected/Clear (the motion helper
// input_text.<device>_motion), Occupied/Clear, Open/Closed. The pair's words as shown in a
// legend, nullptr for any other state; `on` is set to which of the two it is.
struct OnOffWords {
    const char *on;
    const char *off;
};
const OnOffWords *on_off_words(std::string_view state, bool *on = nullptr);

// supported_features of a lock: it can open (unlatch) the door (LockEntityFeature.OPEN)
constexpr int32_t kLockOpen = 1;
// supported_features of a weather entity: the forecasts it has (WeatherEntityFeature)
constexpr int32_t kForecastDaily = 1;
constexpr int32_t kForecastHourly = 2;

// The values of `fields` for one entity, from one template result.
struct EntityUpdate {
    EntityIndex entity = kNone;
    FieldMask fields = 0;
    EntityState values;
};

// The values of the subscribed page, from one template result. `generation` is the one of the
// subscription it belongs to; the UI drops the results of an older one (docs/architecture.md
// section 4, "Stale results").
struct PageValues {
    uint32_t generation = 0;
    memory::Vector<EntityUpdate> updates;
};

// A service call was refused or failed.
struct ServiceFailed {
    EntityIndex entity = kNone; // of the intent
    memory::String message;
};

// What the history graph of a sensor panel shows (docs/ui/lvgl/lvgl-panels.md).
enum class HistoryKind : uint8_t {
    Statistics,  // the 5 minute means of the long term statistics (a sensor with a state_class)
    States,      // the state changes of an on/off entity (a binary sensor has no statistics)
    Temperature, // the current temperature of a climate from its state history (no statistics either)
};

// Statistics: the start of a 5 minute bucket and its mean. States: the time of a change and the
// state from then on: 1 on (a lock: unlocked or open), 0 off (locked), kProblem (a jammed lock),
// NaN unknown or unavailable. Temperature: the time of a change and the current temperature from
// then on (NaN while the climate had none).
constexpr float kProblem = 2;

struct HistoryPoint {
    uint32_t time = 0; // epoch seconds
    float value = kUnknown;
};

// The history of one entity over [start, end), in time order. `generation` is the one of the
// request; the UI drops the answers of an older one.
struct History {
    uint32_t generation = 0;
    EntityIndex entity = kNone;
    HistoryKind kind = HistoryKind::Statistics;
    uint32_t start = 0; // epoch seconds
    uint32_t end = 0;
    memory::Vector<HistoryPoint> points;
    memory::String error; // empty: the request worked (the points may still be empty)
};

// What the weather panel shows (docs/ui/lvgl/lvgl-panels.md): a day, or an hour.
enum class ForecastType : uint8_t {
    None,   // no forecast wanted (ends the subscription)
    Daily,
    Hourly,
};

struct ForecastEntry {
    uint32_t time = 0;           // epoch seconds: the start of the day or the hour
    Condition condition = Condition::Unknown;
    float temperature = kUnknown; // the day's high, or the hour's temperature
    float templow = kUnknown;     // the day's low
    int8_t precipitation_probability = -1; // percent, -1 unknown
};

// the entries kept of an answer: 8 days, or the next 24 hours
constexpr size_t kMaxForecastEntries = 24;

// The forecast of one entity, from its subscription: again with every update of the integration.
// `generation` is the one of the subscription; the UI drops the answers of an older one.
struct Forecast {
    uint32_t generation = 0;
    EntityIndex entity = kNone;
    ForecastType type = ForecastType::None;
    memory::Vector<ForecastEntry> entries;
    memory::String error; // empty: it worked (the entries may still be empty)
};

// The entity search of the screen's edit mode (docs/ui/lvgl/lvgl-dashboard.md section 7): the
// entities whose id or friendly name has every word of the search, in order of the id.
struct EntityRow {
    memory::String id;
    memory::String name; // friendly_name, up to 40 characters
};

// `generation` is the one of the search; the UI drops the answers of an older one.
struct EntityList {
    uint32_t generation = 0;
    bool more = false; // rows were left out
    memory::Vector<EntityRow> rows;
    memory::String error; // empty: it worked (the rows may still be empty)
};

// What a new tile needs of its entity.
struct EntityDetails {
    uint32_t generation = 0;
    memory::String id;
    bool missing = false; // Home Assistant has no such entity
    bool dimmer = false;  // a light whose supported_color_modes has more than onoff
    memory::String error;
};

// What the connection hands to the UI.
using HassEvent = std::variant<PageValues, ServiceFailed, History, Image, Forecast, EntityList, EntityDetails>;

class EntityStore {
public:
    explicit EntityStore(size_t entities);

    size_t size() const
    {
        return _states.size();
    }
    const EntityState &state(EntityIndex entity) const
    {
        return _states[entity];
    }
    // room for `entities` (a tile was added while editing); the values of the others stay
    void grow(size_t entities)
    {
        if (entities > _states.size()) {
            _states.resize(entities);
        }
    }

    // Copies the fields of `update` into its entity. True when a value differs from before, so
    // the tiles of the entity have to be drawn again.
    bool apply(const EntityUpdate &update);

    // The connection is gone: the values stay, but the UI shows them as old.
    void set_stale(bool stale)
    {
        _stale = stale;
    }
    bool stale() const
    {
        return _stale;
    }

private:
    memory::Vector<EntityState> _states;
    bool _stale = true;
};

// What a gesture asks for (docs/architecture.md section 5, "Intents"). The meaning of a gesture
// is decided in ui, the service call that does it in hass_protocol.
enum class IntentKind : uint8_t {
    Toggle,         // switch, light, input_boolean, fan, automation (enables or disables it)
    Press,          // button, input_button; a scene is turned on
    SetLevel,       // light, value = brightness in percent (0 turns it off)
    SetTemperature, // climate, value = degrees
    SetHvacMode,    // climate, text
    SetPreset,      // climate, text
    SetFanMode,     // climate, text
    SetEffect,      // light, text
    SetColor,       // light, value = hue (0 to 360), value2 = saturation (0 to 100)
    SetColorTemp,   // light, value = kelvin
    SetLock,        // lock, value = kLock, kUnlock or kOpen
    SetTimer,       // timer, value = kTimerStart (value2 = the duration in seconds, 0: the entity's), kTimerPause, kTimerCancel or kTimerFinish
    Run,            // automation (its actions, conditions skipped), script; value = kRun or kStop (a running script)
};

// the values of IntentKind::SetLock
constexpr float kLock = 0;
constexpr float kUnlock = 1;
constexpr float kOpen = 2;
// the values of IntentKind::SetTimer
constexpr float kTimerStart = 0;
constexpr float kTimerPause = 1;
constexpr float kTimerCancel = 2;
constexpr float kTimerFinish = 3;
// the values of IntentKind::Run
constexpr float kRun = 0;
constexpr float kStop = 1;

// The seconds of a Home Assistant duration ("0:30:00", "1 day, 2:00:00", "0:00:05.500000");
// NaN for anything else.
float duration_seconds(std::string_view text);
// "H:MM:SS" of the seconds, as Home Assistant writes a duration
memory::String duration_text(float seconds);

struct Intent {
    IntentKind kind = IntentKind::Toggle;
    EntityIndex entity = kNone;
    float value = 0;
    float value2 = 0;
    memory::String text;
};

// The text a sensor tile shows for its state, without the unit: the state with the tile's
// `decimals` when it is a number, else the value rounded by Home Assistant, else the state.
memory::String display_value(const EntityState &state, const Tile &tile);

// The unit a sensor tile shows: the tile's own, else the one Home Assistant reports.
std::string_view display_unit(const EntityState &state, const Tile &tile);

// The name a tile shows: its own, else the entity's friendly_name, else the entity id.
std::string_view display_name(const EntityState &state, const Tile &tile, std::string_view entity_id);

} // namespace hc::core
