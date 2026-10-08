# win32drv

LVGL's Windows driver, `win32drv` of [lv_drivers](https://github.com/lvgl/lv_drivers) **v8.3.0**
(the last tag of the 8.x line, MIT license, see LICENSE). The upstream file cannot be used as
it is for the simulator (docs/architecture.md section 1), so it is changed here:

| Upstream | Here | Why |
| --- | --- | --- |
| window and frame buffer of a fixed size | the window and the frame follow `lv_disp_set_rotation()` (`drv_update_cb`) | the firmware turns the display with it (Settings, Display, the orientation sensor); the window has to take the new shape |
| frame buffer is a private DIB section | `lv_win32_framebuffer()` | screenshots without a person at the window |
| DPI scaling (`WIN32DRV_MONITOR_ZOOM` at build time) | one display pixel is `zoom` x `zoom` window pixels, `zoom` set at run time, pixels repeated, never smoothed | a design review needs the pixels of the panel, not a resampled picture |
| keypad and encoder input devices | removed; keys go to `lv_win32_pop_key()` | the screens take touch only; the simulator uses keys for rotate, scenario and screenshot |
| the display refresh timer is replaced | LVGL's own, `LV_DISP_DEF_REFR_PERIOD` as in the firmware | the same frame cadence as the device |
| pointer position as the window shows it | turned back to the panel's base orientation before LVGL turns it again | LVGL expects a touch controller to report panel coordinates (the firmware's `esp_lcd_touch` does) |
| `lv_drv_conf.h` | none | one switch less |
| (none) | `lv_win32_pointer_override()`, hidden window (`SW_HIDE`) | scripted taps and swipes and headless screenshots |

Nothing else of the upstream file is kept as it was; read it as a rewrite on the same structure
(a window thread with its message loop, a pointer read callback, a flush callback).
