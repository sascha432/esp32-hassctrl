# TODO

State as of 2026-10-16: steps 1 to 6 of [architecture.md](architecture.md) section 13 run on the
WT32-SC01 (COM9), the JC3248W535 (COM10) and the JC4880P443C (COM11); the TTGO T4 (COM13) is a
test device only. Step 7 (LVGL 9) has not started. What is next is in "Next steps" at the end.
The design is in [architecture.md](architecture.md) and [ui/](ui/README.md).

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
- [x] New boards arriving 2026-10-06: bring them up with
      [adding-a-board.md](adding-a-board.md). Done (the JC3248W535 and the JC4880P443C run).

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
      on the board by the user (2026-10-08; the fade was replaced by the slide). 2026-10-13: the
      dialog did not stand out of the dimmed page on the board (your report): veil 80 %, dialog
      body `theme::kDialog` 0x161d25, alert strip 90 % on all boards; variant picked by you in the
      sim (B of A/B), flashed on the P4, confirmed working by you on the board (2026-10-13).
- [x] **Check the start orientation** on the WT32 leaning on the desk: it should be upright
      within about a second after boot, without turning it. Working, confirmed by you 2026-10-10.
- [x] PSRAM of the WT32: the device page says 4 MB (decided 2026-10-07), whatever the boot log
      reports.
- [x] Test screen out of the firmware (2026-10-09): only with `CONFIG_HC_BOARD_BRING_UP=y` in the
      board's sdkconfig.defaults (docs/adding-a-board.md), none of the four boards sets it; the
      map files have no `show_test_screen`. The start screen (Home Assistant logo, docs/ui/lvgl/lvgl-dashboard.md
      section 1.2) took its place.
- [x] **Start screen and backlight on the P4** (2026-10-09, confirmed working by the user on the
      JC4880P443C; built for all four boards): the logo with the runner after boot until the dashboard's values are in,
      alert strip and pull-down over it, "No dashboard yet" without one; backlight off until the
      first frame (fades up in 0.5 s); backlight off before a restart. The cyan screen came back on
      the P4 (user, 2026-10-09): the pin hold does not survive its reset and the pin is undriven
      for 1.3 s until `backlight_init` (0.85 s of it the PSRAM test). Now the restart also switches
      the panel off and to sleep: the cyan is gone. The runner left a dot at the top for the first
      second (a slow first frame moved it further than the redrawn stretch); it now redraws from
      where it was at the last step, the dot is gone.
- [x] Start screen and restart on the WT32-SC01 and the JC3248W535 (same code). Confirmed by you 2026-10-10.

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
- [x] WT32: add the password of network 1 again (user).
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
- **Login** (docs/ui/web/web-app.md section 3): the web UI is open in setup mode until a login exists.
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

**Screen** (docs/ui/lvgl/lvgl-settings.md), running on both boards since 2026-10-06, tested through the
console (`swipe`, `tap`, `screenshot`) in every rotation:

- **Pull-down zone** (80 x 24 px, top center, `components/ui/src/overlay.cpp`): dragging down
  30 px opens Settings.
- **Alert strip** (top center, rounded bottom corners, 60 % black): Wi-Fi error, "Wi-Fi is not
  set up", connecting, then the one-time hint "Swipe down for Settings". A tap opens the
  matching page; the hint stays until Settings was opened by the swipe (stored in NVS `ui`).
  Since 2026-10-10 it also stays while Home Assistant is not set up, and goes after a swipe once
  Wi-Fi and Home Assistant are connected (confirmed by you on the P4).
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
  first, which cut "1  My network" to "1...". In a row the name is as wide as its text (at most
  60 %) and the value takes the rest; the value used to stop at 50 %, which cut a long Wi-Fi
  name beside "Wi-Fi" (fixed 2026-10-10, confirmed on the WT32). Hotspot and Home Assistant pages put the QR code
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
      change (`platform::set_blob_later`, written at a restart too). Defaults: dim after 5 min to
      20 %, off after 1 h (were 1 min and never off until 2026-10-11). Tested on the WT32: dims
      at 30 s, off at 60 s, the waking touch does not reach the screen, a touch on the dimmed
      screen does.
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
- [x] **Pull-down hard to hit** (your report, 2026-10-07): the start zone grew from 80 x 24 to
      160 x 40 px, the drag to open from 30 to 24 px (`components/ui/src/overlay.cpp`). Flashed;
      tried on the WT32, confirmed by you 2026-10-10. A tap on the top center of a tile under the zone now hits the zone.
- [x] **First setup screen** (2026-10-07, `components/ui/src/setup.cpp`, lvgl-settings.md section 4):
      shown at boot while no network is saved (console: `setup`). Two ways side by side
      (stacked in portrait): "Choose Wi-Fi" on the screen, or the phone (QR code to join the
      hotspot, its name and password, then the portal or http://192.168.4.1). Once a network is
      saved, here or from the phone, it follows the connection: connecting, connected (address,
      `<name>.local`, Next to the dashboard) or failed (the error, "Other network", "Try again").
      Tested on the T4 through the console: welcome, Choose Wi-Fi and back, adding a network,
      connected, Next.
- [x] Setup screen: the Web UI login and Done pages of lvgl-settings.md section 4 dropped (2026-10-10):
      the browser setup on the phone does Wi-Fi, creating the login (signs in at once), then
      Done with "Keep the hotspot on", which works (tried by you); `Next` on the screen goes straight to the dashboard.
- [x] Setup screen: the "failed" view seen and tested by the user on the WT32 (2026-10-10): the
      wrong password showed, but the device kept retrying with no hotspot for good. Fixed
      (`setup_attempt_failed`, network.cpp): during the first setup, or while no saved network
      ever connected, a wrong password brings the hotspot back at once, other errors after 3
      failed attempts; it stays on until a connection. Wrong password, a network that is not
      found and a successful connection with its IP all show; the setup finished and the
      "hotspot off" choice was kept for the main firmware (uploaded through `/recovery`).
- [x] **Files of a broken file system** (2026-10-07, `api_files.cpp`): `GET /api/fs/files.zip`
      streams every readable file as one .zip (stored, CRC per file; what cannot be read is
      listed in `unreadable.txt` inside), `POST /api/fs/format` with `confirm=yes`. In the
      built-in page (`/recovery`, also when the web UI is gone) and on the System page. Tested on
      the T4: the archive checks out in Python and unpacks in Windows, the files are identical to
      the sources; formatted, then the web image uploaded again.
- [x] **Settings export/import**: decided 2026-10-07 (one YAML backup file, sections from a table in `main`, see "Open decisions"); done 2026-10-10, confirmed by you on the WT32.
  - [x] Step 1, "password needed" in `network` (2026-10-09): a saved network has the flag (own NVS
        key `pw_needed`, the stored networks keep their layout); such a network is never tried,
        not even by the fast reconnect; with no network left to try the hotspot comes on as at
        the first setup. `import_networks()` merges a file's list (plain C++ `merge.cpp`, 7 host
        tests): the file's order, then the saved ones it does not name, at most 5, the network in
        use is never cut off, a saved network keeps its password. Screen: "password needed" in
        the list, the saved network page explains it and has "Enter password"; web UI: a badge
        and an inline password field; console `wifi list` marks it. On the WT32: both saved
        networks load after the update and it connects; after an import (step 5) the flagged
        network is skipped and the device connects to the next one, also after a restart.
        `saved_networks()` returns a struct per network (name, open, password needed).
  - [x] Step 2 (2026-10-09): the file format in `config` (`hc/config/backup.hpp`, 10 host tests
        with made-up sections): header (`format: hassctrl-backup`, `version`, `board`,
        `firmware`), `Writer` and `Fields` for named fields, two passes (check everything, then
        apply). Not in `webserver` as decided on 2026-10-07: then the recovery app (4 % free, no
        backup) would link the YAML parser too. The sections in `main/backup.cpp`: `network`
        (policy, hotspot_on, names, `open: true`, a hand-added `password:`), `display`, `sound`
        (volume, tap/slider sound, vibration), `alarm` (time, days `[mon, ...]`, snooze, sound),
        `hass` (url, a hand-added token; `hass::check_config()` checks it before anything is
        applied), and the dashboard (one file, `import_yaml` with check). A section the device
        lacks (alarm on the WT32) is named "not on this device". The UI's settings change under
        the LVGL lock, then `ui::settings_imported()` applies brightness, rotation and touch
        feedback and closes Settings.
  - [x] Step 3 (2026-10-09): `GET /api/backup.yaml[?parts=settings|dashboard]`
        (`<device>-backup.yaml`), `POST /api/backup/import[?check=1]` (the file as the body, 256 KB
        at most; answer: sections, problems, ignored, notes; 400 when refused). Built in memory,
        not chunked: the file is a few KB to a few dozen.
  - [x] Step 4 (2026-10-09): the Backup card on the System page, above Update (Export all,
        Settings only, Dashboard only; file, Check, Import, the report below).
  - [x] Step 5, on the WT32 (2026-10-09, over the API with a script; the Backup card tried and
        confirmed by you 2026-10-10): export 15 KB (your
        dashboard, 2 networks, display, vibration, hass URL), its own export checks clean,
        mistakes are refused with their path (policy, short password, brightness 0, rotation 45,
        ftp URL, a newer version, tabs, a token without URL), a settings import with one network
        added without password: saved as "password needed" (screen and console), the connection
        stayed, after a restart it connects to the next network; a hand-added `password:` clears
        the flag; the full export imports again (dashboard revision follows). The JC3248W535 and
        the P4 build (not flashed). Still open: the sound and alarm sections on the P4 and
        JC3248W535.
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
- [x] **Try the update page in the browser** (both boards): System part of the built-in page,
      firmware by file, then by URL. On the WT32 it goes through the recovery app too.
      Confirmed by you 2026-10-10: WT32 by URL (download from the PC, written, started; the
      recovery screen shows the progress in percent) and by file; the .bin update by file works
      on all boards. URL on the other boards not tried separately: same code path as the WT32.
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
- [x] Try the linear automatic brightness on the WT32 (done, user, 2026-10-10).
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
- [x] The setup in the captive portal after a factory reset (user, 2026-10-10): the portal opens
      by itself on an iPhone. The rest of the web UI is still yours to try.
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
- [x] The WT32 I2C failures were first put down to the wiring while the modules were moved (your finding, later corrected: task priority):
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
- [x] I2C unplug tests: dropped (2026-10-10). The failed reads were not the wiring but the starved
      orientation task (see "MPU6050 I2C timeouts" below); the timeouts, the 2 s retry and the failure
      count stay and passed your cable test on the WT32.
- [x] Check the direction of "Rotate": it should turn the picture clockwise (confirmed by you
      2026-10-10).
- [x] Fades by eye (3 s down, 0.5 s up): look good (your report, 2026-10-07).
- [x] **Board check of update images** (2026-10-07): every image carries its board name in
      ESP-IDF's custom app description (`hc::board::ImageTag`, right after `esp_app_desc_t`),
      checked with the first bytes before anything is written. Tested on the T4 with a WT32
      image: "the firmware is for the WT32-SC01, this is the TTGO T4", nothing erased.
- [x] **Board variants** (2026-10-15): one unit's add-ons (LDR, motor, relays, MPU6050 calibration,
      a config option) as a private `boards/<board>/variants/<name>/` the repository ignores, its
      `hc_board_variant.hpp` changing the board's `Info` at compile time (`variant::apply`), built as
      `<board>.<name>` (architecture.md section 8, adding-a-board.md "Variants"). The board files
      keep the developer's units as the example. The image tag stays the board type. Console
      `orient capture` prints the calibration in both forms.
- [x] **Recovery app screen** (built and tested on the WT32 by the user, 2026-10-10,
      `recovery/recovery.cpp`, architecture.md section 9.5): board, firmware in flash, Wi-Fi with
      its IP, the hotspot (always on in the recovery app, `network::force_hotspot`) and a QR code
      that joins it, drawn without LVGL. An update shows as "Update downloading: 42 %" (receiving
      for an upload), green when done, red with the reason when it failed; download with
      percent confirmed by the user on the WT32.
- [x] Recovery app screen on the JC3248W535 and P4 (confirmed by you 2026-10-10). The app no
      longer fit the 1 MB factory partition of the 16 MB layout (boot loop, black screen): now
      1.25 MB, ota_0 at 0x150000 and 5.75 MB. The P4 flickered cyan during an upload: fixed with
      `CONFIG_LCD_DSI_ISR_CACHE_SAFE` in `sdkconfig.recovery.esp32p4`.
- [x] Web UI in the recovery app (2026-10-12): the System page showed the Backup card, whose
      export and import answer 404 there. The card is left out when `/api/update` reports
      `app: recovery`.
- [x] P4 main firmware: no flicker during an update from the main firmware (tested by you
      2026-10-10), so it keeps `CONFIG_LCD_DSI_ISR_CACHE_SAFE` off (it would need esp_lvgl_port's
      DSI callback in IRAM).
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
- **Logs page** (2026-10-10, [ui/web/web-app.md](ui/web/web-app.md) section 4): new component `logbuf`, a 16 KB
  ring buffer in PSRAM fed by an `esp_log` output hook, started first in `app_main` of both
  apps (the recovery app has it as well, 16 KB of its partition left). Errors and warnings are
  always kept; Debug (stored, `logbuf/debug`) keeps info messages too. The serial output has
  its own level (`log off` keeps the console quiet, the log still fills): esp_log runs at the
  more detailed of the two, the hook prints only what the console asked for. Messages the Wi-Fi
  driver writes in pieces are put together. APIs `GET|POST /api/log` (debug), `GET /api/log.txt`
  (login required). The page: the log as of loading, filter (errors / warnings and errors /
  everything), Refresh, Download (a .txt with clock times), the Debug switch; the device's clock
  time once it is set, else the time since the start.
  - [x] WT32: built (main and recovery), flashed with the web image; 401 without login, warnings
        kept with the clock time, Debug adds the info messages (Wi-Fi lines whole), `debug=x`
        refused, `log off` on the console stays quiet while the log fills.
  - [x] Your review (2026-10-10): pages 800 px wide instead of 480 so the log lines are
        readable (login page checked at 1280 px); a 404 is logged with its path
        (`web: 404 GET /favicon.ico`, checked on the WT32).
  - [x] Try the Logs page in the browser (works, you, 2026-10-10).
  - [x] JC3248W535 and JC4880P443C: recovery app, main firmware and web image flashed, `/api/log`
        answers on both (2026-10-10).
  - [x] Your request (2026-10-14): a bar right of the message box drags the page's width (wider
        than 800 px, double-click for the default, kept in the browser). WT32 web image flashed;
        your try open.
  - [x] P4: every boot logged `E lcd_panel: esp_lcd_panel_swap_xy(60): swap_xy is not supported by
        this panel`. Caller: esp_lvgl_port (LVGL 8) sets the orientation once in `lvgl_port_add_disp_dsi`
        even with `sw_rotate`. The ST7701 driver has no swap_xy, so the board file gives the panel a
        swap_xy that does nothing (the panel is never swapped). Flashed on the P4 2026-10-10, 20 s
        boot log without the error.
  - [x] With `log off` and Debug on, two empty lines still reach the serial port during a Wi-Fi
        reconnect (cosmetic; probably Wi-Fi driver pieces from a second task). Closed by you 2026-10-10,
        not fixed.

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
- [x] **Internal RAM on the WT32**: 25 KB left with Home Assistant connected, before the
      dashboard. `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` keeps every allocation under 16 KB
      in internal RAM (LVGL objects too; 512 on the ESP32 since 2026-10-10, see Next steps). Decided 2026-10-07: the dashboard lives in PSRAM (as
      in the old firmware), small hot values in a compact array in internal RAM
      (architecture.md section 11). Measure before and after step 6.
- [x] A Home Assistant with a self-signed certificate (https without a public CA) is refused
      ("certificate is not trusted"). Done 2026-10-10: trusted on first use (architecture.md
      section 6, web-app.md section 4). Tested on the WT32 over the API:
      - `https://hassctrltestdomain.com`: "the host name cannot be resolved";
      - `https://google.com`: certificate accepted by the bundle, then "no Home Assistant at this
        address (HTTP 404)". It needed cross-signed verification in the bundle (GTS Root R1 comes
        cross-signed by GlobalSign Root CA, which the common bundle lacks): refused before;
      - `https://self-signed.badssl.com`: refused, the offered SHA-256 equals openssl's; trusted
        (pasted with colons), then HTTP 404 as for google; kept over a restart, no bundle error
        in the log anymore; a wrong stored fingerprint gives "the server's certificate changed";
        not hex: 400; a new address forgets it.
- [x] Try the "Certificate not trusted" card of the web UI (Home Assistant page) with
      `https://self-signed.badssl.com`: fingerprint, **Trust this certificate**, **Forget**.
      Confirmed working by the user (2026-10-10).
- [ ] A new address while a connection attempt hangs (up to 10 s, the network timeout) takes
      effect when that attempt ends; the page shows "connecting" meanwhile.
- [x] Host tests for `hass/src/url.cpp` (2026-10-07): `python tools/host_test.py`, 8 tests.
      `strncasecmp` (POSIX, missing in MSVC) replaced by a small helper in `url.cpp`.
- [x] **Windows version of the UI** for the design review (decided 2026-10-07; the final version
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
  backup file with the settings sections, and `hass:` of an import is only reported as ignored
  (2026-10-12: the settings sections of an import are applied, see "Settings sections in a
  dashboard import").
- [ ] **Dashboard editing** (planned 2026-10-08, [ui/web/web-dashboard-editor.md](ui/web/web-dashboard-editor.md), demo
      `docs/ui/web/web-dashboard-editor-demo.html` reviewed by you): tile positions per orientation in the
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
  - [x] Entity picker (2026-10-09): `GET /api/hass/entities?domain=&q=` and
        `GET /api/hass/entity?id=` (one-shot `render_template` over the hass connection, variables
        instead of escaping, host tests); the Add dialog searches Home Assistant (domain filter,
        every word in id or name, at most 100 rows, a typed id still works, the dashboard's
        entities without a connection), the inspector has Pick. Tried on the WT32 against your
        Home Assistant (read only, 200 to 370 ms an answer) and headless in Edge. Tested by
        you (2026-10-09). With it: "+ Area" opens the new area's page, the screen stays on its
        page after a save from the web UI (by the area tile's id), the editor no longer names
        the screen, the Import & export tab counts pages and tiles of every level.
  - [x] `web/icons.svg`: MDI paths from `tools/build_icons.py`, flashed on the WT32; icons confirmed by you (2026-10-10).
  - [x] **Bug (your report): a size the portrait preview has room for is refused** (2026-10-13):
        on the Cameras page (5x4, arranged in landscape with the camera tiles in rows 1 and 2) a
        new 1x2 picture tile placed in portrait got "no free 1x2 block left in the 5x4 grid of its
        page" without a word that landscape was full. Now the error names the orientation and
        the grid, a click on the problem switches the preview to that orientation, "no place"
        in the Position line is a link there, the size hint in portrait says what the size turns
        into. The rule stays (lvgl-dashboard.md section 6, see Open decisions). Host tests 96.
  - [x] **Bug (your report): a size change on an arranged page "bugs out"** (2026-10-13): the
        weather tile at landscape 1,2 of the 5x3 main page set to 1x2 kept its cell and ran past
        the grid. `refit()` in dashboard.js: keep the cell when the new block is free, else a drop
        onto it with push aside (shifted into the grid at the edge), else the cell is dropped.
        Checked under node with your main page (the tile grows upward, the tile above moves to
        the free cell). Web image flashed on the P4 and the S3 (the WT32 was busy, your word).
  - [x] **Format version 2** (2026-10-14, your request "the dashboard needs to be more
        flexible", plan decided with you): size, cell and hidden per orientation, 1x1 default,
        any size for any kind, overflow instead of placement errors, `defaults.navigation`;
        `core`, `config`, the API's cells, the subscription per orientation, the web editor
        (tray, size per orientation, hidden, the swipe checkbox of an area; `refit()` gone),
        `tools/convert_dashboard.py`, the sample; 98 host tests. Your dashboard needs the
        conversion once (`docs/.retracted/dashboard` too, the host test of it fails until then).
        The tile looks per shape (step 3) came the same day; the swipe on the screen (step 4)
        is below under Open decisions.
  - [x] **Adding tiles on the screen** (2026-10-16, your request: a search field to find an
        entity; ui/lvgl/lvgl-dashboard.md section 7.2): a tap on an empty cell in edit mode opens
        the search (keyboard, Home Assistant filters, 30 rows), a row adds a 1x1 tile there.
        Host tests 112. Tried on the WT32 against your Home Assistant (search, pick, live value of
        the new tile; then Cancel, your dashboard is unchanged) and with Done in the Windows
        version. Found on the way: the first tap after the long press was swallowed (LVGL sent
        the edit layer nothing of the holding finger); fixed.
  - [x] Free cells show a dashed "+", a keyboard button hides the keyboard, and adding goes in
        steps (your requests, 2026-10-16): title and type (tile, area, clock), the entity, the
        options; an area comes with its first tile. 113 host tests; all three kinds tried in the
        Windows version (Done included), a clock on the WT32 (then Cancel).
  - [x] Changing a tile (your request, 2026-10-16): a tap on a tile in edit mode opens its
        title, then its options with its entity (`core::change_tile`, host tests 114). Tried in
        the Windows version (title, another entity of another domain, 2x1, Save) and on the WT32
        (options of your power sensor, then Back and Cancel).
  - [x] Adding a tile, an area and a clock, and changing one, Done included: confirmed by you in
        landscape and portrait on every board (2026-10-16); the lock tile's double tap too.
  - [x] Hold and drag in one move removed (2026-10-16, your decision): the finger that starts
        edit mode is ignored until it is lifted (`lv_indev_wait_release`), so it neither moves the
        tile nor asks to remove it (that dialog under the hold was the bug). The carried-finger
        flag, its timer and `compat::pressed` are gone. Confirmed by you.
- [x] Approved by you (2026-10-09): the climate panel opening on the modes while off, the camera
      spinner's track at 50 %. The P4's tile gap is 7 px (was 8, your request 2026-10-14).
- [x] Dimmer and climate panels on the P4 (2026-10-09, your drafts): larger header buttons,
      steppers, options row, list items and sliders (docs/ui/lvgl/lvgl-panels.md), the color
      temperature marker a rounded bar. Fix: the climate `+`/`-` in portrait did nothing (the arc
      took their taps). Approved by you, tested on a 3.5" board and the P4.

Home Assistant values (2026-10-07, running on the T4 against your Home Assistant):

- `core/entity.hpp`: `EntityState` (one per entity, large heap), `EntityStore::apply()` (says
  whether a value changed), `Intent`, and the display texts (`decimals`, the rounded state of HA,
  the unit, the name).
- `hass_protocol` (new, pure, host tested): one field table (code, attribute, kind) drives the
  template and the decoder. The fields come from the tiles of the page: a tile with its own name
  does not fetch the `friendly_name`, a light without `dimmer` only its state, brightness and color modes, a sensor with
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
  bars, area, back tile, picture placeholder), `icons.cpp`. Described in `docs/ui/lvgl/lvgl-dashboard.md`.
- `core/pending.hpp`: `PendingTracker` (expected state, level within 3, setpoint within 0.05,
  5 s timeout), host tested. `PageValues`/`ServiceFailed`/`HassEvent` moved to `core`, so `ui`
  does not depend on `hass`. The `Backend` has `dashboard`, `subscribe_page`, `call_service`,
  `take_event`.
- Icons: `tools/build_icons.py` (MDI 7.4.47 downloaded into `build/mdi`, lv_font_conv 1.5.3 by
  npx) writes `components/ui/src/fonts/hc_mdi_24.c` / `hc_mdi_48.c`, `icon_glyphs.inc` and `web/icons.svg`. The
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

- [x] **Camera page on the JC4880P443C** (2026-10-10): the tiles stayed empty. Three causes, fixed:
      the PPA's blocking scale did not come back now and then (right after a JPEG of the hardware
      decoder) and the only media task waited for good; the requests queued behind it and the
      fifth fell out of the queue of 4 silently; a tile threw away an image that came after its
      next request (15 s refresh, printercam takes 3-9 s). Now: up to 3 media tasks fetch at the
      same time on the P4 (1 on the ESP32 and the S3: three left the S3 without internal RAM for a
      socket), one decode at a time; a tile asks again only once its image
      came (or after 60 s); a request that falls out of the queue is answered with an error; the
      PPA has 500 ms, then decoder and PPA stay off until the restart (software decoding, 25-30 ms
      a tile). Checked by me on the P4 over the serial console: 4 cameras every 15 s for 100 s,
      one PPA timeout caught; JC3248W535 and WT32 flashed and checked the same way. Open points
      below.
  - [x] Confirmed by you on the P4 (2026-10-10).
  - [x] Why the PPA hangs: silicon bug DIG-734 (2026-10-13, below).
- [ ] Review the dashboard in the Windows version (`python tools/sim.py`, your dashboard with
      `--dashboard docs/.retracted/dashboard/hass.yaml`); the open points are in
      `docs/ui/lvgl/lvgl-dashboard.md` section 8.
- [x] Camera full screen zoom and move (2026-10-11): "+", "-", "X", swiping, kept zoom and
      position, the preview in the stream's place and the 30 s wait for a stream; confirmed by you.
      Flashed on all three boards.
- [x] Camera full screen pinch to zoom (2026-10-11): two fingers measured on all three boards
      first (GT911 and AXS15231B fine, FT6336U merges them below about 3 cm), then built; the
      JC3248W535 reads two fingers itself and moved from esp_lvgl_port's reader to `touch_input`
      in the LVGL task. Confirmed by you on all three boards. Then (your wish): the pinch zooms
      freely, only the buttons have steps, the smallest zoom shows the whole picture; confirmed
      by you on the P4.
- [x] **P4: the PPA hangs on the first camera image after most boots** (2026-10-11, also without
      the zoom change: 4 of 4 boots of the last commit, 5 of 6 with it). Always the cameras page's
      first scale (balconycam, 264x216 from a 272x224 decoder frame, block 258x169 at 3,23 to
      226x148, scale 14/16); no answer even after 3.5 s. A warm-up scale of 64x64 before it works
      and changes nothing; even offsets and sizes change nothing; the DIG-734 workaround of the
      driver does not apply to it by its formula. Then decoder and PPA stay off until the restart
      and every image is decoded in software (doorspy 1280x960: 315 ms a frame instead of 55 + 21).
  - [x] Fixed (2026-10-13, your link to esp-idf issue 19023): silicon bug DIG-734, the 2D-DMA
        miscounts a last macro block smaller than its FIFO and never raises done. The driver's
        workaround sets the SRM engine's "bypass macro block order" flag only when its formula
        predicts a leftover below the FIFO (1536 bits); ours computes to 3696, the flag stayed
        clear, and the issue's fix (469aa16, 90° and 270° rotation only, backports promised)
        does not reach rotation 0. Measured over the serial console (restart, cameras page): the
        flag forced on, 8 of 8 boots ran (the scale in 2 to 8 ms); the driver as shipped, 3 of 3
        hung. `components/esp_driver_ppa` overrides the IDF's component: its own sources, and
        `ppa_srm.c` copied from v6.1.0 with the flag always set (README: the numbers, the diff to
        check after an IDF update, when to drop it). The 500 ms timeout and the software fallback
        stay. Built for all three boards, flashed on the P4.
- [x] **Bug (your report): MJPEG streams fall behind over time** (2026-10-11): Doorspy (go2rtc,
      460x600, `-q:v 5 -r 10`, 37 KB frames, 370 KB/s) was 10 s behind on the P4 after a few
      minutes and 30 s on the WT32. go2rtc queues up to 64 frames per client that reads slower
      than the source sends (its API showed 2.4 MB queued and drops for all three boards); the
      boards read 7.5 (P4), 6.8 (S3) and 1.5 fps (WT32). Now a stream whose next frame is already
      waiting in the socket after every frame for 10 s is opened again (the new connection before
      the old one closes, so go2rtc keeps its source running); a frame that cannot be decoded is
      skipped (3 in a row end the stream, one came in a minute on the WT32). Checked by me with
      2 s over the serial console, Doorspy full screen: S3 5.2-6.1 fps shown, WT32 1.2-1.5, P4 4.9
      (software decoding after the PPA hang above); go2rtc keeps one client per board with 3 to 7
      frames queued, the same ffmpeg. Skipping frames to catch up was tried and dropped: the
      boards read this stream slower than it comes even without decoding (the S3 9.5 of 10 fps).
      The stream carries no timestamp (part headers and JPEG markers checked) to measure the delay
      exactly. 10 s tested and confirmed by you on the P4 (a single stream, it goes on after a
      reconnect). The real fix for a stream too large for the Wi-Fi is a lower bit rate at the
      source; the reconnect is the fallback for streams without that option.
- [ ] Panels: light, climate, sensor history (statistics), camera image. Your first test of the
      Windows version (2026-10-07): tapping a sensor, a dimmer and a climate tile does nothing,
      because these panels are not built yet (the taps are reserved for them). Order (your
      decision): light, climate, graph.
  - [x] **Light panel** built (2026-10-07, `panel.cpp`, `light_panel.cpp`, docs/ui/lvgl/lvgl-panels.md
        section 2): power, level slider with steppers, color wheel, color temperature, effects.
        Checked by me in the Windows version at 480x320, 320x240 and 320x480; firmware builds for
        the WT32 (1.78 MB). Approved by you in the Windows version (2026-10-07); the gap between
        the back tile and the name widened from 4 to 12 px after your review.
  - [x] **Climate panel** built (2026-10-07, `climate_panel.cpp`, docs/ui/lvgl/lvgl-panels.md):
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
  - [x] **Sensor history panel** built (2026-10-07, `sensor_panel.cpp`, docs/ui/lvgl/lvgl-panels.md
        section 4): live value, 48/24/12 h, the statistics curve with round value labels and
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
  - [x] **Camera image** built (2026-10-07, docs/ui/lvgl/lvgl-camera.md): the picture tile
        shows the image with its name on a band and refreshes it (`refresh`), a tap shows it full
        screen. New: `core::Image` with the cover scaling (host tested, 54 tests), the media task
        of `hass` (`media.cpp`: camera proxy over `esp_http_client`, `esp_jpeg` 1.3.1 with the ROM
        decoder), `Backend::fetch_image`; the Windows version draws a landscape. Checked by me in
        the Windows version (your Cameras page, full screen and back); firmware builds for the
        WT32 (1.83 MB). **Not tested against your cameras** (the device was not flashed): stack
        use, decode time and progressive JPEGs still to see. Approved by you in the Windows version
        (2026-10-07), the name on the band centered after your review.
  - [x] **MJPEG stream test** (2026-10-08, your request; docs/ui/lvgl/lvgl-camera.md "MJPEG stream"):
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
        test aids (the fps label is gone since 2026-10-12, the media log has the rate).
  - [x] **Stream reconnect** (2026-10-12, your request; confirmed working on the P4 by you): a
        full screen stream that stopped (ended, broke off, 30 s without data) is tried again after
        2 s. "Stream stopped, retrying" while a try connects, "Stream loading ..." from the TCP
        connection on (go2rtc answers only with the first picture) until the first picture; when
        a try fails "⟳ Stream failed: <reason>" until the next try. A try starts every 10 s, the
        connection gives up after 5 s (a restarting server answered nothing for 18 s), so the
        stream comes back by itself after a server restart; a tap tries at once. The media task
        reports the connection (`Image::opened`) and the cause (`StreamProblem`). Your design
        after confusing first versions (the reason in a centered popup that did not fit on the
        P4, endless retries, "Loading" that did not mean connected, 27 s to come back). Checked in
        the Windows version (new steps `stream down|up`, a server that times out and starts slow)
        at 480x800, 800x480 and 320x480 before flashing. The fps label is gone.
  - [x] **Alert strip slides, event messages** (2026-10-12, your request: alerts slide down from
        the top, errors never in a popup; confirmed working on the P4 by you): the strip slides
        down in 250 ms and back up; `overlay::show()` puts an event message into it (5 s, or
        until its code clears it; with a tap action for "Stream failed"). Used by the stream and
        the edit mode ("Changes not saved", "Tile not removed", "Order not reset", "Edit
        dropped"), which used the popup too. One short line each (your review: no 160 character
        errors on the screen); the reasons go to the log. The camera view's buttons move down
        with the strip (two lines in portrait covered them by 10 px, seen in the Windows version
        before flashing). Rules in lvgl-design.md section 8. Only the P4 flashed (your
        instruction); the WT32 and the JC3248W535 get it with their next update.
  - [x] **S3 streams** (2026-10-08, your request, JC3248W535 on COM10): `esp_new_jpeg` for the
        S3, media task on the second core, TCP window 23 KB. 640x320 at 10 fps shows all frames
        (was 4.6), `_xl` 800x480 6.6 fps (scaling in the decoder). The pinning tested on the P4
        (unchanged) and the WT32 (640x320 2.3-2.6 fps, 800x480 1.8 fps). Optional: a stream in
        the S3's screen size (480x320) needs no scaling.
- [x] **Bug (your report): the P4 showed "--" on every tile after a start** until a panel was
      opened and closed. The UI subscribed its page 80 ms before `hass` had created its queue,
      and the subscription was dropped (`event 7 dropped`); the queue is created with its first
      use now. Checked on the P4 after a restart (2026-10-08).
  - [x] Test the panels on the WT32 against your Home Assistant (history requests, the
        climate panel changes the real thermostat). Done by you (2026-10-14).
- [x] **Bug (your report): switchtest1 on -> dimmertest1 showed 100 % before the real 33 %.** The
      pending "on" of a light whose level HA does not report while off was drawn as "a light
      without a level" (100 %). Now the store keeps the last level (`last_brightness`) and a light
      that is switched on shows it; one never seen on shows `On`. Checked in the Windows version,
      confirmed by you together with the light panel (2026-10-07). The cyan border you saw is the pending
      look (docs/ui/lvgl/lvgl-dashboard.md section 4): the action is on its way until HA reports it.

- [x] **Lock tile and panel** (2026-10-11, your request: `lock.front_door` showed as a sensor
      without a graph). `lock.*` is its own kind; the tile opens the lock panel (never unlocks
      by a tap): HA's two-place slider (drag or tap the other half), the open button with a
      second tap (only with supported_features OPEN), and the timeline behind the second option
      button. The sensor panel's graph is shared (`history_view`). Checked in the Windows version
      (unlock with locking/unlocking, the timeline with a jammed stretch, the armed open button,
      landscape, portrait, the P4 dialog). Flashed on all three boards.
  - [x] Tested by you on the WT32: open, lock and the timeline (2026-10-11).
  - [x] Your colors (2026-10-11): unlocked green, locked orange, jammed red in the tile, the
        panel and the timeline; `Jammed` in the legend only while the window has a jam. Checked
        on the WT32 over the serial console (flashed there only), confirmed by you.

## Your reports of 2026-10-07 (afternoon)

- [x] Bug: a network that failed with "network security not supported" was tried again and
      again (the first of the order). Now any failed attempt, not only a wrong password, moves on
      to the next saved network in range; when every saved network in range has failed, a new
      round starts in the configured order. A connection that was up and broke is tried first
      again. (`network.cpp`, `on_disconnected` / `on_scan_done`.) Not reproduced on purpose:
      tested only that both boards still connect.
- [x] Bug: the time zone was never taken from Home Assistant (it was not built yet). Now `hass`
      sends `get_config` after every authentication and hands `time_zone` ("America/Vancouver")
      to `platform::set_time_zone()`, which turns it into a POSIX rule (table of 598 zones,
      `tools/build_timezones.py` from the IANA tzdata on PyPI; was posix_tz_db, stuck at 2025b) and stores the name in the settings
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
      measure the render times, or use fewer draw buffer lines. To decide. Lowered to 512 on
      2026-10-10, see Next steps.

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

## Audio (2026-10-09)

Speaker on the JC3248W535 (NS4168 on BCLK 42 / WS 2 / DOUT 41), docs/architecture.md section 9.6.
The WT32-SC01 and the T4 build without audio (no amplifier; checked: no audio objects).

Your report of the first version (2026-10-09):

- [x] Sound is clear, no pop when the channel starts or stops.
- [x] Switches were hard to toggle, the row behind them took the taps: a row with a switch and no
      action now flips the switch with a tap anywhere on it; a row without an action takes no
      touches (they scroll the page). Checked in the sim (tapping the row labels flips both
      switches of the Sound page).
- [x] The master "Sound" switch overrode the touch sound: removed. The volume (0 = silent) is the
      only common setting, on quick settings and on the Sound page; the touch sounds have a switch
      each under "Touch sounds" ("Click on taps", "Tick on sliders"); the alarm ignores the volume.
- [x] Volume slider on quick settings when audio is built in.
- [x] Slider ticks: one per 1/25 of a slider's range (14 px on the light panel's bar), at most one
      per 25 ms, so the rate follows the speed (sim: 9/s slow, 33/s fast).
- [x] MP3: esp_audio_codec (Espressif-only license, prebuilt). Played on the board from the
      console: `sound play /fs/sounds/alarms/Helium.mp3` and `sound alarm <file>`. The ESP32-P4
      has no hardware audio decoder (only JPEG and H.264); the library has optimized P4 builds.
- [x] Alarm in Settings with time, days, snooze, sound; full volume, cannot be muted by the
      volume; six AOSP alarm sounds (Apache 2.0) with a preview on selection.

Checks for you (the second version is on the board):

- [x] COM10 dropped out during a flash: you were restarting the board then; flashed afterwards.
Your report of the second version (2026-10-09):

- [x] Alarm with snooze works (`sound ring`, you snoozed it); the sounds are not too loud.
- [x] Switch rows: fixed.
- [x] Slider ticks ("drag noise") are good.
- [x] CPU load is low while an MP3 plays; scrolling is as smooth as without.
- [x] A slider at 100 % (or 0 %) was hard to drag. First fix (touch area 16 px past the slider's
      ends) did not help on the P4 (your report). Reproduced there with the console's virtual
      pointer: a drag starting 14 px above or below the knob did nothing. LVGL looks for the
      slider only inside its parent, and the parent (the track) was 22 px tall. The track is now
      the touch area, 16 px above and below the slider (the card's gaps are smaller by as much, so
      it looks the same) and takes no touches itself. On the P4 every start point now drags:
      the knob's center and outer edge, and 14 to 20 px above or below it. Flashed on both boards.
- [x] Better, but still hard on the P4 (your report: its screen is denser, 42 px is about 5 mm):
      the whole slider card is now the touch area (the track lets the slider's touch area overflow,
      the card's edges cut it off). Sim: drags starting on the card's title move the slider, the
      row below still opens its page.
- [x] Check on both boards: drag a slider from anywhere on its card, at 100 % and 0 %. Easy to
      reach now (you, 2026-10-14).
- [x] Audio on the P4 did not work (your report): two causes.
      1. The driver's stop (`es8311_suspend`) writes 0 to the DAC volume and its start leaves it
         there: only the first sound after a restart played. The volume is set on every start now.
      2. Loud sounds restarted the board ("power on" reset, seen with the console's `sound tone`
         at 70 %; 50 % played): the amplifier pulled the supply down. Also with the hub at 10 W
         and the DAC at 0 dB it reset at 70 %, so it is the board's side (regulator or cable). The
         DAC is at -12 dB now (`kDacDb` in es8311.cpp); the driver's hidden +3.6 dB (5 V
         amplifier vs. 3.3 V DAC) is gone. Checked: a 500 Hz tone at 100 %, all six alarm MP3s and
         the beeps at full volume, no restart.
- [x] Quick taps were lost, and the click got louder when tapping fast (your report): the touch
      was read by LVGL every 30 ms and not while it rendered, so a quick tap during a long render
      never arrived. Now `touch_input` samples every 10 ms in a task of its own and queues every
      change. The click's start was swallowed by the amplifier waking up when the channel had
      stopped: 20 ms of silence come first now. The click sound no longer checks the finger's
      distance except on sliders (the sparse samples during a render made taps look like drags).
- [x] With that the JC3248W535's touch went mad (touch and panel are one chip, AXS15231B). Reading
      only between frames (a lock with frame_display) did not help: you reported it still broken,
      and a 3-minute trace (`touch log`) had about 16 000 good reads and not one point (whether you were tapping then is not known). The
      JC3248W535 is back on esp_lvgl_port's reader in the LVGL task, as it was when it worked
      (`Info::touch_in_panel`); the lock is removed again. The P4 and the WT32 keep the new reader.
- [ ] Open: why the AXS15231B's touch does not work from a task of its own. Ideas: it may need its
      reads paced (30 ms worked, 10 ms not), or the reads collide with the panel's QSPI traffic in
      a way a lock around whole frames does not cover. Until then quick taps during a long render
      can still be lost on the JC3248W535. New since 2026-10-11: the controller's answers include
      shifted records and garbage after a lift (docs/devices/JC3248W535), which Espressif's driver
      took for touches; the board's own read drops them now. Worth trying the task again
      (`touch_input::Config::in_lvgl_task` false).
- [x] The JC3248W535's touch works again, all clicks play (your report).
- [x] A click about a second after the last sound (800 to 1200 ms) was quieter: the channel
      stopped 1 s after a sound and the NS4168 was caught going to sleep or waking. The channel
      now runs all the time on both boards (`Info::audio_always_on`).
- [x] The P4 still had clicks of different loudness (your report). Cause in the audio task: after
      a pause all DMA buffers are back with the I2S driver, and the first one it hands out is the
      one being sent at that moment (i2s_common.c, "queue almost full"); the click written there
      lost the part the DMA had already passed, depending on the moment of the tap. Now the first
      sound after a pause gets a buffer of silence first (it lands in the buffer being sent), every
      sound is padded to whole buffers, and the wake-up silence is whole buffers (+8 ms on the
      first sound after a pause). Flashed on both boards.
- [x] The JC3248W535 restarted at random ("power on" resets): gone with a new USB cable (you
      replaced it; 3 minutes without a restart after that).
- [x] P4 flashed with the fix after you put in a new cable; clicks, a tone and an MP3 at 97 %
      volume play without a restart.
- [x] Every click as loud as the others, at any pace, on both boards: confirmed by you
      (2026-10-09).
- [x] P4 louder: the DAC at 0 dB (`kDacDb` in es8311.cpp, was -12 dB), kept by you
      (2026-10-14). Tested on a USB hub with 2.4 A: volume 50, 70 and 100 %, a 500 Hz tone (1.5 s
      and 5 s), 200 Hz, 15 s of the alarm MP3, no restart. On 2026-10-09 the same level restarted
      the board at 70 % (worn cable then). No separate 5 V supply needed.
- [x] MP3 memory (your question): the decoder's state, the read buffer and the PCM frame are in
      PSRAM. Its code and tables are in flash (S3: 40 KB code, 13 KB tables; 1.1 KB of hand-written
      assembly in IRAM, by Espressif, for speed), 12 bytes in internal RAM. The 5 KB of internal
      RAM a playing file still took on the S3 were stdio's FILE buffer and its cache: the file is
      read with open/read now, internal RAM stays at 39 KB while it plays.
- [x] Sounds from HTTP (Home Assistant TTS), what plays them (2026-10-14, your choice: an event
      of Home Assistant): the event `hassctrl_say` with a `message` and a tts `engine`, or the
      `url` of an MP3, `device` picks the device (architecture.md section 9.6, the Home Assistant
      side in docs/dashboard-sample/hass.yaml). Downloaded into PSRAM by a task of its own, then
      played as an announcement (touch sounds wait, an alarm comes first). Console
      `sound say <tts entity> <text>`, `sound url <url>`. 99 host tests. On the JC3248W535 over
      the console: speech of tts.google_en_com 19 to 29 KB in 0.5 to 0.9 s, HTTP 404 and HA's
      500 for an unknown engine are logged, an 8.7 MB MP3 is refused (1 MB), redirects to https
      work; internal RAM after a sound as before (32 KB free).
- [x] Heard on the JC4880P443C (you, 2026-10-14): the event from Home Assistant's Developer tools
      ("Hello") and `sound say` on the console. The JC3248W535 has no speaker connected now, it
      was tested over the console only. P4 at 100 %: no restart.
- [x] Priority on the P4 (you, 2026-10-14): taps do not click while it speaks, the alarm takes
      over.
- [ ] Check for you on the P4: an event with another `device` name stays silent.
- [x] The event's `options` go to the engine (2026-10-14, your request after asking for the
      speed): Google Translate has `tld` (the accent) only, no speed. `tld` works (you,
      2026-10-14).
- [ ] Open: a WAV from an engine that cannot give MP3 is refused (HA converts with
      `preferred_format`, so it should not come); a `media_player` entity (DLNA renderer) only
      if `tts.speak` or Music Assistant are wanted.

## Vibration (2026-10-09)

Component `vibration` (docs/architecture.md section 9.7): a motor on the P4's GPIO 28, a pulse
with every tap that clicks, a pattern while the alarm rings, the "Vibrate" switch on Settings,
Sound. Built for the JC3248W535 (without it) and the JC4880P443C; the Sound page checked in the
sim.

- [x] First test by you: the motor turned on with the first tap and stayed on until a restart.
      Cause: the off-timer asked `esp_timer_is_active()`, which is true inside the timer's own
      callback, so it always thought a newer pulse had taken over. Now a deadline decides. Flashed
      on the P4; the pin read back with the new `vibrate status`: on during a 300 ms pulse, off
      after it; the alarm pattern 500 ms on, 500 ms off.
- [x] Click length: shorter than 70 ms does not move this motor (you); now 75 ms, a constant of
      the board (`Info::vibration_click_ms`), measured by the developer (adding-a-board.md step 10).
- [ ] New, seen 2026-10-14: 2 of 15 hard resets with esptool panicked during ESP-IDF's start, before any code
      of ours (`Store access fault` in a ROM memset, `esp_startup_start_app` ->
      `xTaskCreatePinnedToCore` -> `prvInitialiseNewTask`, filling the first task's stack with 0xa5
      at internal RAM 0x4ff4cd40). The board restarted by itself (reset reason "panic") and ran
      normally. Cause not known; it only costs a second boot.

- [x] Working on the device, confirmed by you (2026-10-09).
- [x] Bug (you): with both touch sounds off, letting go of the volume slider still clicked. It
      clicks now only when one of them is on.
- [x] WT32-SC01: motor on IO27 (2026-10-09, flashed), the first board with a motor and no audio
      (main.cpp included the vibration header only with audio). Click 50 ms on both boards (you,
      after `vibrate 50`; the P4 had 75 ms).
- [ ] **LVGL ran out of memory on the WT32** (your crash, 2026-10-09): the task watchdog fired every
      5 s, the LVGL task hung in `LV_ASSERT_MALLOC` of `lv_obj_add_event_cb` (`theme::line_text`,
      `tiles::update`, `build_page` after a page was opened). After the restart 3.9 MB PSRAM and
      30 KB internal were free, so the size of the failed request is the question. Now every
      failed allocation prints its size, caps and both heaps (also with the log off), and a
      failed LVGL assert aborts (panic and restart) instead of halting. To do: catch the line.
- [x] Tune the click (`kClickMs`, 25 ms now) with `vibrate <ms>`: the shortest pulse that is
      felt clearly; and the alarm rhythm (500 / 500 ms). Done by you (2026-10-14).
- [x] Check (done by you, 2026-10-14): taps vibrate, the switch turns it off, `sound ring` vibrates until Stop or Snooze.
- [x] Vibration on touch-down (2026-10-10, you asked; before it came on release): touching a
      button buzzes at once, the click sounds on release, one pulse per touch. Confirmed by you on
      the WT32, which now has a 15 ms click (`Info::vibration_click_ms`); the P4 keeps its own.

## Battery remote (2026-10-11, cancelled)

- [x] ~~Deep sleep on the JC3248W535~~: cancelled by you. The board takes 30 to 40 mA in deep sleep
      (see [devices/JC3248W535](devices/JC3248W535/JC3248W535.md), "Deep sleep"); the code was removed.

## Camera (2026-10-11 test, a feature since 2026-10-12)

Component `camera` (P4 only, `CONFIG_HC_BOARD_HAS_CAMERA`, docs/architecture.md section 9.8):
Settings, Camera with its preview, and `/api/camera.jpg`, `/api/camera.mjpg`. Until 2026-10-12 a
test: the picture over the dashboard and a fixed login guest / password.

- [x] Sensor: an OV02C10 (chip ID 0x5602 at 0x36 on the touch bus). esp_cam_sensor 2.6 has no
      driver for it; the vendor demo's driver (Apache-2.0) is in `components/camera/ov02c10`, with
      `tline_ns` added (esp_video 2.x failed the auto exposure without it). 1 lane, 1288x728 RAW10,
      30 fps through the ISP as RGB565.
- [x] The capture task runs on core 0: the CSI's DMA interrupt shares the DW-GDMA group with the
      display's DPI channel, created on core 0; from core 1 its allocation failed.
- [x] Checked over serial and the network: capture 30.1 fps; picture on the screen (screenshot);
      snapshot 48 KB in 0.2 s; stream about 10.6 fps (500 KB/s over the C6's Wi-Fi); two streams
      at once, a third gets 503; 401 without or with a wrong login; internal RAM (247 KB) and
      PSRAM unchanged after the streams.
- [x] Your first test: the stream in the browser was fine, but the picture on the screen tore and
      the exposure flickered between bright and dark at a threshold.
- [x] Tearing on the screen (not in the MJPEG, so the display): LVGL redrew the full screen
      picture in bands with software rotation, about 50 ms (three refreshes) into the frame
      buffer the panel scans. Now the DPI panel has two frame buffers; while the picture is shown,
      LVGL's flush does nothing and a task scales and rotates each camera frame in one pass into
      the buffer not scanned, which the DPI driver takes at the end of a frame. Measured: 30 fps on
      the screen, a frame drawn in about 33 ms; the dashboard is drawn again after the tap.
- [x] Exposure flicker: the vendor's AE tuning had a frame delay of 1 for exposure and gain
      (OmniVision sensors take 2; the AE read stale frames and overshot) and anti-flicker for 50 Hz,
      which snaps the exposure to 10 ms steps, a factor of 2 at the bottom, wider than the target
      window (62..105). Now delay 2 and 60 Hz (Vancouver). No exposure change in 30 s here.
- [x] Your second test: smooth and without tearing; the 60 Hz flicker of the lights is gone; the
      exposure still jumped between bright and dark once a movement changed the light enough.
- [x] Cause: the vendor's gain table. It took the analog gain register 0x3508 = 1..6 for 1, 2,
      4 ... 32x and stepped the digital gain 1.5..2x in between, so the exposure control believed
      in steps of 3 %; on the sensor 0x3508 is the whole analog gain (1..6x, as the Linux driver
      writes it), so index 31 -> 32 made the picture 52 % brighter (1.98x -> 3x) and two later
      steps made it darker. Now the analog gain alone, 1x to 15.5x in 1/16 (0x3508 whole part,
      0x3509 sixteenths), the digital gain at 1x; each gain change is logged for now.
- [x] The picture stays as it is: the sensor is 1288x728 (not square), cut to the screen (you).
- [x] Your test of the exposure: much smoother now, about what this camera can do (you). The log
      (23:41 to 23:51): the gain ramps in steps of one frame or two (41 -> 211/16 when the view got
      dark, back to 41 in 0.5 s) and then holds; small corrections of a few 1/16, no back and forth.
- [x] The test was successful, the camera stays (you, 2026-10-12): Settings, Camera below Sound;
      no dashboard tile; no picture over the dashboard any more.
- [x] Settings, Camera (docs/ui/lvgl/lvgl-settings.md section 3): Preview; Mains 50 / 60 Hz; Stream on/off,
      Login, Size (1288x728 / 644x364), Frames/s (5 / 10 / 15 / 30), Quality (30 to 95). The stream
      starts off and goes on only with a login (your decision); the login form is the Wi-Fi
      password page's (username, password, eye). The preview: the whole picture (portrait: above
      the controls; landscape: beside them), Auto exposure with Brightness, or Exposure and Gain by
      hand (your decision: gain too), Mirror, Flip.
- [x] Checked over serial and the network: the page and the login form typed with the
      keyboard; 404 while off, 401 without or with a wrong login, 200 with it; half size
      644x364; 5 fps gave 50 frames in 10 s, 30 fps half size 25 over the Wi-Fi; a snapshot that
      starts the capture waits half a second (the first frame was dark and green); the layout in
      the Windows version in landscape.
- [x] Mirror and flip of the vendor driver (bit 1 of 0x3821 / 0x3820) broke the readout. Measured
      with the new console commands `camreg` and `camera`: mirror is bit 3 of 0x3820, flip bit 4;
      both shift the color filter (magenta), so the window starts a pixel further: x 7 -> 8 for
      mirror, y 4 -> 3 for flip (y 5 gave one frame and then none). Written while the sensor
      streams, a mirror or flip stopped the frames for good; now only in standby (a change
      restarts the capture), and a capture without a frame for 1 s starts again. All four states
      stream at 15 fps (3 s, over the Wi-Fi) with the right colors.
- [x] A snapshot after the capture had stopped got the JPEG of minutes before (the cache was
      only checked against the caller's frame); now only the newest frame's counts.
- [x] OK and Enter on the keyboard: Enter did nothing on the forms (LVGL sends its
      LV_EVENT_READY to the text field, OK to the keyboard and the field); the forms listen on
      their fields now (`set_form`), also the Wi-Fi password page.
- [x] Your test on the device: the settings and the preview work (2026-10-12).
- [x] The camera's settings in the backup file (docs/ui/web/web-app.md section 4): a `camera:` section
      on a device with a camera, ignored as "not on this device" elsewhere; the stream's password
      is not written, a `stream_password:` added by hand is used (your decision). Checked: the
      P4's export (only comments mention passwords), a check with a `stream_password:`, an import
      of mirror and 10 fps and back (seen on the device), the P4's file checked on the WT32
      (camera ignored, no problems; its own export has no `camera:`). Not applied: a new
      password (yours stays). The import of a stream password tested by you: it works.
- [x] The log line of each gain change (`ov02c10: set gain`) removed (you).
- [x] The stream on the web UI (your request): a Camera page with Stream on, Size, Frames per
      second, Quality, Username, Password and the addresses; `GET/POST /api/camera` behind the web
      UI login, the password never sent back. The picture's settings stay on the screen (they
      need the preview). Checked: the answer without the password; a change of frame rate and
      quality and back; another username without a password while the stream is on refused
      (nothing changed); a wrong frame rate refused; the P4 reports the camera, the WT32 not (no
      menu entry, `/api/camera` 404).
- [x] Your look at the web UI's Camera page (layout, saving): approved and tested by you (2026-10-14).

## Relays (2026-10-15)

Component `relay` (architecture.md section 9.7a): `input_boolean.<device>_relay_<n>` / `switch.` switches a GPIO.
A permanent feature of the JC4880P443C (relay 1 on GPIO 32), no longer a test.

- [x] Template, decoder (host tests `protocol_relays`), handler in hass, console `relay`; P4 relay 1 on GPIO 32.
      Built, flashed, `relay 1 on|off` toggles the pin.
- [x] Test with Home Assistant (you, 2026-10-15): the pin follows the entity at once. A template over
      `states.<domain>` was delayed up to a second by Home Assistant's rate limit; it names the entities now.
- [x] `Info::relay_pullup` (2026-10-16): open drain with the internal pullup, closed at boot; kitchen variant
      relay 1. Confirmed by you: the opto module switches on the internal pullup.
- [x] Kitchen variant (2026-10-16, your report): 20-30 brownouts in a row, each right after "relay 1 off",
      with the relay wire on GPIO 32 and nothing on its other end; fine after moving the wire to GPIO 28.
      Cause unclear (`relay 1 off` on GPIO 32 without the wire: no reset). Relay 1 on GPIO 28 now, confirmed
      by you; the variant also turns the FPS overlay off.

## Timer tiles (2026-10-16)

Your request: timer support (`timer.bathroom_fan_30min`) with a proper UI; a tap opens the details, a
double tap starts or stops.

- [x] `TileKind::Timer` for `timer.*`, three fields (`duration`, `remaining` as seconds, `finishes_at` as epoch
      seconds: the template renders `as_timestamp(...)`), `IntentKind::SetTimer` (start with an optional
      duration, pause, cancel, finish).
- [x] Tile: the countdown from `finishes_at` by the device's clock, once a second; `Idle`; filled while active;
      the icons of the HA frontend by state. Double tap: cancel while active, start else.
- [x] Panel (`timer_panel.cpp`): countdown, "Ends at HH:MM" / Paused / Idle, duration steppers (a minute
      from two minutes up, else 10 s), Start / Resume / Restart, Pause, Cancel, Finish (those the state
      allows), the timeline (active orange, idle, paused). Checked in the simulator at 480x320, 320x480,
      320x240 and in the P4's dialog; confirmed by you on the WT32-SC01.
- [x] Your report on the WT32 (2026-10-16): the countdown was off by up to half a minute either way (30:30 at
      the start, stuck at 0:00 while HA had 26 s left). Cause: `finishes_at` was a float, which holds epoch
      seconds only to the nearest 128 s; an `int64_t` now (your remark: an integer, not a double). Your report that a
      tap "disabled" the entity came from that build too and is gone with it (2026-10-17). Start with a changed
      duration showed the old time in cyan for a moment: the duration is part of the expectation now. Confirmed
      by you on the WT32-SC01 (2026-10-17).
- [ ] The countdown needs the device clock and Home Assistant's clock to agree (SNTP, within a second);
      `timer.change` (add or take time while it runs) is not offered.
- [ ] The web editor has no timer options (there are none); the preview shows the timer glyph.

## History buttons (2026-10-15)

Your requests: a switch's history on a double tap, a history button in the light and the climate
panel left of the power button.

- [x] Switch and plain light tiles: the toggle waits for the double-tap window (300 ms, as the dimmer's
      tap waits), a second tap opens the sensor panel with the timeline (`panel::has_panel` includes them,
      the compact frame).
- [x] Light panel: `chart-timeline-variant` left of the power button (`panel::left_of_action`), the
      on/off timeline in the place of the control; a mode button leaves it.
- [x] Climate panel: the same button (it takes the power button's place while that is hidden); the graph
      is `HistoryKind::Temperature`: `history_during_period` with the attributes, `current_temperature`
      of every state, one hour per request (the attributes make a state about 300 bytes), each value
      holding until the next change. A climate has no long term statistics.
- [x] Your report: the panel of a double-tapped light had no icon and no state. `panel::update` sent
      every light tile to the light panel's update, which has nothing for a plain light; it now picks
      the panel as `build` does (dimmer or not).
- [x] Your report (2026-10-16): an automation tile switched on a tap, as a switch. 2026-10-17: automations
      and scripts are an `Automation` tile kind: the time of the last run on the tile (`last_triggered`,
      `Disabled`, `Running`), a double tap runs it (`automation.trigger` with `skip_condition`, `script.turn_on`,
      `script.turn_off` while it runs), a tap opens the automation panel (Enabled switch, last run, Run, the
      timeline; lvgl-panels.md section 8). Checked in the simulator at every size, the double taps drive
      `call_service` there. An enabled automation is a plain card, a disabled one has the off glyph and its name
      struck through (your choice). Confirmed working by you on the WT32 (2026-10-17). Open: a script that runs
      long enough to show `Running` and `Stop` on the board.
- [x] Your report (2026-10-16): the double tap on a plain light tile of a dimmable light opened the
      sensor graph. `panel::build` now gives a light tile the light panel when it has `dimmer` or its
      light dims (a color mode other than `onoff`, or a reported level); the page subscribes the color
      modes of every light for that. Confirmed by you.
- [x] Climate graph on the P4 with the Lyric thermostat: 70 points over 24 h, checked over the console.
- [x] Confirmed by you on the P4 (2026-10-15): the timeline of a switch after a double tap, the light
      panel's button, the climate graph (the console's virtual taps are too slow for the 300 ms
      window, also on the dimmer's double tap, so the double tap is a finger test).

## Motion sensor (2026-10-15)

Component `motion` (architecture.md section 9.7b): a PIR on `Info::motion_gpio` sets the text helper
`input_text.<device>_motion` to Detected / Clear (Occupied until your change of 2026-10-15, to match Home
Assistant's motion sensors). The JC4880P443C: GPIO 34, active low; its kitchen variant: a radar on GPIO 49, active high.

- [x] Component (100 ms sampling, two samples), `motion_message` (host test `protocol_motion`), the hass
      command with the resend after `auth_ok`, console `motion`; built and flashed on the P4.
- [x] Test with Home Assistant (confirmed by you, 2026-10-15): `input_text.hassctrl_7e7d_motion` follows
      the sensor; `motion detected` from the console sets it by hand. Without the helper the log
      warns once per connection.
- [x] A tile on the helper showed no history (your report, 2026-10-15): the sensor panel asked for
      statistics, which a text entity has none of, and the timeline knew `on`/`off` only. Now a state
      of two words (`core::on_off_words`) gets the timeline with its own words as the legend.
- [x] Kitchen radar (2026-10-16): GPIO 35 is the P4's boot-mode strap and the first radar died there;
      the replacement is on GPIO 49, active high (3.3 V with motion, 0 V without), confirmed by you.
- [x] Wake on motion (2026-10-16, your request): Settings, Display, "Wake on motion" (on by default);
      motion brightens a dimmed screen, turns it on and keeps it on. Sim checked at 480x800 and 480x320
      (no row without a sensor); confirmed by you on the kitchen P4.
- [x] Motion hold (2026-10-16, your request): the sensor resets after 1-2 s and flickered the helper; now it
      stays Detected until the sensor saw nothing for 1 to 60 min (Settings, Display, "Motion hold", default
      5 min). Sim checked at 480x800 and 480x320; confirmed by you on the kitchen P4.

## Known issues

- [x] **"Header fields are too long" (431)** in your browser on 192.168.0.199 (2026-10-07): the
      web server took 1 KB of request headers; your browser sends more, most likely cookies of a
      device that had this address before. Now 8 KB (`max_req_hdr_len`; the buffer grows only as
      far as a request needs). Tested on both boards with 4.8 KB of cookies, the login cookie is
      found before and after them.
- [x] The web files are served gzip-compressed even to a client that does not ask for it
      (`curl` without `--compressed` gets binary data). Browsers always accept gzip. Now 406
      without `Accept-Encoding: gzip` (your choice, 2026-10-14; `send_file` in webserver.cpp).

- [ ] **ESP-IDF 6.1 bug**: `otatool.py write_ota_partition` fails with "unexpected keyword
      argument 'input'". `tools/build.py` writes ota_0 with `parttool.py` instead. Worth
      reporting to Espressif; `read_ota_partition` probably has the same problem.
- [x] ~~Size check against the recovery partition~~: with Wi-Fi the main firmware passed 1 MB
      (1.22 MB). ESP-IDF 6.1 only warns that 1 of 2 app partitions is too small and the build
      succeeds, so nothing blocks. The warning stays in the build output.
- [x] ~~Both applications compiled every component~~: the recovery build failed once it compiled
      the UI without the main firmware's LVGL settings. Each application now builds only its
      top-level component and what it requires (`COMPONENTS` in the root `CMakeLists.txt`).
- [x] esp_lvgl_port 2.9.0 calls the deprecated `esp_lcd_touch_get_coordinates` (warning in its
      own code). Goes away with a newer esp_lvgl_port; nothing to do on our side.
- [x] ESP-IDF 6.1 prints component validation warnings about its own `esp_wifi` and
      `wpa_supplicant` components. Not ours; nothing to do.

## Open decisions

Setup and network ([architecture.md](architecture.md) section 9.2):

- [x] Fast reconnect with the stored access point and channel (decided and running).
- [ ] Slowing down reconnect scans while a phone is on the hotspot: still open, less important
      now that most reconnects need no scan.

Screen ([ui/lvgl/lvgl-touch.md](ui/lvgl/lvgl-touch.md) section 6, [ui/lvgl/lvgl-dashboard.md](ui/lvgl/lvgl-dashboard.md) section 8):

- [ ] Does a swipe from the left edge also go back on a page with a back tile? (On a swipe
      page a swipe right goes back from anywhere, 2026-10-14.)
- [x] Pull-down start zone 80 x 24 px; message order errors, connecting, hint (implemented,
      to confirm by hand).
- [x] Line limit of the alert strip: 3 lines, then "..." (approved by you 2026-10-14; it wraps at
      90 % of the width).

Orientation ([architecture.md](architecture.md) section 8.1):

- [x] Implemented with the old firmware's defaults (500 ms hold, rest check), rotation in the
      panel controller. Running on the WT32-SC01.
- [x] MPU6050 wiring documented on the WT32-SC01 device page.

Web UI ([ui/web/web-app.md](ui/web/web-app.md) section 7):

- [x] OTA push method in detail: done, see ui/web/web-app.md section 5. Section 10 of web.md (now web-app.md section 7) had no open points then (2026-10-10).

Settings export and import (2026-10-07; decided: no secrets in the export, Wi-Fi networks as
names only, YAML like the dashboard, an import may carry passwords added by hand):

- [x] **What goes in** (settled 2026-10-09). Today's settings in NVS, sorted:

  | Namespace / key | Export | Why |
  | --- | --- | --- |
  | `network/networks` | names and order, no passwords | decided |
  | `network/policy`, `network/ap_enabled` | yes | user choices |
  | `network/ap_password` | no | a secret; the new device keeps its own |
  | `network/last_seen`, `last_index` | no | state of this device's radio, not a setting |
  | `ui/display` | yes, as named fields | brightness, dim, off, automatic, rotation, lock |
  | `ui/hint` | no | state ("hint seen") |
  | `webui/*` | no; since 2026-10-12 an import may set it (`webui:` `user`, `password`) | the login is a secret, the new device keeps its own |
  | `light/scale`, `light/ldr_points` | no | per device, the developer's calibration |
  | `console/log_level` | no | development |
  | `logbuf/debug` | no | diagnosis of this device |
  | `hass/url` | yes | |
  | `hass/token` | no, an import may carry it | decided 2026-10-09 |
  | `ui/sounds`, `ui/sound`, `ui/vibrate`, `ui/alarm` | yes, own sections | decided 2026-10-09 |

- [x] **Format** (decided 2026-10-07): YAML, the same as the dashboard, readable and editable by
      hand, one section per owner, only named fields (never NVS blobs, whose layout changes
      between versions):

      format: hassctrl-backup
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
      2026-10-09: the Home Assistant token too (`hass: token:` added by hand). Not the hotspot
      password and not the web UI login: an import must not change who can log in.
- [x] **Networks without a password** need a change in `network`: a saved network gets a flag
      "password needed" (an empty password is a valid open network, so it cannot be the marker).
      Such a network is never tried. The Wi-Fi page (screen and web) shows "Password needed" on it,
      and a tap asks for the password. Imported names are merged: a network that is already
      saved keeps its password, so an import over Wi-Fi does not cut the connection; the order
      comes from the file; more than 5 are cut off with a note.
      2026-10-10 (confirmed by the user on the WT32): in the scan lists (screen and web) "saved"
      means saved with a password; the tag is worked out when the list is read, so it follows a
      password entered after the scan, and the web page redraws the list after Enter and Forget.
- [x] **Who writes the sections** (decided 2026-10-07): a table of sections filled in `main`, like
      `hc::ui::Backend` and `HassAccess`. Each entry has a name, `write` (export), `check`
      (import, step 1) and `apply` (import, step 2); nothing is applied before every section
      passed its check. The main firmware registers `network`, `ui`, `sound`, `hass` and the
      dashboard (2026-10-09: no recovery app, sounds, vibration and alarm in their own section). The web server owns the request and the file
      format, nothing else. Rejected: a self-registering registry in `platform` (what is in an
      app becomes implicit), everything in `webserver` (it would depend on the UI and the
      dashboard).
- [x] **JSON reading**: settled. `espressif/cjson` from the registry is in the main firmware since
      step 5 (`components/hass`, about 5 KB of flash, allocations prefer PSRAM); the dashboard
      storage reads it too. The settings file is YAML now, so its import does not need it, and
      the recovery app stays without cJSON.
- [x] **Where** (decided 2026-10-07): System page, a "Backup" card with Export and Import
      **above** the update card, so the backup is seen before an update (web-app.md section 4).
      2026-10-09: no backup in the recovery app (was: the network section next to files.zip).
      Recovery gets the device back on the network; the import follows in the main firmware.
      NVS survives a firmware reflash, files.zip covers LittleFS, and the recovery partition is
      4 % free.

Dashboard ([architecture.md](architecture.md) section 14):

- [x] ~~**Landscape strict, portrait falls back**~~ (2026-10-13): gone with format version 2
      (2026-10-14): both orientations leave a tile without room out of the screen, the web
      editor's tray shows it.
- [ ] **Dimmer options** (your request 2026-10-14, later): `+`/`-` bars on a large dimmer card
      with a key to turn them off; `toggle_on_tap` with details on a hold of about 500 ms (the
      hold cancels the edit watch; my recommendation: no double tap on such a dimmer, see
      lvgl-dashboard.md section 8).
- [x] Your requests of 2026-10-14 (afternoon): grid up to 32x32, dots 50 % / 100 % larger,
      full-screen panels slide in from the right, the card decides the font size. Built,
      host tests 98, sim; flashed on all three boards. Confirmed working by you (2026-10-14).
- [x] Edit mode has no page swipe, no dots and no opening of areas any more (your request,
      2026-10-14, confirmed working on the P4). The `.tar` files in your backups folder hold version 1 files
      and were not converted (the YAML backups were).
- [x] **Swipe navigation per area** (2026-10-14; the first build, a strip of the main page
      and its areas, was not what you asked for and is gone): `swipe: true` on an area takes
      the back tile off its page, a swipe right goes back, the dots show the depth of nested
      swipe areas (lvgl-dashboard.md section 9). `swipe.cpp` (the direction decided after
      10 px, 40 px sideways swipes and takes the rest of the press), the dot band (16 px, a tap
      on its left half), the slide on every board, the edit layer's own detector, the dimmer's
      drag only on an up or down lock. Tried in the sim and on the boards over the console,
      confirmed working by you (2026-10-14). Free
      internal RAM on the WT32 39 KB (largest block 21 KB) before and right after a swipe; the
      two pages of a slide fit.
- [x] **Tile looks for every shape** (step 3 of the plan of 2026-10-14, built the same day):
      the shape follows the span (tall, wide, one-cell look), the climate's bars down the left
      and right of a wide card, the wide row (icon left, texts right) for the other kinds,
      reviewed in the sim at 480x320, 320x480 and 800x480 (lvgl-dashboard.md section 3). Open:
      a 2x2 switch on the P4 looks empty with the 48 px glyph (no larger glyph size).
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
      architecture.md checked against the code on 2026-10-12: section 4 now describes the
      queues as built (no typed mailbox, no `ReloadConfig`: the UI polls the current layout),
      the components, partitions, repository layout and section 13 brought up to date.
- [x] **UI docs split into LVGL and web** (2026-10-12, your request: no design guides found).
      `docs/ui/lvgl/` (lvgl-design, lvgl-touch, lvgl-dashboard, lvgl-panels, lvgl-camera,
      lvgl-settings) and `docs/ui/web/` (web-design, web-app, web-dashboard-editor,
      web-recovery); design-system.md, screen.md, dashboard.md, web.md and web-dashboard.md are
      gone, their sections moved unchanged. New: the two design guides (lvgl-design.md: the theme
      as built, the building blocks of a page, texts, checklist; web-design.md: the page frame,
      the helpers of `app.js`, Bootstrap patterns, checklist) and lvgl-touch.md (targets, every
      gesture with its values, click and vibration, waking). The terms are in ui/README.md. Every
      reference in code, tools and docs points to the new files and sections (the changelog's
      old entries keep their names). design-system.md was out of date: fonts (no 24/32, the large
      set), icons 24/48 (not 20/48), no `lv_dpx()` scaling (the board options instead).
- [x] Sample config (2026-10-12, your question): `docs/dashboard-sample/hass.yaml` said an import
      sets the Home Assistant connection from `hass:`; the dashboard import only reports it as
      ignored. The header now says where each password and token goes (backup file, web UI,
      screen) with a backup example. Rule: the sample is updated with every change of the format
      or of where a setting lives (CLAUDE.md).
- [x] **Settings sections in a dashboard import** (2026-10-12, your request "add those password
      fields to the config"): `hass.yaml` may carry `hass`, `network`, `display`, `sound`,
      `alarm`, `camera` like the backup file, tokens and passwords included. `main` hands the
      main file to `config::backup::read_settings` (the backup's section table); checked with the
      dashboard, applied after it; the answer has `settings`, `notes`, `ignored`. The sample shows
      every section commented out (`tools/test_dashboard.py` imports it onto test devices) and
      got `stream:` examples (and `webui:`, see below). Host tests: 91 (2 new: `read_settings`, the sample's sections
      uncommented parse). On the WT32, check only (your dashboard stays): the sample, `display`
      with an unknown `weather`, `display.brightness: 101` refused, `hass` with a token,
      `camera` "not on this device", a network without password (note). Confirmed working by
      you (2026-10-12).
- [ ] **Login of a tile's `stream:` URL** (2026-10-12): `http://user:password@host/...` is parsed
      by esp_http_client, but `media.cpp` opens with `esp_http_client_open`, which sends no
      Basic auth (auth_type stays none, only `perform` answers a 401), so a go2rtc with a login
      answers 401. Fix: Basic auth when the URL has a user. Open: the password then sits in the
      dashboard JSON and in every export, against "no secrets in the export"; alternative a
      `stream_user`/`stream_password` per tile, left out of the export like the token.
- [x] **Web UI login from a file** (2026-10-12, your decision: import yes, export no): a `webui:`
      section (`user`, `password`, both or neither) in the backup file and in `hass.yaml`. Checked
      like the setup page (`hc::webserver::import_login`), stored as a hash, every session ends
      (the note says "sign in again"). The export writes only a commented hint. On the WT32:
      short password, bad user and a user alone refused with the field named, a valid one
      checked; the export hint; an applied backup import with the same login: the old cookie
      is logged out, signing in again works. Confirmed working by you (2026-10-12).
- [ ] **Change password in the web UI**: there is none (reset on the screen + setup again, or a
      file with `webui:`). Proposal: a form on the System page (current password, new one twice).
- [ ] The accent cyan `0x00ffff` is defined three times (`panel::kPendingBorder`, `runner.cpp`,
      `history_view.cpp`); proposal: `theme::kAccent` (lvgl-design.md section 11).

## Next steps

State as of 2026-10-14. Steps 1 to 6 of [architecture.md](architecture.md) section 13 run on the
WT32-SC01, the JC3248W535 and the JC4880P443C; what is left of them is listed in their sections
above. The dashboard format is version 2 (sizes, hiding and overflow per orientation); the tile
looks per shape and the swipe navigation on the screen are the next two steps (Open decisions).
In order:

1. [ ] **WT32 memory** (the only open crash): LVGL ran out of memory (Vibration, 2026-10-09) and
       internal RAM is nearly used up (Step 6).
       - [x] 2026-10-10: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 16384 -> 512 for the main firmware
             on the ESP32 (`sdkconfig.main.esp32`): allocations above 512 bytes go to PSRAM. New
             console command `mem` (free and largest block of internal, 8-bit, DMA, PSRAM). WT32
             with Home Assistant connected: internal RAM that can hold data 11 -> 19 KB free
             (largest block 8 -> 14 KB), unchanged over five page swipes; no failed allocation
             in the boot log. Render times: not measured again, LVGL already allocated from PSRAM
             (`hc_lv_mem.h`), so its speed does not depend on this setting.
       - [ ] Use it for a while: does the LVGL crash come back? If it does, the failed allocation
             prints its size and both heaps (the log with it is what is needed). Note: with
             PSRAM 3.9 MB free, LVGL's own allocations should not fail; the crash may come from
             another cause.
       - [ ] If more is needed: 20 instead of 40 draw buffer lines frees about 25 KB of DMA RAM,
             at some cost in render time (measure first).
2. [x] Test the panels on the WT32 against your Home Assistant (Step 6, done 2026-10-14).
3. [x] Vibration checks: click length, alarm rhythm, the switch, `sound ring` (done 2026-10-14).
4. [x] `web/icons.svg` (2026-10-10, confirmed by you).

Clock tile (2026-10-16, your request: like Home Assistant's clock card):

- [x] `type: clock` with `zone`, `hours`, `seconds`, `date`, `style`, `border`, `ticks`
      ([ui/lvgl/lvgl-dashboard.md](ui/lvgl/lvgl-dashboard.md) section 3, architecture.md section
      5, the sample's Rooms and Workshop pages). The time zones moved from platform to core
      (`core::time_zone_rule`, `local_time`: a POSIX rule evaluated in C++, tested against
      Python's zoneinfo; the C library keeps the device's zone). Digit fonts 48 and 72 px
      (`build_icons.py`), the analog face is an `lv_meter`. Faces approved by you in the Windows
      version at 320x480 and rotated (2026-10-16). Web editor: "+ Clock", its fields, the zone
      list from the device.
- [x] Flashed on the JC3248W535 (COM10) only, firmware and web image (2026-10-16). Checked by
      me over its API and serial console: an unknown zone and `ticks` on a digital clock refused
      by the device's check, the sample checks with 21 tiles, the editor answer carries 598 zones
      and `clock-outline`; two scratch dashboards (digital, analog) imported and screenshotted
      (`build/screens/device-*.png`), the second hand and the minute change seen a minute apart.
      The board's own dashboard (24 pages, 160 tiles) was backed up first and restored after.
- [x] Confirmed by you on the JC3248W535 (2026-10-16): clocks added in the web editor, analog
      and digital, with seconds (they update), date, time zone and the name.
- [ ] The clock timer keeps ticking while the screen is off (a `clock()` call and a comparison
      per tick; the redraw does nothing on a dark screen).
- [ ] `core::local_time` takes the year from standard time: a transition within hours of New
      Year would be looked up in the wrong year (no zone has one). A zone a future tzdata drops
      fails `finish()` like an unknown icon. The table is tzdata 2026e; a new release means
      `python tools/build_timezones.py` and a firmware update, nothing updates it on the device.
      Morocco (`Africa/Casablanca`) differs between 2026e (`<+00>0`) and the 2024a of an old
      Python: not a bug of the evaluator.
- [ ] Open: a 1x1 analog clock with name and date leaves a small face (approved as is; the date
      could go first).

Weather tile and panel (2026-10-13, your request: a tile and its details, no history):

- [x] `weather.*` is a tile of its own (1x1, 1x2, 2x1): condition glyph, temperature, condition
      and humidity; the panel shows the current values and the daily and hourly forecast
      ([ui/lvgl/lvgl-panels.md](ui/lvgl/lvgl-panels.md) section 6, architecture.md section 6).
      Designed in the Windows version at four board sizes and approved by you, then the backend
      (`weather/subscribe_forecast`), 96 host tests. Tested on the WT32-SC01 with your
      OpenWeatherMap: tile, both forecasts, scrolling, closing and reopening. The WT32 was
      reflashed completely first (it ran another firmware: recovery app, firmware, web image).
- [x] Confirmed working on the WT32-SC01 by you (2026-10-13): unit changes show at once, values
      follow Home Assistant, the forecast panel works; after a restart of Home Assistant the chips
      and the strip came back (bugs fixed the same day).
- [x] A change of the values (wind speed) after 30 minutes and a reconnect while the panel is
      open: seen by you. Not tested: the other boards (they build).
- [ ] Not supported: the TTGO T4's 320x240 (decided: only "Feels like" fits above the forecast).
- [ ] `twice_daily` forecasts, the night glyph of `partlycloudy`.

Not built yet (placeholders in the code or the documentation):

- [ ] Climate `heat_cool` with two setpoints (climate panel, Step 6).
- [ ] Home Assistant events waking the screen (`ui/src/power.hpp`).
- [ ] "Start the firmware" on the System page of the web UI in the recovery app (only the
      built-in page `/recovery` has it).
- [x] Trusting a self-signed Home Assistant certificate on first use (Step 5, 2026-10-10).
- [x] OTA push method in detail (Open decisions).

- [x] **WT32 replaced + LDR** (2026-10-07; the lux table is dropped 2026-10-10, see the two-point setup below): new WT32-SC01 flashed on COM9 (recovery app, main, web UI image).
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
- [x] ~~LDR table as board defaults~~ dropped (your decision, 2026-10-10): the LDR does not work well
      enough for a lux table. Instead a simple two-point setup for the automatic brightness: the light
      where the display is at its minimum, and the light where it is at 100 %. Built and flashed on the WT32 2026-10-10 (Display page: "Lowest brightness at" /
      "Full brightness at", tap in the room's light; defaults 0 and 350 lx; in the backup). Confirmed by you 2026-10-10.
