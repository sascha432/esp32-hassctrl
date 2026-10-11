#include "new_jpeg.hpp"

#include <cstdio>

#include "internal.hpp"

#if __has_include("esp_jpeg_dec.h")
#define HC_NEW_JPEG 1
#include "esp_jpeg_dec.h"
#else
#define HC_NEW_JPEG 0
#endif

namespace hc::hass::new_jpeg {

#if HC_NEW_JPEG

namespace {

// a decoder for one picture, closed with it
struct Decoder {
    jpeg_dec_handle_t handle = nullptr;

    Decoder() = default;
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    ~Decoder()
    {
        if (handle != nullptr) {
            jpeg_dec_close(handle);
        }
    }
};

// opens `decoder` and reads the header of `jpeg` into `info`
bool open(Decoder &decoder, jpeg_dec_config_t &config, const uint8_t *jpeg, size_t size, jpeg_dec_io_t &io,
          jpeg_dec_header_info_t &info)
{
    if (jpeg_dec_open(&config, &decoder.handle) != JPEG_ERR_OK) {
        decoder.handle = nullptr;
        return false;
    }
    io = {};
    io.inbuf = const_cast<uint8_t *>(jpeg);
    io.inbuf_len = static_cast<int>(size);
    return jpeg_dec_parse_header(decoder.handle, &io, &info) == JPEG_ERR_OK && info.width != 0 && info.height != 0;
}

} // namespace

} // namespace hc::hass::new_jpeg

// The library's own buffers (linked with --wrap, CMakeLists.txt). It takes them from internal RAM
// first, whatever is left: 29 KB for a 176x144 camera image, which on the S3 would leave a few
// hundred bytes to Wi-Fi and lwIP. The output frame (jpeg_calloc_align) is PSRAM first and stays.
extern "C" void *__wrap_jpeg_calloc_align_inner(size_t size, int aligned)
{
    return hc::hass::detail::hot_alloc(size, aligned > 0 ? static_cast<size_t>(aligned) : 4);
}

extern "C" void *__wrap_jpeg_calloc_inner(size_t size)
{
    return hc::hass::detail::hot_alloc(size, 4);
}

namespace hc::hass::new_jpeg {

bool decode(const uint8_t *jpeg, size_t size, bool swap_bytes, uint32_t box_width, uint32_t box_height,
            core::Pixels &out, uint32_t &width, uint32_t &height, uint32_t &picture_width, uint32_t &picture_height,
            char *error, size_t error_size)
{
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = swap_bytes ? JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_io_t io{};
    jpeg_dec_header_info_t info{};
    // the header first: the scale follows from the picture's size
    {
        Decoder probe;
        if (!open(probe, config, jpeg, size, io, info)) {
            std::snprintf(error, error_size, "not a JPEG esp_new_jpeg knows");
            return false;
        }
    }
    picture_width = info.width;
    picture_height = info.height;
    uint32_t scaled_width = 0;
    uint32_t scaled_height = 0;
    if (info.width % 8 == 0 && info.height % 8 == 0 &&
        core::cover_size8(info.width, info.height, box_width, box_height, scaled_width, scaled_height)) {
        config.scale.width = static_cast<uint16_t>(scaled_width);
        config.scale.height = static_cast<uint16_t>(scaled_height);
    }
    Decoder decoder;
    if (!open(decoder, config, jpeg, size, io, info)) {
        std::snprintf(error, error_size, "esp_new_jpeg could not read the header");
        return false;
    }
    int length = 0;
    if (jpeg_dec_get_outbuf_len(decoder.handle, &length) != JPEG_ERR_OK || length <= 0) {
        std::snprintf(error, error_size, "esp_new_jpeg gave no size");
        return false;
    }
    // the decoder writes with 16-byte stores (SIMD): its own aligned allocation
    auto *data = static_cast<uint16_t *>(jpeg_calloc_align(static_cast<size_t>(length), 16));
    if (data == nullptr) {
        std::snprintf(error, error_size, "no memory for the frame (%d KB)", length / 1024);
        return false;
    }
    core::Pixels pixels(data, [](uint16_t *p) { jpeg_free_align(p); });
    io.outbuf = reinterpret_cast<uint8_t *>(data);
    if (jpeg_dec_process(decoder.handle, &io) != JPEG_ERR_OK) {
        std::snprintf(error, error_size, "esp_new_jpeg could not decode it");
        return false;
    }
    width = config.scale.width != 0 ? config.scale.width : info.width;
    height = config.scale.height != 0 ? config.scale.height : info.height;
    if (static_cast<size_t>(width) * height * 2 > static_cast<size_t>(length)) {
        std::snprintf(error, error_size, "esp_new_jpeg's frame is smaller than %ux%u", static_cast<unsigned>(width),
                      static_cast<unsigned>(height));
        return false;
    }
    out = std::move(pixels);
    return true;
}

#else

bool decode(const uint8_t *, size_t, bool, uint32_t, uint32_t, core::Pixels &, uint32_t &, uint32_t &, uint32_t &,
            uint32_t &, char *error, size_t error_size)
{
    std::snprintf(error, error_size, "esp_new_jpeg is not built in");
    return false;
}

#endif

} // namespace hc::hass::new_jpeg
