#pragma once

// What the files of the light panel share (light_panel.cpp: the level, the effects, the mode
// buttons, the power button, build and update; light_color.cpp: the color wheel and the color
// temperature). One panel at a time: its state is `g`.

#include <cstdint>
#include <string_view>

#include "hc/core/pending.hpp"
#include "hc/memory/large.hpp"
#include "light_panel.hpp"
#include "lvgl.h"
#include "panel.hpp"

namespace hc::ui::light_panel::detail {

enum class Mode : uint8_t { Level, Color, Temp, Effect, Count };
constexpr int kModes = static_cast<int>(Mode::Count);

constexpr uint8_t bit(Mode mode)
{
    return static_cast<uint8_t>(1u << static_cast<int>(mode));
}

// the marker of the color temperature: a white bar this tall, this far from the track's sides
constexpr lv_coord_t kMarkerHeight = 12;
constexpr lv_coord_t kMarkerInset = 6;
constexpr lv_coord_t kMarkerRadius = 4;

// a vertical slider: a track with a fill (level) or a marker (color temperature) and the value in
// its middle, dragged like the dimmer tile
struct Slider {
    lv_obj_t *track = nullptr;
    lv_obj_t *fill = nullptr;
    lv_obj_t *marker = nullptr;
    lv_obj_t *value = nullptr;
    lv_coord_t height = 0; // as built: LVGL lays the objects out only before the next frame
    float min = 0;
    float max = 100;
    bool dragging = false;
    float drag_value = 0;
    void (*on_release)(float value) = nullptr;
};

struct State {
    panel::Frame frame;
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    lv_obj_t *power = nullptr;
    // the history button left of the power button: the timeline in the place of the control
    lv_obj_t *history = nullptr;
    bool show_history = false;
    lv_obj_t *buttons[kModes] = {};
    uint8_t offered = 0; // the modes the buttons are laid out for
    Mode mode = Mode::Level;
    lv_obj_t *control = nullptr; // the container of the control of `mode`

    // level
    Slider level;
    lv_obj_t *plus = nullptr;
    lv_obj_t *minus = nullptr;
    lv_obj_t *note = nullptr; // a light without a level: its state
    int step_level = -1;      // the level the steps have collected, -1: none
    lv_timer_t *send_timer = nullptr;
    uint32_t last_step = 0;

    // color
    lv_obj_t *wheel = nullptr;
    lv_obj_t *knob = nullptr;
    memory::Vector<lv_color_t> pixels;
    lv_coord_t wheel_size = 0;
    bool wheel_dragging = false;
    float drag_hue = 0;
    float drag_saturation = 0;

    // color temperature
    Slider temp;

    // effects
    lv_obj_t *list = nullptr;
    memory::Vector<memory::String> effects; // the list the items were made from
};

extern State g;

const core::EntityState &state();
bool available();
void send(core::IntentKind kind, float value, float value2, std::string_view text, core::Expectation expected);

// the vertical sliders of the level and the color temperature (light_panel.cpp)
void slider_show(Slider &s, float value, bool known);
Slider make_slider(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, float min, float max,
                   void (*on_release)(float));
void slider_events(Slider &s);
lv_obj_t *slider_label(Slider &s, uint32_t color);
panel::SliderPlace slider_place(lv_coord_t room, lv_coord_t h);

// ---------------------------------------------------------------------- light_color.cpp

void build_color(lv_coord_t w, lv_coord_t h);
void update_color(const core::EntityState &s, const core::Expectation *pending, bool avail);
void build_temp(lv_coord_t w, lv_coord_t h);
void update_temp(const core::EntityState &s, const core::Expectation *pending, bool avail);

} // namespace hc::ui::light_panel::detail
