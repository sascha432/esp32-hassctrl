#pragma once

// The LVGL calls that differ between LVGL 8 and 9 (docs/architecture.md, "Rules that keep the later
// switch cheap"): every other file uses the calls both versions have.

#include "lvgl.h"

namespace hc::ui::compat {

#if LVGL_VERSION_MAJOR >= 9
#error "port the calls of lvgl_compat.hpp to LVGL 9"
#else

// one pixel of a canvas buffer in the display's format (RGB565, byte swapped where the board
// needs it: lv_color_t follows LV_COLOR_16_SWAP)
using Pixel = lv_color_t;

inline Pixel pixel(lv_color_t color)
{
    return color;
}

// shows `pixels` (width x height, kept alive by the caller) on `canvas`
inline void canvas_set_buffer(lv_obj_t *canvas, Pixel *pixels, lv_coord_t width, lv_coord_t height)
{
    lv_canvas_set_buffer(canvas, pixels, width, height, LV_IMG_CF_TRUE_COLOR);
}

// an image of `pixels` (width x height in the display's format, kept alive by the caller)
using ImageDsc = lv_img_dsc_t;

inline void image_dsc(ImageDsc &dsc, const void *pixels, uint16_t width, uint16_t height)
{
    dsc = {};
    dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc.header.w = width;
    dsc.header.h = height;
    dsc.data_size = static_cast<uint32_t>(width) * height * sizeof(lv_color_t);
    dsc.data = static_cast<const uint8_t *>(pixels);
}

// a spinner turning once a second (LVGL 9 takes the time with lv_spinner_set_anim_params)
inline lv_obj_t *spinner(lv_obj_t *parent)
{
    return lv_spinner_create(parent, 1000, 60);
}

// LVGL keeps what it read of an image source by its address: before the pixels of `dsc` change or
// are freed, the cache has to forget them
inline void image_forget(const ImageDsc &dsc)
{
    lv_img_cache_invalidate_src(&dsc);
}

// Calls `callback` with the code of every event a touch on `indev` sends (LVGL 9: an event
// callback on the input device). Leaves a device that has one alone; false then.
inline bool set_touch_feedback(lv_indev_t *indev, void (*callback)(uint8_t code))
{
    static void (*s_callback)(uint8_t) = nullptr;
    if (indev->driver->feedback_cb != nullptr) {
        return false;
    }
    s_callback = callback;
    indev->driver->feedback_cb = [](lv_indev_drv_t *, uint8_t code) { s_callback(code); };
    return true;
}

#endif

} // namespace hc::ui::compat
