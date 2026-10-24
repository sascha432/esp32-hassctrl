#include "hc/sample/sample_hass.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

#include "hc/config/dashboard.hpp"
#include "lvgl.h"
#include "hc/sample/stub_backend.hpp"

namespace hc::sample {

namespace {

using hc::core::EntityIndex;
using hc::core::EntityState;
using hc::core::Field;

constexpr uint32_t kValuesMs = 150;
uint32_t g_action_ms = 400;
constexpr uint32_t kHistoryMs = 300;
constexpr uint32_t kForecastMs = 300;
constexpr uint32_t kImageMs = 500;
constexpr uint32_t kEntityQueryMs = 300;
// a stream plays 5 frames a second
constexpr uint32_t kStreamFrameMs = 200;
// the first frame this long after the connection, like a go2rtc that first starts ffmpeg
constexpr uint32_t kStreamStartMs = 4000;
// a stream's server takes the connection this long after the request
constexpr uint32_t kAnswerMs = 300;
// a server that is down answers nothing: the device's connect gives up after this (media.cpp)
constexpr uint32_t kConnectTimeoutMs = 5000;

// every field of the store: the samples carry what their domain has
constexpr hc::core::FieldMask kAllFields = (hc::core::FieldMask{1} << static_cast<int>(Field::Count)) - 1;

struct Delivery {
    uint32_t at;
    hc::core::HassEvent event;
};

struct State {
    std::shared_ptr<const hc::core::Layout> layout;
    std::vector<EntityState> samples;
    // the layout of the subscription: the dashboard, or the copy of the edit mode (same entities)
    std::shared_ptr<const hc::core::Layout> shown;
    hc::core::PageIndex page = hc::core::kNone;
    uint32_t generation = 0;
    bool was_connected = false;
    std::deque<Delivery> queue;

    // the stream that plays; generation 0: none
    struct Play {
        hc::core::EntityIndex entity = hc::core::kNone;
        uint16_t width = 0;
        uint16_t height = 0;
        uint32_t generation = 0;
        uint32_t next = 0; // lv_tick_get() of the next frame
        uint32_t frame = 0;
    } play;
    bool stream_server_down = false;
    // the stream's server wants a login (answers 401): `--do "stream login"`
    bool stream_login = false;
    ForecastMode forecast_mode = ForecastMode::Ok;
};

State g;

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

hc::memory::Vector<hc::memory::String> list(std::initializer_list<const char *> items)
{
    hc::memory::Vector<hc::memory::String> out;
    for (const char *item : items) {
        out.emplace_back(item);
    }
    return out;
}

// "aurora_ceiling" -> "Aurora Ceiling"
hc::memory::String friendly(std::string_view object_id)
{
    hc::memory::String name;
    bool upper = true;
    for (const char c : object_id) {
        if (c == '_') {
            name += ' ';
            upper = true;
        } else {
            name += upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            upper = false;
        }
    }
    return name;
}

void sensor(EntityState &s, std::string_view id, size_t i)
{
    struct Kind {
        const char *part;
        const char *device_class;
        const char *unit;
        float base;
        int decimals;
    };
    static constexpr Kind kKinds[] = {
        {"temperature", "temperature", "\xc2\xb0" "C", 21.37f, 1},
        {"humidity", "humidity", "%", 48.2f, 0},
        {"power", "power", "W", 1234.5f, 0},
        {"energy", "energy", "kWh", 8.43f, 2},
        {"co2", "carbon_dioxide", "ppm", 612, 0},
        {"pressure", "pressure", "hPa", 1018.2f, 1},
        {"voltage", "voltage", "V", 229.6f, 0},
    };
    for (const Kind &kind : kKinds) {
        if (contains(id, kind.part)) {
            const float value = kind.base + static_cast<float>(i % 5);
            char text[24];
            std::snprintf(text, sizeof(text), "%.3f", static_cast<double>(value));
            s.state = text;
            std::snprintf(text, sizeof(text), "%.*f", kind.decimals, static_cast<double>(value));
            s.rounded = text;
            s.device_class = kind.device_class;
            s.unit = kind.unit;
            return;
        }
    }
    s.state = "42";
    s.rounded = "42";
}

EntityState make_sample(std::string_view id, size_t i)
{
    EntityState s;
    const std::string_view domain = id.substr(0, id.find('.'));
    s.name = friendly(id.substr(id.find('.') + 1));
    if (domain == "light") {
        s.state = i % 3 == 2 ? "off" : "on";
        s.brightness = s.state == "on" ? static_cast<int16_t>(60 + (i * 53) % 190) : -1;
        s.last_brightness = s.brightness >= 0 ? s.brightness : int16_t{84};
        s.color_modes = list({"hs", "color_temp"});
        s.min_color_temp = 2000;
        s.max_color_temp = 6500;
        // every other light in a color, the others white
        if (i % 2 == 0) {
            s.color_mode = "hs";
            s.hue = static_cast<float>((i * 47) % 360);
            s.saturation = 80;
        } else {
            s.color_mode = "color_temp";
            s.color_temp = 3000;
            s.hue = 30;
            s.saturation = 45;
        }
        // as many as a WLED strip has a part of, so the list scrolls
        s.effect = "Solid";
        s.effect_list = list({"Solid", "Blink", "Breathe", "Wipe", "Wipe Random", "Random Colors", "Sweep", "Dynamic",
                              "Colorloop", "Rainbow", "Scan", "Dual Scan", "Fade", "Theater", "Running", "Saw",
                              "Twinkle", "Dissolve", "Sparkle", "Strobe", "Fire 2012", "Candle", "Fireworks", "Rain"});
    } else if (domain == "switch" || domain == "input_boolean" || domain == "fan") {
        s.state = i % 2 == 0 ? "on" : "off";
        if (contains(id, "plug") || contains(id, "outlet")) {
            s.device_class = "outlet";
        }
    } else if (domain == "automation") {
        // enabled or not, ran 37 minutes ago
        s.state = i % 2 == 0 ? "on" : "off";
        s.last_triggered = now() - 37 * 60;
    } else if (domain == "climate") {
        s.state = "heat";
        s.target = 21.5f;
        s.current = 20.8f;
        s.hvac_action = "heating";
        s.min_temp = 7;
        s.max_temp = 30;
        s.hvac_modes = list({"off", "heat", "auto"});
        s.preset_mode = "comfort";
        s.preset_modes = list({"none", "eco", "comfort", "away"});
        s.fan_mode = "auto";
        s.fan_modes = list({"auto", "on", "circulate"});
        if (contains(id, "heat_pump") || contains(id, "ecobee")) {
            // heat_cool with two setpoints, like HA's demo climate.ecobee: `temperature` is null
            s.state = "heat_cool";
            s.target = hc::core::kUnknown;
            s.target_low = 20;
            s.target_high = 24;
            s.current = 22.3f;
            s.hvac_action = "idle";
            s.hvac_modes = list({"off", "heat", "cool", "heat_cool"});
            if (contains(id, "ecobee")) {
                // HA's demo reports no action: the tile and the panel show the mode
                s.hvac_action.clear();
            }
        }
    } else if (domain == "sensor") {
        sensor(s, id, i);
    } else if (domain == "binary_sensor") {
        s.state = i % 2 == 0 ? "on" : "off";
        s.device_class = contains(id, "motion") ? "motion" : contains(id, "door") || contains(id, "gate") ? "door" : "";
    } else if (domain == "lock") {
        s.state = "locked";
        // the gate can open (unlatch)
        s.features = hc::core::kLockOpen;
    } else if (domain == "camera") {
        s.state = "idle";
    } else if (domain == "weather") {
        // OpenWeatherMap as the screenshots of HA show it (2026-10-13): no gusts, no UV index
        s.state = "cloudy";
        s.temperature = 11.8f;
        s.temperature_unit = "\xc2\xb0" "C";
        s.apparent_temperature = 10.2f;
        s.humidity = 81;
        s.pressure = 1023;
        s.pressure_unit = "hPa";
        s.wind_speed = 25.74f;
        s.wind_speed_unit = "km/h";
        s.wind_bearing = 67;
        s.visibility = 3.74f;
        s.visibility_unit = "km";
        s.cloud_coverage = 90;
        s.dew_point = 8.6f;
        s.features = hc::core::kForecastDaily | hc::core::kForecastHourly;
    } else if (domain == "timer") {
        // a 30 minute timer that ran for 37 seconds (as the clock of the PC has it)
        s.state = "active";
        s.duration = 1800;
        s.remaining = 1800;
        s.finishes_at = now() + 1800 - 37;
    } else if (domain == "script") {
        // not running, ran yesterday
        s.state = "off";
        s.last_triggered = now() - 86400;
    } else {
        // scene, button: the time of the last use
        s.state = "2026-10-07T18:12:00+00:00";
    }
    if (i % 10 == 9) {
        s.state = "unavailable";
    }
    return s;
}

void queue_values(const std::vector<EntityIndex> &entities, uint32_t delay)
{
    hc::core::PageValues values;
    values.generation = g.generation;
    for (const EntityIndex entity : entities) {
        hc::core::EntityUpdate update;
        update.entity = entity;
        update.fields = kAllFields;
        update.values = g.samples[entity];
        values.updates.push_back(std::move(update));
    }
    g.queue.push_back({lv_tick_get() + delay, std::move(values)});
}

void queue_page()
{
    if (!g.shown || g.page >= g.shown->pages.size()) {
        return;
    }
    std::vector<EntityIndex> entities;
    for (const auto index : g.shown->pages[g.page].tiles) {
        const EntityIndex entity = g.shown->tiles[index].entity;
        if (entity != hc::core::kNone && std::find(entities.begin(), entities.end(), entity) == entities.end()) {
            entities.push_back(entity);
        }
    }
    queue_values(entities, kValuesMs);
}

// what a thermostat does at its setpoints and the room temperature
const char *hvac_action(const EntityState &s)
{
    if (s.state == "off") {
        return "off";
    }
    if (!std::isnan(s.target_low) && !std::isnan(s.target_high)) {
        return s.current < s.target_low ? "heating" : s.current > s.target_high ? "cooling" : "idle";
    }
    if (s.state == "cool") {
        return s.target < s.current ? "cooling" : "idle";
    }
    return s.target > s.current ? "heating" : "idle";
}

// an entity without an action (HA's demo climate.ecobee) keeps none
void update_action(EntityState &s)
{
    if (!s.hvac_action.empty()) {
        s.hvac_action = hvac_action(s);
    }
}

// as the integrations with a range do (ecobee, HA's demo): heat_cool has the two setpoints and
// no `temperature`, heat and cool the one of their side
void set_hvac_mode(EntityState &s, std::string_view mode)
{
    const bool range = !std::isnan(s.target_low) && !std::isnan(s.target_high);
    if (mode == "heat_cool" && !range) {
        s.target_low = std::isnan(s.target) ? 20 : s.target;
        s.target_high = s.target_low + 4;
        s.target = hc::core::kUnknown;
    } else if ((mode == "heat" || mode == "cool") && range) {
        s.target = mode == "heat" ? s.target_low : s.target_high;
        s.target_low = s.target_high = hc::core::kUnknown;
    }
    s.state.assign(mode.data(), mode.size());
    update_action(s);
}

} // namespace

void set_action_ms(uint32_t ms)
{
    g_action_ms = ms;
}

void set_dashboard(std::shared_ptr<const hc::core::Layout> layout)
{
    g = {};
    g.layout = std::move(layout);
    if (!g.layout) {
        return;
    }
    for (size_t i = 0; i < g.layout->entities.size(); i++) {
        g.samples.push_back(make_sample(g.layout->entities[i], i));
    }
}

std::shared_ptr<const hc::core::Layout> dashboard()
{
    return g.layout;
}

bool save_dashboard(const hc::core::Layout &layout, uint32_t revision, char *error, size_t error_size)
{
    if (!g.layout || g.layout->revision != revision) {
        std::snprintf(error, error_size, "the dashboard was changed in the meantime");
        return false;
    }
    // through JSON like the device, so the stored layout is what a file would hold
    auto stored = hc::config::read_json(hc::config::write_json(layout));
    if (!stored) {
        std::snprintf(error, error_size, "%s", stored.error().c_str());
        return false;
    }
    stored->revision = revision + 1;
    say("dashboard saved: revision %u\n", static_cast<unsigned>(stored->revision));
    set_dashboard(std::make_shared<const hc::core::Layout>(std::move(*stored)));
    return true;
}

void subscribe_page(std::shared_ptr<const hc::core::Layout> layout, hc::core::PageIndex page, hc::core::Orientation,
                    hc::core::TileIndex, uint32_t generation)
{
    // the copy of the edit mode has the same entities, and those of the tiles added to it at the end
    const bool known = layout && g.layout && layout->entities.size() >= g.layout->entities.size();
    if (known) {
        g.samples.resize(g.layout->entities.size());
        for (size_t i = g.samples.size(); i < layout->entities.size(); i++) {
            g.samples.push_back(make_sample(layout->entities[i], i));
        }
    }
    g.shown = known ? std::move(layout) : nullptr;
    g.page = known ? page : hc::core::kNone;
    g.generation = generation;
    // the values of the page that was shown go; a history answer stays (the panel that just
    // opened subscribes after it asked for it)
    g.queue.erase(std::remove_if(g.queue.begin(), g.queue.end(),
                                 [](const Delivery &d) { return std::holds_alternative<hc::core::PageValues>(d.event); }),
                  g.queue.end());
    if (hass_connected()) {
        queue_page();
    }
}

void call_service(const hc::core::Intent &intent, std::string_view entity_id)
{
    say("[sim] call_service kind %d for %.*s, value %g\n", static_cast<int>(intent.kind),
                static_cast<int>(entity_id.size()), entity_id.data(), static_cast<double>(intent.value));
    if (!hass_connected()) {
        g.queue.push_back({lv_tick_get(), hc::core::ServiceFailed{intent.entity, "not connected to Home Assistant"}});
        return;
    }
    if (intent.entity >= g.samples.size()) {
        return;
    }
    EntityState &s = g.samples[intent.entity];
    using hc::core::IntentKind;
    switch (intent.kind) {
    case IntentKind::Toggle:
        s.state = s.is_on() ? "off" : "on";
        if (entity_id.starts_with("light.")) {
            // like HA: off reports no level, on comes back at the last one
            s.brightness = s.is_on() ? s.last_brightness : int16_t{-1};
        }
        break;
    case IntentKind::SetLevel:
        s.state = intent.value >= 0.5f ? "on" : "off";
        s.brightness = s.is_on() ? static_cast<int16_t>(std::lround(intent.value * 255 / 100)) : int16_t{-1};
        if (s.is_on()) {
            s.last_brightness = s.brightness;
        }
        break;
    case IntentKind::SetColor:
        // HA reports what it sent to the light, rounded
        s.state = "on";
        s.brightness = s.last_brightness;
        s.color_mode = "hs";
        s.hue = std::round(intent.value * 10) / 10;
        s.saturation = std::round(intent.value2 * 10) / 10;
        s.color_temp = hc::core::kUnknown;
        break;
    case IntentKind::SetColorTemp:
        // through mireds, as HA does
        s.state = "on";
        s.brightness = s.last_brightness;
        s.color_mode = "color_temp";
        s.color_temp = std::round(1e6f / std::round(1e6f / intent.value));
        break;
    case IntentKind::SetEffect:
        s.state = "on";
        s.brightness = s.last_brightness;
        s.effect = intent.text;
        break;
    case IntentKind::SetTemperature:
        s.target = intent.value;
        update_action(s);
        break;
    case IntentKind::SetTemperatureRange:
        s.target_low = std::round(intent.value * 10) / 10;
        s.target_high = std::round(intent.value2 * 10) / 10;
        update_action(s);
        break;
    case IntentKind::SetHvacMode:
        set_hvac_mode(s, intent.text);
        break;
    case IntentKind::SetPreset:
        s.preset_mode = intent.text;
        break;
    case IntentKind::SetFanMode:
        s.fan_mode = intent.text;
        break;
    case IntentKind::SetTimer: {
        // as HA: a start (re)starts with the duration given or the entity's, resumes a paused
        // timer with its rest; a pause keeps the rest; cancel and finish end it
        const int64_t now = hc::sample::now();
        if (intent.value == hc::core::kTimerStart) {
            if (intent.value2 > 0) {
                s.duration = intent.value2;
            }
            const float run = s.state == "paused" && !std::isnan(s.remaining) ? s.remaining : s.duration;
            s.state = "active";
            s.remaining = run;
            s.finishes_at = now + static_cast<int64_t>(std::lround(run));
        } else if (intent.value == hc::core::kTimerPause) {
            if (s.state == "active") {
                s.remaining = static_cast<float>(std::max<int64_t>(0, s.finishes_at - now));
                s.finishes_at = 0;
                s.state = "paused";
            }
        } else {
            s.state = "idle";
            s.remaining = hc::core::kUnknown;
            s.finishes_at = 0;
        }
        break;
    }
    case IntentKind::Run: {
        // an automation runs at once; a script runs for a second
        const int64_t now = hc::sample::now();
        if (entity_id.starts_with("automation.")) {
            s.last_triggered = now;
            break;
        }
        if (intent.value == hc::core::kStop) {
            s.state = "off";
            break;
        }
        s.state = "on";
        s.last_triggered = now;
        queue_values({intent.entity}, g_action_ms);
        s.state = "off";
        queue_values({intent.entity}, g_action_ms + 1000);
        return;
    }
    case IntentKind::SetLock: {
        // like a real lock: locking or unlocking for a second, an opened door locks again
        const bool open = intent.value == hc::core::kOpen;
        const bool lock = intent.value == hc::core::kLock;
        s.state = open ? "opening" : lock ? "locking" : "unlocking";
        queue_values({intent.entity}, g_action_ms);
        s.state = open ? "open" : lock ? "locked" : "unlocked";
        queue_values({intent.entity}, g_action_ms + 1000);
        if (open) {
            s.state = "locked";
            queue_values({intent.entity}, g_action_ms + 4000);
        }
        return;
    }
    default:
        return;
    }
    queue_values({intent.entity}, g_action_ms);
}

void fetch_history(std::string_view entity_id, hc::core::EntityIndex entity, hc::core::HistoryKind kind, uint32_t hours,
                   uint32_t generation)
{
    say("[sim] history of %.*s, %u hours, %s\n", static_cast<int>(entity_id.size()), entity_id.data(), static_cast<unsigned>(hours),
                kind == hc::core::HistoryKind::Statistics   ? "statistics"
                : kind == hc::core::HistoryKind::Temperature ? "temperature"
                                                             : "states");
    hc::core::History history;
    history.generation = generation;
    history.entity = entity;
    history.kind = kind;
    history.end = static_cast<uint32_t>(now());
    history.start = history.end - hours * 3600;
    if (!hass_connected()) {
        history.error = "not connected to Home Assistant";
    } else if (entity < g.samples.size()) {
        const EntityState &s = g.samples[entity];
        // the same curve for every request of the entity: it follows the clock
        const double phase = static_cast<double>(entity % 7);
        // an hour without data 30 hours ago (the recorder was down)
        const uint32_t gap = history.end - 30 * 3600;
        if (kind != hc::core::HistoryKind::States) {
            // a temperature: the climate's room temperature, a change every 10 minutes
            const bool temperature = kind == hc::core::HistoryKind::Temperature;
            char *end = nullptr;
            const double base = temperature ? static_cast<double>(s.current) : std::strtod(s.state.c_str(), &end);
            const bool numeric = temperature ? !std::isnan(s.current) : !s.state.empty() && end != nullptr && *end == 0;
            const double amplitude = std::max(1.0, std::fabs(base) * 0.1);
            const uint32_t step = temperature ? 600 : 300;
            for (uint32_t t = history.start / step * step; numeric && t < history.end; t += step) {
                const double day = 2 * 3.14159265 * (t % 86400) / 86400.0;
                const double wobble = 0.15 * amplitude * std::sin(t / 1700.0 + phase);
                hc::core::HistoryPoint point;
                point.time = t;
                point.value = t >= gap && t < gap + 3600 ? hc::core::kUnknown
                                                         : static_cast<float>(base + amplitude * std::sin(day + phase) + wobble);
                history.points.push_back(point);
            }
        } else {
            // on for a few minutes now and then, more by day
            uint32_t t = history.start;
            uint32_t seed = static_cast<uint32_t>(entity) * 2654435761u;
            bool on = false;
            history.points.push_back({t, 0.0f});
            while (t < history.end) {
                seed = seed * 1103515245u + 12345u;
                t += on ? 120 + (seed >> 8) % 900 : 600 + (seed >> 8) % 7200;
                on = !on;
                if (t >= gap && t < gap + 3600) {
                    history.points.push_back({gap, hc::core::kUnknown});
                    t = gap + 3600;
                    on = false;
                }
                history.points.push_back({t, on ? 1.0f : 0.0f});
            }
            if (entity_id.starts_with("lock.") || entity_id.starts_with("timer.")) {
                // jammed (a timer: paused) for half an hour 20 hours ago
                const uint32_t jammed = history.end - 20 * 3600;
                auto &points = history.points;
                points.erase(std::remove_if(points.begin(), points.end(),
                                            [jammed](const hc::core::HistoryPoint &p) {
                                                return p.time >= jammed && p.time <= jammed + 1800;
                                            }),
                             points.end());
                auto at = std::find_if(points.begin(), points.end(),
                                       [jammed](const hc::core::HistoryPoint &p) { return p.time > jammed; });
                at = points.insert(at, {jammed, hc::core::kProblem});
                points.insert(at + 1, {jammed + 1800, 0.0f});
            }
            if (entity_id.starts_with("timer.")) {
                // a timer made 26 hours ago: the 48 h window starts without history
                const uint32_t created = history.end - 26 * 3600;
                auto &points = history.points;
                points.erase(std::remove_if(points.begin(), points.end(),
                                            [created](const hc::core::HistoryPoint &p) { return p.time < created; }),
                             points.end());
                points.insert(points.begin(), {created, 0.0f});
            }
        }
    }
    g.queue.push_back({lv_tick_get() + kHistoryMs, std::move(history)});
}

void subscribe_forecast(std::string_view entity_id, hc::core::EntityIndex entity, hc::core::ForecastType type,
                        uint32_t generation)
{
    using hc::core::ForecastType;
    if (type == ForecastType::None) {
        say("[sim] forecast ends\n");
        return;
    }
    const bool daily = type == ForecastType::Daily;
    say("[sim] %s forecast of %.*s\n", daily ? "daily" : "hourly", static_cast<int>(entity_id.size()),
                entity_id.data());
    hc::core::Forecast forecast;
    forecast.generation = generation;
    forecast.entity = entity;
    forecast.type = type;
    if (g.forecast_mode == ForecastMode::Loading) {
        return; // no answer: the panel shows "Loading..."
    }
    if (!hass_connected()) {
        forecast.error = "not connected to Home Assistant";
    } else if (g.forecast_mode == ForecastMode::Error) {
        forecast.error = "forecast not supported for this entity";
    } else {
        // from the start of this day or hour, the conditions in turn so every glyph is seen
        const auto now = static_cast<uint32_t>(hc::sample::now());
        const uint32_t step = daily ? 86400 : 3600;
        const uint32_t first = now / step * step;
        const size_t count = daily ? 8 : hc::core::kMaxForecastEntries;
        for (size_t i = 0; i < count; i++) {
            hc::core::ForecastEntry entry;
            entry.time = first + static_cast<uint32_t>(i) * step;
            entry.condition = static_cast<hc::core::Condition>(1 + i % 15);
            const double wave = std::sin(static_cast<double>(i) * (daily ? 0.9 : 0.27));
            entry.temperature = static_cast<float>(std::round((daily ? 15.0 : 12.0) + 5.0 * wave) * 10) / 10.0f +
                                (i % 3 == 0 ? 0.0f : 0.3f);
            if (daily) {
                entry.templow = entry.temperature - 6.5f;
            }
            entry.precipitation_probability = static_cast<int8_t>(i % 4 == 1 ? 0 : (i * 17) % 100);
            forecast.entries.push_back(entry);
        }
    }
    g.queue.push_back({lv_tick_get() + kForecastMs, std::move(forecast)});
}

void set_forecast_mode(ForecastMode mode)
{
    g.forecast_mode = mode;
}

// A landscape: the sky, a hill, a house, and a sun at `phase` (0..1) of its way across the sky.
// Single precision and the hill's line once per column: the ESP32 computes doubles in software,
// and the firmware's benchmark mode draws this in the LVGL task.
hc::core::Pixels landscape(hc::core::EntityIndex entity, uint16_t width, uint16_t height, float phase)
{
    hc::core::Pixels pixels = hc::core::allocate_pixels(static_cast<size_t>(width) * height);
    const float tint = static_cast<float>(entity % 5) * 0.12f;
    const float sun_x = width * (0.1f + 0.8f * phase);
    const float sun_y = height * (0.45f - 0.3f * std::sin(phase * 3.14159265f));
    const float sun_r = std::max(4.0f, height * 0.08f);
    std::vector<float> hills(width);
    for (uint16_t x = 0; x < width; x++) {
        hills[x] = 0.62f + 0.08f * std::sin(static_cast<float>(x) / width * 6.0f + tint * 10);
    }
    const auto c = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255); };
    for (uint16_t y = 0; y < height; y++) {
        const float fy = static_cast<float>(y) / height;
        for (uint16_t x = 0; x < width; x++) {
            const float fx = static_cast<float>(x) / width;
            const float hill = hills[x];
            float r = 0;
            float gr = 0;
            float b = 0;
            if (fy > hill) {
                r = 0.20f + tint;
                gr = 0.45f - 0.2f * (fy - hill);
                b = 0.18f;
            } else {
                r = 0.35f + 0.4f * fy;
                gr = 0.55f + 0.35f * fy;
                b = 0.95f - 0.2f * fy;
            }
            // the house on the hill
            if (fx > 0.62f && fx < 0.78f && fy > hill - 0.16f && fy <= hill + 0.02f) {
                r = 0.75f;
                gr = 0.7f;
                b = 0.62f;
            }
            const float dx = x - sun_x;
            const float dy = y - sun_y;
            if (dx * dx + dy * dy < sun_r * sun_r && fy < hill) {
                r = 1.0f;
                gr = 0.85f;
                b = 0.3f;
            }
            pixels.get()[static_cast<size_t>(y) * width + x] = lv_color_make(c(r), c(gr), c(b)).full;
        }
    }
    return pixels;
}

void fetch_image(std::string_view entity_id, hc::core::EntityIndex entity, uint16_t width, uint16_t height, bool whole,
                 uint32_t generation)
{
    (void)whole; // the landscape is drawn in any shape: the whole picture is the box
    say("[sim] image of %.*s, %ux%u\n", static_cast<int>(entity_id.size()), entity_id.data(), width, height);
    hc::core::Image image;
    image.generation = generation;
    image.entity = entity;
    if (!hass_connected()) {
        image.error = "not connected to Home Assistant";
        g.queue.push_back({lv_tick_get() + kImageMs, std::move(image)});
        return;
    }
    // the sun crosses the sky once a minute
    image.width = width;
    image.height = height;
    image.source_width = width; // drawn in the box's shape: nothing is cut
    image.source_height = height;
    image.pixels = landscape(entity, width, height, static_cast<float>(now() % 60) / 60.0f);
    g.queue.push_back({lv_tick_get() + kImageMs, std::move(image)});
}

// an event at `at`, in time order (the queue is taken from its front)
void queue_at(uint32_t at, hc::core::HassEvent event)
{
    auto it = g.queue.begin();
    while (it != g.queue.end() && static_cast<int32_t>(at - it->at) >= 0) {
        ++it;
    }
    g.queue.insert(it, {at, std::move(event)});
}

void play_stream(std::string_view entity_id, std::string_view stream, hc::core::EntityIndex entity, uint16_t width,
                 uint16_t height, uint32_t generation)
{
    say("[sim] stream of %.*s, %ux%u: %.*s\n", static_cast<int>(entity_id.size()), entity_id.data(), width,
                height, static_cast<int>(stream.size()), stream.data());
    if (g.play.generation != 0 && g.play.entity == entity) {
        // another size of the stream that plays: it goes on
        g.play.width = width;
        g.play.height = height;
        g.play.generation = generation;
        return;
    }
    if (g.stream_login) {
        hc::core::Image refused;
        refused.generation = generation;
        refused.entity = entity;
        refused.live = true;
        refused.error = "login needed";
        refused.problem = hc::core::StreamProblem::Login;
        refused.http_status = 401;
        queue_at(lv_tick_get() + kAnswerMs, std::move(refused));
        return;
    }
    if (g.stream_server_down) {
        hc::core::Image refused;
        refused.generation = generation;
        refused.entity = entity;
        refused.live = true;
        refused.error = "the stream cannot be reached";
        refused.problem = hc::core::StreamProblem::Unreachable;
        queue_at(lv_tick_get() + kConnectTimeoutMs, std::move(refused));
        return;
    }
    // the answer, then the first frame after a while, like a go2rtc that first starts ffmpeg
    hc::core::Image opened;
    opened.generation = generation;
    opened.entity = entity;
    opened.opened = true;
    queue_at(lv_tick_get() + kAnswerMs, std::move(opened));
    g.play = {.entity = entity, .width = width, .height = height, .generation = generation,
              .next = lv_tick_get() + kAnswerMs + kStreamStartMs};
}

void set_stream_login(bool login)
{
    g.stream_login = login;
}

void set_stream_server(bool up)
{
    g.stream_server_down = !up;
    if (!up && g.play.generation != 0) {
        hc::core::Image ended;
        ended.generation = g.play.generation;
        ended.entity = g.play.entity;
        ended.live = true;
        ended.error = "the stream ended";
        ended.problem = hc::core::StreamProblem::Ended;
        g.queue.push_back({lv_tick_get(), std::move(ended)});
        g.play = {};
    }
    say("[sim] stream server %s\n", up ? "up" : "down");
}

void stop_stream()
{
    if (g.play.generation != 0) {
        say("[sim] stream stopped\n");
    }
    g.play = {};
}

namespace {

// the made-up house of the entity search: every room has every thing
std::vector<std::string> house()
{
    static const char *const kRooms[] = {"kitchen", "living_room", "bedroom", "office", "bathroom",
                                         "hallway", "garage", "garden", "attic", "basement"};
    static const char *const kThings[] = {"light.%s_ceiling", "light.%s_lamp_onoff", "switch.%s_socket",
                                          "sensor.%s_temperature", "sensor.%s_humidity", "binary_sensor.%s_motion",
                                          "climate.%s", "cover.%s_blind", "lock.%s_door", "fan.%s_fan",
                                          "scene.%s_evening", "camera.%s"};
    std::vector<std::string> ids;
    for (const char *room : kRooms) {
        for (const char *thing : kThings) {
            char id[64];
            std::snprintf(id, sizeof(id), thing, room);
            ids.emplace_back(id);
        }
    }
    return ids;
}

std::string lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

} // namespace

void find_entities(std::string_view query, size_t max, uint32_t generation)
{
    say("[sim] entity search '%.*s'\n", static_cast<int>(query.size()), query.data());
    hc::core::EntityList list;
    list.generation = generation;
    if (!hass_connected()) {
        list.error = "not connected to Home Assistant";
        queue_at(lv_tick_get() + kEntityQueryMs, std::move(list));
        return;
    }
    std::vector<std::string> ids = house();
    if (g.layout) {
        for (const auto &id : g.layout->entities) {
            ids.emplace_back(id.data(), id.size());
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    // every word has to be in the id or the name, as in Home Assistant's template
    std::vector<std::string> words;
    std::string text = lower(query);
    for (size_t at = 0; at < text.size();) {
        const size_t end = std::min(text.find(' ', at), text.size());
        if (end > at) {
            words.push_back(text.substr(at, end - at));
        }
        at = end + 1;
    }
    for (const std::string &id : ids) {
        const hc::memory::String name = friendly(std::string_view(id).substr(id.find('.') + 1));
        const std::string haystack = id + " " + lower(name);
        if (!std::all_of(words.begin(), words.end(), [&](const std::string &w) { return contains(haystack, w); })) {
            continue;
        }
        if (list.rows.size() == max) {
            list.more = true;
            break;
        }
        list.rows.push_back({hc::memory::String(id.c_str()), name});
    }
    queue_at(lv_tick_get() + kEntityQueryMs, std::move(list));
}

void fetch_entity(std::string_view entity_id, uint32_t generation)
{
    hc::core::EntityDetails details;
    details.generation = generation;
    details.id = hc::memory::String(entity_id);
    if (!hass_connected()) {
        details.error = "not connected to Home Assistant";
    } else {
        details.dimmer = entity_id.starts_with("light.") && !contains(entity_id, "onoff");
    }
    queue_at(lv_tick_get() + kEntityQueryMs, std::move(details));
}

void browse_media(std::string_view id, uint32_t generation)
{
    say("[sim] media '%.*s'\n", static_cast<int>(id.size()), id.data());
    hc::core::MediaList media;
    media.generation = generation;
    media.id = hc::memory::String(id);
    if (!hass_connected()) {
        media.error = "not connected to Home Assistant";
        queue_at(lv_tick_get() + kEntityQueryMs, std::move(media));
        return;
    }
    const std::string base = "media-source://media_source/local";
    auto folder = [&](const std::string &title, const std::string &item_id) {
        media.items.push_back({hc::memory::String(item_id.c_str()), hc::memory::String(title.c_str()), "", true, false});
    };
    auto file = [&](const std::string &title, const std::string &type) {
        media.items.push_back({hc::memory::String((std::string(id) + "/" + title).c_str()),
                              hc::memory::String(title.c_str()), hc::memory::String(type.c_str()), false, true});
    };
    if (id.empty()) {
        media.title = "Media sources";
        folder("Camera", "media-source://camera");
        folder("My media", "media-source://media_source");
        folder("Text-to-speech", "media-source://tts");
    } else if (id == "media-source://media_source") {
        media.title = "My media";
        folder("music", base + "/music");
        media.items.push_back({hc::memory::String((base + "/doorbell.mp3").c_str()), "doorbell.mp3", "audio/mpeg",
                              false, true});
        media.items.push_back({hc::memory::String((base + "/garden.jpg").c_str()), "garden.jpg", "image/jpeg", false,
                              true});
    } else if (id == base + "/music") {
        media.title = "music";
        folder("Pelagial", base + "/music/Pelagial");
        folder("Phanerozoic", base + "/music/Phanerozoic");
    } else if (id.starts_with(base + "/music/")) {
        media.title = hc::memory::String(id.substr(id.rfind('/') + 1));
        for (const char *title : {"01 - Epipelagic.mp3", "02 - Mesopelagic Into the Uncanny.mp3",
                                  "03 - Bathyalpelagic I Impasses.mp3", "04 - Abyssopelagic I Boundless Vasts.mp3",
                                  "05 - Hadopelagic I Omen of the Deep.mp3"}) {
            file(title, "audio/mpeg");
        }
    } else {
        media.title = hc::memory::String(id.substr(id.rfind('/') + 1));
    }
    queue_at(lv_tick_get() + kEntityQueryMs, std::move(media));
}

bool take_event(hc::core::HassEvent &out)
{
    // like the connection: everything again once it is back
    const bool connected = hass_connected();
    if (connected && !g.was_connected) {
        queue_page();
    }
    g.was_connected = connected;
    if (g.play.generation != 0 && static_cast<int32_t>(lv_tick_get() - g.play.next) >= 0) {
        // the next frame of the stream that plays: the sun a step further each time
        g.play.next = lv_tick_get() + kStreamFrameMs;
        g.play.frame++;
        hc::core::Image image;
        image.generation = g.play.generation;
        image.entity = g.play.entity;
        image.live = true;
        image.width = g.play.width;
        image.height = g.play.height;
        image.source_width = g.play.width;
        image.source_height = g.play.height;
        image.pixels = landscape(g.play.entity, g.play.width, g.play.height, static_cast<float>(g.play.frame % 100) / 100.0f);
        out = std::move(image);
        return true;
    }
    if (g.queue.empty() || static_cast<int32_t>(lv_tick_get() - g.queue.front().at) < 0) {
        return false;
    }
    out = std::move(g.queue.front().event);
    g.queue.pop_front();
    return true;
}

} // namespace hc::sample
