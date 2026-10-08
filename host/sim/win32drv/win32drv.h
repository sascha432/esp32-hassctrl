/**
 * @file win32drv.h
 *
 * LVGL's Windows driver, from lv_drivers v8.3.0 (win32drv, MIT license, see README.md in this
 * folder), changed for the simulator of this project: the window follows the display rotation,
 * the pixels are readable for screenshots, a window can be created hidden, the zoom is a run
 * time value, and the keyboard and the mouse wheel are not LVGL input devices any more (keys go
 * to a queue the simulator reads).
 */

#ifndef HC_SIM_WIN32DRV_H
#define HC_SIM_WIN32DRV_H

#include <Windows.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Set when the window was closed. */
extern bool lv_win32_quit_signal;

/** The mouse as an LVGL pointer, registered by lv_win32_init(). */
extern lv_indev_t *lv_win32_pointer_device_object;

/**
 * Creates the window and registers the display (full frame buffer, direct mode) and the pointer.
 * `hor_res` x `ver_res` is the base orientation; lv_disp_set_rotation() turns the window.
 * `zoom` shows every display pixel as zoom x zoom window pixels. With SW_HIDE the window is never
 * shown, the display works the same (screenshots without a window on the screen).
 */
bool lv_win32_init(HINSTANCE instance_handle, int show_window_mode, lv_coord_t hor_res, lv_coord_t ver_res,
                   int zoom);

/** Window title, UTF-8. From any thread. */
void lv_win32_set_title(const char *title);

/** The frame of the display in its current orientation, RGB565 (LV_COLOR_16_SWAP is 0). */
const lv_color_t *lv_win32_framebuffer(lv_coord_t *width, lv_coord_t *height);

/**
 * Replaces the mouse by a virtual finger (the simulator's scripted taps and swipes): while
 * `active`, the pointer reads x, y and pressed from here.
 */
void lv_win32_pointer_override(bool active, lv_coord_t x, lv_coord_t y, bool pressed);

/** The next key pressed in the window (virtual-key code, 0x100 added with Shift), 0 when none. */
int lv_win32_pop_key(void);

#ifdef __cplusplus
}
#endif

#endif /* HC_SIM_WIN32DRV_H */
