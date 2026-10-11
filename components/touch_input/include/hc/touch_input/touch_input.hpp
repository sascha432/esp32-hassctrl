#pragma once

// The touch screen as LVGL's pointer, read by a task of its own (docs/architecture.md section 8).
//
// esp_lvgl_port reads the touch controller in the LVGL task, every 30 ms, and not at all while
// LVGL renders: a quick tap that began and ended during a long render (a page being built) was
// never seen (2026-10-09, quick taps on the boards). Here a task samples the controller every
// 10 ms and queues every change; LVGL's read takes them in order, all of them in one read when
// several are waiting (continue_reading), so a press and its release always arrive.
//
// The coordinates are the controller's (the panel's own orientation), as with esp_lvgl_port:
// LVGL turns them with the display's rotation.
//
// A touch controller inside the panel controller (hc::board::Info::touch_in_panel, the AXS15231B
// of the JC3248W535) is read in the LVGL task instead (Config::in_lvgl_task), as esp_lvgl_port's
// reader did: read by this task it reported points all over the screen, and with the reads kept
// between frames none at all (2026-10-09).
//
// LVGL's pointer is one finger, the one that came down first. The UI asks for both fingers of a
// two-finger gesture with fingers() (the camera's pinch to zoom).

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

namespace hc::touch_input {

struct Config {
    lv_disp_t *disp;
    esp_lcd_touch_handle_t touch;
    // the controller's coordinates go up to these (the panel's own size less 1); a point beyond
    // them is a misread and dropped
    uint16_t max_x;
    uint16_t max_y;
    // read in LVGL's read callback instead of a task of its own (every 30 ms, none during a render)
    bool in_lvgl_task = false;
};

// Adds the pointer and starts the sampling task. Call with the LVGL lock held, once. nullptr when
// it could not.
lv_indev_t *add(const Config &config);

// The fingers on the screen now (0 to 2), in the display's coordinates (turned as LVGL's pointer
// is). Call from the LVGL task.
uint8_t fingers(lv_point_t out[2]);

// Prints every sample of the controller that differs from the one before and every failed read
// (console `touch log`), for finding out what a controller reports.
void set_trace(bool on);
bool trace();

} // namespace hc::touch_input
