// The texts of the tiles and panels (tiles.hpp): states in words, setpoints, timers, times.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "dashboard.hpp"
#include "tiles_parts.hpp"

namespace hc::ui::tiles {

namespace {

using detail::domain_of;
using detail::number;

constexpr float kDefaultClimateStep = 0.5f;

} // namespace

const char *binary_words(std::string_view device_class, bool on)
{
    if (device_class == "door" || device_class == "garage_door" || device_class == "window" ||
        device_class == "opening") {
        return on ? "Open" : "Closed";
    }
    if (device_class == "motion" || device_class == "occupancy" || device_class == "presence") {
        return on ? "Detected" : "Clear";
    }
    if (device_class == "moisture") {
        return on ? "Wet" : "Dry";
    }
    if (device_class == "connectivity") {
        return on ? "Connected" : "Disconnected";
    }
    if (device_class == "lock") {
        return on ? "Unlocked" : "Locked";
    }
    return on ? "On" : "Off";
}

std::string sensor_text(const core::Tile &tile, std::string_view entity_id, const core::EntityState &state)
{
    if (domain_of(entity_id) == "binary_sensor") {
        return binary_words(state.device_class, state.is_on());
    }
    std::string value(core::display_value(state, tile));
    const char *end = nullptr;
    std::strtod(value.c_str(), const_cast<char **>(&end));
    const bool numeric = !value.empty() && end != nullptr && *end == 0;
    if (!numeric) {
        // a template sensor can report several lines; a tile has room for the first one
        value = words(value.substr(0, value.find('\n')));
    }
    const std::string_view unit = core::display_unit(state, tile);
    if (numeric && !unit.empty()) {
        value += ' ';
        value += unit;
    }
    return value;
}

std::string words(std::string_view state)
{
    std::string out(state);
    std::replace(out.begin(), out.end(), '_', ' ');
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

std::string temperature(float value)
{
    return std::isnan(value) ? std::string("--") : number(value, std::fabs(value - std::round(value)) < 0.05f ? 0 : 1);
}

float climate_step(const core::Tile &tile, const core::EntityState &state)
{
    if (!std::isnan(state.temp_step) && state.temp_step > 0) {
        return state.temp_step;
    }
    return tile.step > 0 ? tile.step : kDefaultClimateStep;
}

bool climate_range(const core::EntityState &state, const core::Expectation *pending, float &low, float &high)
{
    const bool waiting = pending != nullptr && !std::isnan(pending->target_low) && !std::isnan(pending->target_high);
    low = waiting ? pending->target_low : state.target_low;
    high = waiting ? pending->target_high : state.target_high;
    return !std::isnan(low) && !std::isnan(high);
}

TimerState timer_shown(const core::EntityState &state, const core::Expectation *pending)
{
    const std::string_view shown = pending != nullptr && !pending->state.empty() ? std::string_view(pending->state)
                                                                                  : std::string_view(state.state);
    if (shown == "active") {
        return TimerState::Active;
    }
    if (shown == "paused") {
        return TimerState::Paused;
    }
    if (shown == "idle") {
        return TimerState::Idle;
    }
    return TimerState::Unknown;
}

float timer_left(const core::EntityState &state, int64_t now)
{
    if (state.state == "active" && now != 0 && state.finishes_at != 0) {
        return static_cast<float>(std::max<int64_t>(0, state.finishes_at - now));
    }
    if (!std::isnan(state.remaining) && state.state != "idle") {
        return state.remaining;
    }
    return state.duration;
}

std::string when_text(int64_t when, bool with_time)
{
    if (when == 0) {
        return "Never";
    }
    const Backend &backend = dashboard::backend();
    const int64_t now = backend.clock();
    std::tm at{};
    backend.local_time(when, at);
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", at.tm_hour, at.tm_min);
    // today and yesterday by the local day; while the clock is not set, every day is a date
    const char *day = nullptr;
    if (now != 0) {
        std::tm today{};
        std::tm yesterday{};
        backend.local_time(now, today);
        backend.local_time(now - 86400, yesterday);
        if (at.tm_year == today.tm_year && at.tm_yday == today.tm_yday) {
            day = "Today";
        } else if (at.tm_year == yesterday.tm_year && at.tm_yday == yesterday.tm_yday) {
            day = "Yesterday";
        }
    }
    char date[24] = "";
    if (day == nullptr && std::strftime(date, sizeof(date), "%d %b", &at) == 0) {
        date[0] = 0;
    }
    if (!with_time) {
        // the tile: the time today, else the day
        if (day == nullptr) {
            return date;
        }
        return std::string_view(day) == "Today" ? std::string(clock) : std::string(day);
    }
    return std::string(day != nullptr ? day : date) + " " + clock;
}

std::string timer_text(float seconds)
{
    if (std::isnan(seconds)) {
        return "--:--";
    }
    const auto total = static_cast<long>(std::lround(std::max(seconds, 0.0f)));
    char text[24];
    if (total >= 3600) {
        std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", total / 3600, total / 60 % 60, total % 60);
    } else {
        std::snprintf(text, sizeof(text), "%02ld:%02ld", total / 60, total % 60);
    }
    return text;
}

int shown_level(const core::EntityState &state, const core::Expectation *pending, bool &on)
{
    on = state.is_on();
    int brightness = state.brightness;
    bool unknown = false;
    if (pending != nullptr) {
        if (!pending->state.empty()) {
            on = pending->state == "on";
        }
        if (pending->brightness >= 0) {
            brightness = pending->brightness;
        } else if (on && brightness < 0) {
            // switched on (by this tile or another one of the light): HA brings back the last level
            brightness = state.last_brightness;
            unknown = brightness < 0;
        }
    }
    if (!on) {
        return 0;
    }
    if (unknown) {
        return -1;
    }
    if (brightness < 0) {
        return 100; // a light without a level
    }
    return std::max(1, (brightness * 100 + 127) / 255);
}

} // namespace hc::ui::tiles
