#pragma once

// The actions on their way to Home Assistant (docs/architecture.md section 5): a tap shows its
// result at once, marked as pending, until the entity reports it or the time is up. Owned by the
// UI task, like the store. Plain C++, host tested.

#include <cmath>
#include <cstdint>

#include "hc/core/entity.hpp"
#include "hc/memory/large.hpp"

namespace hc::core {

// What the entity is expected to report. Unset parts are not compared.
struct Expectation {
    memory::String state;   // "on", "off"; empty: not compared
    int16_t brightness = -1; // 0 to 255; -1: not compared
    float target = kUnknown; // climate setpoint
    float target_low = kUnknown; // climate range, with `target_high`
    float target_high = kUnknown;
    float hue = kUnknown;        // light color, with `saturation`
    float saturation = kUnknown;
    float color_temp = kUnknown; // kelvin
    memory::String effect;       // empty: not compared
    memory::String preset;       // climate preset_mode; empty: not compared
    memory::String fan_mode;     // climate; empty: not compared
    float duration = kUnknown;   // timer: the duration a start was sent with, seconds

    bool operator==(const Expectation &) const = default;

    // false when nothing is compared, so there is nothing to wait for
    bool says_something() const
    {
        return !state.empty() || brightness >= 0 || !std::isnan(target) || !std::isnan(target_low) ||
               !std::isnan(target_high) || !std::isnan(hue) ||
               !std::isnan(color_temp) || !effect.empty() || !preset.empty() || !fan_mode.empty() ||
               !std::isnan(duration);
    }
};

class PendingTracker {
public:
    // How long an action may take before the tile shows the store's value again.
    static constexpr uint32_t kTimeoutMs = 5000;

    // A new action on `entity`; it replaces one that is still pending.
    void start(EntityIndex entity, Expectation expected, uint32_t now_ms);

    // nullptr when nothing is pending for `entity`
    const Expectation *find(EntityIndex entity) const;

    // After the store applied an update of `entity`: true when the action is done, because the
    // entity reports what was expected.
    bool resolve(EntityIndex entity, const EntityState &state);

    // The action failed (ServiceFailed): it ends at once. True when one was pending.
    bool fail(EntityIndex entity);

    // Ends the actions whose time is up and hands their entities to `ended`.
    template <class Function>
    void expire(uint32_t now_ms, Function ended)
    {
        for (size_t i = 0; i < _actions.size();) {
            if (static_cast<int32_t>(now_ms - _actions[i].deadline) >= 0) {
                const EntityIndex entity = _actions[i].entity;
                _actions.erase(_actions.begin() + static_cast<std::ptrdiff_t>(i));
                ended(entity);
            } else {
                i++;
            }
        }
    }

    void clear()
    {
        _actions.clear();
    }
    bool empty() const
    {
        return _actions.empty();
    }

private:
    struct Action {
        EntityIndex entity;
        uint32_t deadline;
        Expectation expected;
    };

    memory::Vector<Action> _actions;
};

} // namespace hc::core
