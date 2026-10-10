#pragma once

// Display rotation (Settings, Display). With an orientation sensor the screen follows it unless
// the rotation is locked; without one the rotation is set by hand. "Rotate" turns the screen 90
// degrees clockwise and, with a sensor, locks the rotation, so the sensor does not turn it back.
// The rotation and the lock are kept in the display settings.

#include "hc/ui/backend.hpp"
#include "lvgl.h"

namespace hc::ui::rotation {

// Applies the stored rotation when there is no sensor or the rotation is locked.
void init(lv_disp_t *disp, const Backend &backend, bool sensor);

// the sensor's rotation (LVGL steps); taken over unless locked
void from_sensor(uint8_t rotation);

// the stored rotation and lock changed: shows the stored rotation unless the sensor's applies
void settings_changed();

void rotate_clockwise();
void set_locked(bool locked);

bool has_sensor();
bool locked();
// the rotation shown now in degrees clockwise of the panel's base orientation
uint16_t degrees();

} // namespace hc::ui::rotation
