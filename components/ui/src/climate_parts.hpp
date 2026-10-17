#pragma once

// What the files of the climate panel share (climate_panel.cpp: the pills, the lists, the power
// button, build and update; climate_arc.cpp: the arc of the setpoint and the ring of a range). One
// panel at a time: its state is `g`.

#include <cstdint>
#include <string_view>

#include "climate_panel.hpp"
#include "hc/core/pending.hpp"
#include "hc/memory/large.hpp"
#include "lvgl.h"

namespace hc::ui::climate_panel::detail {

// the three options of the thermostat card
enum class Option : uint8_t { Mode, Preset, Fan, Count };
constexpr int kOptions = static_cast<int>(Option::Count);

// the arc's range when the entity reports none
constexpr float kDefaultMin = 7;
constexpr float kDefaultMax = 35;
// the two setpoints of a range
enum : uint8_t { kLow, kHigh, kEnds };

struct State {
    panel::Frame frame;
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    lv_obj_t *power = nullptr; // turns the climate off; shown only while it is on
    // the history button left of the power button (in its place while that is hidden): the
    // graph of the room temperature in the place of the arc or the list
    lv_obj_t *history = nullptr;
    bool show_history = false;
    lv_obj_t *pills[kOptions] = {};
    lv_obj_t *pill_glyphs[kOptions] = {};
    lv_obj_t *pill_values[kOptions] = {};
    uint8_t offered = 0;     // the options the pills are laid out for
    int list_option = -1;    // the option whose list is shown, -1: the arc
    // while the climate is off the arc does nothing: the panel shows the modes first, once per
    // opening and after the power button (a tap on the pill goes to the arc)
    bool modes_first = true;
    lv_obj_t *control = nullptr;

    // arc
    lv_obj_t *arc = nullptr;
    lv_obj_t *action = nullptr;
    lv_obj_t *setpoint = nullptr;
    lv_obj_t *current = nullptr;
    lv_obj_t *plus = nullptr;
    lv_obj_t *minus = nullptr;
    float min = kDefaultMin;    // the range the arc was built for
    float max = kDefaultMax;
    float step = 0.5f;
    bool dragging = false;
    float drag_target = 0;
    float step_target = core::kUnknown; // the setpoint the steps have collected
    lv_timer_t *send_timer = nullptr;
    uint32_t last_step = 0;

    // heat_cool: two setpoints on one ring. The arc only draws it (the indicator between them),
    // `ring` takes the touches, the knobs are objects of their own.
    bool range = false;     // the arc was built for a range
    lv_obj_t *ring = nullptr;
    lv_obj_t *knobs[kEnds] = {};
    lv_obj_t *ends[kEnds] = {}; // the setpoints in the middle; a tap selects one for the steppers
    int selected = kLow;        // the setpoint the steppers change; a drag selects the nearer knob
    lv_coord_t arc_x = 0;
    lv_coord_t arc_y = 0;
    lv_coord_t arc_size = 0;
    lv_coord_t ring_width = 0;
    float drag_range[kEnds] = {};
    float drag_from[kEnds] = {}; // the range when the drag began: a tap that changes nothing sends nothing
    float step_range[kEnds] = {core::kUnknown, core::kUnknown};

    // list
    lv_obj_t *list = nullptr;
    memory::Vector<memory::String> items; // the values the items were made from
};

extern State g;

const core::EntityState &state();
bool available();
void send(core::IntentKind kind, float value, std::string_view text, core::Expectation expected);
void send_target(float target);
core::Expectation range_expectation(float low, float high);
void send_range(core::EntityIndex entity, float low, float high);

// --------------------------------------------------------------------- climate_arc.cpp

// the arc in the control area (`w` x `h`), the steppers beside or below it
void build_arc(lv_coord_t w, lv_coord_t h);
void update_arc(const core::EntityState &s, const core::Expectation *pending, bool avail);
// sends the setpoint or the range the steps collected
void send_steps();
bool collecting_steps();

} // namespace hc::ui::climate_panel::detail
