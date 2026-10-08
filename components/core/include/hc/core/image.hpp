#pragma once

// The camera image of a picture tile (docs/ui/dashboard.md section 6): RGB565 frames the size of
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
    memory::String error; // empty: the frame is there
    bool live = false;    // a frame of a stream that plays (the next one is decoded once this is taken)
};

// A header line of a part of an MJPEG stream (multipart/x-mixed-replace): true with `length` set
// for "Content-Length: 19993" (any case, spaces allowed).
bool mjpeg_content_length(std::string_view line, size_t &length);

// The decoder's scale as a shift (0: 1:1 ... 3: 1:8): the smallest image that still covers the
// box, so the frame is cut from the fewest pixels without being enlarged.
uint8_t cover_shift(uint32_t source_width, uint32_t source_height, uint32_t box_width, uint32_t box_height);

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
