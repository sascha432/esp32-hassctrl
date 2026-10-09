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

// How the screens move and look on a board type (board::Info says which, the sim's --size preset)
struct Options {
    // a page of the dashboard slides in from the right when it opens and out to the right when
    // the back tile leads to its parent (docs/ui/dashboard.md section 1)
    bool slide_pages = false;
    // the panels of a dimmer, a climate and a sensor are a dialog over the dimmed page, 620 x 420
    // (440 x 600 in portrait), fading in and out, instead of the whole screen (docs/ui/dashboard.md
    // section 6)
    bool panel_dialogs = false;
    // every text about 28 % larger (Montserrat 16 to 36 instead of 12 to 28); the board's
    // sdkconfig.defaults has to build the sizes 18, 26 and 36 in
    bool large_fonts = false;
    // the gap between the tiles of the dashboard in pixels (the JC4880P443C's 800x480 has 8)
    uint8_t tile_gap = 4;
};

// Builds the dashboard, the pull-down and the alert strip. Call with the LVGL lock held. The
// backend has to outlive the UI. `orientation_sensor`: the board turns the screen by a sensor
// (sensor_rotation()), else the rotation is set in Settings.
void start(lv_disp_t *disp, const Backend &backend, const TestScreenConfig &test_screen, bool orientation_sensor,
           const Options &options = {});

// Shows the first setup (it also comes by itself while no network is saved). From any task; it
// opens in the LVGL task within half a second. For the development console.
void show_setup();

// The orientation sensor's rotation in LVGL steps, also before start(); taken over unless the
// rotation is locked in Settings. Call with the LVGL lock held.
void sensor_rotation(uint8_t rotation);

} // namespace hc::ui
