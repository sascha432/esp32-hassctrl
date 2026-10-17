#include "hc/core/pending.hpp"

#include <cstdlib>

namespace hc::core {

namespace {

// Home Assistant rounds a level in percent to 0..255 its own way (30 % is 76), and a
// setpoint arrives as the entity's step allows
constexpr int kBrightnessTolerance = 3;
constexpr float kTargetTolerance = 0.05f;
// a color goes through HA's conversions (hs -> rgb -> hs, kelvin -> mireds -> kelvin)
constexpr float kHueTolerance = 2;
constexpr float kSaturationTolerance = 2;
constexpr float kColorTempTolerance = 0.02f; // of the value
// a timer's duration comes back as HA's "0:45:00", whole seconds
constexpr float kDurationTolerance = 0.5f;

bool near(float expected, float reported, float tolerance)
{
    return std::isnan(expected) || (!std::isnan(reported) && std::fabs(reported - expected) <= tolerance);
}

// the hue is a circle: 359 and 1 are 2 apart
bool near_hue(float expected, float reported)
{
    if (std::isnan(expected)) {
        return true;
    }
    if (std::isnan(reported)) {
        return false;
    }
    const float distance = std::fabs(std::fmod(reported - expected + 540.0f, 360.0f) - 180.0f);
    return distance <= kHueTolerance;
}

bool met(const Expectation &expected, const EntityState &state)
{
    if (!expected.state.empty() && state.state != expected.state) {
        return false;
    }
    if (expected.brightness >= 0 && std::abs(state.brightness - expected.brightness) > kBrightnessTolerance) {
        return false;
    }
    if (!near(expected.target, state.target, kTargetTolerance)) {
        return false;
    }
    if (!near_hue(expected.hue, state.hue) || !near(expected.saturation, state.saturation, kSaturationTolerance)) {
        return false;
    }
    if (!near(expected.color_temp, state.color_temp, expected.color_temp * kColorTempTolerance)) {
        return false;
    }
    if (!expected.effect.empty() && state.effect != expected.effect) {
        return false;
    }
    if (!expected.preset.empty() && state.preset_mode != expected.preset) {
        return false;
    }
    if (!expected.fan_mode.empty() && state.fan_mode != expected.fan_mode) {
        return false;
    }
    if (!near(expected.duration, state.duration, kDurationTolerance)) {
        return false;
    }
    return true;
}

} // namespace

void PendingTracker::start(EntityIndex entity, Expectation expected, uint32_t now_ms)
{
    for (Action &action : _actions) {
        if (action.entity == entity) {
            action.deadline = now_ms + kTimeoutMs;
            action.expected = std::move(expected);
            return;
        }
    }
    _actions.push_back({entity, now_ms + kTimeoutMs, std::move(expected)});
}

const Expectation *PendingTracker::find(EntityIndex entity) const
{
    for (const Action &action : _actions) {
        if (action.entity == entity) {
            return &action.expected;
        }
    }
    return nullptr;
}

bool PendingTracker::resolve(EntityIndex entity, const EntityState &state)
{
    for (size_t i = 0; i < _actions.size(); i++) {
        if (_actions[i].entity == entity && met(_actions[i].expected, state)) {
            _actions.erase(_actions.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool PendingTracker::fail(EntityIndex entity)
{
    for (size_t i = 0; i < _actions.size(); i++) {
        if (_actions[i].entity == entity) {
            _actions.erase(_actions.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

} // namespace hc::core
