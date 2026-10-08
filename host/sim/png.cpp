#include "png.hpp"

#include <cstdio>
#include <vector>

namespace sim {

namespace {

uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

void put32(std::vector<uint8_t> &out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
    put32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(body.data(), body.size()));
}

} // namespace

bool write_png(const char *path, const lv_color_t *frame, int width, int height)
{
    // one filter byte (0: none) and RGB per row
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 3));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        for (int x = 0; x < width; x++) {
            const uint16_t c = frame[y * width + x].full;
            const uint8_t r = static_cast<uint8_t>((c >> 11) & 0x1f);
            const uint8_t g = static_cast<uint8_t>((c >> 5) & 0x3f);
            const uint8_t b = static_cast<uint8_t>(c & 0x1f);
            raw.push_back(static_cast<uint8_t>((r << 3) | (r >> 2)));
            raw.push_back(static_cast<uint8_t>((g << 2) | (g >> 4)));
            raw.push_back(static_cast<uint8_t>((b << 3) | (b >> 2)));
        }
    }

    // zlib stream of stored blocks
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size();) {
        const size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                 raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<uint8_t> header;
    put32(header, static_cast<uint32_t>(width));
    put32(header, static_cast<uint32_t>(height));
    header.insert(header.end(), {8, 2, 0, 0, 0}); // 8 bit RGB, no interlace
    chunk(file, "IHDR", header);
    chunk(file, "IDAT", z);
    chunk(file, "IEND", {});

    FILE *f = std::fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    return std::fclose(f) == 0 && ok;
}

} // namespace sim
