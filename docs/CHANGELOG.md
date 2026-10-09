# Changelog

## 2026-10-08

- Initial commit (`e276f3e`)
- New board `jc4880p443c`: Guition JC4880P443C-I-W, ESP32-P4 with 32 MB PSRAM, 4.3" 480x800
  ST7701S over MIPI-DSI (`Info::mipi_dsi`: esp_lvgl_port's DSI display, LVGL software rotation),
  GT911 touch, Wi-Fi through the on-board ESP32-C6 (esp_hosted + esp_wifi_remote over SDIO).
  Brought up on COM11; colors, no tearing, rotation, touch and backlight (minimum duty 9)
  checked.
- ESP32-C6 firmware project `boards/jc4880p443c/c6` (esp_hosted 3.0.9 coprocessor, matching the
  host side) and console command `c6 version|update <url>`, which updates the C6 from the P4 over
  SDIO. The C6 of the first unit went from the vendor's 2.3.2 to 3.0.9.
- Device name from the base MAC on chips without a radio (it was `hassctrl-0000` on the P4).
- Large-screen look for the JC4880P443C only (`Info::ui_slide_pages`, `ui_panel_dialogs`,
  `ui_large_fonts`, passed to the UI as `hc::ui::Options`): area pages slide in from the right and
  back out to the right (250 ms); the dimmer, climate and sensor panels are an opaque 620x420
  dialog (440x600 in portrait) over the page dimmed to 50 %, fading in and out (200 ms), a tap
  beside it closes it; fonts Montserrat 16/18/20/26/36 instead of 12/14/16/20/28 (+117 KB, only
  in this board's build). The sensor graph's label sizes grow with the fonts
  (`theme::scaled`). The sim's `--size p4` shows the same. Design approved by the user in the
  sim.
- Camera streams on the JC4880P443C: the P4's hardware JPEG decoder decodes the frames
  (800x480 in 17 ms instead of 139 ms; other chips, progressive JPEGs and pictures whose sides
  are not divisible by 8 stay with the software decoder) and its PPA scales them to cover the
  screen (`hass/src/image_hw.cpp`, `core::cover_block`), so LVGL shows them 1:1 (its zoom took
  100 ms a frame). Every stream now shows all frames its source sends: go2rtc 800x480 at 15 fps,
  Home Assistant's doorspy camera at 9.4 fps (was 2.5); the user measured 18 to 22 fps with
  `-r 25`.
- Stream loop: the next frame is read (and, with the hardware decoder, decoded) while the UI
  draws the last; the UI's take wakes the media task (a semaphore instead of polling at the
  10 ms tick). The software decoder still waits for the UI first.
- JC4880P443C: memory got corrupted while a stream played (crashes in the heap, lwIP and
  FreeRTOS lists at 4 of 4 stream starts); the DMA2D copy of LVGL's bands into the frame buffer
  was the cause and is off, the CPU copies them (no tearing, checked by the user). TCP window
  64 KB (with 5.7 KB go2rtc waited while a frame decoded) and LVGL's rotation buffer 48 KB.
- `stream: hass` on a picture tile plays Home Assistant's own MJPEG stream of the camera
  (`/api/camera_proxy_stream`) full screen; the tile keeps the camera proxy's snapshots.
- A stream's frame is scaled to show at least 3/4 of it (`core::stream_shift`; a 1280x960
  camera showed only its middle full screen). The software decoder gets a work area of 8 KB per
  image (esp_jpeg's 3100 bytes failed on a Home Assistant snapshot).
