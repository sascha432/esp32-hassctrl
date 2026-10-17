#pragma once

// Display bring-up test (docs/adding-a-board.md). Plain LVGL. Only a board in
// bring-up shows it (CONFIG_HC_BOARD_BRING_UP, docs/adding-a-board.md); main shows it after
// hc::ui::start() and hands hide_test_screen() to the dashboard, so it lies over the start screen
// until a dashboard is drawn. Without the option nothing calls it and it is not linked.
//
// "Display" tab: color bars (checks the color order and the byte swap), a spinner and a bar that
// redraw small areas continuously, and the measured refresh times. Every two seconds the whole
// screen is redrawn once and the times are logged with LV_LOG_USER. With cycle_rotation the screen
// turns by 90 degrees every ten seconds, which exercises the rotation through the panel
// controller; without it the rotation is left to the caller (orientation sensor). A touch shows a
// dot and the coordinates, which checks the touch mapping in every rotation.
//
// "Backlight" tab: a brightness slider with a linear or perceptual curve, the PWM frequency, the
// minimum brightness test (the lowest raw duty at which the screen is still readable), and gray
// and color ramps to judge contrast at every brightness. These are the backlight steps of
// docs/adding-a-board.md.

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace hc::ui {

// access to the board's backlight, see hc/board/board.hpp
struct BacklightControl {
    void (*set)(uint8_t percent, bool perceptual);
    void (*set_duty)(uint16_t duty);
    void (*set_minimum)(uint16_t duty);
    uint16_t (*minimum)();
    bool (*set_frequency)(uint32_t hz);
    uint16_t max_duty;
    // the board's frequency, preselected
    uint32_t frequency_hz;
};

struct TestScreenConfig {
    const char *board_name;
    bool cycle_rotation;
    // the backlight tab is only shown when set
    const BacklightControl *backlight;
    // network state as text, shown on the display tab and refreshed every second; nullptr hides it
    void (*network_text)(char *out, size_t size);
};

// Call with the LVGL lock held, after hc::ui::start().
void show_test_screen(lv_disp_t *disp, const TestScreenConfig &config);

// Removes the test screen, its timers and its refresh measurement (the dashboard takes the screen).
void hide_test_screen();

} // namespace hc::ui
