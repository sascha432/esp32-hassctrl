#include "hc/core/image.hpp"

#include <algorithm>

namespace hc::core {

Pixels allocate_pixels(size_t count)
{
    auto *data = static_cast<uint16_t *>(memory::allocate_large(count * sizeof(uint16_t)));
    if (data == nullptr) {
        return {};
    }
    return Pixels(data, [](uint16_t *p) { memory::release_large(p); });
}

bool mjpeg_content_length(std::string_view line, size_t &length)
{
    constexpr std::string_view kName = "content-length:";
    if (line.size() <= kName.size()) {
        return false;
    }
    for (size_t i = 0; i < kName.size(); i++) {
        const char c = line[i];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != kName[i]) {
            return false;
        }
    }
    size_t value = 0;
    bool digits = false;
    for (size_t i = kName.size(); i < line.size(); i++) {
        const char c = line[i];
        if (c >= '0' && c <= '9') {
            if (value > 100000000) {
                return false;
            }
            value = value * 10 + static_cast<size_t>(c - '0');
            digits = true;
        } else if (c != ' ' && c != '\t' && c != '\r') {
            return false;
        }
    }
    length = value;
    return digits;
}

uint8_t cover_shift(uint32_t source_width, uint32_t source_height, uint32_t box_width, uint32_t box_height)
{
    uint8_t shift = 0;
    while (shift < 3 && scaled_size(source_width, static_cast<uint8_t>(shift + 1)) >= box_width &&
           scaled_size(source_height, static_cast<uint8_t>(shift + 1)) >= box_height) {
        shift++;
    }
    return shift;
}

uint8_t stream_shift(uint32_t source_width, uint32_t source_height, uint32_t box_width, uint32_t box_height)
{
    uint8_t shift = 0;
    for (; shift < 3; shift++) {
        const uint64_t width = scaled_size(source_width, shift);
        const uint64_t height = scaled_size(source_height, shift);
        const uint64_t shown = std::min<uint64_t>(width, box_width) * std::min<uint64_t>(height, box_height);
        if (shown * 4 >= width * height * 3) {
            break;
        }
    }
    return shift;
}

CoverBlock cover_block(uint32_t frame_width, uint32_t frame_height, uint32_t box_width, uint32_t box_height,
                       uint32_t steps)
{
    CoverBlock block;
    if (frame_width == 0 || frame_height == 0 || box_width == 0 || box_height == 0 || steps == 0) {
        return block;
    }
    const auto up = [](uint64_t a, uint64_t b) { return static_cast<uint32_t>((a + b - 1) / b); };
    block.scale = std::max<uint32_t>({up(static_cast<uint64_t>(box_width) * steps, frame_width),
                            up(static_cast<uint64_t>(box_height) * steps, frame_height), 1});
    // as much of the frame as the box takes at this scale (rounded down: never more than the box)
    block.width = std::min<uint32_t>(frame_width, static_cast<uint32_t>(static_cast<uint64_t>(box_width) * steps / block.scale));
    block.height =
        std::min<uint32_t>(frame_height, static_cast<uint32_t>(static_cast<uint64_t>(box_height) * steps / block.scale));
    block.x = (frame_width - block.width) / 2;
    block.y = (frame_height - block.height) / 2;
    block.out_width = static_cast<uint32_t>(static_cast<uint64_t>(block.width) * block.scale / steps);
    block.out_height = static_cast<uint32_t>(static_cast<uint64_t>(block.height) * block.scale / steps);
    block.out_x = (box_width - block.out_width) / 2;
    block.out_y = (box_height - block.out_height) / 2;
    return block;
}

CoverBlock whole_block(uint32_t frame_width, uint32_t frame_height, uint32_t box_width, uint32_t box_height,
                       uint32_t steps, uint32_t max_pixels)
{
    CoverBlock block = cover_block(frame_width, frame_height, box_width, box_height, steps);
    if (block.scale == 0) {
        return block;
    }
    // the largest scale whose frame has at most max_pixels: scale^2 * w * h <= max * steps^2
    while (max_pixels > 0 && block.scale > 1 &&
           static_cast<uint64_t>(frame_width) * block.scale / steps * (static_cast<uint64_t>(frame_height) * block.scale / steps) >
               max_pixels) {
        block.scale--;
    }
    block.x = block.y = 0;
    block.width = frame_width;
    block.height = frame_height;
    block.out_x = block.out_y = 0;
    block.out_width = static_cast<uint32_t>(static_cast<uint64_t>(frame_width) * block.scale / steps);
    block.out_height = static_cast<uint32_t>(static_cast<uint64_t>(frame_height) * block.scale / steps);
    return block;
}

bool cover_size8(uint32_t picture_width, uint32_t picture_height, uint32_t box_width, uint32_t box_height,
                 uint32_t &width, uint32_t &height)
{
    if (picture_width == 0 || picture_height == 0 || box_width == 0 || box_height == 0) {
        return false;
    }
    const auto up8 = [](uint64_t value) { return static_cast<uint32_t>((value + 7) / 8 * 8); };
    // the scale is the larger of the two ratios: whichever side reaches the box first
    const bool by_width = static_cast<uint64_t>(box_width) * picture_height >= static_cast<uint64_t>(box_height) * picture_width;
    uint32_t w = 0;
    uint32_t h = 0;
    if (by_width) {
        w = up8(box_width);
        h = up8((static_cast<uint64_t>(picture_height) * box_width + picture_width - 1) / picture_width);
    } else {
        h = up8(box_height);
        w = up8((static_cast<uint64_t>(picture_width) * box_height + picture_height - 1) / picture_height);
    }
    // not larger: as it is; at most 1/8
    if (w >= picture_width || h >= picture_height) {
        return false;
    }
    w = std::max(w, up8((picture_width + 7) / 8));
    h = std::max(h, up8((picture_height + 7) / 8));
    width = w;
    height = h;
    return true;
}

void shrink(const uint16_t *source, uint32_t source_width, uint32_t source_height, uint32_t stride, uint8_t shift,
            uint16_t *out)
{
    const uint32_t width = scaled_size(source_width, shift);
    const uint32_t height = scaled_size(source_height, shift);
    for (uint32_t y = 0; y < height; y++) {
        const uint16_t *row = source + static_cast<size_t>(y << shift) * stride;
        for (uint32_t x = 0; x < width; x++) {
            *out++ = row[x << shift];
        }
    }
}

void cover_copy(const uint16_t *source, uint32_t source_width, uint32_t source_height, uint16_t *box,
                uint32_t box_width, uint32_t box_height)
{
    if (source_width == 0 || source_height == 0 || box_width == 0 || box_height == 0) {
        return;
    }
    // the part of the image with the box's aspect ratio, as large as fits
    uint32_t crop_w = static_cast<uint32_t>(static_cast<uint64_t>(source_height) * box_width / box_height);
    uint32_t crop_h = source_height;
    if (crop_w > source_width) {
        crop_w = source_width;
        crop_h = static_cast<uint32_t>(static_cast<uint64_t>(source_width) * box_height / box_width);
    }
    crop_w = std::max<uint32_t>(crop_w, 1);
    crop_h = std::max<uint32_t>(crop_h, 1);
    const uint32_t crop_x = (source_width - crop_w) / 2;
    const uint32_t crop_y = (source_height - crop_h) / 2;
    // The source column of every column of the box, once per frame: a division per pixel made a
    // 480x320 frame take about 230 ms on the ESP32. Sizes up to 65535 keep the products in 32 bits.
    memory::Vector<uint32_t> columns(box_width);
    for (uint32_t x = 0; x < box_width; x++) {
        columns[x] = std::min(crop_x + (x * crop_w + crop_w / 2) / box_width, source_width - 1);
    }
    for (uint32_t y = 0; y < box_height; y++) {
        const uint32_t sy = std::min(crop_y + (y * crop_h + crop_h / 2) / box_height, source_height - 1);
        const uint16_t *row = source + static_cast<size_t>(sy) * source_width;
        uint16_t *out = box + static_cast<size_t>(y) * box_width;
        if (crop_w == box_width) {
            // 1:1 across: one copy of the row
            std::copy(row + crop_x, row + crop_x + box_width, out);
            continue;
        }
        for (uint32_t x = 0; x < box_width; x++) {
            out[x] = row[columns[x]];
        }
    }
}

} // namespace hc::core
