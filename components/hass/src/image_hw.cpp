#include "image_hw.hpp"

#include <cstdio>
#include <cstring>

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_JPEG_DECODE_SUPPORTED || SOC_PPA_SUPPORTED
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif
#if SOC_JPEG_DECODE_SUPPORTED
#include "driver/jpeg_decode.h"
#endif
#if SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#include "esp_cache.h"
#endif

namespace hc::hass::image_hw {

#if SOC_JPEG_DECODE_SUPPORTED || SOC_PPA_SUPPORTED
namespace {
constexpr const char *kTag = "image_hw";
} // namespace
#endif

// ------------------------------------------------------------------------------- JPEG decoder

#if SOC_JPEG_DECODE_SUPPORTED

namespace {

// a frame takes milliseconds; a broken one is given up after this
constexpr int kTimeoutMs = 500;

// the engine, taken with the first JPEG and kept (only the media task decodes)
jpeg_decoder_handle_t g_decoder = nullptr;
bool g_no_decoder = false;

bool decoder()
{
    if (g_decoder == nullptr && !g_no_decoder) {
        jpeg_decode_engine_cfg_t config{};
        config.timeout_ms = kTimeoutMs;
        if (jpeg_new_decoder_engine(&config, &g_decoder) != ESP_OK) {
            g_decoder = nullptr;
            g_no_decoder = true;
            ESP_LOGW(kTag, "no JPEG decoder engine, JPEGs are decoded in software");
        }
    }
    return g_decoder != nullptr;
}

// the block (MCU) of a sampling: 4:2:0 16x16, 4:2:2 16x8, 4:4:4 and gray 8x8
uint32_t block_width(jpeg_down_sampling_type_t sampling)
{
    return sampling == JPEG_DOWN_SAMPLING_YUV420 || sampling == JPEG_DOWN_SAMPLING_YUV422 ? 16 : 8;
}

uint32_t block_height(jpeg_down_sampling_type_t sampling)
{
    return sampling == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
}

uint32_t round_up(uint32_t value, uint32_t step)
{
    return (value + step - 1) / step * step;
}

// The output buffer: the DMA writes it and the driver drops the cache over it afterwards, so its
// start and its size are whole cache lines (a neighbour in the same line would lose its writes);
// the driver's allocator rounds both (and zeroes the buffer).
core::Pixels allocate_decoded(size_t bytes, size_t &allocated)
{
    jpeg_decode_memory_alloc_cfg_t config{};
    config.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    auto *data = static_cast<uint16_t *>(jpeg_alloc_decoder_mem(bytes, &config, &allocated));
    if (data == nullptr) {
        return {};
    }
    return core::Pixels(data, [](uint16_t *p) { heap_caps_free(p); });
}

} // namespace

bool decode(const uint8_t *jpeg, size_t size, bool swap_bytes, Frame &frame, char *error, size_t error_size)
{
    if (!decoder()) {
        std::snprintf(error, error_size, "no hardware decoder");
        return false;
    }
    jpeg_decode_picture_info_t info{};
    if (jpeg_decoder_get_info(jpeg, static_cast<uint32_t>(size), &info) != ESP_OK || info.width == 0 ||
        info.height == 0) {
        std::snprintf(error, error_size, "not a JPEG the hardware knows");
        return false;
    }
    if (info.width % 8 != 0 || info.height % 8 != 0) {
        // the driver refuses them (with three error lines): Home Assistant's snapshots in a tile's
        // size are often such
        std::snprintf(error, error_size, "%ux%u is not divisible by 8", static_cast<unsigned>(info.width),
                      static_cast<unsigned>(info.height));
        return false;
    }
    const uint32_t width = round_up(info.width, block_width(info.sample_method));
    const uint32_t height = round_up(info.height, block_height(info.sample_method));
    size_t allocated = 0;
    core::Pixels pixels = allocate_decoded(static_cast<size_t>(width) * height * 2, allocated);
    if (!pixels) {
        std::snprintf(error, error_size, "no memory for the frame (%u KB)", static_cast<unsigned>(width * height * 2 / 1024));
        return false;
    }
    jpeg_decode_cfg_t config{};
    config.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    // BGR is RGB565 with the low byte first
    config.rgb_order = swap_bytes ? JPEG_DEC_RGB_ELEMENT_ORDER_RGB : JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    config.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t written = 0;
    if (jpeg_decoder_process(g_decoder, &config, jpeg, static_cast<uint32_t>(size), reinterpret_cast<uint8_t *>(pixels.get()),
                             static_cast<uint32_t>(allocated), &written) != ESP_OK) {
        std::snprintf(error, error_size, "the hardware could not decode it");
        return false;
    }
    if (written != width * height * 2) {
        // the blocks were not the ones expected: the frame would be read with the wrong rows
        std::snprintf(error, error_size, "the hardware wrote %u bytes for %ux%u", static_cast<unsigned>(written),
                      static_cast<unsigned>(width), static_cast<unsigned>(height));
        return false;
    }
    frame.pixels = std::move(pixels);
    frame.width = width;
    frame.height = height;
    frame.picture_width = info.width;
    frame.picture_height = info.height;
    return true;
}

#else

bool decode(const uint8_t *, size_t, bool, Frame &, char *error, size_t error_size)
{
    std::snprintf(error, error_size, "no hardware decoder");
    return false;
}

#endif

// ------------------------------------------------------------------------------- PPA

#if SOC_PPA_SUPPORTED

namespace {

// the PPA's scale is a whole number and sixteenths
constexpr uint32_t kScaleSteps = 16;
// the output's start and size in whole lines of both caches (the driver drops them afterwards)
constexpr size_t kLine = CONFIG_CACHE_L2_CACHE_LINE_SIZE > CONFIG_CACHE_L1_CACHE_LINE_SIZE ? CONFIG_CACHE_L2_CACHE_LINE_SIZE
                                                                                            : CONFIG_CACHE_L1_CACHE_LINE_SIZE;

ppa_client_handle_t g_scaler = nullptr;
bool g_no_scaler = false;

bool scaler()
{
    if (g_scaler == nullptr && !g_no_scaler) {
        ppa_client_config_t config{};
        config.oper_type = PPA_OPERATION_SRM;
        config.max_pending_trans_num = 1;
        if (ppa_register_client(&config, &g_scaler) != ESP_OK) {
            g_scaler = nullptr;
            g_no_scaler = true;
            ESP_LOGW(kTag, "no PPA, frames are scaled by LVGL");
        }
    }
    return g_scaler != nullptr;
}

} // namespace

bool cover(const Frame &frame, uint32_t width, uint32_t height, core::Pixels &out, char *error, size_t error_size)
{
    if (!scaler()) {
        std::snprintf(error, error_size, "no PPA");
        return false;
    }
    const core::CoverBlock block = core::cover_block(frame.picture_width, frame.picture_height, width, height, kScaleSteps);
    if (block.out_width == 0 || block.out_height == 0) {
        std::snprintf(error, error_size, "nothing to scale");
        return false;
    }
    const size_t bytes = static_cast<size_t>(width) * height * 2;
    const size_t allocated = (bytes + kLine - 1) / kLine * kLine;
    auto *data = static_cast<uint16_t *>(heap_caps_aligned_alloc(kLine, allocated, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data == nullptr) {
        std::snprintf(error, error_size, "no memory for the scaled frame");
        return false;
    }
    core::Pixels pixels(data, [](uint16_t *p) { heap_caps_free(p); });
    if (block.out_width != width || block.out_height != height) {
        // the pixel or two the rounded scale leaves: black, written back before the PPA's lines
        // are dropped from the cache
        std::memset(data, 0, allocated);
        esp_cache_msync(data, allocated, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    ppa_srm_oper_config_t config{};
    config.in.buffer = frame.pixels.get();
    config.in.pic_w = frame.width;
    config.in.pic_h = frame.height;
    config.in.block_w = block.width;
    config.in.block_h = block.height;
    config.in.block_offset_x = block.x;
    config.in.block_offset_y = block.y;
    config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    config.out.buffer = data;
    config.out.buffer_size = static_cast<uint32_t>(allocated);
    config.out.pic_w = width;
    config.out.pic_h = height;
    config.out.block_offset_x = block.out_x;
    config.out.block_offset_y = block.out_y;
    config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    config.scale_x = static_cast<float>(block.scale) / kScaleSteps;
    config.scale_y = config.scale_x;
    config.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(g_scaler, &config) != ESP_OK) {
        std::snprintf(error, error_size, "the PPA could not scale %ux%u", static_cast<unsigned>(frame.picture_width),
                      static_cast<unsigned>(frame.picture_height));
        return false;
    }
    out = std::move(pixels);
    return true;
}

#else

bool cover(const Frame &, uint32_t, uint32_t, core::Pixels &, char *error, size_t error_size)
{
    std::snprintf(error, error_size, "no PPA");
    return false;
}

#endif

} // namespace hc::hass::image_hw
