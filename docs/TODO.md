# TODO

State as of 2026-10-07: steps 1 and 2 of [architecture.md](architecture.md) section 13 run on
the TTGO T4 (COM13, the test device for the hotspot and web server work) and the WT32-SC01
(COM9); step 3 runs on the T4. The design is in [architecture.md](architecture.md) and [ui/](ui/README.md).

## Step 1: first boot

Built with ESP-IDF 6.1, both applications, no warnings in the project's own code.

| Image | WT32-SC01 | TTGO T4 | Partition |
| --- | --- | --- | --- |
| main firmware | 217 KB | 217 KB | ota_0, 2.4 MB |
| recovery app | 147 KB | 148 KB | factory, 1 MB |
| bootloader | 26 KB, 8 % free | 26 KB, 8 % free | |

Tested on the TTGO T4 on COM13 on 2026-10-06:

- Recovery app flashed with bootloader and partition table, boots from `factory`.
- Main firmware written to `ota_0` by `tools/build.py`, boots from `ota_0`, confirms itself
  without a warning. 365 KB free internal RAM, 4 MB PSRAM in the heap.
- The T4 has 8 MB PSRAM (4 MB mapped) and a Boya flash chip, see
  [devices/TTGO_T4](devices/TTGO_T4/TTGO_T4.md).
- The PSRAM memory test adds about one second to every boot. It can be switched off later
  (`CONFIG_SPIRAM_MEMTEST`).
- The recovery app starts even when the PSRAM fails (`CONFIG_SPIRAM_IGNORE_NOTFOUND`), so a
  hardware fault does not end in a boot loop.

Open:

- [x] **Flash the WT32-SC01** (done 2026-10-06) once COM9 is free. Flashing replaces the firmware that is on the
      board now, including its settings:

      python tools/build.py wt32_sc01 --app recovery -p COM9 flash
      python tools/build.py wt32_sc01 -p COM9 flash

      Expected in the log: `PSRAM 4096 KB`, `flash 4096 KB`.

## Step 2: display

Running on the TTGO T4 since 2026-10-06: ILI9341 over SPI at 40 MHz, PWM backlight, LVGL 8.4
through esp_lvgl_port 2.9.0, two 40-line draw buffers in internal DMA RAM. The test screen
(`components/ui`) shows color bars, a spinner and a bar, and turns by 90 degrees every ten
seconds through the panel controller. Main firmware 488 KB.

| Measurement | Result |
| --- | --- |
| full screen redraw, 76,800 px | 35 ms in every rotation (30.7 ms is the SPI wire time at 40 MHz) |
| partial redraws | about 95 per second, 2 to 4 ms each (LVGL's refresh period is 10 ms) |
| free internal RAM after boot | 334 KB before LVGL starts |

- [x] Checked on the T4 by eye (2026-10-06): fast, no borders, colors correct, orientation
      correct in all four rotations, no tearing when the rotation switches.

WT32-SC01 (COM9) since 2026-10-06: ST7796S over SPI at 80 MHz, FT6336U touch polled over I2C
(the INT line is unusable), MPU6050 orientation on the same bus, all settings taken from the
working kfc_fw configuration. Main firmware 509 KB, recovery app 133 KB.

| Measurement | Result |
| --- | --- |
| full screen redraw, 153,600 px | 35 to 45 ms (30.7 ms is the SPI wire time at 80 MHz) |
| partial redraws | 76 to 92 per second, 4 to 9 ms each |
| free internal RAM after boot | 334 KB before LVGL starts |
| orientation | MPU6050 found, reported 180 degrees after boot and the screen followed |
| touch | four touches arrived in LVGL with plausible coordinates |

- [x] Checked on the WT32 by eye (2026-10-06): display, touch and rotation work, the rotation
      switches with the expected delay and angle.
- [x] Backlight test on the WT32 (2026-10-06): the perceptual curve feels smooth down to the
      lowest levels, the linear one gets choppy; 1, 5 and 25 kHz look the same, 25 kHz stays.
- [x] Minimum brightness of the WT32: duty 10 of 1023 at 25 kHz, readable in daylight and easy in
      the dark (2026-10-06), now `Info::backlight_min_duty`.
- [ ] New boards arriving 2026-10-06: bring them up with
      [adding-a-board.md](adding-a-board.md).

JC3248W535 (COM10) since 2026-10-08: ESP32-S3, 16 MB Boya flash (QIO), 8 MB octal PSRAM,
AXS15231B over QSPI at 40 MHz (init table of Arduino_GFX, no DISPON from the driver: see the
device page), its touch polled over I2C, console on the USB-Serial/JTAG. Main
firmware 1.79 MB, recovery app 960 KB (6 % of the factory partition free: watch it). The
AXS15231B takes no row address over QSPI and cannot rotate a frame (MADCTL MV: empty screen), so
the board sets `Info::full_frames` and `main` uses `components/frame_display`: LVGL direct mode
into one PSRAM frame, sent from the top down to the lowest changed row in 32-line bands, rotated
in software while copied into two internal DMA buffers (40 KB). Dead ends: esp_lvgl_port's
`trans_size` path (full refresh, no rotation), DMA straight from PSRAM ("DMA TX underflow"), 80 MHz
(garbled picture), 60 MHz (= 40 MHz, the SPI clock is 80 MHz divided by a whole number).

| Measurement (test screen at 270 degrees, 2026-10-08) | Result |
| --- | --- |
| partial refreshes | 42 per second, avg 22 ms (was 65 ms with esp_lvgl_port full refresh at 32 MHz) |
| full screen | 40-45 ms |
| free internal RAM after boot | 56 KB (web API) |

- [x] Boots from erased flash: recovery app, main firmware in ota_0, web UI image, first setup
      screen (LVGL screenshot 320x480), no errors in the log (2026-10-08).
- [x] Picture on the panel (2026-10-08, camera): the LVGL test screen, full refresh 65-70 ms.
- [x] Touch works, base orientation portrait (user, 2026-10-08).
- [x] Software rotation: 270 degrees correct on the camera, not mirrored (2026-10-08).
- [x] Backlight tab: the rows wrap in portrait (it was cut off, the duty label had no room);
      slider and -/+ change the backlight (camera: 100 % 187, duty 30 about 96).
- [x] Colors correct, rotation works in all orientations, no tearing (user, 2026-10-08; the TE
      line, GPIO 38, stays unused).
- [x] Backlight: minimum duty 3 of 1023 (user, 2026-10-08), now `Info::backlight_min_duty`;
      25 kHz kept.
JC4880P443C (COM11) since 2026-10-08: ESP32-P4 rev v3.2, 16 MB Boya flash, 32 MB PSRAM, ST7701S
over MIPI-DSI (esp_lvgl_port's DSI display, `Info::mipi_dsi`, LVGL software rotation), GT911 at
0x5D, Wi-Fi through the ESP32-C6 (esp_hosted + esp_wifi_remote). Details on the device page.

- [x] Boots from erased flash: recovery app, main firmware in ota_0, first setup screen
      (screenshot 480x800), hotspot `hassctrl-7e7d`, Wi-Fi scan through the C6 (2026-10-08).
- [x] Picture on the panel (camera, 2026-10-08): the setup screen, turned because the board
      stands in landscape and rotation 0 is the panel's portrait.
- [x] Colors correct, no tearing, rotation works (user, 2026-10-08).
- [x] Rotation and touch work (user, 2026-10-08).
- [x] Backlight: minimum duty 9 of 1023 (user, 2026-10-08), now `Info::backlight_min_duty`;
      25 kHz kept.
- [x] Redraw times (test screen at 90 degrees, 2026-10-08): full screen 50-55 ms for 384000 px,
      small partial refreshes under 1 ms.
- [x] Station mode: joined the user's Wi-Fi through the hotspot setup, web UI login created
      (user, 2026-10-08).
- [x] esp_hosted version mismatch: the C6 now runs our esp_hosted 3.0.9 build, updated from the
      P4 over SDIO with console `c6 update <url>` (2026-10-08, device page "C6 firmware"); the
      hanging first connection after a reset is gone with it.
- [x] Web UI image written over USB (parttool, storage partition), HA token entered by the user,
      HA connected (2026-10-08).
- [x] **Large-screen look** (2026-10-08, only this board: `Info::ui_slide_pages`,
      `ui_panel_dialogs`, `ui_large_fonts`; sim `--size p4`): area pages slide in from the right,
      back slides out to the right (250 ms); dimmer, climate and sensor panels are a 620x420
      dialog (440x600 in portrait, opaque, page behind dimmed 50 %, slides down from above the
      screen and back up in 250 ms, a tap beside it closes it); fonts Montserrat 16/18/20/26/36
      instead of 12/14/16/20/28 (+117 KB firmware). Design approved by the user in the sim; tested
      on the board by the user (2026-10-08; the fade was replaced by the slide).
- [ ] **Check the start orientation** on the WT32 leaning on the desk: it should be upright
      within about a second after boot, without turning it.
- [x] PSRAM of the WT32: the device page says 4 MB (decided 2026-10-07), whatever the boot log
      reports.
- [ ] Remove the test screen once the first real screen exists.

## Step 3: platform and network

Running on the TTGO T4 (COM13) since 2026-10-06. Main firmware 1.22 MB with Wi-Fi.

- Storage: system NVS, settings NVS, LittleFS at `/fs` (448 KB). The first boot formatted the
  storage partition, which held leftovers of the old firmware and no LittleFS; the second boot
  mounted it. The system NVS held old data too and was cleared once.
- Hotspot: on while no network is saved, `hassctrl-86ed`, password generated on the first boot
  and kept over reboots.
- Scan: 15 access points found.
- Development console on the serial port (`help` lists the commands).

- Wi-Fi: connected to the home network (WPA3), ping works. Fast reconnect after a reboot: 680
  to 840 ms to the IP address, 3.4 s when it has to scan.
- Hotspot: a phone joins and gets 192.168.4.2. No web server and no DNS redirect yet (step 4).
- Console: `log` switches the debug output (stored), `reboot` is an alias of `restart`.

Open:

- [x] Wrong password, network out of range, a second network, `wifi policy strongest`,
      `wifi remove`, `hotspot on|off|password` (tested on the T4 2026-10-07; `factory-reset yes`
      only through the web API refusal test, the T4 keeps its settings).
- [x] After a power cut the WPA3 router still holds the old connection (reason 2), and the device
      moved to network 2 for good. Now the same network gets one more try (T4 2026-10-07: 10 of
      10 hard resets back on network 1).
- [x] WT32: "network security not supported" (reason 210) for network 1 while the T4 joins it.
      Cause: network 1 was saved on the WT32 without a password, so the driver looked for an open
      network. Now: the error says "the network needs a password, add it again" (no second try),
      and the screen, web API and console refuse passwords of 1 to 7 characters (2026-10-07).
- [ ] WT32: add the password of network 1 again (user).
- [x] How the password got lost: `wifi add` on the console, then the hotspot web setup (no login
      yet) opened "Choose your Wi-Fi", the saved network was picked with an empty password field
      and Connect saved it again without a password. Now a saved network added again with an
      empty password keeps its password (all paths: web, screen, console); the web UI says
      "empty keeps the saved password" for saved networks (2026-10-07, built, not yet flashed:
      COM9 was in use).
- [ ] Faster than 680 ms, if a remote needs it: staying associated in light sleep (no reconnect
      at all), a static address (no DHCP), or WPA2 for a network that offers both (saves the
      WPA3 handshake). To decide with the remote use case.

## Step 4: first setup, Settings and web server

Step 4 is done in increments. Running on the T4 since 2026-10-06:

- **Web server** on port 80, on the Wi-Fi and the hotspot (`components/webserver`). From the LAN:
  `/api/status` answers the status as JSON, `/` the built-in page (54 ms), unknown files 404.
- **Captive portal** on the hotspot: a DNS responder answers every name with 192.168.4.1, the
  hotspot's DHCP announces the portal address (option 114), and a request for any other host
  is redirected to `http://192.168.4.1/` with a body, which iOS needs to detect the portal.
- **Static files** from `/fs/www` with gzip (`file.gz`); without them the built-in status page.
- A normal boot scans and follows the network order (network 1 was chosen, 4.1 s); the fast
  reconnect is kept for waking from deep sleep.
- **Login** (docs/ui/web.md section 5): the web UI is open in setup mode until a login exists.
  `POST /api/setup/user` creates it (only in setup mode), `/api/login`, `/api/logout`,
  `/api/logout-all`, `GET /api/session`. PBKDF2-HMAC-SHA256 through PSA crypto (mbedTLS 4.1),
  checked at boot against the RFC 7914 test vector; token = HMAC with the key derived from the
  stored login and the counter. Tested from the LAN: setup mode open, weak password rejected,
  401 without cookie, 200 with it, wrong login 401, "log out everywhere" ends old cookies, a
  cookie survives a restart.
- PBKDF2 cost: 10000 iterations took 5.3 s on the ESP32 and blocked the web server; now 1000,
  a login takes about 0.4 s.
- **Setup API**: `GET /api/wifi` (saved networks, last scan), `POST /api/wifi/scan`,
  `POST /api/wifi/add`. Passwords are write-only.
- The built-in page has the setup forms (Wi-Fi, create login), the login form and log out.
- **Wi-Fi power save off**: with modem sleep the ping took 18 to 200 ms (107 ms average) and
  single requests up to seconds; without it 19 ms average and 70 to 140 ms per request. A
  battery remote will need its own setting.
- SNTP starts with the network (pool.ntp.org), needed for the token expiry.
- Console: `login-reset yes` removes the web UI login (setup mode again).

**Screen** (docs/ui/screen.md), running on both boards since 2026-10-06, tested through the
console (`swipe`, `tap`, `screenshot`) in every rotation:

- **Pull-down zone** (80 x 24 px, top center, `components/ui/src/overlay.cpp`): dragging down
  30 px opens Settings.
- **Alert strip** (top center, rounded bottom corners, 60 % black): Wi-Fi error, "Wi-Fi is not
  set up", connecting, then the one-time hint "Swipe down for Settings". A tap opens the
  matching page; the hint stays until Settings was opened by the swipe (stored in NVS `ui`).
- **Settings** (`settings.cpp`, `settings_pages.cpp`): status bar with back + title, hotspot,
  Wi-Fi symbol, HA, clock (UTC until the time zone comes from Home Assistant). Pages: Quick
  (brightness, Wi-Fi, hotspot toggle, Home Assistant, Display, System), Wi-Fi (status, saved
  networks, add, reconnect), saved network (up, down, forget), add network (scan list, other
  network), password with on-screen keyboard, Hotspot (toggle, name, password, QR code to join,
  new password), Home Assistant (web UI address and QR code), Display, System (device
  information, reset web UI login, restart, factory reset). Confirm dialogs for everything
  destructive. Closes after 5 minutes without a touch unless a form has input, by `<` from
  Quick. No swipe up to close (removed 2026-10-07, see below).
- The screens get everything through `hc::ui::Backend` (function table), filled in
  `main/ui_backend.cpp`; the UI component still only depends on LVGL.
- Texts that do not fit end in "..." (`theme::line_text`): LVGL's `LONG_DOT` wraps at spaces
  first, which cut "1  My network" to "1...". Hotspot and Home Assistant pages put the QR code
  beside the text in landscape and below it in portrait.
- ESP32 IRAM was full (2.5 KB over) with the new LVGL widgets: `sdkconfig.main.esp32` turns off
  `CONFIG_ESP_WIFI_RX_IRAM_OPT` (Wi-Fi receive speed only matters for throughput tests).
  `tools/build.py` now also watches the `<layer>.<target>` files.
- WT32 after boot with Settings: 37 KB free internal RAM, 4 MB PSRAM. To watch once the
  WebSocket comes.

Open:

- [x] Tried by hand on the WT32 (2026-10-06): touch navigation works, typing on the keyboard is
      fine. The hotspot QR code joins the phone, the captive portal opens, the network was
      saved over the portal and the WT32 connected. Hotspot on/off, new hotspot password,
      creating the web login over the hotspot and logging in work; the built-in page looks ok.
- [x] **Keyboard lost the input**: LVGL's bottom left key ("hide keyboard") sat where shift is
      expected and the page treated it as cancel. Own key maps without that key
      (`components/ui/src/keyboard.cpp`, the left arrow takes the corner), and `<`
      on a page with input asks "Discard your input?" first. OK in the network name field goes
      on to the password.
- [x] **Display settings** (your request): dim after an idle time to a dimmed brightness of its
      own, screen off after an idle time. Stored in NVS (`ui/display`) 5 s after the last
      change (`platform::set_blob_later`, written at a restart too). Defaults: dim after 1 min to
      20 %, never off. Tested on the WT32: dims at 30 s, off at 60 s, the waking touch does not
      reach the screen, a touch on the dimmed screen does.
- [x] **Ambient light sensor** (2026-10-07): BH1750 on the WT32 found at 0x23 (ADDR open).
      `components/light_sensor` looks at 0x23 and 0x5C (optional per device, no board setting),
      averages a second. Display page: "Automatic" with lux and the backlight level; the curve
      runs from 4 % of the slider's brightness in the dark to 100 % at 5000 lx, smoothed over 2 s.
      Measured at the desk: 650 lx gives 73 %. To judge by eye: dark room, covering the sensor.
- [x] **Text cursor invisible** (your report): LVGL's default theme draws it in its dark text
      color. Now orange, 2 px, blinking from the moment a field gets the focus. Checked on the
      WT32: the cursor shows and moves with the arrow keys.
- [x] **Swipe up closed Settings by mistake while scrolling** (your report, 2026-10-07): the
      screen gesture and the overscroll close are removed; Settings closes only by `<` from
      Quick (and the timeout). Flashed 2026-10-07.
- [ ] **Pull-down hard to hit** (your report, 2026-10-07): the start zone grew from 80 x 24 to
      160 x 40 px, the drag to open from 30 to 24 px (`components/ui/src/overlay.cpp`). Flashed;
      to try on the WT32. A tap on the top center of a tile under the zone now hits the zone.
- [x] **First setup screen** (2026-10-07, `components/ui/src/setup.cpp`, screen.md section 4):
      shown at boot while no network is saved (console: `setup`). Two ways side by side
      (stacked in portrait): "Choose Wi-Fi" on the screen, or the phone (QR code to join the
      hotspot, its name and password, then the portal or http://192.168.4.1). Once a network is
      saved, here or from the phone, it follows the connection: connecting, connected (address,
      `<name>.local`, Next to the dashboard) or failed (the error, "Other network", "Try again").
      Tested on the T4 through the console: welcome, Choose Wi-Fi and back, adding a network,
      connected, Next.
- [ ] Setup screen: the Web UI login and Done pages of screen.md section 4 (login on the
      screen, "Keep the hotspot on", QR code of the web UI) are not built. Today `Next` goes
      straight to the dashboard; the login is made in the web UI. Build them, or drop them from
      the design?
- [ ] Setup screen: the "failed" view was not seen yet (the T4 always found its network). Try:
      `setup`, then a network with a wrong password.
- [x] **Files of a broken file system** (2026-10-07, `api_files.cpp`): `GET /api/fs/files.zip`
      streams every readable file as one .zip (stored, CRC per file; what cannot be read is
      listed in `unreadable.txt` inside), `POST /api/fs/format` with `confirm=yes`. In the
      built-in page (`/recovery`, also when the web UI is gone) and on the System page. Tested on
      the T4: the archive checks out in Python and unpacks in Windows, the files are identical to
      the sources; formatted, then the web image uploaded again.
- [ ] **Settings export/import**: decided 2026-10-07 (one YAML backup file, sections from a table in `main`, see "Open decisions"); to be built.
- [x] ~~2-step orientation calibration in System~~: dropped, it is a developer step (`orient capture`,
      board info `imu_axes`), not a Settings function.
- The sensors (MPU6050, BH1750) stay on the T4 (decided 2026-10-07: the wiring on the WT32 is
  too fragile).

- [x] Captive portal with a phone and the setup in the browser: tested by you on 2026-10-06
      (see the screen part below).
- **Firmware and file system update** (2026-10-07, `components/ota`, `api_update.cpp`):
  upload as the raw request body or download by URL (http and https), progress in
  `GET /api/update`. The **recovery app** now runs storage, Wi-Fi, hotspot and the web server with
  the same login (934 KB of 1 MB; TLS client only, no IPv6, `sdkconfig.recovery`). The built-in
  page has an Update section; `tools/update.py` pushes a build. Tested on the T4:
  - main firmware to the recovery app: 1.5 MB in 26 s, starts and confirms itself;
  - from the main firmware: 409 "update from the recovery app", restart into the recovery app
    and back, the whole round trip with `tools/update.py` (27 s upload);
  - refused before anything is erased: the recovery image ("not the panel firmware"), random
    data ("not a firmware image" / "not a LittleFS image"), no login (401);
  - file system image uploaded to the running main firmware: written in 8 s, mounted again
    without a restart, files served at once;
  - URL: HTTP from the PC (good image, bad image, 404), HTTPS to example.com and a GitHub
    redirect (TLS with the common CA bundle works).
- [ ] **Try the update page in the browser** (both boards): System part of the built-in page,
      firmware by file, then by URL. On the WT32 it goes through the recovery app too.
- **Tests on the T4** (2026-10-07), all passed after the fixes below:
  - Wi-Fi: a missing network at position 1 is skipped (connected to #2 in 3.9 s); a wrong
    password (the WT32's hotspot with a wrong one) is detected, skipped, and the T4 is back on
    the home network after 8 s; with the right password it joins the WT32's hotspot (2.8 s, the
    WT32 shows 1 client); "strongest" picks the strongest network, "entered order" the first in
    range; removing and reordering keep your two networks as they were.
  - Hotspot: on, off, five new passwords (8 digits + 2 letters, never i, l or o); the Wi-Fi
    connection stays up meanwhile.
  - Web server, `tools/test_web.py` (30 checks, also on the WT32): 401 without login, no second
    login, wrong and malformed logins, 8 KB body, path traversal, cookie flags, no passwords in
    answers, 60 requests from 8 connections at once (no errors, memory unchanged).
  - **Fixed**: a client that announced a body and stalled blocked the web server (one task)
    until it disconnected. Now one receive timeout ends a form request; an image upload gets
    3 timeouts of 5 s. Other requests wait 14 s at most.
  - Delayed settings write: the brightness changed and restarted within a second is kept.
  - Update interrupted by a reset: the recovery app starts again. **Fixed**: it reported the
    half-written firmware as present; it now checks the whole image, and "Start the firmware"
    answers "no complete firmware in flash, upload one first".
  - Rollback: a firmware that aborts at start runs once, the bootloader returns to the recovery
    app. **Added**: the page and the log say "did not start, rolled back".
- [x] Logging out removes the cookie in the browser; the token stays valid until it expires,
      "Log out everywhere" ends all of them (decided 2026-10-07: fine, no revocation list).
- [x] While an image is uploaded the web server answers nothing else (decided: fine, the browser
      shows the upload progress itself).
- [x] Automatic brightness calibrated by you: 30 % of the first curve looked right, so the curve
      is stretched by 1/0.3 (full brightness from about 30 lx, 13 % in the dark).
      (Superseded 2026-10-07: from 30 lx up the slider acted as a cap. Now linear as you asked:
      0 lx 1 %, 175 lx half the slider, 350 lx and more the slider's value; first 500 lx, lowered
      to 350 lx by you.)
- [ ] Try the linear automatic brightness on the WT32 (built, not flashed).
- **Web UI, first increment** (2026-10-07, `web/`): page frame with menu (overlay on phones,
  sidebar from 800 px), light/dark/automatic, setup (Wi-Fi, login, done), login, status, Wi-Fi
  (saved networks with order and forget, policy, reconnect, add from the scan), hotspot (keep
  on, password, new password), system (update by file or URL through the recovery app, restart,
  factory reset). Home Assistant, dashboard and logs are placeholders. New APIs:
  `/api/wifi/remove|move|policy|reconnect`, `/api/hotspot`, `/api/hotspot/password`,
  `/api/factory-reset`; `/recovery` always serves the built-in page. Image 40 KB of 448 KB
  (Bootstrap 30 KB gzipped). Checked in headless Edge at phone and desktop width, light and
  dark; moving a network and the hotspot switch tried in the browser.
- **Fixed**: the web server ran out of sockets as soon as a browser held its connections open
  (10 lwIP sockets: 7 sessions, 2 for the server, 1 for the captive DNS). Now 16 sockets and
  12 sessions; 60 requests from 8 connections pass on both boards.
- [ ] Try the web UI yourself: the setup in the captive portal after a factory reset is the one
      flow I could not run (it needs a phone on the hotspot).
- **Your report, 2026-10-07** (all on both boards unless noted):
  - **Screen froze when the sensor lost its 3.3 V**: the touch controller's I2C reads (LVGL task)
    had no timeout (`esp_lcd` waits forever when none is set), and an unpowered module on the
    shared bus holds the lines low. Now 50 ms (20 ms gave spurious timeouts). "Choppy" came from the light sensor task retrying
    three transfers every 250 ms; now it tries one transfer every 2 s until the sensor answers.
  - **Automatic brightness had no effect**: not a bug. The sensor is outside and reads ~480 lx,
    the curve reaches the full brightness at 30 lx. New: calibration on the Display page
    ("Calibrate" in the room's normal light, "Reset"), stored per device, and a default factor in
    the board info (`light_sensor_scale`). Tested: 482 lx read as 30 lx after calibrating.
    (Superseded the same day: the calibration moved to the board, see below.)
  - **Rotation**: "Rotate" (90 degrees clockwise) on the Display page, "Lock rotation" only with a
    sensor; Rotate locks it. Stored with the display settings (older stored settings are taken
    over). Tested on the WT32 (rotate, lock, unlock back to the sensor) and the T4 (four steps
    round, kept over a restart). The T4's test screen no longer turns by itself.
  - **Connect on the screen kept the old network**: "Connect" now connects to the new network at
    once (once; the order applies again after a restart or when it fails). Console:
    `wifi connect <ssid> <password>`. Tested on the T4 against the WT32's hotspot: a wrong
    password falls back to the home network in 10 s, the right one connects in 2.8 s.
  - **Fades** like the old firmware: dimming and turning off over 3 s, waking in 0.5 s.
- **Found while testing** (2026-10-07):
  - **Updates could roll back**: the firmware confirmed itself only at the end of `app_main`,
    10 s after the start (the console's terminal probe holds it), and `update.py` reported
    "running" after 3 s. A reset in between, like the one in my test, rolled the update back to
    the recovery app. Now it confirms as soon as screen, network and web server run, before the
    console; `/api/update` has `confirmed`, and `update.py`, the web UI and the built-in page
    wait for it.
  - **I2C on the WT32 with both modules**: about 40 % of the MPU6050 reads timed out (250 of 600
    a minute), hidden until now because a failed read was skipped silently. Without the BH1750:
    none in 130 s. On the T4 with both modules: none. Longer SCL wait (clock stretching) made it
    worse. Suspects: the WT32 bus (touch controller polled at 400 kHz, 10 k pull-ups) with two
    modules on cables, or the BH1750's open ADDR pin. The tasks now count failed reads (logged
    once a minute) and treat only 5 (MPU6050) or 3 (BH1750) in a row as "gone".
  - The rotation of the last start is kept: until the sensor reports (or without it) the
    screen starts in the last rotation instead of 0 degrees.
  - Your sensors are on the T4 now: I2C on the IP5306 bus (SDA 21, SCL 22), MPU6050 at 0x68,
    BH1750 at 0x23.
- [x] The WT32 I2C failures came from the wiring while the modules were moved (your finding):
      rewired on the T4, both modules on the bus, no failed read in 130 s. The timeouts, the
      2 s retry and the failure count stay: they cost nothing while the bus is fine, and a loose
      wire in a wall panel must not freeze the screen.
- **Your review, 2026-10-07**:
  - Rotation: one row "Rotation 90°". Sensor in charge: a tap locks ("Lock"). Locked or no
    sensor: a tap opens a centered window with "Rotate" and, with a sensor, "Use the sensor";
    it closes 5 s after the last tap or with a tap outside (`settings::popup`, which also closes
    with Settings). Tested on the T4: lock, rotate (the window follows the new orientation),
    timeout, use the sensor.
  - Light sensor calibration moved from the Display page to the board: `light_sensor_scale` and
    `light_sensor_backlight_lux` (the screen's own light, taken off in proportion to the
    backlight) in the board info, found with the console command `light` (docs/adding-a-board.md
    section 8). The user has the slider.
- [x] Once: an update to the WT32's recovery app broke off (ota_0 half written). Cause: you
      moved the device during the upload. It stayed in the recovery app as designed, the next
      two uploads went through.
- [ ] **I2C unplug tests** (now on the T4 with MPU6050 and BH1750; the freeze itself needs the
      WT32, where the touch shares the bus; by hand, then plug back in):
      1. BH1750 3.3 V off and on (the case that froze);
      2. BH1750 GND off and on;
      3. BH1750 SDA off and on, then SCL off and on;
      4. the same four for the MPU6050 (the rotation stops while it is off and works again after);
      5. touch the screen during each: it may stutter for a moment, it must not freeze.
      Expected in the log: "BH1750 does not answer, trying every 2 s" and "BH1750 answers again".
      Your report on the WT32 after the timeout fix: no problems while playing with the cables of
      both sensors. The list above for the T4 wiring is still open.
- [ ] Check the direction of "Rotate": it should turn the picture clockwise.
- [x] Fades by eye (3 s down, 0.5 s up): look good (your report, 2026-10-07).
- [x] **Board check of update images** (2026-10-07): every image carries its board name in
      ESP-IDF's custom app description (`hc::board::ImageTag`, right after `esp_app_desc_t`),
      checked with the first bytes before anything is written. Tested on the T4 with a WT32
      image: "the firmware is for the WT32-SC01, this is the TTGO T4", nothing erased.
- [ ] **Recovery app screen** (not built yet, `recovery/recovery.cpp`): dark for now. Planned: a few status lines and a QR code of its
      address, drawn without LVGL (architecture.md section 9.5).
- **2026-10-07, after the break**:
  - Light sensor: the average of the last second is a rolling sum over the raw counts
    (integers, your TODO), no low-pass filter on top of the brightness's own smoothing. Fixed
    with it: after the sensor answered again, the first second mixed in readings from before.
  - WT32 updated to the current firmware and web image; `tools/test_web.py` 42 of 42 on both
    boards. `tools/update.py` retries its small requests (session, login): the first run to the
    WT32 broke off with a timeout right after the restart into the recovery app.
  - WT32 log output off again (stored).
- [x] Build the web UI (Bootstrap) and its LittleFS image in `tools/build.py` (done: `tools/build_fs.py`); the littlefs
      component's `littlefs_create_partition_image` (littlefs-python 0.15) creates images that
      mount on the device (tested with a test image).

## Step 5: Home Assistant connection

First part running on both boards since 2026-10-07 (`components/hass`): address and token from
the web UI, one WebSocket, authentication, ping, reconnect. `hass_protocol`, the subscription
of a page and `call_service` followed with step 6 (2026-10-07, see there).

- **Web UI** Home Assistant page: state, problem, version, "Try now"; address and token
  (write-only); `Search` finds Home Assistant over mDNS. **Screen**: Settings row and page (state,
  last problem, address, version, "Try now", QR code to the web UI), `HA` in the status bar, the
  alert strip after 10 s of a problem. `/api/status` has a `hass` part.
- **mDNS** (`network`): the device now answers as `<name>.local` and announces its web server.
  Before this, the setup screen showed `<name>.local` but nothing answered it. Also in the
  recovery app, which has no screen, so the name is how to find it.
- **TLS in PSRAM** for the main firmware (`sdkconfig.main`).
- Tested against a stand-in for Home Assistant's WebSocket API on the PC (auth, ping; it can
  reject the token, fall silent or refuse connections):
  - connect and authenticate in 0.3 s; the ping 25 s after the authentication;
  - the server closes the connection: back after 2 s;
  - wrong token: tried once, "token rejected", no further try (also not after a Wi-Fi
    reconnect; one try after a restart);
  - silent server: the connection is dropped 70 s after the last answer and opened again;
  - server refuses: tries after 2, 4, 8, 16, 30, 30 s; connected at the next try once it answers;
  - address errors in words: no Home Assistant at this address (HTTP 400/404/405), the host name
    cannot be resolved, the host does not answer; refused before storing: ftp://, no host,
    port 99999, a token with a space, a token over 511 characters;
  - `.local` names resolve on the device (`mockha.local`); `homeassistant.local` will too;
  - `wss://` with the certificate bundle on the WT32 (a public echo server): certificate
    checked in 4.7 s, 19 KB internal RAM left during the handshake;
  - `Search` found your Home Assistant ("Home", http://192.168.0.4:8123, 2026.9.0) and the
    stand-in, in 2.4 s. I did not connect to yours: without your token every try is a failed
    login there.
  - `tools/test_web.py` has 11 more checks (53), passed on both boards.
- Memory with a `ws://` connection: T4 8 KB internal RAM, WT32 25 KB internal left.
  Recovery app 971 KB of 1 MB (mDNS: +37 KB), 52 KB free.
- [x] **Connect your Home Assistant**: done by you on the T4 (2026-10-07), with a token of its
      own: connected, 2026.9.0. The T4 is at 192.168.0.201 now. The WT32 stays without Home
      Assistant (decided): the developer tests the final version on it.
- **Testing against your Home Assistant** (rules of 2026-10-07): reading is allowed (states,
  history, camera images). Switching only these: `light.led_strip_xmas_led_strip`,
  `light.led_strip_frame_led_strip`, `light.wled_esp32_kfc0a4271_led_matrix`;
  `sensor.bme280_living_room` may be used too (a sensor, read only anyway). **Never** the
  thermostat (`climate.lyric_thermostat_thermostat`), `lock.front_door`,
  `button.phone_ctrl_answer` (answers the intercom), `switch.phone_ctrl_allow`, the blinds
  (`switch.blinds_living_room`) and the fireplace (`switch.fireplace`). Anything else: ask.
- [x] **Save and connect** stays (decided 2026-10-07): a new token replaces the stored one, that
      is the user's choice; no test before storing.
- [ ] **Internal RAM on the WT32**: 25 KB left with Home Assistant connected, before the
      dashboard. `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` keeps every allocation under 16 KB
      in internal RAM (LVGL objects too). Decided 2026-10-07: the dashboard lives in PSRAM (as
      in the old firmware), small hot values in a compact array in internal RAM
      (architecture.md section 11). Measure before and after step 6.
- [ ] A Home Assistant with a self-signed certificate (https without a public CA) is refused
      ("certificate is not trusted"). Option later: trust the certificate on first use.
- [ ] A new address while a connection attempt hangs (up to 10 s, the network timeout) takes
      effect when that attempt ends; the page shows "connecting" meanwhile.
- [x] Host tests for `hass/src/url.cpp` (2026-10-07): `python tools/host_test.py`, 8 tests.
      `strncasecmp` (POSIX, missing in MSVC) replaced by a small helper in `url.cpp`.
- [ ] **Windows version of the UI** for the design review (decided 2026-10-07; the final version
      is tested on the WT32 by the developer): GUI only, no network, sample states written by
      the agent, taps change the sample states; LVGL 8.4 with its Windows driver; board window
      sizes and a rotate key (architecture.md section 1). Built 2026-10-07 (README, "The UI on
      the PC"), waiting for your review: `python tools/sim.py`. The driver is win32drv of
      lv_drivers 8.3.0 **with changes** (`host/sim/win32drv/README.md`): upstream cannot turn or
      resize its window and keeps its pixels private.

## Step 6: dashboard

First part done on 2026-10-07, tested on the PC (`python tools/host_test.py`: 37 tests, no
warnings at /W4); the firmware still builds (T4 main, T4 recovery: 995 KB of 1 MB, unchanged).

- **Host build** (`host/`, `tools/host_test.py`): MSVC of Visual Studio 2022 with the cmake and
  ninja of ESP-IDF, a small test harness of its own (`host/tests/check.hpp`), cJSON compiled from
  `managed_components/`.
- **`memory`**: the heap for large data. `memory::Vector` and `memory::String` allocate from it;
  `main` will point it at PSRAM (not wired yet), the PC uses malloc.
- **`formats`**: `JsonWriter` moved here from the web server (one copy; `needed()` for writing
  in two passes, `decimal()`), and a new YAML reader for the subset of architecture.md section 5
  (errors with the line; tabs, anchors, tags, block scalars, flow maps are refused).
- **`core`**: `Layout` with pages, tiles and interned entities; the kind from the entity domain;
  one `finish()` with every rule of the tile format and the placement of the old firmware
  (first-fit in reading order, back tile in the first cell of an area page, transposed portrait
  grid with the span swapped, portrait falls back to the first cell).
- **`config`**: the YAML import with includes (paths relative to the including file, at most 4
  levels, loops and paths outside the set refused, errors as `file:line: message (included from
  file:line, ...)`), `/fs/dashboard.json` read and write, the YAML export. Round trips
  JSON -> JSON and YAML -> JSON give the same text. No compatibility with the old firmware's file
  (decided): `type:` only for area and spacer, MDI icon names only, `grid: 4x3` only.
- **Sample and guide**: `docs/dashboard-sample/` (3 files, made-up names) documents every key;
  the host tests import it, so it cannot go stale.
- **Your converted dashboard** (`docs/.retracted/dashboard/`) imports: 24 pages, 133 tiles; both
  round trips pass. The test is skipped where the folder does not exist.

On the device (2026-10-07, running on the T4 at 192.168.0.201):

- `main` points the large heap at PSRAM; the new `dashboard` component loads
  `/fs/dashboard.json` at start and owns the current layout (a layout never changes once built,
  a save or import swaps in a new one; temporary file, then rename).
- Web API (`api_dashboard.cpp`, login required, main firmware only; the recovery app has none):
  `GET /api/dashboard` (JSON), `POST /api/dashboard?revision=N` (409 when the stored one is
  newer), `GET /api/dashboard.yaml` (export), `POST /api/dashboard/import[?check=1]` (multipart:
  the files of the folder, the filename of a part is its path; `hass.yaml` is the main file).
  The web server reaches it through `DashboardAccess`, filled in `main`, like `HassAccess`.
- The HTTP server task has 8 KB of stack now (was 6 KB): the import parses on it. Imported
  without trouble: your 24 files and 133 tiles, 3 include levels.
- `tools/test_dashboard.py <device>`: 23 checks passed (login required, check only, typo with
  file, line and include chain, missing include, not multipart, 600 KB refused, a refused import
  changes nothing, import, edit with revision, 409, broken JSON, a rule of the format, YAML
  export imports again). It replaces the dashboard on the device. After a restart the stored
  dashboard loads again (revision 5, 24 pages, 133 tiles). Memory: 55 KB internal RAM free
  (was 56), about 30 KB PSRAM for your dashboard; the stored JSON is 10.8 KB.
- Not done: the web UI page for it (the Dashboard page still says "comes later"), the combined
  backup file with the settings sections, and `hass:` of an import is only reported as ignored.
- [ ] **Dashboard editing** (planned 2026-10-08, [ui/web-dashboard.md](ui/web-dashboard.md), demo
      `docs/ui/dashboard-editor-demo.html` reviewed by you): tile positions per orientation in the
      format, edit mode on the screen (1000 ms long press, drag with push aside, long press on a
      tile removes it, Done/Cancel), web UI editor with an Import & export tab; the backup goes
      to the System page. Steps in section 5 of the plan.
  - [x] Format (2026-10-08): `landscape`/`portrait` cells in `core` and `config`, `move_tile`
        (push aside), `reset_arrangement`, `remove_tile`; 68 host tests; the sample's Workshop page
        is arranged.
  - [x] Screen edit mode (2026-10-08, `ui/edit_mode.cpp`): tried in the Windows version (long
        press, drag with push aside, remove, Cancel, Done, an area page, turning the display).
        Tested and approved by you on the WT32 (2026-10-09). Still open: measure how long the
        save blocks the LVGL task.
  - [x] API (2026-10-08): `POST /api/dashboard?check=1`, answers with the stored dashboard and
        the cells of every tile, problems with the tile's path, `GET /api/dashboard/icons`.
        `tools/test_dashboard.py` has checks for them; not run against a device yet. T4 main and
        recovery build (recovery partition 4 % free).
  - [x] Web editor (2026-10-09, `web/dashboard.js`, loaded by the Dashboard page; tabs Editor
        and Import & export): tried headless in Edge with your dashboard as data, the API calls
        tried on the P4 (check=1, problem path, icons). Approved by you (2026-10-09): moving,
        adding, removing and saving work. The board choice is gone (the preview is the device's
        own screen, `GET /api/dashboard/editor`); no live values in the preview (decided).
        All three boards updated with firmware and web image (`build_fs.py -p` keeps the
        dashboard).
  - [ ] `web/icons.svg` (the editor's icons are stand-ins), the entity picker
        (`/api/hass/entities`, `/api/hass/entity`; the editor takes typed entity ids meanwhile).
- [x] Approved by you (2026-10-09): the climate panel opening on the modes while off, the camera
      spinner's track at 50 %. The P4's tile gap is 8 px (your choice after trying 5).
- [x] Dimmer and climate panels on the P4 (2026-10-09, your drafts): larger header buttons,
      steppers, options row, list items and sliders (docs/ui/dashboard.md section 6), the color
      temperature marker a rounded bar. Fix: the climate `+`/`-` in portrait did nothing (the arc
      took their taps). Approved by you, tested on a 3.5" board and the P4.

Home Assistant values (2026-10-07, running on the T4 against your Home Assistant):

- `core/entity.hpp`: `EntityState` (one per entity, large heap), `EntityStore::apply()` (says
  whether a value changed), `Intent`, and the display texts (`decimals`, the rounded state of HA,
  the unit, the name).
- `hass_protocol` (new, pure, host tested): one field table (code, attribute, kind) drives the
  template and the decoder. The fields come from the tiles of the page: a tile with its own name
  does not fetch the `friendly_name`, a light without `dimmer` only its state, a sensor with
  `decimals` no rounded state; the open panel adds its fields. Result as an object or as text.
  `call_service` messages for all intents (toggle, press, `turn_on` for scene and script, level,
  temperature, modes, effect, color, color temperature), checked against the domain.
- `hass`: `subscribe_page()` (with generation), `call_service()`, `take_event()` for the UI
  (`PageValues`, `ServiceFailed`). Subscribed again at once after a reconnect, every 60 s as a
  resync, after an error at once but not more than once a minute. The task stack is 6 KB (was 4).
- Console (instead of the screen for now): `hass page <n>|off`, `hass values`,
  `hass toggle <entity>`, `hass level <entity> <percent>`.
- Tested: all 24 pages of your dashboard subscribed and decoded; `light.led_strip_xmas_led_strip`
  toggled on, set to 30 % (brightness 76), toggled off again, each change pushed by HA within a
  second; the 60 s resync; a call that does not fit the domain is refused on the device.
  Firmware 1.70 MB (`hass_protocol` 13 KB, `core` 6.6 KB).
- Found on the way: Jinja reads `{{%` as an expression, so the `{%set%}` of an entity comes after
  its key. Some of your template sensors have two-line states (`25.5 °C` and `56.5 %`); the tile
  will show the first line only, or both on a 1x2 tile (to decide with the UI).
- Not done: `PendingTracker` (comes with the UI), the climate unit of the HA configuration
  (`get_config`, for the default step 0.5 °C / 1 °F), statistics and history, camera images.

Dashboard screen (2026-10-07, in the Windows version and on the T4 with your dashboard):

- `ui`: `dashboard.cpp` (pages, navigation, the entity store, pending actions, the values from the
  backend every 20 ms, a new layout rebuilds the screen, turning rebuilds the page), `tiles.cpp`
  (switch, light, dimmer fill with drag and double tap, button, sensor, climate 1x2 with the step
  bars, area, back tile, picture placeholder), `icons.cpp`. Described in `docs/ui/dashboard.md`.
- `core/pending.hpp`: `PendingTracker` (expected state, level within 3, setpoint within 0.05,
  5 s timeout), host tested. `PageValues`/`ServiceFailed`/`HassEvent` moved to `core`, so `ui`
  does not depend on `hass`. The `Backend` has `dashboard`, `subscribe_page`, `call_service`,
  `take_event`.
- Icons: `tools/build_icons.py` (MDI 7.4.47 downloaded into `build/mdi`, lv_font_conv 1.5.3 by
  npx) writes `components/ui/src/fonts/hc_mdi_24.c` / `hc_mdi_48.c` and `icon_glyphs.inc`. The
  `core` list grew to 43 names with the HA defaults (robot, script-text, palette, door, eye,
  checkbox/radiobox); plus arrow-left, plus, minus. Flash: 24 px 8.8 KB, 48 px 16.4 KB.
  Montserrat 28 for the values: 37 KB. Firmware 1.81 MB of the T4's 2.44 MB slot.
- The test screen shows only while there is no dashboard; an import replaces it at once
  (`hide_test_screen()`).
- Windows version: `sample_hass.cpp` makes up sample states from domain and name (every tenth
  entity unavailable), values 150 ms after a page is opened, an action changes the sample after
  400 ms (the pending border is seen); nothing arrives while the scenario's Home Assistant is not
  connected. `--dashboard none` shows the test screen. I edited `host/sim` for this (the other
  agent was done): CMake (the fonts, `entity.cpp`, `pending.cpp`), the backend table, `main.cpp`.
- On the T4 (no touch; console taps): your main page and Rooms with live values, 71 KB internal
  RAM free. A value that is too wide steps down through the font sizes (found on the T4: "31 ppm"
  and "Detected" were cut in the 74 px cells of 240x320).
- Note: the console's `hass page`/`hass values` now compete with the screen for the subscription
  and the values; they are for checks only.
- The WT32 build directory reported "opening deps log: Permission denied" (another program held
  `build/wt32_sc01/main/.ninja_deps`); not built for the WT32 this time.

Next:

- [ ] Review the dashboard in the Windows version (`python tools/sim.py`, your dashboard with
      `--dashboard docs/.retracted/dashboard/hass.yaml`); the open points are in
      `docs/ui/dashboard.md` section 5.
- [ ] Panels: light, climate, sensor history (statistics), camera image. Your first test of the
      Windows version (2026-10-07): tapping a sensor, a dimmer and a climate tile does nothing,
      because these panels are not built yet (the taps are reserved for them). Order (your
      decision): light, climate, graph.
  - [x] **Light panel** built (2026-10-07, `panel.cpp`, `light_panel.cpp`, docs/ui/dashboard.md
        section 6): power, level slider with steppers, color wheel, color temperature, effects.
        Checked by me in the Windows version at 480x320, 320x240 and 320x480; firmware builds for
        the WT32 (1.78 MB). Approved by you in the Windows version (2026-10-07); the gap between
        the back tile and the name widened from 4 to 12 px after your review.
  - [x] **Climate panel** built (2026-10-07, `climate_panel.cpp`, docs/ui/dashboard.md section 6):
        mode/preset/fan pills with their lists, the arc (drag), the steppers. A tap on a climate
        1x1 tile, or beside the bars of a 1x2 tile, opens it. Checked by me in the Windows version
        at 480x320, 320x240 and 320x480; firmware builds for the WT32 (1.80 MB; 5 new glyphs).
        Approved by you (2026-10-07). Not done: `heat_cool` with two setpoints.
  - [x] Your review (2026-10-07): landscape gap between the left column and the control doubled
        to 8 px (both panels); a list that fits is centered; the selected item of a list that
        opens out of view is scrolled into the middle (effects, climate options). Checked in the
        Windows version; one list implementation in `panel.cpp` for both panels now. After your
        portrait screenshot (3 modes in 2 columns, the third alone): a list that fits in one
        column is a single centered column.
  - [x] **Sensor history panel** built (2026-10-07, `sensor_panel.cpp`, docs/ui/dashboard.md
        section 6): live value, 48/24/12 h, the statistics curve with round value labels and
        local hour lines, the on/off timeline of binary sensors with its legend. New on the way:
        `core::History`, `hass_protocol::history_message`/`decode_history` (host tested, 53
        tests), the hass task reads the window in 6 h chunks (`hass::fetch_history`), the
        `Backend` has `fetch_history`, `clock`, `local_time`; the Windows version makes up a
        daily curve and motion changes. Checked by me in the Windows version at 480x320, 320x240
        and 320x480; firmware builds for the WT32 (1.82 MB). **Not tested against your Home
        Assistant yet** (the device was not flashed). Approved by you in the Windows version
        (2026-10-07). Open: a sensor
        without a state_class has no statistics (message instead of a graph).
        Your review (2026-10-07): the motion strip has 16 px left and right (it kept the column of
        the value labels), is centered in the card above the time labels, the legend centered
        above it.
  - [x] **Camera image** built (2026-10-07, docs/ui/dashboard.md section 6): the picture tile
        shows the image with its name on a band and refreshes it (`refresh`), a tap shows it full
        screen. New: `core::Image` with the cover scaling (host tested, 54 tests), the media task
        of `hass` (`media.cpp`: camera proxy over `esp_http_client`, `esp_jpeg` 1.3.1 with the ROM
        decoder), `Backend::fetch_image`; the Windows version draws a landscape. Checked by me in
        the Windows version (your Cameras page, full screen and back); firmware builds for the
        WT32 (1.83 MB). **Not tested against your cameras** (the device was not flashed): stack
        use, decode time and progressive JPEGs still to see. Approved by you in the Windows version
        (2026-10-07), the name on the band centered after your review.
  - [x] **MJPEG stream test** (2026-10-08, your request; docs/ui/dashboard.md "MJPEG stream"):
        `stream:` of a picture tile, first frame on the tile, live full screen with the frame rate.
        Tested on the WT32 (COM9, 192.168.0.199) with your TestCams: 800x480 1.3-1.4 fps,
        640x320 2.3-2.4 fps, the 2 fps stream complete. HA snapshots tested on the device too
        (your Cameras page, 0.5-2.6 s per image). To decide: keep it, the ffmpeg settings, the
        overlap of reading and decoding on the second core. The fps label and the console's
        `hass streamscale` are test aids.
        Your feedback (2026-10-08): turning the display in full screen rebuilt the view at the
        old size (the parent was not laid out yet; the screen's size is used now) and the stream
        at it; the tiles stayed without an image after closing (they ask again now); a tap
        closes on the release (a finger that moved a little counted as a swipe); a spinner shows
        until the first frame. With `-q:v 7` the frames stay at 35 KB (before: 10 KB after the
        start): 640x320 1.5 fps, 800x480 0.9 fps; a higher q:v (smaller frames) is faster.
        Your PC's firewall blocked go2rtc for the LAN (Block rules of go2rtc.exe on your
        Desktop); after your OK the TCP Block rule is disabled and "go2rtc MJPEG (hassctrl
        test)" allows TCP 1984 on Private networks.
        P4 (JC4880P443C, COM11, your report "800x480 -r 10 -q:v 15 is choppy", 2026-10-08):
        the stream came at 3.3 fps because go2rtc waited while a frame decoded (TCP window
        5.7 KB); with 64 KB 5.3-5.6 fps (decode 139 ms), 640x320 all 3 fps. The P4 also
        restarted when a stream started (memory corrupted: heap, lwIP, FreeRTOS lists); the
        DMA2D copy into the frame buffer was the cause (4 of 4 starts crashed with it, 0 of 17
        without), it is off now. `stream: hass` plays HA's MJPEG stream: camera.balconycam
        704x576 2 fps, camera.doorspy_cam_axxon0 1280x960 at 1:2 2.5 fps (a stream frame is now
        scaled to show at least 3/4 of it, doorspy showed only its middle before); the HA
        snapshot of camera.balconycam failed (decoder work area too small, 8 KB now). Your
        TestCams has a 4x3 grid and the two HA streams. To check by you: tearing and the redraw
        without the DMA2D (no tearing, checked by you).
        Hardware decoder (your request, 2026-10-08): the P4's JPEG decoder and PPA
        (`hass/src/image_hw.cpp`), the stream reads and decodes while the UI draws, LVGL's
        rotation buffer 48 KB. Shown now: `_xl` 15 fps, `balconycam_mjpeg` 12 fps (all the
        source sends), `_m`/`_s` 5/3 fps, HA doorspy 9.4 fps (was 2.5), HA balconycam 2 fps;
        6 starts and 3 minutes of `_xl` without a fault. TestCams has a 4x4 grid with `_xl`.
        The UI's 50 ms per 800x480 frame (LVGL 8's software rotation) is the next limit
        (about 20 fps). Checked by you: no tearing with the CPU copy, 18-22 fps with
        `-r 25`; finalized by you (2026-10-08). The fps label and `hass streamscale` stay as
        test aids.
  - [x] **S3 streams** (2026-10-08, your request, JC3248W535 on COM10): `esp_new_jpeg` for the
        S3, media task on the second core, TCP window 23 KB. 640x320 at 10 fps shows all frames
        (was 4.6), `_xl` 800x480 6.6 fps (scaling in the decoder). The pinning tested on the P4
        (unchanged) and the WT32 (640x320 2.3-2.6 fps, 800x480 1.8 fps). Optional: a stream in
        the S3's screen size (480x320) needs no scaling.
- [x] **Bug (your report): the P4 showed "--" on every tile after a start** until a panel was
      opened and closed. The UI subscribed its page 80 ms before `hass` had created its queue,
      and the subscription was dropped (`event 7 dropped`); the queue is created with its first
      use now. Checked on the P4 after a restart (2026-10-08).
  - [ ] Test the panels on the WT32 against your Home Assistant (history requests, the
        climate panel changes the real thermostat).
- [x] **Bug (your report): switchtest1 on -> dimmertest1 showed 100 % before the real 33 %.** The
      pending "on" of a light whose level HA does not report while off was drawn as "a light
      without a level" (100 %). Now the store keeps the last level (`last_brightness`) and a light
      that is switched on shows it; one never seen on shows `On`. Checked in the Windows version,
      confirmed by you together with the light panel (2026-10-07). The cyan border you saw is the pending
      look (docs/ui/dashboard.md section 3): the action is on its way until HA reports it.

## Your reports of 2026-10-07 (afternoon)

- [x] Bug: a network that failed with "network security not supported" was tried again and
      again (the first of the order). Now any failed attempt, not only a wrong password, moves on
      to the next saved network in range; when every saved network in range has failed, a new
      round starts in the configured order. A connection that was up and broke is tried first
      again. (`network.cpp`, `on_disconnected` / `on_scan_done`.) Not reproduced on purpose:
      tested only that both boards still connect.
- [x] Bug: the time zone was never taken from Home Assistant (it was not built yet). Now `hass`
      sends `get_config` after every authentication and hands `time_zone` ("America/Vancouver")
      to `platform::set_time_zone()`, which turns it into a POSIX rule (table of 461 zones,
      `tools/build_timezones.py` from posix_tz_db, MIT) and stores the name in the settings
      (`time`/`zone`) only when it changed. At start the stored zone is set before the network:
      on the T4 at 2.4 s after boot, before Home Assistant answered.
- [x] NTP took 2 minutes after a restart: SNTP started before the station had an address, and
      lwIP's retries backed off (15, 30, 60 s). Now SNTP starts on every new address, without the
      random startup delay, with two servers (pool.ntp.org, time.cloudflare.com). On the T4 the
      clock is set 150 ms after the address (5.7 s after boot).
- [x] WT32 built and flashed on COM9 (recovery app, then the main firmware; the file system
      image was left as it is). It runs and connected to 'WhatDoesTheFoxSay2' (192.168.0.209).

- [x] Bug: the serial console on COM9 (WT32) took no input. Its task (10 KB stack, `screenshot`
      renders on it) was created last in the boot, after Wi-Fi and Home Assistant (set up on the
      WT32 now) had used and fragmented the internal RAM: `xTaskCreate` failed (`ESP_FAIL`).
      Because the stored log level is applied by the console, the log was on as well. Now
      `devconsole` runs its own REPL task (as ESP-IDF's console example does), created right
      after the storage: the stack is taken while the RAM is in one piece, the terminal probe runs
      in that task instead of holding app_main, the stored log level applies from the start, and
      commands are taken once `main` calls `devconsole::ready()`. Tested on both boards (WT32
      with Home Assistant connected: `wifi status`, `hass values`, `screenshot`).
- [x] LVGL's memory in PSRAM (your rule from 2026-10-07, which I had applied to the dashboard
      model only): `main/lvgl/hc_lv_mem.h` (PSRAM first, internal RAM only when the PSRAM is
      full, no fixed pool, so no limit to hit), set on the LVGL library in `main/CMakeLists.txt`;
      the tile views and their names on the large heap. WT32 with Home Assistant connected, test
      screen: internal RAM free 23 -> 45 KB; partial refresh avg 7 -> 10 ms (max 15 -> 25 ms,
      80 -> 70/s), full screen 55 -> 60-65 ms.
- [ ] **Internal RAM of the WT32 is nearly used up**: 23 KB free with Home Assistant connected,
      and about 21 KB of that is IRAM, which data cannot use. The T4 has 71 KB. The large users
      are the two DMA draw buffers (320 x 40 lines x 2 bytes x 2 = 51 KB) and every allocation
      below 16 KB, which goes to internal RAM first (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`,
      LVGL objects, strings). Proposal: lower that limit so small allocations go to PSRAM and
      measure the render times, or use fewer draw buffer lines. To decide.

## Your review of 2026-10-07 (evening)

- [x] **Slider knob cut off at 100 %**: the track left 10 px beside the slider, the knob reaches
      out 11. Now 16 px. Checked on the T4 at 100 %.
- [x] **Dim after up to 1 h, screen off up to 24 h** with `-` and `+` and a growing step like the
      old firmware (17 and 19 values instead of dropdowns of 6). Screen off is now stored in
      32 bits; the stored settings of the older versions are converted (by their size), tested
      on the T4: dim after, dimmed brightness and screen off were kept. 24 h stays over a restart.
      Holding a button steps 4 times a second (the touch repeats every 100 ms, which ran to 24 h
      in 1.5 s).
- [x] **Rotation popup moved its button when the display turned** (your report): now a fixed 200 px
      square at the center with the content placed by hand, the Rotate button a square at its
      exact center. Checked on the T4 in landscape and portrait.
- [x] The fades: marked done after your report. The recovery screen was **not** built; its item
      says so now.

## Known issues

- [x] **"Header fields are too long" (431)** in your browser on 192.168.0.199 (2026-10-07): the
      web server took 1 KB of request headers; your browser sends more, most likely cookies of a
      device that had this address before. Now 8 KB (`max_req_hdr_len`; the buffer grows only as
      far as a request needs). Tested on both boards with 4.8 KB of cookies, the login cookie is
      found before and after them.
- [ ] The web files are served gzip-compressed even to a client that does not ask for it
      (`curl` without `--compressed` gets binary data). Browsers always accept gzip. Fix if a
      tool needs it: answer 406, or keep uncompressed copies.

- [ ] **ESP-IDF 6.1 bug**: `otatool.py write_ota_partition` fails with "unexpected keyword
      argument 'input'". `tools/build.py` writes ota_0 with `parttool.py` instead. Worth
      reporting to Espressif; `read_ota_partition` probably has the same problem.
- [ ] **Your WLED controller on COM7** was flashed with this project's step 1 firmware by mistake
      on 2026-10-06 (COM7 was taken for the T4). You are reflashing WLED yourself.
- [x] ~~Size check against the recovery partition~~: with Wi-Fi the main firmware passed 1 MB
      (1.22 MB). ESP-IDF 6.1 only warns that 1 of 2 app partitions is too small and the build
      succeeds, so nothing blocks. The warning stays in the build output.
- [x] ~~Both applications compiled every component~~: the recovery build failed once it compiled
      the UI without the main firmware's LVGL settings. Each application now builds only its
      top-level component and what it requires (`COMPONENTS` in the root `CMakeLists.txt`).
- [ ] esp_lvgl_port 2.9.0 calls the deprecated `esp_lcd_touch_get_coordinates` (warning in its
      own code). Goes away with a newer esp_lvgl_port; nothing to do on our side.
- [ ] ESP-IDF 6.1 prints component validation warnings about its own `esp_wifi` and
      `wpa_supplicant` components. Not ours; nothing to do.

## Toolchain cleanup (optional)

- [ ] `C:\Espressif\python_env\idf6.1_py3.11_env`, `C:\Espressif\idf-env.json` and
      `C:\Espressif\espidf.constraints.v6.1.txt` came from an extra run of ESP-IDF's
      `install.ps1`. EIM's own environment is `C:\Espressif\tools\python\v6.1\venv`, so these can
      be deleted.
- [ ] `%USERPROFILE%\.espressif` still holds the ESP-IDF 4.4 tools.
- [ ] VS Code ESP-IDF extension: its user settings still point to the removed ESP-IDF 4.4.

## Open decisions

Setup and network ([architecture.md](architecture.md) section 9.2):

- [x] Fast reconnect with the stored access point and channel (decided and running).
- [ ] Slowing down reconnect scans while a phone is on the hotspot: still open, less important
      now that most reconnects need no scan.

Screen ([ui/screen.md](ui/screen.md) section 7):

- [ ] Does a swipe from the left edge also go back?
- [x] Pull-down start zone 80 x 24 px; message order errors, connecting, hint (implemented,
      to confirm by hand).
- [ ] Line limit of the alert strip (proposal 3 lines); it wraps at 90 % of the width now.

Orientation ([architecture.md](architecture.md) section 8.1):

- [x] Implemented with the old firmware's defaults (500 ms hold, rest check), rotation in the
      panel controller. Running on the WT32-SC01.
- [x] MPU6050 wiring documented on the WT32-SC01 device page.

Web UI ([ui/web.md](ui/web.md) section 10):

- [ ] OTA push method in detail.

Settings export and import (2026-10-07; decided: no secrets in the export, Wi-Fi networks as
names only, YAML like the dashboard, an import may carry passwords added by hand):

- [ ] **What goes in.** Today's settings in NVS, sorted:

  | Namespace / key | Export | Why |
  | --- | --- | --- |
  | `network/networks` | names and order, no passwords | decided |
  | `network/policy`, `network/ap_enabled` | yes | user choices |
  | `network/ap_password` | no | a secret; the new device keeps its own |
  | `network/last_seen`, `last_index` | no | state of this device's radio, not a setting |
  | `ui/display` | yes, as named fields | brightness, dim, off, automatic, rotation, lock |
  | `ui/hint` | no | state ("hint seen") |
  | `webui/*` | no | the login is a secret, the new device keeps its own |
  | `light/scale` | no | per device, the developer's calibration |
  | `console/log_level` | no | development |
  | later: Home Assistant URL / token | URL yes, token no | the token is a secret |

- [x] **Format** (decided 2026-10-07): YAML, the same as the dashboard, readable and editable by
      hand, one section per owner, only named fields (never NVS blobs, whose layout changes
      between versions):

      format: hassctrl-settings
      version: 1
      board: TTGO T4
      firmware: 0.1.0
      network:
        policy: order
        hotspot_on: false
        networks:
          - name: Home
          - name: Garage
            password: added-by-hand   # optional, the export never writes it
      display:
        brightness: 80
        dim_brightness: 20
        dim_after_s: 60
        off_after_s: 0
        automatic: true
        rotation: 90
        rotation_locked: false

- [x] **One file for settings and dashboard** (decided 2026-10-07): one backup file with optional
      sections (`hass`, `network`, `display`, `defaults`, `tiles`); the export offers settings,
      dashboard or both. One small pure C++ YAML reader and writer (host tests) serves both, also
      the recovery app for the network section.
- [x] **Transfer** (decided 2026-10-07): the export is written straight into the HTTP answer (chunked,
      like `files.zip`), no file on the device. The import is received into PSRAM (cap about
      256 KB; the old dashboard is about 20 KB), checked completely, then applied; no staging
      directory. A split set (includes) comes as one multipart request from the folder picker.
      No tar.gz: the files have to be in memory anyway because an include can name any file of
      the set, and compression saves nothing that matters for 20 KB on a LAN.
- [x] **Import rules** (accepted 2026-10-07): the whole file is checked before anything is written; a
      section that is missing stays as it is; an unknown section or field is ignored and named in
      the answer; another board is fine (only the rotation may not make sense, it is applied
      anyway). The answer lists what was applied and which networks need a password.
- [x] **Passwords in an import** (decided 2026-10-07): a network in the file may carry a
      `password:` added by hand, it is used as it is. The export never writes one.
- [x] **Networks without a password** need a change in `network`: a saved network gets a flag
      "password needed" (an empty password is a valid open network, so it cannot be the marker).
      Such a network is never tried. The Wi-Fi page (screen and web) shows "Password needed" on it,
      and a tap asks for the password. Imported names are merged: a network that is already
      saved keeps its password, so an import over Wi-Fi does not cut the connection; the order
      comes from the file; more than 5 are cut off with a note.
- [x] **Who writes the sections** (decided 2026-10-07): a table of sections filled in `main`, like
      `hc::ui::Backend` and `HassAccess`. Each entry has a name, `write` (export), `check`
      (import, step 1) and `apply` (import, step 2); nothing is applied before every section
      passed its check. The main firmware registers `network`, `display`, `hass` and the
      dashboard; the recovery app only `network`. The web server owns the request and the file
      format, nothing else. Rejected: a self-registering registry in `platform` (what is in an
      app becomes implicit), everything in `webserver` (it would depend on the UI and the
      dashboard).
- [x] **JSON reading**: settled. `espressif/cjson` from the registry is in the main firmware since
      step 5 (`components/hass`, about 5 KB of flash, allocations prefer PSRAM); the dashboard
      storage reads it too. The settings file is YAML now, so its import does not need it, and
      the recovery app stays without cJSON.
- [x] **Where** (decided 2026-10-07): System page, a "Backup" card with Export and Import
      **above** the update card, so the backup is seen before an update (web.md section 6). The
      recovery app's page offers the export of the network section next to files.zip.

Dashboard ([architecture.md](architecture.md) section 14):

- [x] **Discussed 2026-10-07.** Decided: only `defaults` (grid, more later) and `tiles`; URL,
      token, poll and the other connection values are not part of the dashboard; no reload by
      file size; the web UI edits areas and tiles, the screen only allows a few settings; YAML
      is only the import/export format, with includes; resync fixed at 60 s, immediately after
      an error.
- [x] **Storage in JSON** (decided 2026-10-07, details too): `/fs/dashboard.json`, atomic write, format version, revision
      (`409` on a conflict), stable tile ids. One validator for JSON and YAML.
- [x] **YAML import** (decided 2026-10-07, kept simple): on the device in `config`, with a new
      parser (the old one is AI-generated and only a reference); upload of the whole set (folder
      picker), "check only" option; `- include: path` as an entry of any tile list, included
      file is a plain list at column 0, relative paths inside the set, 4 levels, no globs, no
      variables; any indentation and key order; `hass:` is accepted (`url`, `token`; a file
      without a token is preferred); the export dumps everything into one `hass.yaml` for
      backup, without the token and without the comments of the import.
      2026-10-07: folder picker / single YAML file, Check, Import and YAML download are on the
      built-in page (`/recovery`, works with an empty LittleFS); on the WT32, check-only import
      of the sample passes. The web UI Dashboard page has the same (stored revision/tiles,
      import, check, download); on the WT32, not yet tried in the browser.
      Test input: your dashboard converted to the new format, one file per area, in
      `docs/.retracted/dashboard/` (24 files, git-ignored, 3 include levels, no secrets). The
      4 test pages that used `position:` got a spacer each, so first-fit gives the old cells;
      checked: same tiles and same landscape cells on all 24 pages.
- [x] **Order per orientation** (decided 2026-10-07): automatic order as in the old firmware, no
      moving of tiles for now, no portrait grid of its own (both postponed).
- [x] **Tile format review** (decided 2026-10-07): kind from the entity domain, `type` only for
      `area`/`spacer`, `dimmer` type replaced by a `dimmer` flag of lights; MDI icon names
      without duplicates; sizes as `4x3`; `position:` dropped; `decimals` from HA's display
      precision; the web UI fills in new tiles from HA.
- [x] **Tile format details** (decided 2026-10-07, architecture.md section 5): domain table,
      import of the old `type:`, icon order the same as or close to Home Assistant,
      `friendly_name` fallback, climate step from `target_temp_step`, light step for `+`/`-`
      (default 10 %, repeat while held), no `min`/`max` for lights.
- [ ] **Camera live view** (later): Home Assistant streams every camera as MJPEG on
      `/api/camera_proxy_stream/<entity>` (from still images, at least 0.5 s apart, unless the
      camera has its own MJPEG stream), so no extra MJPEG source is needed for tests (`stream:
      hass` plays it since 2026-10-08, see the MJPEG stream test above). The P4 has a
      hardware JPEG decoder; H.264 is hardware encode only, decoding is software. The fullscreen
      view of the old firmware already asks for a frame in display size.

## Review

- [ ] Read [architecture.md](architecture.md) and [ui/](ui/README.md) once from top to bottom.
      Everything not marked as decided is a proposal; mark disagreements as comments or in chat.

## Next steps

These follow [architecture.md](architecture.md) section 13:

1. ~~Project skeleton~~ runs on the T4, see step 1 above.
2. ~~Board bring-up~~ runs on the T4 and the WT32-SC01 (with touch and orientation), see step 2.
3. ~~Platform and network~~ runs on the T4, see step 3.
4. First setup, Settings and web server: done except the open points of step 4.
5. Home Assistant connection: first part running (connect, authenticate, reconnect), see step 5.

- [ ] **WT32 replaced + LDR** (2026-10-07): new WT32-SC01 flashed on COM9 (recovery app, main, web UI image).
      LDR on IO34 (ADC1_CH6) with 10k to GND, `hc::light_sensor::ldr` (ldr.hpp): pin voltage -> LDR
      resistance -> lux through a table of up to 8 points, log-log interpolated; default GL5528 curve until
      there are points. Console: `light`, `light watch [s]`, `light calibrate <lux>` (adds a point),
      `light points`, `light reset`. Reads 2.6 V / 2.6 kOhm in room light. Waiting for the reference
      lux meter measurements to see how linear it is and pick the calibration points.
- [x] MPU6050 on the new WT32 (2026-10-07, checked on the device by the user: all four rotations correct):
      the module reads 1.55 g standing still, offset (-0.12, -0.53, +0.21) g, gain 0.99, tilt 2.5 degrees.
      Measured with the developer tool `orient capture 0|90|180|270|flat` (0 and 90 89.9 degrees apart, the
      pairs agree to 0.00 g) and written to `wt32_sc01.cpp` as `imu_axes` and `imu_offset`. With the offset
      taken off, all five poses read 0.99 to 1.00 g. What it took:
      - the measurement was taken the other way up than the screen reads at rotation 0 (landscape upside
        down): both axes negated in the board info; the documentation now says how to pick pose 0;
      - the portrait branch of `rotation_from_calibration` was the wrong way round (reading along e1, the
        quarter turn clockwise, needs rotation 90 here), fixed in the code;
      - a new rotation came late and at random: the gyroscope reads 12 deg/s at rest, the old rest limit was
        15. Rest is now judged from the accelerometer alone (reading changes less than 0.05 g between two
        samples, 0.010 to 0.040 seen at rest), the gyroscope is not read for it.
- [x] **MPU6050 I2C timeouts** (2026-10-07), the cause was not the wiring: the orientation task ran at priority 2
      and was starved by LVGL (4), hass (4) and network (5) for up to a second, so a transfer timed out about
      every 4 s even untouched and `orient capture` found no fresh reading (33 of 150 polls). Now priority 6
      (0 of 150 polls, 0 failed, 0 retried), back to 400 kHz for the module (it was 100 kHz while guessing; stable on the T4 at 400), two immediate retries, a `slow:` warning for
      gaps over 400 ms, `orient` shows the failed and retried reads. The wiring explanation above (rewired on the
      T4) may have hidden part of this: check the task priorities first if reads fail again.
- [x] `log lvgl` (2026-10-07): the test screen report every 2 s and the touch messages (LV_LOG_USER, tag `lvgl`)
      are off by default, `log lvgl` toggles them and the choice is stored; warnings and errors of LVGL stay.
- [ ] LDR table as board defaults (now only in the NVS of this unit); recalibrate the dim end at night.
