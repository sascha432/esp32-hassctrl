#pragma once

// The touch sounds (docs/ui/screen.md section 5, Sound), each with its own switch in Settings:
// - a click for every tap on a tap target (Backend::play_touch_sound). Tap targets are LVGL's
//   input widgets (button, switch, slider, arc, button matrix and the keyboard) and the plain
//   objects marked with tappable(). A tap on a container, a backdrop or the screen, and the end
//   of a drag, make none;
// - ticks while a slider or an arc moves, one per 1/25 of its range, and while a drag control
//   marked with draggable() moves, one per 14 px (Backend::play_slider_tick): the faster it
//   moves, the more ticks.
// The vibration motor pulses with every tap that clicks (Backend::vibrate_click), with a switch of
// its own; it follows neither the volume nor the click's switch. Sliders do not vibrate.

#include "hc/ui/backend.hpp"
#include "lvgl.h"

namespace hc::ui::sound {

// Hooks the pointer devices LVGL has now. Call after the touch device is added.
void init(const Backend &backend);

// the backend has the audio output: Settings show the volume and the touch sounds
bool available();

// the backend has a vibration motor: Settings show its switch
bool vibration_available();

// Settings show the Sound page: audio or a motor
inline bool page_available()
{
    return available() || vibration_available();
}

// the sound settings changed (Settings, Sound)
void settings_changed();

// `obj`, a plain lv_obj, is a tap target: a tile, a row of Settings, a button made of a plain object
void tappable(lv_obj_t *obj);

// `obj`, a plain lv_obj, is a drag control that sets a value (the brightness bar of a light)
void draggable(lv_obj_t *obj);

} // namespace hc::ui::sound
