#include "hc/frame_display/frame_display.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

namespace hc::frame_display {

namespace {

constexpr const char *kTag = "frame_display";

static_assert(sizeof(lv_color_t) == 2, "the bands are RGB565");

struct Display {
    Config config{};
    lv_disp_draw_buf_t draw_buf{};
    lv_disp_drv_t drv{};
    uint16_t *frame = nullptr;
    uint16_t *bands[2] = {};
    // which band buffer is next; runs on across frames, so the first band of a frame never lands in
    // the buffer of the last band of the previous one, which may still be on its way
    uint32_t band_counter = 0;
    // the lowest panel row changed in this refresh, -1 for none yet
    int last_row = -1;
};

Display g;

// Copies native rows [y0, y1) of the frame into `out`, turned from LVGL's rotation to the panel's
// orientation. The mapping is the inverse of the one LVGL applies to touch points
// (lv_indev.c, indev_pointer_proc), so a touch lands on what is shown under it.
void copy_band(const uint16_t *frame, lv_disp_rot_t rotation, uint16_t *out, int y0, int y1)
{
    const int w = g.config.width;
    const int h = g.config.height;
    const int lines = y1 - y0;
    switch (rotation) {
    case LV_DISP_ROT_NONE:
        std::memcpy(out, frame + y0 * w, static_cast<size_t>(lines) * w * 2);
        break;
    case LV_DISP_ROT_180:
        for (int y = y0; y < y1; y++) {
            const uint16_t *src = frame + (h - 1 - y) * w + (w - 1);
            uint16_t *dst = out + (y - y0) * w;
            for (int x = 0; x < w; x++) {
                dst[x] = *src--;
            }
        }
        break;
    case LV_DISP_ROT_90:
        // logical (h - 1 - y, x), logical width h: one logical row per panel column, read forwards
        for (int x = 0; x < w; x++) {
            const uint16_t *src = frame + x * h + (h - 1 - y0);
            uint16_t *dst = out + x;
            for (int i = 0; i < lines; i++) {
                *dst = *src--;
                dst += w;
            }
        }
        break;
    case LV_DISP_ROT_270:
        // logical (y, w - 1 - x), logical width h
        for (int x = 0; x < w; x++) {
            const uint16_t *src = frame + (w - 1 - x) * h + y0;
            uint16_t *dst = out + x;
            for (int i = 0; i < lines; i++) {
                *dst = *src++;
                dst += w;
            }
        }
        break;
    }
}

// The lowest panel row an area of LVGL's (rotated) screen covers
int last_panel_row(const lv_area_t &area, lv_disp_rot_t rotation)
{
    const int h = g.config.height;
    switch (rotation) {
    case LV_DISP_ROT_NONE:
        return area.y2;
    case LV_DISP_ROT_180:
        return h - 1 - area.y1;
    case LV_DISP_ROT_90:
        return h - 1 - area.x1;
    case LV_DISP_ROT_270:
        return area.x2;
    }
    return h - 1;
}

// Sends the frame from the top down to `last_row` in bands; the rows below did not change. Every
// write starts at the top, so this is the least that can be sent. esp_lcd queues a band's pixels
// and returns; the next band's address command waits until they are out, so copying a band
// overlaps sending the one before.
void send_frame(lv_disp_rot_t rotation, int last_row)
{
    const int w = g.config.width;
    const int h = (last_row + 1 < g.config.height) ? last_row + 1 : g.config.height;
    for (int y = 0; y < h; y += g.config.band_lines) {
        const int y1 = (y + g.config.band_lines < h) ? y + g.config.band_lines : h;
        uint16_t *band = g.bands[g.band_counter++ & 1];
        copy_band(g.frame, rotation, band, y, y1);
        // y 0 starts the frame (RAMWR), every other band continues it (RAMWRC)
        esp_err_t err = esp_lcd_panel_draw_bitmap(g.config.panel, 0, y, w, y1, band);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "band at %d: %s", y, esp_err_to_name(err));
            return;
        }
    }
}

// Direct mode calls this for every area of a refresh, all of them already in the frame. The
// frame goes out once, after the last one. The flush returns when the last band is copied: the
// frame is free for LVGL again, the last band is still being sent from its own buffer.
void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *)
{
    const auto rotation = static_cast<lv_disp_rot_t>(drv->rotated);
    const int row = last_panel_row(*area, rotation);
    if (row > g.last_row) {
        g.last_row = row;
    }
    if (lv_disp_flush_is_last(drv)) {
        send_frame(rotation, g.last_row);
        g.last_row = -1;
    }
    lv_disp_flush_ready(drv);
}

} // namespace

lv_disp_t *add(const Config &config)
{
    g = {};
    g.config = config;
    const size_t pixels = static_cast<size_t>(config.width) * config.height;
    g.frame = static_cast<uint16_t *>(heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM));
    for (auto &band : g.bands) {
        band = static_cast<uint16_t *>(
            heap_caps_malloc(static_cast<size_t>(config.width) * config.band_lines * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    if (g.frame == nullptr || g.bands[0] == nullptr || g.bands[1] == nullptr) {
        ESP_LOGE(kTag, "no memory for the frame (%u KB PSRAM) or the bands", static_cast<unsigned>(pixels * 2 / 1024));
        heap_caps_free(g.frame);
        heap_caps_free(g.bands[0]);
        heap_caps_free(g.bands[1]);
        g = {};
        return nullptr;
    }
    std::memset(g.frame, 0, pixels * 2);

    lv_disp_draw_buf_init(&g.draw_buf, g.frame, nullptr, pixels);
    lv_disp_drv_init(&g.drv);
    g.drv.hor_res = static_cast<lv_coord_t>(config.width);
    g.drv.ver_res = static_cast<lv_coord_t>(config.height);
    g.drv.draw_buf = &g.draw_buf;
    g.drv.flush_cb = flush;
    g.drv.direct_mode = 1;
    // rotation is done while the bands are copied, LVGL renders in the rotated size
    g.drv.sw_rotate = 0;
    ESP_LOGI(kTag, "%ux%u, frame in PSRAM, bands of %u lines", config.width, config.height, config.band_lines);
    return lv_disp_drv_register(&g.drv);
}

} // namespace hc::frame_display
