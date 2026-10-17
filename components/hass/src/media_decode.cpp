// The JPEG decoders of the media worker: the hardware decoder and the PPA (ESP32-P4), esp_new_jpeg
// (ESP32-S3), TJpgDec (all). One decodes at a time (media.cpp holds the lock).

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "image_hw.hpp"
#include "internal.hpp"
#include "jpeg_decoder.h"
#include "media.hpp"
#include "new_jpeg.hpp"

namespace hc::hass::media {

namespace {

constexpr const char *kTag = "media";
// TJpgDec's work area: esp_jpeg's own 3100 bytes hold the usual tables only, a snapshot of Home
// Assistant (camera.balconycam) needed more (JDR_MEM1); with all four quantization and Huffman
// tables at their largest it takes about 6.2 KB
constexpr size_t kDecoderWork = 8192;

// the decoder's work area for one image (it reads its tables for every pixel)
struct DecoderWork {
    uint8_t *data = static_cast<uint8_t *>(detail::hot_alloc(kDecoderWork, 4));

    DecoderWork() = default;
    DecoderWork(const DecoderWork &) = delete;
    DecoderWork &operator=(const DecoderWork &) = delete;
    ~DecoderWork()
    {
        std::free(data);
    }
};

} // namespace

const char *decoder_name(const DecodeTimes &times)
{
    return times.scaled ? "hw+ppa" : times.hardware ? "hw" : times.simd ? "simd" : "sw";
}

namespace {

// The decoder's scale for a picture: a stream's frame shows most of the picture, a still image
// covers the request's box (core::stream_shift, core::cover_shift); `shift_wanted` >= 0 overrides
// it (the console's `hass streamscale`).
uint8_t scale_of(const Request &request, uint32_t width, uint32_t height, bool live, int shift_wanted)
{
    if (shift_wanted >= 0) {
        return static_cast<uint8_t>(std::min(shift_wanted, 3));
    }
    return live ? core::stream_shift(width, height, request.width, request.height)
                : core::cover_shift(width, height, request.width, request.height);
}

// The hardware decoder (ESP32-P4, image_hw): the picture at full size, scaled by the PPA to cover
// the request's size, so LVGL shows it as it is (its zoom took 100 ms a frame). With a scale from
// the console, or without a PPA, it is made smaller by core::shrink when the scale asks for it.
// False when there is no decoder or it cannot take the JPEG.
bool decode_hardware(const Request &request, const uint8_t *jpeg, size_t size, bool live, int shift_wanted,
                     core::Pixels &decoded, uint32_t &width, uint32_t &height, DecodeTimes &times)
{
    char error[64];
    image_hw::Frame frame;
    const int64_t started = esp_timer_get_time();
    if (!image_hw::decode(jpeg, size, request.swap_bytes, frame, error, sizeof(error))) {
        ESP_LOGD(kTag, "%s: %s, decoded in software", request.entity_id.c_str(), error);
        return false;
    }
    const int64_t decoded_at = esp_timer_get_time();
    times.decode_us = decoded_at - started;
    times.source_width = static_cast<uint16_t>(frame.picture_width);
    times.source_height = static_cast<uint16_t>(frame.picture_height);
    times.hardware = true;
    const bool fits = frame.width == request.width && frame.height == request.height &&
                      frame.picture_width == frame.width && frame.picture_height == frame.height;
    if (shift_wanted < 0 && !fits) {
        if (image_hw::cover(frame, request.width, request.height, live || request.whole, decoded, width, height, error,
                            sizeof(error))) {
            times.scaled = true;
            times.copy_us = esp_timer_get_time() - decoded_at;
            return true;
        }
        ESP_LOGD(kTag, "%s: %s", request.entity_id.c_str(), error);
    }
    const uint8_t shift = scale_of(request, frame.picture_width, frame.picture_height, live, shift_wanted);
    times.shift = shift;
    if (shift == 0) {
        // whole blocks: their edge beyond the picture repeats it
        decoded = std::move(frame.pixels);
        width = frame.width;
        height = frame.height;
        return true;
    }
    width = core::scaled_size(frame.picture_width, shift);
    height = core::scaled_size(frame.picture_height, shift);
    decoded = core::allocate_pixels(static_cast<size_t>(width) * height);
    if (!decoded) {
        return false;
    }
    core::shrink(frame.pixels.get(), frame.picture_width, frame.picture_height, frame.width, shift, decoded.get());
    times.copy_us = esp_timer_get_time() - decoded_at;
    return true;
}

// esp_new_jpeg (ESP32-S3, new_jpeg): a quarter of TJpgDec's time, scaled while it decodes to the
// smallest size that covers the request's (sides in multiples of 8), so a stream's frame needs no
// zoom of LVGL. Not with a scale from the console. False when it is not built in or cannot take
// the JPEG.
bool decode_simd(const Request &request, const uint8_t *jpeg, size_t size, int shift_wanted, core::Pixels &decoded,
                 uint32_t &width, uint32_t &height, DecodeTimes &times)
{
    if (shift_wanted >= 0) {
        return false;
    }
    char error[64];
    uint32_t picture_width = 0;
    uint32_t picture_height = 0;
    const int64_t started = esp_timer_get_time();
    if (!new_jpeg::decode(jpeg, size, request.swap_bytes, request.width, request.height, decoded, width, height,
                          picture_width, picture_height, error, sizeof(error))) {
        ESP_LOGD(kTag, "%s: %s", request.entity_id.c_str(), error);
        return false;
    }
    times.decode_us = esp_timer_get_time() - started;
    times.source_width = static_cast<uint16_t>(picture_width);
    times.source_height = static_cast<uint16_t>(picture_height);
    times.simd = true;
    return true;
}

// The software decoder (esp_jpeg, TJpgDec), which scales while it decodes.
bool decode_software(const Request &request, const uint8_t *jpeg, size_t size, bool live, int shift_wanted,
                     core::Pixels &decoded, uint32_t &width, uint32_t &height, DecodeTimes &times, char *error,
                     size_t error_size)
{
    const DecoderWork work_area;
    uint8_t *work = work_area.data;
    if (work == nullptr) {
        std::snprintf(error, error_size, "no memory for the decoder");
        return false;
    }
    esp_jpeg_image_cfg_t config{};
    config.indata = const_cast<uint8_t *>(jpeg);
    config.indata_size = static_cast<uint32_t>(size);
    config.out_format = JPEG_IMAGE_FORMAT_RGB565;
    config.advanced.working_buffer = work;
    config.advanced.working_buffer_size = kDecoderWork;
    esp_jpeg_image_output_t info{};
    if (esp_jpeg_get_image_info(&config, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        std::snprintf(error, error_size, "not a JPEG the decoder knows (a progressive one?)");
        return false;
    }
    const uint8_t shift = scale_of(request, info.width, info.height, live, shift_wanted);
    times.source_width = info.width;
    times.source_height = info.height;
    times.shift = shift;
    times.hardware = false;
    const uint32_t out_width = core::scaled_size(info.width, shift);
    const uint32_t out_height = core::scaled_size(info.height, shift);
    const size_t bytes = static_cast<size_t>(out_width) * out_height * 2;
    decoded = core::allocate_pixels(static_cast<size_t>(out_width) * out_height);
    if (!decoded) {
        std::snprintf(error, error_size, "no memory for the decoded image (%u KB)", static_cast<unsigned>(bytes / 1024));
        return false;
    }
    config = {};
    config.indata = const_cast<uint8_t *>(jpeg);
    config.indata_size = static_cast<uint32_t>(size);
    config.outbuf = reinterpret_cast<uint8_t *>(decoded.get());
    config.outbuf_size = static_cast<uint32_t>(bytes);
    config.out_format = JPEG_IMAGE_FORMAT_RGB565;
    config.out_scale = static_cast<esp_jpeg_image_scale_t>(shift);
    config.flags.swap_color_bytes = request.swap_bytes ? 1 : 0;
    config.advanced.working_buffer = work;
    config.advanced.working_buffer_size = kDecoderWork;
    const int64_t started = esp_timer_get_time();
    if (esp_jpeg_decode(&config, &info) != ESP_OK) {
        std::snprintf(error, error_size, "the JPEG could not be decoded");
        return false;
    }
    times.decode_us = esp_timer_get_time() - started;
    width = info.width;
    height = info.height;
    return true;
}

} // namespace

// The JPEG into a frame, in hardware where the chip has a decoder, else in software. A still image
// is cut to the request's size (the tile keeps a small frame); a stream's frame (`live`) and a
// whole picture are the decoder's output as it is: the UI scales it to cover its box and the
// screen cuts off what sticks out, which saves a copy of the whole frame through PSRAM (166 ms at
// 480x320 on the ESP32).
bool decode(const Request &request, const uint8_t *jpeg, size_t size, bool live, int shift_wanted, core::Image &image,
            DecodeTimes &times, char *error, size_t error_size)
{
    times = {};
    core::Pixels decoded;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!decode_hardware(request, jpeg, size, live, shift_wanted, decoded, width, height, times) &&
        !decode_simd(request, jpeg, size, shift_wanted, decoded, width, height, times) &&
        !decode_software(request, jpeg, size, live, shift_wanted, decoded, width, height, times, error, error_size)) {
        return false;
    }
    image.source_width = times.source_width;
    image.source_height = times.source_height;
    if (live || request.whole || times.scaled) {
        // a stream's frame or a whole picture as it is; the PPA's needs no cut either
        image.width = static_cast<uint16_t>(width);
        image.height = static_cast<uint16_t>(height);
        image.pixels = std::move(decoded);
        return true;
    }
    const int64_t copy_started = esp_timer_get_time();
    core::Pixels pixels = core::allocate_pixels(static_cast<size_t>(request.width) * request.height);
    if (!pixels) {
        std::snprintf(error, error_size, "no memory for the image");
        return false;
    }
    core::cover_copy(decoded.get(), width, height, pixels.get(), request.width, request.height);
    times.copy_us += esp_timer_get_time() - copy_started;
    image.width = request.width;
    image.height = request.height;
    image.pixels = std::move(pixels);
    return true;
}

} // namespace hc::hass::media

namespace hc::hass::detail {

void *hot_alloc(size_t size, size_t align)
{
    // what the others need while a decoder holds its buffers: the S3 had 24 KB free with the media
    // task, the P4 239 KB
    constexpr size_t kInternalReserve = 32 * 1024;
    constexpr uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    align = std::max<size_t>(align, 4);
    // the largest block too: a request that cannot fit is not even tried (the failure hook logs it)
    if (heap_caps_get_free_size(kInternal) >= size + kInternalReserve &&
        heap_caps_get_largest_free_block(kInternal) >= size + align) {
        if (void *data = heap_caps_aligned_calloc(align, 1, size, kInternal)) {
            return data;
        }
    }
    return heap_caps_aligned_calloc(align, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

} // namespace hc::hass::detail
