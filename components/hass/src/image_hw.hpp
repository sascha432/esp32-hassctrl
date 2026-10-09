#pragma once

// The image hardware of the ESP32-P4 for the media task (media.cpp): the JPEG decoder (an 800x480
// frame in 17 ms instead of 140 ms in software) and the PPA, which scales a frame to the size it is
// shown at (LVGL's zoom took 100 ms a frame). On other chips both answer false and the software
// does it. The decoder decodes at full size only; what it cannot decode (a progressive JPEG) goes
// to the software decoder.

#include <cstddef>
#include <cstdint>

#include "hc/core/image.hpp"

namespace hc::hass::image_hw {

// A picture at its full size. The decoder writes whole blocks of 8 or 16 pixels: the frame is
// `width` x `height`, the picture in it `picture_width` x `picture_height` (the rest repeats its
// edge).
struct Frame {
    core::Pixels pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t picture_width = 0;
    uint32_t picture_height = 0;
};

// false with `error` set when the chip has no decoder or it cannot take this JPEG.
// `swap_bytes`: RGB565 with the high byte first (LV_COLOR_16_SWAP).
bool decode(const uint8_t *jpeg, size_t size, bool swap_bytes, Frame &frame, char *error, size_t error_size);

// The picture of `frame` scaled to cover `width` x `height` (centered, what sticks out cut off;
// core::cover_block), into `out`. false with `error` set when the chip has no PPA or it failed.
bool cover(const Frame &frame, uint32_t width, uint32_t height, core::Pixels &out, char *error, size_t error_size);

} // namespace hc::hass::image_hw
