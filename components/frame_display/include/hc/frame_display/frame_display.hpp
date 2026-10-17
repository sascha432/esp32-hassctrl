#pragma once

// LVGL display for panels that only take whole frames, written from the top (docs/architecture.md
// section 8, hc::board::Info::full_frames): the AXS15231B over QSPI has no row address, and it
// cannot rotate while it takes a frame (MADCTL MV gave an empty screen).
//
// LVGL renders in direct mode: only what changed, at its place in one screen-sized frame in PSRAM.
// After each refresh the frame goes to the panel top to bottom in bands, turned to the panel's
// orientation while they are copied into two DMA buffers in internal RAM: one band is copied while
// the other is sent. Rotation is therefore software (lv_disp_set_rotation).

#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

namespace hc::frame_display {

struct Config {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    // the panel in its own orientation, which is LVGL's rotation 0
    uint16_t width;
    uint16_t height;
    // lines of one band, each of the two band buffers takes width x lines pixels of internal RAM
    uint16_t band_lines;
};

// Registers the display with LVGL. Call after lvgl_port_init(), with the LVGL lock held. Returns
// nullptr when the memory is missing.
lv_disp_t *add(const Config &config);

} // namespace hc::frame_display
