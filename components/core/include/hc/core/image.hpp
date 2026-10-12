#pragma once

// The camera image of a picture tile (docs/ui/lvgl/lvgl-camera.md): RGB565 frames the size of
// the tile or of the screen, cut from a larger JPEG. Plain C++, host tested; the decoding itself is
// in hass (ESP-IDF's JPEG decoder).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "hc/core/layout.hpp"
#include "hc/memory/large.hpp"

namespace hc::core {

// The pixels of a frame on the large heap, shared by whoever shows it, freed with the last owner.
using Pixels = std::shared_ptr<uint16_t>;

// `count` pixels, not initialized (the decoder writes every one; zeroing 300 KB of PSRAM costs
// time a stream does not have); null when the heap is full.
Pixels allocate_pixels(size_t count);

// A frame for the UI: `width` x `height` RGB565 pixels in the display's byte order. Shared, so
// the tile and its full screen view can show the same frame. `generation` is the one of the
// request; an answer to an older one is dropped.
struct Image {
    uint32_t generation = 0;
    EntityIndex entity = kNone;
    uint16_t width = 0;
    uint16_t height = 0;
    Pixels pixels;        // width x height
    // the picture the frame was cut from (only its shape counts): a tile's frame is its middle in
    // the tile's shape, the full screen view places it where it lies in the whole picture; 0: unknown
    uint16_t source_width = 0;
    uint16_t source_height = 0;
    memory::String error; // empty: the frame is there
    bool live = false;    // a frame of a stream that plays (the next one is decoded once this is taken)
};

// A header line of a part of an MJPEG stream (multipart/x-mixed-replace): true with `length` set
// for "Content-Length: 19993" (any case, spaces allowed).
bool mjpeg_content_length(std::string_view line, size_t &length);

// The decoder's scale as a shift (0: 1:1 ... 3: 1:8): the smallest image that still covers the
// box, so the frame is cut from the fewest pixels without being enlarged.
uint8_t cover_shift(uint32_t source_width, uint32_t source_height, uint32_t box_width, uint32_t box_height);

// The scale of a stream's frame, which is shown centered in the box with what sticks out cut off:
// the largest image that still shows at least 3/4 of the frame. A frame of another shape loses a
// strip (704x576 on 800x480), a large one is made smaller instead of showing its middle only
// (1280x960 at 1:2).
uint8_t stream_shift(uint32_t source_width, uint32_t source_height, uint32_t box_width, uint32_t box_height);

// The cover of a box by a scaler that scales in steps of 1/`steps` (the P4's PPA: 16): `scale`
// steps per pixel, the smallest that fills the box, and the block of the frame (centered) that is
// scaled. The scaled block (`out_width` x `out_height`) is at most the box; with the rounding of
// the scale it can be a pixel or two short, and it is placed at `out_x`, `out_y`.
struct CoverBlock {
    uint32_t scale = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t out_x = 0;
    uint32_t out_y = 0;
    uint32_t out_width = 0;
    uint32_t out_height = 0;
};

CoverBlock cover_block(uint32_t frame_width, uint32_t frame_height, uint32_t box_width, uint32_t box_height,
                       uint32_t steps);

// The same scale as cover_block, but for the whole frame: nothing is cut off, the scaled frame
// (`out_width` x `out_height`, at 0, 0) covers the box and sticks out of it on one side (a pixel
// or two short with the rounding). For the full screen view, which moves the picture around. At
// most `max_pixels` (0: any): a larger one is scaled less and does not cover the box.
CoverBlock whole_block(uint32_t frame_width, uint32_t frame_height, uint32_t box_width, uint32_t box_height,
                       uint32_t steps, uint32_t max_pixels = 0);

// The size a decoder that scales down to sides in multiples of 8 (esp_new_jpeg, at most 1/8)
// decodes a picture at to cover a box: the smallest such that is still as large as the box. false
// when the picture is not larger than the box (it is decoded as it is).
bool cover_size8(uint32_t picture_width, uint32_t picture_height, uint32_t box_width, uint32_t box_height,
                 uint32_t &width, uint32_t &height);

// `source` (source_width x source_height, rows `stride` pixels apart) made smaller by 1 << shift
// into `out` (scaled_size of both sides): every (1 << shift)-th pixel of every (1 << shift)-th row.
// For a hardware decoder that cannot scale.
void shrink(const uint16_t *source, uint32_t source_width, uint32_t source_height, uint32_t stride, uint8_t shift,
            uint16_t *out);

// The size the decoder gives for a shift (it rounds up).
constexpr uint32_t scaled_size(uint32_t size, uint8_t shift)
{
    return (size + (1u << shift) - 1) >> shift;
}

// Fills the box with the middle of the image: the image is scaled to cover the box (nearest
// neighbour, the center of a pixel decides) and what sticks out is cut off equally on both sides.
void cover_copy(const uint16_t *source, uint32_t source_width, uint32_t source_height, uint16_t *box,
                uint32_t box_width, uint32_t box_height);

} // namespace hc::core
