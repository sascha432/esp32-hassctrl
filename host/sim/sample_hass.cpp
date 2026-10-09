#include "sample_hass.hpp"

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
#include "stub_backend.hpp"

namespace sim {

namespace {

using hc::core::EntityIndex;
using hc::core::EntityState;
using hc::core::Field;

constexpr uint32_t kValuesMs = 150;
uint32_t g_action_ms = 400;
constexpr uint32_t kHistoryMs = 300;
constexpr uint32_t kImageMs = 500;
// a stream plays 5 frames a second
constexpr uint32_t kStreamFrameMs = 200;
constexpr uint32_t kStreamStartMs = 1500;

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
    } else if (domain == "switch" || domain == "input_boolean" || domain == "fan" || domain == "automation") {
        s.state = i % 2 == 0 ? "on" : "off";
        if (contains(id, "plug") || contains(id, "outlet")) {
            s.device_class = "outlet";
        }
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
    } else if (domain == "sensor") {
        sensor(s, id, i);
    } else if (domain == "binary_sensor") {
        s.state = i % 2 == 0 ? "on" : "off";
        s.device_class = contains(id, "motion") ? "motion" : contains(id, "door") || contains(id, "gate") ? "door" : "";
    } else if (domain == "lock") {
        s.state = "locked";
    } else if (domain == "camera") {
        s.state = "idle";
    } else if (domain == "script") {
        s.state = "off";
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
    std::printf("dashboard saved: revision %u\n", static_cast<unsigned>(stored->revision));
    set_dashboard(std::make_shared<const hc::core::Layout>(std::move(*stored)));
    return true;
}

void subscribe_page(std::shared_ptr<const hc::core::Layout> layout, hc::core::PageIndex page, hc::core::TileIndex,
                    uint32_t generation)
{
    const bool known = layout && g.layout && layout->entities.size() == g.layout->entities.size();
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
    std::printf("[sim] call_service kind %d for %.*s, value %g\n", static_cast<int>(intent.kind),
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
        s.hvac_action = s.state == "off" ? "off" : s.target > s.current ? "heating" : "idle";
        break;
    case IntentKind::SetHvacMode:
        s.state = intent.text;
        s.hvac_action = s.state == "off" ? "off" : s.target > s.current ? "heating" : "idle";
        break;
    case IntentKind::SetPreset:
        s.preset_mode = intent.text;
        break;
    case IntentKind::SetFanMode:
        s.fan_mode = intent.text;
        break;
    default:
        return;
    }
    queue_values({intent.entity}, g_action_ms);
}

void fetch_history(std::string_view entity_id, hc::core::EntityIndex entity, hc::core::HistoryKind kind, uint32_t hours,
                   uint32_t generation)
{
    std::printf("[sim] history of %.*s, %u hours, %s\n", static_cast<int>(entity_id.size()), entity_id.data(), hours,
                kind == hc::core::HistoryKind::Statistics ? "statistics" : "states");
    hc::core::History history;
    history.generation = generation;
    history.entity = entity;
    history.kind = kind;
    history.end = static_cast<uint32_t>(std::time(nullptr));
    history.start = history.end - hours * 3600;
    if (!hass_connected()) {
        history.error = "not connected to Home Assistant";
    } else if (entity < g.samples.size()) {
        const EntityState &s = g.samples[entity];
        // the same curve for every request of the entity: it follows the clock
        const double phase = static_cast<double>(entity % 7);
        // an hour without data 30 hours ago (the recorder was down)
        const uint32_t gap = history.end - 30 * 3600;
        if (kind == hc::core::HistoryKind::Statistics) {
            char *end = nullptr;
            const double base = std::strtod(s.state.c_str(), &end);
            const bool numeric = !s.state.empty() && end != nullptr && *end == 0;
            const double amplitude = std::max(1.0, std::fabs(base) * 0.1);
            for (uint32_t t = history.start / 300 * 300; numeric && t < history.end; t += 300) {
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
        }
    }
    g.queue.push_back({lv_tick_get() + kHistoryMs, std::move(history)});
}

// A landscape: the sky, a hill, a house, and a sun at `phase` (0..1) of its way across the sky.
hc::core::Pixels landscape(hc::core::EntityIndex entity, uint16_t width, uint16_t height,
                                                        double phase)
{
    hc::core::Pixels pixels = hc::core::allocate_pixels(static_cast<size_t>(width) * height);
    const double tint = static_cast<double>(entity % 5) * 0.12;
    const double sun_x = width * (0.1 + 0.8 * phase);
    const double sun_y = height * (0.45 - 0.3 * std::sin(phase * 3.14159265));
    const double sun_r = std::max(4.0, height * 0.08);
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            const double fx = static_cast<double>(x) / width;
            const double fy = static_cast<double>(y) / height;
            const double hill = 0.62 + 0.08 * std::sin(fx * 6.0 + tint * 10);
            double r = 0;
            double gr = 0;
            double b = 0;
            if (fy > hill) {
                r = 0.20 + tint;
                gr = 0.45 - 0.2 * (fy - hill);
                b = 0.18;
            } else {
                r = 0.35 + 0.4 * fy;
                gr = 0.55 + 0.35 * fy;
                b = 0.95 - 0.2 * fy;
            }
            // the house on the hill
            if (fx > 0.62 && fx < 0.78 && fy > hill - 0.16 && fy <= hill + 0.02) {
                r = 0.75;
                gr = 0.7;
                b = 0.62;
            }
            const double dx = x - sun_x;
            const double dy = y - sun_y;
            if (dx * dx + dy * dy < sun_r * sun_r && fy < hill) {
                r = 1.0;
                gr = 0.85;
                b = 0.3;
            }
            const auto c = [](double v) { return static_cast<uint8_t>(std::clamp(v, 0.0, 1.0) * 255); };
            pixels.get()[static_cast<size_t>(y) * width + x] = lv_color_make(c(r), c(gr), c(b)).full;
        }
    }
    return pixels;
}

void fetch_image(std::string_view entity_id, std::string_view stream, hc::core::EntityIndex entity, uint16_t width,
                 uint16_t height, uint32_t generation)
{
    std::printf("[sim] image of %.*s%s, %ux%u\n", static_cast<int>(entity_id.size()), entity_id.data(),
                stream.empty() ? "" : " (first frame of its stream)", width, height);
    hc::core::Image image;
    image.generation = generation;
    image.entity = entity;
    if (!hass_connected() && stream.empty()) {
        image.error = "not connected to Home Assistant";
        g.queue.push_back({lv_tick_get() + kImageMs, std::move(image)});
        return;
    }
    // the sun crosses the sky once a minute
    image.width = width;
    image.height = height;
    image.pixels = landscape(entity, width, height, static_cast<double>(std::time(nullptr) % 60) / 60.0);
    g.queue.push_back({lv_tick_get() + kImageMs, std::move(image)});
}

void play_stream(std::string_view entity_id, std::string_view stream, hc::core::EntityIndex entity, uint16_t width,
                 uint16_t height, uint32_t generation)
{
    std::printf("[sim] stream of %.*s, %ux%u: %.*s\n", static_cast<int>(entity_id.size()), entity_id.data(), width,
                height, static_cast<int>(stream.size()), stream.data());
    // the first frame after a while, like a go2rtc that first starts ffmpeg
    g.play = {.entity = entity, .width = width, .height = height, .generation = generation,
              .next = lv_tick_get() + kStreamStartMs};
}

void stop_stream()
{
    if (g.play.generation != 0) {
        std::printf("[sim] stream stopped\n");
    }
    g.play = {};
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
        image.pixels = landscape(g.play.entity, g.play.width, g.play.height, (g.play.frame % 100) / 100.0);
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

} // namespace sim
