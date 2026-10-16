#pragma once

// Screen power (Settings, Display): the backlight follows the idle time of the touch screen.
// With a motion sensor and "Wake on motion" motion counts as a touch: it brightens a dimmed screen,
// turns it on (and the next touch works as usual) and keeps it on while the sensor sees motion.
//
//   Active   the user's brightness
//   Dimmed   after `dim_after_s` without a touch: the dimmed brightness. A touch brightens the
//            screen and works as usual, the screen is still readable.
//   Off      after `off_after_s` without a touch: backlight off. The touch that wakes the screen
//            only wakes it; it does not reach what is below the finger.
//
// Like the old firmware, dimming and turning off fade over 3 s, waking fades up in 0.5 s.
//
// The backlight stays off from the start until the first frame is on the panel, then fades up in
// 0.5 s; before it the panel would show what it held before the start.
//
// Automatic brightness (with a light sensor): the active brightness follows the ambient light on
// a linear curve between two lights the user sets (DisplaySettings::light_min_lux: 1 %,
// light_max_lux: the user's brightness), smoothed over about two seconds. Later: Home Assistant events may wake the screen.

#include "hc/ui/backend.hpp"
#include "lvgl.h"

namespace hc::ui::power {

void init(lv_disp_t *disp, const Backend &backend);

// the settings changed (Display page): applies them at once
void settings_changed();

// shows `percent` while a brightness slider is dragged, until settings_changed()
void preview(uint8_t percent);

// The screen at the user's brightness now, as after a touch; the touch after it is not only a
// wake-up touch (the alarm).
void wake();

// the backlight level set now, in percent
uint8_t backlight();

} // namespace hc::ui::power
