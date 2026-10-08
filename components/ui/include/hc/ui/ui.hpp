#pragma once

// The screens of the panel (docs/ui/screen.md). Plain LVGL, no ESP-IDF drivers; everything else
// comes through the Backend.
//
// The dashboard shows the pages of the layout (the test screen while there is none); the pull-down
// at the top center opens Settings, and the alert strip shows problems and the one-time hint.

#include "hc/ui/backend.hpp"
#include "hc/ui/test_screen.hpp"
#include "lvgl.h"

namespace hc::ui {

// Builds the dashboard, the pull-down and the alert strip. Call with the LVGL lock held. The
// backend has to outlive the UI. `orientation_sensor`: the board turns the screen by a sensor
// (sensor_rotation()), else the rotation is set in Settings.
void start(lv_disp_t *disp, const Backend &backend, const TestScreenConfig &test_screen, bool orientation_sensor);

// Shows the first setup (it also comes by itself while no network is saved). From any task; it
// opens in the LVGL task within half a second. For the development console.
void show_setup();

// The orientation sensor's rotation in LVGL steps, also before start(); taken over unless the
// rotation is locked in Settings. Call with the LVGL lock held.
void sensor_rotation(uint8_t rotation);

} // namespace hc::ui
