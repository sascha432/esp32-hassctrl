#pragma once

// Remote debugging of the screen (docs/architecture.md section 9.4, "remote_debug"): screenshots
// and touch injection, used over the serial console (CONFIG_HC_DEV_CONSOLE). Removable: nothing
// else depends on it.
//
// Screenshot format: RGB565 pixels, run-length encoded as pairs of a count (1 byte, 1..255) and a
// pixel (2 bytes, little endian, red in the top bits). The screen and the top layer (alert strip,
// overlays) are blended into one picture, in the current rotation.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"

namespace hc::uidebug {

// Registers the virtual pointer of the touch injection. Call with the LVGL lock held.
esp_err_t init(lv_disp_t *disp);

using Writer = void (*)(const void *data, size_t size, void *context);

// Takes a screenshot under the LVGL lock and hands the encoded pixels to `write` in pieces.
esp_err_t screenshot(Writer write, void *context, uint16_t &width, uint16_t &height);

// Presses or releases the virtual pointer at a point of the screen as it is shown now (the
// current rotation).
void touch(int16_t x, int16_t y, bool pressed);

} // namespace hc::uidebug
