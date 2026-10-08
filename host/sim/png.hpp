#pragma once

#include <cstdint>

#include "lvgl.h"

namespace sim {

// Writes the frame (RGB565, LV_COLOR_16_SWAP 0) as a PNG; false when the file cannot be written.
// Not compressed (stored deflate blocks): a screenshot is a few hundred KB and needs no zlib.
bool write_png(const char *path, const lv_color_t *frame, int width, int height);

} // namespace sim
