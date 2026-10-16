# Changelog

## 2026-10-16

- Relays: `Info::relay_pullup` for a module without its own pullup: open drain with the internal pullup,
  closed while the board starts. The kitchen variant's relay 1 uses it. Confirmed by you, with its
  motion sensor (a radar, active high) too.

## 2026-10-15

- History in more places (your request): a double tap on a switch or plain light tile opens the sensor
  panel with its on/off timeline (a single tap toggles once the 300 ms window has passed); the light
  panel has a history button left of the power button (the timeline in the place of the control), the
  climate panel too (the room temperature over time, read from the state history with its attributes,
  an hour per request). Confirmed by you on the P4.
- Motion sensor: a PIR on `Info::motion_gpio` (active high or low) sets `input_text.<device>_motion` in
  Home Assistant to Detected or Clear, again after a reconnect. P4: GPIO 34, active low. Console `motion`.
  Confirmed by you with the helper in Home Assistant. A tile on the helper opens the sensor panel with
  a timeline: a state that is one of two words (Detected/Clear, Occupied/Clear, Open/Closed, on/off)
  counts as on or off there, with the words as the legend; before, a text state had no history.
- Board variants: `boards/<board>/variants/<name>/` (not in git) with `hc_board_variant.hpp` changes a
  unit's add-ons and config; built as `<board>.<name>`. Console `orient capture` prints both forms.
- Relays: an entity `input_boolean.<device>_relay_<n>` (or `switch.`) switches GPIO `Info::relay_gpios[n-1]`;
  found by name, followed live. P4: relay 1 on GPIO 32. Console `relay`.

## 2026-10-14

- JC4880P443C: Settings larger (about 28 %: fonts one step up, rows, gaps, padding, thicker sliders
  with a larger knob); dashboard tile gap 7 px (was 8). Confirmed by you on the P4.
- Settings: the +/- buttons of the time steppers larger (3 px, 5 px on the P4), 6 px above and below
  them; the board name and resolution line is gone from Display.
- Edit mode: no area opens, no page swipe, no dots; the tile under the finger is outlined orange;
  click and pulse on start and on the remove dialog, ticks while a tile snaps to cells; the hold
  that started it no longer asks to remove the tile (confirmed by you on the P4).
- Docs: the alarm wakes and undims the screen and keeps it on while it rings (architecture.md 9.6).
- Web UI Logs page: a bar right of the messages drags the page wider, kept in the browser.
- README: a feature list at the top, kept current with every change (CLAUDE.md); the host test
  list names `network`, the T4's port, the conversion of a version 1 dashboard, the screenshot
  and dashboard test tools.
- Sounds from Home Assistant: the event `hassctrl_say` speaks a text (a tts entity) or plays an
  MP3 URL on boards with a speaker; console `sound say` / `sound url` (architecture.md 9.6).
  The event's `options` go to the engine (Google Translate: `tld`, the accent; confirmed by you).
- The alert strip shows at most 3 lines, a longer message ends in "..." (approved by you).
- Web files: a client that does not take gzip gets 406 instead of compressed data (`curl` needs
  `--compressed`).

Everything below confirmed working by you on the boards (2026-10-14, evening).

- JC4880P443C: the DAC at 0 dB (was -12 dB), no restart at 100 % on a 2.4 A USB hub.

- Dashboard format version 2 (your request: a more flexible dashboard; decided with you,
  architecture.md section 5, lvgl-dashboard.md section 6): a tile's size, cell and `hidden`
  live per orientation (`landscape:` / `portrait:` maps), every tile is 1x1 unless set, any kind
  takes any size up to the grid, the portrait size is the landscape size turned unless set.
  Placement is never an error any more: a tile without room, with a cell outside the grid, on
  the back tile or over another tile is left out of that orientation (the device draws nothing,
  the subscription skips it), and the web editor lists it in a tray under the preview to drag
  back; a drag into the tray hides a tile in that orientation. The
  firmware reads version 2 only (your decision: no compatibility, no conversion code in the
  firmware); `tools/convert_dashboard.py` converts a version 1 JSON or YAML file or folder
  once and writes out the old default sizes, so a converted dashboard looks as before. The
  API's `cells` answer is `{"landscape": [c, r] | null, "portrait": ...}`. Web editor: size
  per orientation, hidden checkboxes, the tray, no shifting on a size or grid change
  (`refit()` gone); the mock-up page
  `web-dashboard-editor-demo.html` is deleted. Host tests 98 (new layout tests for overflow,
  hidden, swipe rules); the sample dashboard shows the new keys. Your dashboard on the devices
  needs the conversion before the next flash (the device refuses the stored file otherwise).
- Your requests of the afternoon: the grid of a page goes up to 32x32 (`core::kMaxGrid`; a tile
  stays 1x1 to 8x8, `kMaxTileSize`; `4x3` and `2,1` take two digits now); the dots of a swipe
  page are 50 % larger (24 px band, 9 px dots) and 100 % on the JC4880P443C (32 and 12 px);
  the full-screen panels of the WT32-SC01 and the JC3248W535 (dimmer, climate, sensor, lock,
  weather, camera) slide in from the right and out to the right like an area page, the P4's
  dialogs keep sliding down; the large fonts and glyphs follow the size of the card, not of
  one cell (a 2x2 area on an 8x7 grid of the P4 had the small glyph).
- Tile looks per shape (lvgl-dashboard.md section 3): the shape follows the tile's span in the
  shown orientation, tall (bars top and bottom, the stacked climate and weather looks), wide
  (the climate's bars down the left and right with the stacked look where the card is high
  enough, the icon left and the texts right for every other kind) or the one-cell look; the
  fonts follow the size of one cell. A 1x2 climate turns into a wide 2x1 with side bars on a
  portrait screen. Reviewed in the sim at the three screen sizes.
- Swipe navigation per area (lvgl-dashboard.md section 9, lvgl-touch.md; your request):
  `swipe: true` on an area tile (a checkbox in the web editor) takes the back tile off its
  page, a drag to the right of 40 px (the direction decided after 10 px) slides back to the
  parent page, and a band of dots at the bottom shows how deep the page is in nested swipe
  areas (two on a swipe area below a page with a back tile, three one level further, and so
  on; a tap on the band's left half goes back). The pages slide on every board now (before:
  the JC4880P443C only; `ui_slide_pages = false` switches without the animation). A swipe takes
  the rest of the press, so the tile under the finger is not tapped; the dimmer starts its
  level drag only on an up or down drag. In edit mode a drag to the right on an empty cell
  goes back as well. Tried in the sim and on the boards.

## 2026-10-13

- JC4880P443C (your report): the panel dialogs (dimmer, climate, sensor, lock, weather) did not
  stand out of the page behind them, the alert strip let the tiles show through. The veil behind
  a dialog is black at 80 % (was 50 %), the dialog body has its own color `theme::kDialog`
  (0x161d25, between the page and the cards; was the page color), the alert strip is black at
  90 % (was 60 %, all boards). Picked by you in the sim from two variants; confirmed working
  by you on the P4.
- Web editor (your report): a tile size the portrait preview had room for was refused with
  "no free 1x2 block left in the 5x4 grid of its page" and nothing said that the landscape
  side of the page (arranged, the camera tiles in a band of two rows) was full. The device's
  error names the orientation and the grid ("... in the landscape 5x4 grid ..."), a click on the
  problem switches the preview to the orientation that has it, "no place" in the Position line
  is a link to that orientation, and the size hint in portrait says what the landscape size
  turns into (1x2 shows as 2x1). Four host tests check the new texts.
- Web editor (your report): a size change on an arranged page left the tile's cell as it was,
  so a 1x1 in the last row set to 1x2 ran past the grid ("the landscape cell 1,2 of a 1x2 tile
  lies outside the 5x3 grid"). Now the tile keeps its cell when the new block is free, else it
  is dropped onto that cell with the tiles in the way pushed aside (shifted up or left at the
  edge of the grid), else its cell is dropped and it takes the first free block. Same for the
  dimmer box and a kind change of the entity.
- JC4880P443C: the PPA's first scale of a camera image after most boots never finished, so the
  P4 decoded in software for the rest of the run (TODO of 2026-10-11). Silicon bug DIG-734
  (esp-idf issue 19023, the user's find): the driver's workaround sets the SRM engine's bypass
  flag only when its formula predicts a last macro block smaller than the 2D-DMA's FIFO; ours
  computed to 3696 bits against 1536 and hung anyway, and the issue's fix only corrects 90° and
  270° rotation. New component `components/esp_driver_ppa` overrides the IDF's: the IDF's own
  sources plus a copy of `ppa_srm.c` (v6.1.0) that always sets the flag (README: how to check it
  after an IDF update, when to drop it). Measured on the P4: 8 of 8 boots ran with the flag, 3 of
  3 hung without. `image_hw.cpp` keeps its 500 ms timeout and the software fallback. Existing
  build directories need `python tools/build.py <board> reconfigure` once to pick the component
  up (the first build after adding it still linked the IDF's driver).
- Weather: a `weather.*` entity is a `weather` tile (1x1, 1x2 or 2x1) with the glyph of its
  condition, the temperature, the condition in words and the humidity. A tap opens the weather
  panel: the current values (feels like, humidity, pressure, wind with its compass point, gusts,
  visibility, clouds, dew point, UV index, as many as the entity has and fit) and the daily
  (8 days) and hourly (24 hours) forecast in a strip that scrolls sideways. No history. Not
  supported on the TTGO T4's 320x240 screen.
- The forecast comes from `weather/subscribe_forecast`, subscribed while the panel is open
  (`hass`, `hass_protocol`, `Backend::subscribe_forecast`); the connection's message limit is
  32 KB for the hourly forecast. 15 new entity fields (the field mask is 64 bits), the 15
  condition glyphs (icon fonts and `web/icons.svg` rebuilt), the web editor knows the kind and its
  sizes, the sample dashboard has a weather tile.
- Weather panel after a restart of Home Assistant: the chips stay selected (an unavailable entity
  has no features) and a failed forecast is asked for again every 10 s.
- Connection errors on the status line and the screen no longer carry error numbers ("cannot
  connect to the host", "the connection broke", "Home Assistant closed the connection", "the TLS
  handshake failed"); HTTP status, errno, close code and TLS codes are one warning in the log
  (`connection problem: ...`).
- Windows version: a `weather` sample entity with OpenWeatherMap's values and a made-up forecast,
  step `forecast ok|loading|error`. Host tests 96. Tested on the WT32-SC01.

## 2026-10-12

- Dashboard import: `hass.yaml` may carry the settings sections of the backup file (`hass`,
  `network`, `display`, `sound`, `alarm`, `camera`), with the Home Assistant token, Wi-Fi
  passwords and the camera stream password. They are checked with the dashboard and applied
  after it; the message names them. Before, `hass:` was accepted but ignored.
- Web UI login from a file: a `webui:` section (`user`, `password`) in the backup file or in
  `hass.yaml` sets it, with the rules of the setup page; every session ends. Import only, the
  export writes a commented hint.
- Sample dashboard (`docs/dashboard-sample/`): every settings section with its fields
  (commented out), `webui:` included, `stream:` examples.
- Docs: the UI documents are split into `docs/ui/lvgl/` (the touch screen: design guide, touch,
  dashboard, panels, camera, Settings and first setup) and `docs/ui/web/` (design guide, web app,
  dashboard editor, recovery); the file names start with `lvgl-` or `web-`. Two design guides
  written from the code (`lvgl-design.md`, `web-design.md`) and a touch document
  (`lvgl-touch.md`); `design-system.md` brought up to date and split between them. The
  references in code comments, tools and docs point to the new files.
- Camera full screen: a stream that stops (ended, the connection broke off, no data for 30 s)
  comes back by itself. The alert strip says "Stream stopped, retrying" while a try connects,
  "Stream loading ..." once it is connected and waits for the first picture, and slides away
  when the picture is back. When a try fails, "⟳ Stream failed: <reason>" (a word or two, the
  whole reason is in the log) shows until the next try. A try starts every 10 s and the
  connection gives up after 5 s, so the stream is back within about 10 s of its server; a tap
  on "Stream failed" tries at once. The frame rate in the corner is gone. Confirmed on the P4.
- Alert strip: it slides down from the top when it appears and back up when it goes. Errors are
  never popups: the stream's states and the edit mode's errors ("Changes not saved", "Tile not
  removed", "Order not reset", "Edit dropped", which were popups) are one short line in the
  strip, the reasons go to the log. The camera view's buttons move down with the strip. Design
  guide: lvgl-design.md section 8, Messages. Flashed on the P4 only.
- Web UI in the recovery app: the System page no longer shows the Backup card (the recovery app
  has no backup; export and import failed with 404).
- Time zones from tzdata 2026e (was 2025b: posix_tz_db is no longer updated;
  `tools/build_timezones.py` now reads the IANA data of the tzdata package on PyPI). British
  Columbia stays on -07 after 2026-11-01 (`MST7`, was `PST8PDT`); likewise Alberta, the Northwest
  Territories (-06), Manitoba (-05), Morocco (+00 since 2026-09-20) and Moldova's transition
  times. 598 zones (was 461), the old names like `US/Pacific` included.
- Camera on the web UI: a Camera page (only on a board with a camera) with the stream's
  settings: on or off, size, frame rate, quality, username and password (never shown; empty keeps
  it), and the stream's addresses. The picture's settings stay on the screen with the preview.
  `GET/POST /api/camera`. The log line of every gain change of the camera is gone. Flashed
  (firmware and web image) on all three boards.
- Camera: the stream's quality is 50 by default (was 70), for a device without stored stream
  settings; stored settings keep theirs.

- Camera (JC4880P443C): the test of yesterday is a feature. Settings, Camera below Sound: a
  preview of the picture with Auto exposure and Brightness, or Exposure and Gain by hand, Mirror
  and Flip; the mains frequency; the web stream on or off with its own login (off until there is
  one), its size, frame rate and quality. The picture over the dashboard and the fixed test login
  are gone. Mirror and flip fixed in the sensor driver (the vendor's registers broke the picture);
  the preview draws past LVGL into a second frame buffer, 30 fps without tearing. A snapshot no
  longer returns an old picture, and a capture that stops delivering frames starts again.
  Console: `camera`, `camreg`. OK and Enter on the keyboard of the Settings forms work the same
  now (Enter did nothing). Flashed on all three boards.
- Backup: the camera's settings as a `camera:` section, only on a device with a camera (a file
  from the P4 imports on the other boards with the section ignored). The stream's password is not
  exported; a `stream_password:` added by hand is imported. Flashed on all three boards.

## 2026-10-11

- Camera test on the JC4880P443C: the picture of the CSI camera (an OV02C10) full screen over the
  dashboard (a tap hides it, console `camera` shows it again) and over the web server as
  `/api/camera.jpg` and `/api/camera.mjpg` (HTTP Basic auth, guest / password). New component
  `camera` with esp_video (ISP, auto exposure) and the vendor demo's OV02C10 driver (Apache-2.0,
  `components/camera/ov02c10`); JPEG from the P4's encoder. The web server takes extra routes
  (`Config::add_routes`). Capture 30 fps, stream about 10 fps over Wi-Fi. Firmware 2.6 MB (was
  1.9 MB). A test, to be removed. Flashed on all three boards.
- Camera test, after your first test: the picture on the screen no longer tears. The P4's DPI
  panel has two frame buffers; while the picture is shown, LVGL's flush does nothing and the
  camera frames are drawn (scaled and rotated in one pass) into the buffer the panel does not
  scan, taken over at the end of a frame: 30 fps. The exposure no longer jumps back and forth:
  the AE tuning's frame delays 2 instead of 1, anti-flicker for 60 Hz instead of 50. Flashed on
  the P4.
- Camera test: the exposure no longer jumps at one brightness. The vendor driver's gain table
  did not match the sensor (one step of it made the picture 52 % brighter where the exposure
  control expected 1.6 %); the gain is now the analog gain alone, 1x to 15.5x in steps of 1/16.
  Flashed on the P4.

- Display defaults: the screen dims after 5 minutes (was 1 minute) and turns off after an hour
  (was never). Only for a device without stored display settings; a device that has them keeps
  its times. Not flashed.

- Camera full screen: the pinch zooms freely (it no longer snaps to a step), the buttons go from
  the zoom to the next step up or down. The smallest zoom shows the whole picture, its width or
  its height filling the screen (it was 50 % of covering the screen, which could leave black on all
  sides); a zoom kept below it goes up to it with the first frame. The largest stays 200 %.
  Confirmed by you on the P4, flashed on all three boards.

- Camera full screen: pinch to zoom on all three boards. Two fingers zoom the frame on screen
  (LVGL) and keep the spot between them under them; when a finger lifts, the nearest zoom step is
  taken and its frames asked for, as with "+" and "-". The hint reads "swipe to move, pinch to
  zoom". Measured first over the serial console with your fingers: the GT911 (P4) and the
  AXS15231B (JC3248W535) keep two fingers apart down to 15 and 7 mm, the FT6336U (WT32) merges
  them into one point below about 3 cm, where the pinch ends. Confirmed by you on all three boards.
- `touch_input` reads up to two fingers: LVGL's pointer follows the first one by its track id (the
  FT6336U's list changes its order), `touch_input::fingers()` gives both to the UI
  (`Backend::touch_fingers`, nullptr in the Windows version). New `Config::in_lvgl_task`: read in
  LVGL's read callback instead of the task.
- JC3248W535: touch through `touch_input` in the LVGL task instead of esp_lvgl_port's reader, with
  the board's own read of the AXS15231B (`read_touch`): two fingers (the driver asks for one and
  drops a touch of two) and their ids. An answer with a bad header is no finger (the controller
  answers garbage such as 0x30 throughout after a lift); one with a good header but shifted records
  (about one in 40 during a touch) is dropped as a misread, where the driver took it for a touch at
  a wrong place. The first version took the bad header for a misread too and kept every touch
  pressed (your report: a touch became a long press and the dashboard's edit mode).
- README: a picture of the dashboard on the JC4880P443C (`docs/ui/images/`).

- `lock.*` is a tile of its own (`core::TileKind::Lock`, `lock` in the web editor), no longer a
  sensor: the state in words, filled green while unlocked, orange while locked, red while jammed
  (your choice; tile, panel and timeline); a tap opens the lock panel and never locks or unlocks
  by itself. Tested by the user on the WT32: lock, open and the timeline; the colors confirmed
  by the user on the WT32 (the JC3248W535 and the P4 have the panel with the first colors).
- Lock panel (`ui/src/lock_panel.cpp`): a slider with two places like Home Assistant's (drag or
  tap the other half: `lock.lock` / `lock.unlock`), a round open button that asks for a second tap
  (`lock.open`, only with `supported_features` OPEN, the new `core::Field::Features`), and the
  timeline of the lock behind the second option button.
- The history graph of the sensor panel is `ui/src/history_view.cpp`, used by both panels. A
  timeline knows a problem state (a jammed lock, red; in the legend only while the window has
  one) and takes its colors from the panel (`history_view::Legend`); the history decodes the lock states
  (`hass_protocol/src/history.cpp`). The binary sensor class `lock` reads `Unlocked`/`Locked`.
- `core::IntentKind::SetLock` (lock, unlock, open). Shared panel parts out of the light panel:
  `panel::slider_track`, `panel::slider_place`, `panel::layout_options`, `panel::mark_option`.
- Glyphs `lock-alert` and `chart-timeline-variant` (fonts and `web/icons.svg` generated again).
- MJPEG streams no longer drift behind without limit (your report: Doorspy 10 s behind on the P4
  after a few minutes, 30 s on the WT32). go2rtc queued up to 64 frames for a board whose Wi-Fi
  reads slower than the source sends (Doorspy 370 KB/s). A stream whose next frame is already
  waiting after every frame for 10 s is opened again (the new connection first, so go2rtc keeps
  its source running), which drops the queue. This is the fallback for streams that cannot be made
  smaller; the recommendation is a lower bit rate at the source, and a stream the board can take
  plays without delay. A frame that cannot be decoded is skipped instead of ending the stream
  (three in a row end it). Flashed on all three boards; confirmed by the user on the P4.
- Camera full screen: zoom and move. The view shows the whole picture; 100 % covers the screen
  whatever the camera's resolution, the steps are 50, 75, 100, 150 and 200 %, a swipe moves the
  picture up to its edges. A touch shows the round buttons "X" (back), "+" and "-" and the hint
  "swipe to move" at the top for 5 s; a tap no longer goes back. Requests for the view ask for the
  whole picture (`whole`, `core::whole_block` for the P4's PPA, at most 2 M pixels), a stream
  changes its size without a new connection. The buttons sit below the pull-down zone of Settings,
  which took the taps at the very top. Checked on the P4 over the serial console (still image and
  go2rtc stream through every step, about 14 fps, PSRAM 23 MB or more free).
- Camera full screen: zoom and position are kept per camera while the device runs; a zoom out no
  longer leaves parts of the larger picture on the black (the whole screen is redrawn).
- Camera full screen: the tile's frame shown until the first frame arrives is placed where it lies
  in the whole picture (it was enlarged as if it were the whole picture and the stream jumped);
  a stream played full screen waits 30 s for its answer (go2rtc starting its source took longer
  than 10 s and the view showed "HTTP -1"), the error says "no answer within 30 s". Flashed on
  all three boards; confirmed by the user.
- Measured, not fixed: the P4's PPA hangs on the first camera image after most boots (4 of 4 boots
  of the commit before, also without the zoom), then the decoder and the PPA stay off and every
  image is decoded in software (TODO, Step 6).

## 2026-10-10

- Memory: a JPEG decoder's buffers take internal RAM only while 32 KB stay free, else PSRAM
  (`hass::detail::hot_alloc`). `esp_new_jpeg` (S3) took its work buffers from internal RAM first
  whatever was left: 29 KB for a 176x144 camera image, which would have left a few hundred bytes to
  Wi-Fi and lwIP (on the S3 it only failed and fell back because no block was that large, logging
  `alloc failed` every image). Its two allocators are linked through `hot_alloc` with `--wrap`;
  TJpgDec's 8 KB work area follows the same rule. Checked on the S3 and the WT32: no failed
  allocation, decode times unchanged.
- Full screen camera: the tap that goes back clicks and vibrates like a button (the screen was not
  a tap target). Confirmed by the user.
- Camera tiles: the cameras page of the JC4880P443C stayed empty or showed only some images.
  Three causes: the PPA's blocking scale of a hardware-decoded JPEG did not come back now and then
  and the media task waited for good (silently, the requests piled up behind it); the queue of 4
  dropped the fifth camera without a word; a tile threw away an image that came after its next
  request (15 s refresh, Home Assistant needs up to 5 s for some cameras, one after the other
  longer). Now a tile asks again only once its image came (or after 60 s); on the P4 up to three
  media tasks fetch at the same time (one on the ESP32 and the S3: three left the S3 without
  internal RAM for a socket), one decode at a time; a request that falls out of the queue (now 8)
  is answered with an error; the PPA has 500 ms, then the decoder and the PPA stay off until the
  restart (software decoding). Checked on all three boards over the serial console (cameras every
  15 s, the P4 caught one PPA timeout); confirmed by the user on the P4.
- Web UI dashboard editor: the real MDI icons instead of the hand-drawn stand-ins. `tools/build_icons.py`
  also writes `web/icons.svg` (one `<symbol id="mdi-<name>">` per icon, 18 KB, 6.5 KB gzipped, MDI
  7.4.47); the editor loads it once and draws every tile and the icon picker with `<use>`. Needs the
  web image on the devices (`tools/build_fs.py`). Workflow: a change to the icon list of `core` is
  followed by `python tools/build_icons.py` (fonts and sprite), committed with it. Confirmed by the
  user on the WT32 (2026-10-10).
- Home Assistant over https with a self-signed certificate: trusted on first use. A refused
  certificate's SHA-256 is shown on the Home Assistant page of the web UI ("Certificate not
  trusted", compare it with the browser's); **Trust this certificate** stores it and connects,
  **Forget** removes it, a new address forgets it. The WebSocket and the camera images accept it
  besides the certificate bundle (`hass/src/tls.cpp` wraps the bundle's verify callback). A changed
  certificate reads "the server's certificate changed". API `POST /api/hass/certificate`,
  `certificate` and `refused_certificate` in `GET /api/hass`; the backup carries it
  (`hass: certificate:`). Tested on the WT32 with hassctrltestdomain.com, google.com and
  self-signed.badssl.com; the web UI card confirmed working by the user.
- TLS: cross-signed verification in the certificate bundle
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`, about 700 bytes of heap during a
  handshake). google.com was refused before: it sends GTS Root R1 cross-signed by GlobalSign Root
  CA, which the common bundle does not hold. Also helps the update download by URL.
- P4: the boot no longer logs `esp_lcd_panel_swap_xy: swap_xy is not supported by this panel`.
  esp_lvgl_port (LVGL 8) sets the orientation once when the display is added, even with software
  rotation; the board file gives the ST7701 panel a `swap_xy` that does nothing. Flashed on the P4,
  a 20 s boot log is clean.
- Docs: ui/web.md section 9 (HTTPS): there will be no HTTPS web server (decided by the user); TLS
  stays as a client (wss, camera images, update by URL). Section 10 has no open points (the OTA push
  is described in section 8). architecture.md no longer asks to measure the cost of HTTPS.
- TODO: start orientation on the WT32 and the start screen and restart on the WT32 and JC3248W535
  confirmed by the user; the two-empty-lines item on Wi-Fi reconnect closed without a fix.
- Web UI: the Logs page. New component `logbuf` keeps the last 16 KB of esp_log messages in
  PSRAM (main firmware and recovery app): errors and warnings always, info messages with Debug
  on (switch on the page, stored). Filter, Refresh, Download. The console's `log` level now only
  changes the serial output, so a quiet console still fills the log. APIs `/api/log` and
  `/api/log.txt`. Flashed on all three boards (recovery app, main firmware, web image); the
  page was tried by the user.
- Web UI: pages are up to 800 px wide (was 480), so log lines fit. A request for a missing file
  is logged with its path (`web: 404 GET /favicon.ico`) instead of ESP-IDF's
  "httpd_resp_send_err: 404 Not Found - not found".
- Vibration: the alarm preview in Settings vibrates too, in the alarm's 500 ms on / 500 ms off
  pattern for as long as its sound plays; the last pulse is played in full, not cut when the
  sound ends. `vibration::start_alarm(active)` takes a check that ends the pattern before the
  next pulse. The looping alarm is unchanged. Flashed on the P4, confirmed by the user.
- WT32 internal RAM: a plain `malloc` above 512 bytes goes to PSRAM on the ESP32 (was 16 KB,
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` in `sdkconfig.main.esp32`). With Home Assistant connected
  the internal RAM that can hold data has 19 KB free (was 11), the largest block 14 KB (was 8),
  steady over page changes. Whether the LVGL out-of-memory crash of 2026-10-09 is gone is still
  to be seen. Flashed on the WT32.
- Console: `mem` prints free memory and the largest block of internal, internal 8-bit, DMA and
  PSRAM.
- TODO: "Next steps" rewritten (the order of what comes next) with a list of what is not built
  yet, the web UI's Logs page among it.
- "Swipe down for Settings" stays while the onboarding runs: as long as Home Assistant has no
  address and token (the start screen offers "Set up Home Assistant") it shows, also when it was
  stored as seen. It goes after a swipe once Wi-Fi is up and Home Assistant connected, then stays
  gone. Confirmed by the user on the P4.
- P4 recovery app: the screen flickered cyan during every upload. The DSI interrupt restarts the
  panel's DMA after each frame and could not run while the flash was written (cache off).
  `sdkconfig.recovery.esp32p4` turns on `CONFIG_LCD_DSI_ISR_CACHE_SAFE`. Confirmed by the user.
  The main firmware does not flicker during an update (tested by the user) and keeps it off: it
  would need esp_lvgl_port's DSI callback in IRAM.
- TTGO T4 retired: it will not be updated anymore, its open recovery item is removed.
- `dependencies.lock` (P4): esp_lcd_st7701 2.0.2~2 to 2.0.3.
- Vibration: the motor pulses when the finger touches a tap target, not when it lifts. The
  click sound stays on the release. A press that becomes a scroll or a slider drag vibrates too
  (nobody can tell at the touch). WT32-SC01: click 15 ms (was 50), the user's choice: the pulse
  at touch-down is tactile and short. Flashed and confirmed by the user.
- Automatic brightness: two lights set by the user instead of a fixed 350 lx. Settings, Display has
  "Lowest brightness at" and "Full brightness at"; a tap takes the light in the room now. The screen
  is at 1 % at the first light and at the slider's brightness at the second and above, linear
  between (defaults 0 and 350 lx). A tap that would cross the other point is refused ("not darker",
  "not brighter"). Stored with the display settings (older stored settings load with the defaults)
  and in the backup (`light_min_lx`, `light_max_lx`). The LDR lux table as board defaults is dropped.
  Confirmed by the user on the WT32.
- Setup screen: the Web UI login and Done pages are dropped from the design (screen.md section 4):
  the browser setup on the phone does Wi-Fi, login (signs in at once) and Done ("Keep the hotspot
  on"). Update page in the browser (file and URL) and the larger pull-down zone confirmed by the
  user; TODO items closed.
- Recovery app screen: an update shows its progress ("Update downloading: 42 %", "receiving" for
  an upload), green when done, red with the reason when it failed. Before, nothing showed while
  the image was written. Confirmed by the user on the WT32 (firmware by URL).
- Settings rows: the value takes the space the name does not need (the name at most 60 %). The
  value stopped at 50 %, so a long Wi-Fi name on the Quick page ended in "..." with room to spare.
  Confirmed on the WT32.
- I2C unplug tests dropped from the TODO: the failed reads came from the starved orientation task
  (priority), not the wiring.
- Recovery app screen (`recovery/`), drawn without LVGL: board, firmware in flash, Wi-Fi state with
  its IP, the hotspot's name and password, and a QR code that joins the hotspot (the phone then
  opens the captive portal). 8x8 font from LVGL's unscii-8 (`tools/gen_recovery_font.py`), QR code
  from Nayuki's qrcodegen. The hotspot is always on in the recovery app
  (`network::force_hotspot`, not stored). Tested on the WT32.
- 4 MB layout: factory partition 1.06 MB (the screen drivers made the recovery app larger), ota_0
  from 0x120000 and 2.4 MB. A WT32 or T4 needs the recovery app flashed once for the new table.
- 16 MB layout: factory partition 1.25 MB (the recovery app with its screen was 1.04 to 1.06 MB and
  no longer fit 1 MB: boot loop, black screen), ota_0 from 0x150000 and 5.75 MB. The JC3248W535
  and the P4 need the recovery app flashed once for the new table, then the main firmware again.
  Recovery screen confirmed by the user on both.
- First setup: a failed network brings the hotspot back (wrong password at once, other errors
  after 3 attempts) until a connection works; before, the device retried with no way in. Also
  after a restart while no saved network ever connected.
- Wi-Fi scan lists: "saved" only for a network saved with its password (not one that is
  "password needed"), and up to date without a rescan: the flag is set when the list is read
  (`network::last_scan`). The web Wi-Fi page redraws "Add a network" after Enter and Forget.
  Confirmed by the user on the WT32.
- Start screen without Home Assistant set up (no token after a factory reset and a backup import):
  it ran forever. Now, with Wi-Fi, the runner stops and a "Setup Home Assistant" button opens
  Settings at the Home Assistant page. Flashed on the WT32, not yet confirmed.

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

Audio (docs/architecture.md section 9.6):

- New component `audio` (`CONFIG_HC_AUDIO`, menu "hassctrl audio"): I2S output to the board's
  amplifier, 16 bit mono at 16 kHz in both slots, one task (priority 5) that plays one sound at a
  time (a newer one replaces it), the channel stops a second after the last sound. Volume in
  software on a square curve. Only on boards that `select HC_BOARD_HAS_AUDIO`; without the option
  none of it is compiled (the UI's Backend entries are nullptr, Settings leave the Sound page
  out). The WT32-SC01 build has no audio objects.
- JC3248W535: NS4168 amplifier on BCLK 42 / WS 2 / DOUT 41 (`Info::audio_*`), found in two
  working configurations of the module (the datasheet lists no audio).
- Touch sounds (`CONFIG_HC_AUDIO_TOUCH_SOUND`): a 6 ms tick for every tap on a tap target (LVGL
  input widgets and plain objects marked `sound::tappable()`), through LVGL's input device
  feedback (`compat::set_touch_feedback`); none between tiles, on backdrops or after a drag.
- Settings, Sound: on/off, volume (the touch sound plays when the slider is let go), touch sounds
  on/off; stored in NVS `ui`/`sound`. The sim shows the page (`--page sound`) and prints
  `[stub] touch sound`. `slider_card` is shared between the page files now.
- Console `sound [volume <percent>|tone [hz] [ms]|click]`.
- Planned, not built: WAV and MP3 playback (architecture.md section 9.6).

Audio, second version (after the user's test: clear sound, no pop):

- MP3 (`CONFIG_HC_AUDIO_MP3`): `espressif/esp_audio_codec` 2.6.2 (simple decoder, MP3 only),
  file read in 2 KB pieces, the I2S clock follows the file's rate, stereo mixed to mono. Its
  weak allocators are overridden to prefer PSRAM: internal RAM while playing 35 KB free instead
  of 8 KB. Task stack 5 KB (2.1 KB used). Fetched for esp32s3/esp32p4 targets only.
- Audio task: sources (memory, tone, beeps, file), a queue of four, the newest wins; an alarm
  sound plays at full volume and is not replaced by other sounds.
- Alarm clock (`CONFIG_HC_AUDIO_ALARM`): Settings, Alarm (time on rollers, days, snooze,
  sound with preview), the alarm screen with Snooze and Stop, wakes the screen, at most 10 min;
  `core::alarm_due`/`minutes_until`/`days_text` with host tests (72 pass). Six AOSP alarm sounds
  (Apache 2.0, `sounds/`, `tools/build_sounds.py`) in the file system image of boards with MP3
  (`tools/build_fs.py`).
- Sound settings: the master switch is gone; volume 0 to 100 % (0 silent) also on quick settings;
  "Click on taps" and "Tick on sliders" each on their own (NVS `ui`/`sounds`, the first layout
  under `sound` is converted). Slider ticks follow the speed.
- Switch rows: a tap anywhere on a row with a switch flips it; rows without an action take no
  touches. `power::wake()`, `hc::ui::ring_alarm()`, console `sound [tick|play|alarm|stop|ring]`
  and the audio task's stack and the free internal RAM in `sound`.
- Sliders: the touch area reaches 16 px past the ends (`theme::slider`): at 0 and 100 % half of the
  knob lay outside the slider and a drag starting there did nothing (reported by the user).
- Slider cards: the track around the slider is its touch area, 16 px above and below (it was
  22 px tall in all; LVGL only finds a child inside its parent), checked on the P4 with `swipe`.
  Then the whole card (the track lets the slider's touch area overflow): still hard on the P4.
- Audio on the JC4880P443C: the ES8311 codec on the touch bus with `espressif/esp_codec_dev`
  (control only, I2S slave, MCLK 256 x rate on GPIO 13, amplifier GPIO 11), `board::AudioCodec`
  and `Info::audio_codec`/`audio_mclk_gpio`; `audio::Pins` became `audio::Config` with the MCLK
  pin and the codec's start/stop. The P4 gets the alarm sounds in its file system image.
- P4 audio fixes: the DAC volume is set on every start (the driver's stop zeroes it, so only the
  first sound played); the DAC at -12 dB (`kDacDb`), loud sounds restarted the board through its
  supply ("power on" reset, also on a 10 W hub); the driver's +3.6 dB hardware-gain offset removed.
- MP3 file read with open/read instead of a FILE: no stdio buffer in internal RAM (S3: 39 KB free
  while playing, was 35 KB).
- New component `touch_input`: the touch sampled every 10 ms by a task, changes queued for LVGL
  (quick taps during a render were lost); replaces esp_lvgl_port's touch on the P4 and the WT32.
  Points outside the panel are dropped. Console `touch log`. The JC3248W535 (touch inside the
  panel controller, `Info::touch_in_panel`) keeps esp_lvgl_port's reader: from the task its touch
  went wrong, also with the reads kept between frames.
- Audio: the first sound after a pause gets a DMA buffer of silence before it, every sound is
  padded to whole DMA buffers: the I2S driver hands out the buffer being sent after a pause, and a
  click written there lost its start at random (clicks of different loudness).
- The speaker channel, codec and amplifier run all the time on both boards
  (`Info::audio_always_on`, `audio::Config::keep_running`): on the P4 the amplifier woke too
  slowly and the first click after a pause was not heard; on the JC3248W535 a click about a second
  after the last sound caught the NS4168 waking and was quieter.
- Audio: 20 ms of silence when the channel starts (the amplifier's wake-up swallowed the start of
  a click); the click's distance check only on sliders and drag controls.
- Power: the random "power on" restarts of both boards were their USB cables (gone with new
  ones). docs/adding-a-board.md, "Power": restarts with "power on" or "brownout" are discussed
  with the developer before the firmware works around them; the JC4880P443C recommends 5 V at
  2 to 3 A, a PC's USB port gives 0.5 A.
- Confirmed by the user on both boards: clear sound, no pop; every click equally loud at any pace;
  slider ticks; switch rows; MP3 and the alarm with snooze; the JC3248W535's touch.

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
  (decided). Icons: see 2026-10-10 (`web/icons.svg`).
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

Start screen and backlight (docs/ui/screen.md section 1.2, confirmed by the user on the
JC4880P443C):

- The test screen is only in the firmware of a board in bring-up (`CONFIG_HC_BOARD_BRING_UP`,
  new, off on all four boards); `main` shows it and hands `hide_test_screen` to `hc::ui::start`.
  `hc::ui::start` no longer takes a `TestScreenConfig`; the sim lost its test screen pages.
- Start screen: the Home Assistant logo, 160 x 160, with the runner going round the house until
  Home Assistant is connected and the first page's values are in (`dashboard::loaded()`), at most
  10 s after the connection; without a dashboard a line names the web UI. Alert strip and
  pull-down work over it. `tools/build_logo.py` renders the logo (two 4-bit alpha masks) and the
  track; `runner::set` takes an optional outline.
- The backlight stays off until the first frame is drawn (refresh monitor of the display, at the
  latest after 1 s), then fades up in 0.5 s, and turns off before every `esp_restart()` (shutdown
  handler in board, pin held low through the reset where the chip allows it).
- The restart also switches the panel off and puts it to sleep (`detail::set_restart_panel`): the
  P4's backlight pin is undriven until the next start and lit the ST7701S, which shows cyan
  without a signal. The runner redraws from where its head was at the last step, so a slow frame
  no longer leaves a dot behind (seen at the top of the logo during the first second).

Vibration (docs/architecture.md section 9.7):

- New component `vibration` (`CONFIG_HC_VIBRATION`, menu "hassctrl vibration"): a motor on a
  GPIO, pulses through a one-shot `esp_timer`, the alarm pattern 500 ms on / 500 ms off. Offered
  by boards that `select HC_BOARD_HAS_VIBRATION` with `Info::vibration_gpio`; the JC4880P443C
  has a test motor on GPIO 28.
- A 25 ms pulse with every tap that clicks (`Backend::vibrate_click`, from the touch feedback of
  `ui/sound.cpp`), independent of the volume and the click switch; none for slider ticks. The
  alarm vibrates with its sound (`main/ui_sound.cpp`), not the preview.
- Settings, Sound: "Vibration" section with the "Vibrate" switch (on by default, NVS
  `ui`/`vibrate`); the page is there with audio or a motor. Sim: `[stub] vibration pulse`.
- Console `vibrate [click|<ms>|alarm|stop|status]` (`status` reads the pin back).
- Fix after the first test: the motor stayed on after the first tap. `esp_timer_is_active()` is
  true inside the timer's own callback, so the off-timer always took itself for stale; a deadline
  decides now.
- The click is `Info::vibration_click_ms`, measured per board by the developer (adding-a-board.md
  step 10): 75 ms on the JC4880P443C, whose test motor does not move below 70 ms.
- Confirmed working on the JC4880P443C by the user.
- Fix: letting go of the volume slider clicked even with both touch sounds off; it clicks only
  when one of them is on.
- JC4880P443C device page: the schematic (`JC4880P443_V1.0.pdf`) and the expansion header pins
  (`gpio-header.jpeg`) added.
- Fix: the task watchdog fired on the JC4880P443C a few seconds after boot (IDLE0 starved,
  `taskLVGL` running). esp_lvgl_port's minimum sleep of 5 ms was 0 ticks at the 100 Hz tick, so
  the LVGL task never slept while the screen redrew and its MIPI-DSI flush did not block. The
  FreeRTOS tick is 1 ms now (`CONFIG_FREERTOS_HZ=1000` in `sdkconfig.main`, all boards).
  Confirmed fixed by the user.
- WT32-SC01: vibration motor on IO27, the first board with a motor and no audio (`main.cpp`
  included the vibration header only with audio). The click is 50 ms on the WT32-SC01 and the
  JC4880P443C (the user's choice after `vibrate 50`).
- Diagnostics for an LVGL out-of-memory hang on the WT32-SC01 (task watchdog every 5 s, the LVGL
  task halted in `LV_ASSERT_MALLOC`): every failed allocation prints its size, caps and the free
  and largest block of both heaps with `esp_rom_printf` (also with the log off), and a failed
  LVGL assert calls `abort()` (panic and restart) instead of LVGL's `while(1)`.

Backup of the settings and the dashboard (System page):

- One YAML file (`format: hassctrl-backup`) with the sections `network`, `display`, `sound`,
  `alarm`, `hass` and the dashboard (`defaults`, `tiles`). The System page has a Backup card above
  Update: Export all, Settings only, Dashboard only, and Check / Import with a report (problems,
  notes, ignored). `GET /api/backup.yaml[?parts=settings|dashboard]`,
  `POST /api/backup/import[?check=1]`. The recovery app has none (decided: it gets the device back
  on the network, the import follows in the main firmware).
- No password and no token in an export. An import may carry a Wi-Fi `password:` and the Home
  Assistant `token:` added by hand; not the hotspot password and not the web UI login.
- The whole file is checked before anything is written; one problem and nothing changes. A
  section the device lacks is named "not on this device".
- The file format is in `config` (`hc/config/backup.hpp`, host tested); the sections are in
  `main/backup.cpp`. `hass::check_config()` checks a URL and token without storing them.
  `ui::settings_imported()` applies the imported display and sound settings and closes Settings.
- Network: a saved network can be "password needed" (a network imported without its password,
  own NVS key `pw_needed`). It is never tried, also not by the fast reconnect; with nothing left to
  try the hotspot comes on. `import_networks()` merges the file's list (file order, then the saved
  ones it does not name, at most 5, the network in use is never cut off, a saved network keeps its
  password). The screen's Wi-Fi list and saved network page ("Enter password"), the web Wi-Fi page
  (badge and inline password field) and `wifi list` show it. `saved_networks()` returns a struct
  per network.
- The web server's request helpers (`read_large_body`, `query_value`) are shared by the dashboard
  and the backup routes.
- Sim: the scenario `imported`. Host tests: 89 (`merge`, `choose`, the backup file).
