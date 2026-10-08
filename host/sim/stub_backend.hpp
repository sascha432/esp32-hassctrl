#pragma once

// The Backend of the Windows version of the UI (docs/architecture.md section 1): sample data
// instead of the network, Home Assistant and the board. Taps in the UI change this data (a network
// is forgotten, the hotspot turns on, a connect succeeds or fails after a moment), so every page
// and control can be clicked through. Time is LVGL's tick, so the headless runs of the simulator
// can skip ahead.

#include <cstddef>
#include <cstdint>

#include "hc/ui/backend.hpp"
#include "hc/ui/test_screen.hpp"

namespace sim {

struct ScenarioInfo {
    const char *name;
    const char *description;
};

size_t scenario_count();
const ScenarioInfo &scenario_info(size_t index);
// the index of `name`, or -1
int find_scenario(const char *name);

// Starts the scenario from its sample state (taps made so far are forgotten).
void start_scenario(size_t index);
size_t current_scenario();

// the stored rotation the UI starts in (LVGL's steps)
void set_rotation(uint8_t rotation);

// the board the window stands for, shown on System
void set_board(const char *board_name);
// a light sensor with this much light in the room; without, auto brightness has no effect
void set_ambient_light(bool present, float lux);

// the backlight the UI asked for, in percent (the window title shows it)
uint8_t backlight_percent();
const hc::ui::BacklightControl &backlight_control();

const hc::ui::Backend &backend();

// the scenario's Home Assistant is connected: the sample values arrive (sample_hass.cpp)
bool hass_connected();

} // namespace sim
