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
// A controller that reports less often than every 10 ms gets a longer period (Config::sample_ms):
// the AXS15231B of the JC3248W535 makes a report at 60 Hz and answers "no finger" to a read
// without a new one, so read every 10 ms a held finger was lifted and put down again and again
// (2026-10-17, patterns like 11.11.1.11).
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
    // the task's sample period: at least the controller's own report period
    uint16_t sample_ms = 10;
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
