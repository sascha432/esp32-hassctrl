#pragma once

// The preview of Settings, Camera (hc::ui::Backend::camera_preview_start): the camera's picture in
// a box of the screen, drawn past LVGL into the MIPI-DSI panel's frame buffers.

#include <cstdint>

#include "esp_lcd_types.h"
#include "lvgl.h"

// Takes over LVGL's flush of `disp` and the DPI panel's callbacks (the panel needs two frame
// buffers); nothing changes until a preview starts. Call with the LVGL lock held, after
// hc::camera::start() found a sensor.
void init_camera_preview(lv_disp_t *disp, esp_lcd_panel_handle_t panel);

// The picture in the box x1, y1 to x2, y2 (LVGL's coordinates, inclusive) until
// camera_preview_stop(); a new start moves the box. From the LVGL task.
void camera_preview_start(int16_t x1, int16_t y1, int16_t x2, int16_t y2);
void camera_preview_stop();
