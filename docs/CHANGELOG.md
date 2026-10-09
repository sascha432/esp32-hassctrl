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
- Camera streams on the JC3248W535 (ESP32-S3): Espressif's `esp_new_jpeg` (S3 only, SIMD)
  decodes and scales a frame to cover the screen (`hass/src/new_jpeg.cpp`,
  `core::cover_size8`), TCP window 23 KB. A 640x320 stream at 10 fps shows all frames (was
  4.6 fps), 800x480 6.6 fps (was 2.6).
- The media task runs on the second core of chips that have one (the S3's first core was
  saturated by Wi-Fi, LVGL and the decoding: task watchdog). Tested on the P4 (unchanged) and
  the WT32 (640x320 2.3 to 2.6 fps).
- Fix: after a start the P4's tiles stayed at "--" until a panel was opened. The UI subscribed
  its page before `hass` had created its queue and the subscription was dropped; the queue is
  created with its first use now.
- Climate panel: a power button in the header while the mode is not `off` (and the entity offers
  `off`); it sends `set_hvac_mode off` (`climate.turn_off` needs the entity's TURN_OFF feature).
  Turning on again is the mode pill's job. Without the button the name takes the whole header.
- Climate tile 1x1: the action and the room temperature share one line in the small font
  (`Off • 27°`), on every tile size; the 1x2 tile keeps its thermometer row.
- An action on its way is a cyan dot with a glow and a short tail running round the tile's border
  (`ui/src/runner.cpp`, 220 px a second; each frame redraws only the dot's stretch of the
  border) instead of the 2 px cyan border; smooth on the WT32. The dimmer and climate panels
  have no pending look on their buttons and pills any more.
- JC4880P443C panel dialogs slide down from above the screen and back up (250 ms) instead of
  fading; the veil darkens at once (fading it redrew the whole screen every frame). Fix: the
  first frame showed the dialog in place before the slide (its height was read before LVGL had
  laid it out).
- Light panel: `+`/`-` step 1 % when the tile has no `step` (was 10 %).
- `tools/build.py`: `parttool.py` and `otatool.py` run esptool at 921600 baud (`-b`, `ESPBAUD`)
  instead of their 115200: the WT32 writes the firmware at 770 kbit/s (was 130).
- Sim: `--action-ms` sets how long an action takes in the sample Home Assistant.
- Fix: after a restart the strip showed "the network did not answer in time" until the second
  attempt connected (the router still held the old connection, `WIFI_REASON_AUTH_EXPIRE`). The
  shutdown handler now leaves the network (`esp_wifi_disconnect`, 100 ms for the deauth) before
  the reset, and the first `AUTH_EXPIRE` of an attempt that gets one more try (power cut) is
  logged only, not shown as an error. Checked by the user.
- Fix: the JC4880P443C showed "Wi-Fi is not set up" during the start: the strip read the network
  status before its first report (the P4 waits for the C6 in `esp_wifi_init`). The strip stays
  hidden until the status is ready, like the first setup. Checked by the user.

## 2026-10-09

Dashboard editing (docs/ui/web-dashboard.md, approved by the user after trying it on the boards):

- Format: a tile may have a cell of its own per orientation, `landscape: 2,1` / `portrait: 0,3`
  (`[2, 1]` in JSON, `core::Tile::has_landscape`/`has_portrait`). Per orientation the tiles with
  a cell take it first, the others fill the free blocks first-fit in list order; a page without
  cells is placed as before. A cell outside the grid, on the back tile or over another tile is an
  error. Replaces "position: is dropped, no moving of tiles" of 2026-10-07. The sample's
  Workshop page is arranged and documents the keys.
- `core`: `move_tile` (every tile of the page keeps its current cell in that orientation, the
  tiles in the way are pushed aside: the first free block in reading order from the drop cell,
  wrapping), `reset_arrangement`, `remove_tile` (an area with its pages; the entity table stays,
  so the screen's entity store stays valid while editing). 12 new host tests (68 in all).
- Edit mode on the screen (`ui/edit_mode.cpp`): a long press of 1 s anywhere on a page starts it
  (the tile under the finger does not act; holding on goes straight into a drag). A bar with
  Reset, Cancel and Done takes the top, the grid shrinks below it, the tiles get an outline and
  a transparent layer takes every touch. Drag moves a tile (green or red outline of the target
  block), a long press on a tile asks to remove it, areas and the back tile still change the
  page, turning the display keeps editing. Only Done or Cancel end it, no timeout; a reset drops
  the edit, a save from the web UI meanwhile drops it with a message. Done stores the edited copy
  (`Backend::save_dashboard`, `dashboard::save_layout`, written in the LVGL task) and stays on the
  page. The pull-down is off while editing (`overlay::update`).
- API: `POST /api/dashboard?revision=N&check=1` validates without storing; save and check answer
  `{"checked", "revision", "dashboard"}` with the ids of new tiles and the computed `cells` of
  every tile (the reader skips them); a problem answers `{"error", "path"}`, the tile's index in
  each level of tile lists (`config::tile_path`, `read_json_at`), because new tiles have no id.
  A tile without `id` is new, `finish()` numbers it. `GET /api/dashboard/editor`: the device's
  screen in landscape (name, size, tile gap) and the icon names. `tools/test_dashboard.py` checks
  them.
- Web UI: the Dashboard page has the tabs Editor and Import & export (the YAML import and export
  as before; the backup of settings and dashboard belongs to the System page). The editor
  (`web/dashboard.js`, 15 KB gzipped, loaded only on this page, full width): the page tree, the
  grid of the page or the default, the preview on the device's own screen in landscape and
  portrait, drag to reorder or arrange (push aside as on the screen), add from a typed entity id
  (suggestions from the dashboard; the Home Assistant picker comes later), area and spacer,
  every key of the tile format, Up/Down/Duplicate/Move to/Remove, Undo, Discard, Save with 409
  (Reload or Overwrite), the device's check a second after each change with the problem marked
  on its tile, a question before leaving with unsaved changes. No live values in the preview
  (decided). Icons are stand-ins until `web/icons.svg` exists.
- Rule: the web UI goes with the firmware. The editor copies rules of `core` (placement, sizes
  of a kind, kinds of the domains); a change to the dashboard changes `web/dashboard.js` in the
  same commit, and the web image goes onto the devices with the firmware (architecture.md
  section 5, README "Flash", comments in `layout.cpp` and `dashboard.js`).
- `tools/build_fs.py <board> -p PORT` reads the board's storage partition first and keeps every
  file outside `/www` (the dashboard) in the new image; `--fresh` keeps nothing.
- Docs: `docs/ui/web-dashboard.md` (plan, decisions, what was built differently: an edited copy
  of the layout instead of a list of operations, the save in the LVGL task),
  `docs/ui/dashboard-editor-demo.html` (the reviewed mock-up), architecture.md and screen.md on
  cells per orientation.

Entity picker (docs/ui/web-dashboard.md section 2.3, tested by the user on the WT32):

- `GET /api/hass/entities?domain=&q=` answers `{"more", "rows": [[id, name, domain,
  device_class, unit], ...]}` in order of the id; every word of `q` has to be in the id or the
  friendly name. At most 100 rows and about 10 KB (200 rows of the plan would not fit the 16 KB
  a message may have). `GET /api/hass/entity?id=` answers name, domain, state, icon, device
  class, unit, color modes and climate step, or `"missing": true`.
- Both are one-shot `render_template`s over the hass connection (`hass::list_entities`,
  `hass::entity_details`): the task subscribes, takes the first result and unsubscribes; the
  web server's task waits 6 s at most (503 with the reason). The search text and the entity id
  are `variables` of the template, nothing is escaped into it (`hass_protocol/picker.cpp`, a
  host test, 69 in all).
- Editor: the Add dialog searches Home Assistant (domain filter, 300 ms after the last key,
  Enter takes the first row; rows with name, id, device class, unit, kind and whether the entity
  is on the dashboard); a typed entity id is a row of its own, also one Home Assistant does not
  have (added with a warning); without a connection the dashboard's entities with the reason.
  A tile's Entity field has Pick. A light whose color modes are more than `onoff` gets
  `dimmer: true`; nothing else is copied, the tile follows Home Assistant. The friendly names
  name new tiles in the preview and the inspector while the page is open.
- Editor: "+ Area" opens the new area's page (a "+ Tile" after it landed on the parent page and
  the area stayed empty). The screen's name and size are not shown any more.
- Import & export tab: Pages and Tiles count every level, as an import does (it said 8 tiles,
  the main page's, for 141).
- Screen: a new dashboard (a save in the web UI, an import, Done) keeps the open page, found by
  its area tile's id, else its nearest parent that is left (it went to the main page; Done kept
  the page by its index).

Screen:

- JC4880P443C: 8 px between the dashboard tiles (`board::Info::ui_tile_gap`,
  `ui::Options::tile_gap`; 4 px on the other boards). Approved by the user.
- Climate panel: while the mode is off it opens on the mode list instead of the arc (once per
  opening; the power button leads there too). Approved by the user.
- Camera full screen: the spinner's track is at 50 % opacity, so the image shows through.
  Approved by the user.
- Fix: the climate panel's `+`/`-` did nothing in portrait. The arc's hit test takes the whole
  ring, its gap at the bottom too, where the portrait steppers lie; they are in front of the arc
  now (every board).
- Dimmer and climate panels on the JC4880P443C (`panel::Frame::large`, the dialogs): header
  row, back tile and power button 60 px, steppers 80 px, the portrait options row 55 px (a
  quarter larger); list items in Montserrat 24 (`theme::font_item()`, +33 %,
  `CONFIG_LV_FONT_MONTSERRAT_24` in the board's sdkconfig.defaults) and 54 px tall; the level
  and color temperature sliders 160 px wide and 80 % high in portrait, 140 px wide in landscape
  with 40 px to the steppers.
- Color temperature marker (every board): a white rounded bar 12 px tall, 6 px from the track's
  sides, no border (was a 6 px bar across the track with a dark border).
- Design approved by the user; tested on a 3.5" board and the JC4880P443C.

Devices: WT32-SC01 (COM9), JC3248W535 (COM10) and JC4880P443C (COM11) have this firmware and
its web image, their dashboards kept. The WT32's firmware write failed twice at 921600 baud
(checksum) and went through at 460800.
