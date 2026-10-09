#pragma once

// Espressif's esp_new_jpeg decoder for the media task (media.cpp), built in on the ESP32-S3 only
// (components/hass/idf_component.yml): it uses the S3's SIMD instructions, a 640x320 frame in about
// a quarter of TJpgDec's 148 ms. It scales while it decodes, to any size down to 1/8 whose sides
// are multiples of 8: a frame is decoded at the smallest such size that still covers the box, so
// LVGL shows it without its zoom. Without the component every call answers false.

#include <cstddef>
#include <cstdint>

#include "hc/core/image.hpp"

namespace hc::hass::new_jpeg {

// `jpeg` into `out` (`width` x `height`, RGB565; `swap_bytes`: high byte first): at least
// `box_width` x `box_height` where the picture is larger, else at its own size. false with `error`
// set when the decoder is not built in or cannot take the JPEG (then TJpgDec does).
bool decode(const uint8_t *jpeg, size_t size, bool swap_bytes, uint32_t box_width, uint32_t box_height,
            core::Pixels &out, uint32_t &width, uint32_t &height, uint32_t &picture_width, uint32_t &picture_height,
            char *error, size_t error_size);

} // namespace hc::hass::new_jpeg
