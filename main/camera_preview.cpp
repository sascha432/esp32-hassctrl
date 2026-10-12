// The preview of Settings, Camera (camera_preview.hpp).
//
// LVGL redraws a full screen picture in bands with software rotation, about 50 ms (three refreshes
// of the panel) into the frame buffer the panel scans, so a moving picture tore. The DPI panel has
// two frame buffers for the preview (the board's jc4880p443c.cpp). While a preview runs:
// - LVGL's flush writes its bands into both frame buffers, but nothing inside the box, so the two
//   stay the same around the box;
// - a task scales and turns each camera frame in one pass into the box of the buffer the panel
//   does not scan and hands that buffer over with draw_bitmap; the DPI driver switches buffers when
//   a frame ends, so the panel only ever shows whole frames.
// Without a preview, LVGL's flush is esp_lvgl_port's again and draws into the buffer scanned.

#include "camera_preview.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hc/camera/camera.hpp"

namespace {

constexpr const char *kTag = "cam_preview";

lv_disp_t *g_disp = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
uint16_t *g_fbs[2] = {};
int g_scanned = 0; // the frame buffer the panel scans (the DPI driver starts with the first)
TaskHandle_t g_task = nullptr;
SemaphoreHandle_t g_frame_end = nullptr;
void (*g_lvgl_flush)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *) = nullptr;
std::atomic<bool> g_direct{false};

// The box: LVGL's coordinates and the panel's (the same box turned the way LVGL turns its bands).
struct Box {
    lv_area_t logical;
    lv_area_t native;
    lv_disp_rot_t rotation;
};

// g_write guards the state below (held briefly, also by LVGL's flush); g_draw is held by the
// task while it draws and hands over a frame, so a stop waits until no frame is on its way
std::mutex g_write;
std::mutex g_draw;
bool g_active = false;
Box g_box{};

lv_coord_t panel_width()
{
    return g_disp->driver->hor_res;
}

lv_coord_t panel_height()
{
    return g_disp->driver->ver_res;
}

// LVGL 8's software rotation (lv_refr.c draw_buf_rotate): logical (x, y) lies on the panel at
// 90: (y, H-1-x), 180: (W-1-x, H-1-y), 270: (W-1-y, x); W x H the panel's size
void to_panel(lv_disp_rot_t rotation, lv_coord_t x, lv_coord_t y, lv_coord_t &px, lv_coord_t &py)
{
    const lv_coord_t w = panel_width();
    const lv_coord_t h = panel_height();
    switch (rotation) {
    case LV_DISP_ROT_90:
        px = y;
        py = h - 1 - x;
        break;
    case LV_DISP_ROT_180:
        px = w - 1 - x;
        py = h - 1 - y;
        break;
    case LV_DISP_ROT_270:
        px = w - 1 - y;
        py = x;
        break;
    default:
        px = x;
        py = y;
        break;
    }
}

Box make_box(const lv_area_t &logical)
{
    Box box{.logical = logical, .native = {}, .rotation = lv_disp_get_rotation(g_disp)};
    lv_coord_t ax, ay, bx, by;
    to_panel(box.rotation, logical.x1, logical.y1, ax, ay);
    to_panel(box.rotation, logical.x2, logical.y2, bx, by);
    box.native = {std::max<lv_coord_t>(std::min(ax, bx), 0), std::max<lv_coord_t>(std::min(ay, by), 0),
                  std::min<lv_coord_t>(std::max(ax, bx), panel_width() - 1),
                  std::min<lv_coord_t>(std::max(ay, by), panel_height() - 1)};
    return box;
}

void write_back(uint16_t *fb, lv_coord_t y1, lv_coord_t y2)
{
    const size_t stride = static_cast<size_t>(panel_width());
    esp_cache_msync(fb + y1 * stride, (y2 - y1 + 1) * stride * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// LVGL's bands while a preview runs: into both frame buffers, around the box
void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors)
{
    bool active;
    lv_area_t box;
    {
        std::lock_guard<std::mutex> lock(g_write);
        active = g_active;
        box = g_box.native;
    }
    if (!active) {
        g_lvgl_flush(drv, area, colors);
        return;
    }
    const lv_coord_t width = lv_area_get_width(area);
    const size_t stride = static_cast<size_t>(panel_width());
    const auto *from = reinterpret_cast<const uint16_t *>(colors);
    for (lv_coord_t y = area->y1; y <= area->y2; y++, from += width) {
        // the parts of the row left and right of the box (all of it above and below)
        lv_coord_t left_end = area->x2;
        lv_coord_t right_start = area->x2 + 1;
        if (y >= box.y1 && y <= box.y2) {
            left_end = std::min<lv_coord_t>(area->x2, box.x1 - 1);
            right_start = std::max<lv_coord_t>(area->x1, box.x2 + 1);
        }
        for (uint16_t *fb : g_fbs) {
            uint16_t *row = fb + y * stride;
            if (left_end >= area->x1) {
                std::memcpy(row + area->x1, from, (left_end - area->x1 + 1) * sizeof(uint16_t));
            }
            if (right_start <= area->x2) {
                std::memcpy(row + right_start, from + (right_start - area->x1),
                            (area->x2 - right_start + 1) * sizeof(uint16_t));
            }
        }
    }
    for (uint16_t *fb : g_fbs) {
        write_back(fb, area->y1, area->y2);
    }
    lv_disp_flush_ready(drv);
}

// esp_lvgl_port's own callback (registering replaces it): LVGL's flush is done once its band is
// in the frame buffer; the task's hand-over is none of LVGL's business
bool on_color_trans_done(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *user_ctx)
{
    if (!g_direct) {
        lv_disp_flush_ready(static_cast<lv_disp_drv_t *>(user_ctx));
    }
    return false;
}

bool on_frame_buf_complete(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(g_frame_end, &woken);
    return woken == pdTRUE;
}

// One pass from the camera frame into the box of a frame buffer: the frame scaled to cover the
// box (nearest neighbour, the middle kept, as hc::core::cover_copy) and turned as LVGL turns. The
// source pixel of panel pixel (px, py) is across[px - x1] + down[py - y1].
struct Draw {
    uint16_t *fb;
    Box box;
};

std::vector<uint32_t> g_across;
std::vector<uint32_t> g_down;

void draw_frame(const uint16_t *pixels, uint16_t source_width, uint16_t source_height, void *context)
{
    const auto &draw = *static_cast<Draw *>(context);
    const lv_area_t &logical = draw.box.logical;
    const lv_area_t &native = draw.box.native;
    const uint32_t box_width = lv_area_get_width(&logical);
    const uint32_t box_height = lv_area_get_height(&logical);
    uint32_t crop_w = static_cast<uint32_t>(uint64_t{source_height} * box_width / box_height);
    uint32_t crop_h = source_height;
    if (crop_w > source_width) {
        crop_w = source_width;
        crop_h = static_cast<uint32_t>(uint64_t{source_width} * box_height / box_width);
    }
    const uint32_t crop_x = (source_width - crop_w) / 2;
    const uint32_t crop_y = (source_height - crop_h) / 2;
    // u, v: in the box as LVGL sees it
    auto column = [&](int32_t u) {
        u = std::clamp<int32_t>(u, 0, box_width - 1);
        return std::min<uint32_t>(crop_x + (u * crop_w + crop_w / 2) / box_width, source_width - 1);
    };
    auto row = [&](int32_t v) {
        v = std::clamp<int32_t>(v, 0, box_height - 1);
        return std::min<uint32_t>(crop_y + (v * crop_h + crop_h / 2) / box_height, source_height - 1) *
               source_width;
    };
    const lv_coord_t w = panel_width();
    const lv_coord_t h = panel_height();
    const lv_coord_t native_width = lv_area_get_width(&native);
    const lv_coord_t native_height = lv_area_get_height(&native);
    g_across.resize(native_width);
    g_down.resize(native_height);
    // the panel's x and y back to LVGL's (to_panel the other way), then into the box
    for (lv_coord_t i = 0; i < native_width; i++) {
        const lv_coord_t px = native.x1 + i;
        switch (draw.box.rotation) {
        case LV_DISP_ROT_90:
            g_across[i] = row(px - logical.y1);
            break;
        case LV_DISP_ROT_180:
            g_across[i] = column(w - 1 - px - logical.x1);
            break;
        case LV_DISP_ROT_270:
            g_across[i] = row(w - 1 - px - logical.y1);
            break;
        default:
            g_across[i] = column(px - logical.x1);
            break;
        }
    }
    for (lv_coord_t j = 0; j < native_height; j++) {
        const lv_coord_t py = native.y1 + j;
        switch (draw.box.rotation) {
        case LV_DISP_ROT_90:
            g_down[j] = column(h - 1 - py - logical.x1);
            break;
        case LV_DISP_ROT_180:
            g_down[j] = row(h - 1 - py - logical.y1);
            break;
        case LV_DISP_ROT_270:
            g_down[j] = column(py - logical.x1);
            break;
        default:
            g_down[j] = row(py - logical.y1);
            break;
        }
    }
    for (lv_coord_t j = 0; j < native_height; j++) {
        uint16_t *out = draw.fb + static_cast<size_t>(native.y1 + j) * w + native.x1;
        const uint16_t *base = pixels + g_down[j];
        for (lv_coord_t i = 0; i < native_width; i++) {
            out[i] = base[g_across[i]];
        }
    }
}

void preview_task(void *)
{
    uint32_t generation = 0;
    for (;;) {
        std::unique_lock<std::mutex> drawing(g_draw);
        bool active;
        Draw draw{};
        int back = 0;
        {
            std::lock_guard<std::mutex> lock(g_write);
            active = g_active;
            back = 1 - g_scanned;
            draw = {.fb = g_fbs[back], .box = g_box};
        }
        if (!active) {
            drawing.unlock();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!hc::camera::use_frame(generation, 200, draw_frame, &draw)) {
            continue;
        }
        // the frame buffer as the draw buffer: the driver writes the box's rows back from the cache
        // and scans this buffer from the next frame on; the old one is free once that frame started
        xSemaphoreTake(g_frame_end, 0);
        g_direct = true;
        esp_lcd_panel_draw_bitmap(g_panel, draw.box.native.x1, draw.box.native.y1, draw.box.native.x2 + 1,
                                  draw.box.native.y2 + 1, g_fbs[back]);
        g_direct = false;
        g_scanned = back;
        xSemaphoreTake(g_frame_end, pdMS_TO_TICKS(50));
    }
}

void redraw_all()
{
    lv_obj_invalidate(lv_disp_get_scr_act(g_disp));
    lv_obj_invalidate(lv_disp_get_layer_top(g_disp));
}

} // namespace

void init_camera_preview(lv_disp_t *disp, esp_lcd_panel_handle_t panel)
{
    void *fb0 = nullptr;
    void *fb1 = nullptr;
    g_frame_end = xSemaphoreCreateBinary();
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1) != ESP_OK || g_frame_end == nullptr) {
        ESP_LOGW(kTag, "the panel has no second frame buffer: no preview");
        return;
    }
    g_fbs[0] = static_cast<uint16_t *>(fb0);
    g_fbs[1] = static_cast<uint16_t *>(fb1);
    g_disp = disp;
    g_panel = panel;
    esp_lcd_dpi_panel_event_callbacks_t callbacks{};
    callbacks.on_color_trans_done = on_color_trans_done;
    callbacks.on_frame_buf_complete = on_frame_buf_complete;
    esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, disp->driver);
    g_lvgl_flush = disp->driver->flush_cb;
    disp->driver->flush_cb = flush;
    xTaskCreatePinnedToCore(preview_task, "cam_preview", 4096, nullptr, 5, &g_task, 1);
}

void camera_preview_start(int16_t x1, int16_t y1, int16_t x2, int16_t y2)
{
    if (g_task == nullptr || x2 < x1 || y2 < y1) {
        return;
    }
    const Box box = make_box({x1, y1, x2, y2});
    bool started = false;
    {
        std::lock_guard<std::mutex> drawing(g_draw);
        std::lock_guard<std::mutex> lock(g_write);
        if (!g_active) {
            // the buffer not scanned gets what the panel shows, then LVGL keeps both the same
            const size_t bytes = static_cast<size_t>(panel_width()) * panel_height() * sizeof(uint16_t);
            std::memcpy(g_fbs[1 - g_scanned], g_fbs[g_scanned], bytes);
            write_back(g_fbs[1 - g_scanned], 0, panel_height() - 1);
            g_active = true;
            started = true;
        }
        g_box = box;
    }
    if (started) {
        hc::camera::acquire();
        xTaskNotifyGive(g_task);
    } else {
        redraw_all(); // the old box's place is LVGL's again
    }
}

void camera_preview_stop()
{
    {
        std::lock_guard<std::mutex> drawing(g_draw); // the task's frame is done
        std::lock_guard<std::mutex> lock(g_write);
        if (!g_active) {
            return;
        }
        g_active = false;
    }
    hc::camera::release();
    redraw_all(); // LVGL draws the box's place again, into the buffer the panel scans
}
