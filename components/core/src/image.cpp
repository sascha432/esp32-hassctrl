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
