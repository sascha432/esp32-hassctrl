#pragma once

// Screen power (Settings, Display): the backlight follows the idle time of the touch screen.
//
//   Active   the user's brightness
//   Dimmed   after `dim_after_s` without a touch: the dimmed brightness. A touch brightens the
//            screen and works as usual, the screen is still readable.
//   Off      after `off_after_s` without a touch: backlight off. The touch that wakes the screen
//            only wakes it; it does not reach what is below the finger.
//
// Like the old firmware, dimming and turning off fade over 3 s, waking fades up in 0.5 s.
//
// Automatic brightness (with a light sensor): the active brightness follows the ambient light on
// a linear curve, from 1 % in the dark to the user's brightness at kFullBrightnessLux,
// smoothed over about two seconds. Later: Home Assistant events may wake the screen.

#include "hc/ui/backend.hpp"
#include "lvgl.h"

namespace hc::ui::power {

// The light at which the automatic brightness reaches the user's brightness; half of it gives
// half of the user's brightness.
constexpr float kFullBrightnessLux = 350.0f;

void init(lv_disp_t *disp, const Backend &backend);

// the settings changed (Display page): applies them at once
void settings_changed();

// shows `percent` while a brightness slider is dragged, until settings_changed()
void preview(uint8_t percent);

// the backlight level set now, in percent
uint8_t backlight();

} // namespace hc::ui::power
