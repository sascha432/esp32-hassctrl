# Web app

The setup, login, Status, Home Assistant, Wi-Fi, Hotspot, Camera, Dashboard, Logs and System
pages in `web/`. The look, the page frame and how a page is built:
[web-design.md](web-design.md). The dashboard editor: [web-dashboard-editor.md](web-dashboard-editor.md).
The built-in recovery page and the recovery app: [web-recovery.md](web-recovery.md).

## 1. Site map

```
Setup                     only while the device is not set up, no login
  Wi-Fi  ->  Web UI login  ->  Done
Login
Main pages                after login
  Status                  device, Wi-Fi, hotspot, Home Assistant, memory, uptime
  Home Assistant          URL, token, test connection
  Wi-Fi                   saved networks (up to 5), add, forget, reconnect
  Hotspot                 on or off, name, password, new password
  Camera                  only with a camera: the web stream (on, size, frame rate, quality,
                          login) and its addresses
  Dashboard               areas and tiles editor, YAML import (with includes) and export,
                          validation errors
  Logs                    view and filter, debug mode
  System                  backup (export and import), firmware and file system update,
                          restart, factory reset
Recovery                  compiled into the firmware: login, update, format
```

The dashboard is edited here. YAML import and export are for advanced users, also
with a tool. There is no file manager.
Settings are exported and imported on the System page as YAML, without secrets; an import may
carry passwords added by hand.

## 2. Setup pages

The captive portal opens the first page. Three steps, shown as `1 / 3` in the header.

```
+--------------------------------------------+
| Set up hassctrl-1a2b                 1 / 3 |
+--------------------------------------------+
| Choose your Wi-Fi                 [rescan] |
| +--------------------------------------+   |
| | Home-Net             [lock]  ||||    |   |
| | Neighbor             [lock]  |||     |   |
| | Guest                        ||      |   |
| | Other network ...                    |   |
| +--------------------------------------+   |
| Password                                   |
| [************************]          [show] |
|                              [  Connect  ] |
+--------------------------------------------+
```

The web UI login page follows, with the same fields as on the screen. The last page shows where
the device is now reachable:

```
+--------------------------------------------+
| Set up hassctrl-1a2b                 3 / 3 |
+--------------------------------------------+
| Connected to Home-Net                      |
|                                            |
| Join Home-Net with this phone and open     |
|   http://hassctrl-1a2b.local               |
|   http://192.168.1.57                      |
|                                            |
| [ ] Keep the hotspot on                    |
|                                 [  Done  ] |
+--------------------------------------------+
```

When the device joins the home network, the phone may lose the hotspot for a moment
(accepted). The page keeps polling and shows the result as soon as the phone is back. The hotspot
stays on for 15 minutes after the setup with the same password, so there is no hurry.

If the screen and a phone submit setup data at the same time, the first submission wins and the
other side is not told.

## 3. Login

```
+--------------------------------------------+
| hassctrl-1a2b                              |
+--------------------------------------------+
|                                            |
|   Username  [admin                  ]      |
|   Password  [************           ]      |
|   [x] Remember me                          |
|                             [  Log in  ]   |
+--------------------------------------------+
```

* **Remember me** stores a persistent cookie that lasts 90 days. Without it the
  cookie ends with the browser session.
* The cookie is a signed token holding the username and the expiry, so no session table is
  needed.
* **Anything that changes ends all tokens**: the username, the password, or a counter
  stored in NVS. The counter is increased by "log out everywhere" in the menu.
* **Change the password**: System, card "Password": the current password, the new one
  twice (8 to 64 characters). `POST /api/password` (`current`, `password`, the login of the
  session): a wrong current password waits 1 s and answers 400 (not 401, which would send the web
  UI to its login page). Every other session ends; the browser that changed it gets a new session
  cookie, not remembered. The username stays (a new one: the reset on the screen or a `webui:` file).
* **From a file**: a `webui:` section with `user` and `password` in the backup file or
  in a dashboard import's `hass.yaml` replaces the login (same rules as the setup page; every
  session ends, the browser that imported signs in again). Import only: the device stores a
  hash, the export writes a commented hint where the section goes.
* How: the signing key is derived from the stored login record and the counter,
  `key = HMAC(stored password hash, counter)`. The stored hash is `hash(salt, username,
  password)` with PBKDF2. A new password or username gives a new hash, a new counter gives a new
  key, and either way every old token fails the check. The counter is kept out of the password
  hash itself, because the device stores only the hash: increasing a counter inside it would
  need the plain password. So "log out everywhere" works without asking for the password.
* Failed logins are slowed down (1 s). Log out is in the menu.
* PBKDF2-HMAC-SHA256 with 1000 iterations: 10000 took 5.3 s on the ESP32 (PSA crypto) and
  blocked the web server for every login; 1000 take about 0.4 s.

## 4. Main pages

**Home Assistant.** URLs found by mDNS are offered in the list, a URL can also be typed. The token
field is empty. `stored` says that a token exists.

Code: `components/hass`, `api_hass.cpp`, the `ha` view of `web/app.js`.

* The connection card shows the state (not set up, waiting for Wi-Fi, connecting, connected,
  token rejected, disconnected with the seconds to the next try), the problem in words, the
  address, Home Assistant's version and how long it is connected. It refreshes every 2 s. "Try
  now" after a rejected token or while disconnected.
* `Search` asks the network for `_home-assistant._tcp` (mDNS, 2 s; it runs by itself while no
  address is set). Home Assistant announces its own address (`internal_url`), its name and its
  version; a tap takes the address.
* The address is cleaned up: `http://` is added, `ws://`/`wss://` become `http://`/`https://`,
  a path (`/lovelace/0`) is dropped, the port is checked. The token is checked for characters a
  token cannot have (a space from pasting).
* **Save and connect** stores both and connects; the status shows the result within seconds.
* The header's `HA` is green while connected, red for a rejected token, yellow otherwise.
* A self-signed certificate (https): when the bundle does not trust the server's
  certificate, the card "Certificate not trusted" shows its SHA-256 fingerprint
  (`AB:CD:...`, as browsers show it) and asks to compare it with the browser's. **Trust this
  certificate** stores it and connects. From then on the card "Certificate" shows the trusted
  fingerprint with **Forget**. A new address forgets it. If the server presents a different
  certificate later, the problem reads "the server's certificate changed" and the new
  fingerprint is offered.
* A board with a motion sensor: the row "Motion helper" with `input_boolean.<device>_motion`, and
  "missing" when Home Assistant has no such entity ([motion-sensor.md](../../home-assistant/motion-sensor.md) section 4).
* API: `GET /api/hass` (also `certificate` and `refused_certificate`, SHA-256 as hex, empty:
  none; `motion_helper`: empty without a sensor, `unknown`, `missing`, `found`, and `motion_entity`), `POST /api/hass` (`url`, `token`; an empty token keeps the stored one, an empty url
  removes both), `POST /api/hass/reconnect`, `POST /api/hass/certificate` (`fingerprint`, 64 hex
  digits, colons allowed; empty forgets it), `GET /api/hass/discover`, and for the
  dashboard editor's entity picker `GET /api/hass/entities`, `GET /api/hass/entity`
  ([web-dashboard-editor.md](web-dashboard-editor.md) section 3). The token is write-only. The recovery app has none of them (404).

```
+--------------------------------------------+
| hassctrl-1a2b            [Wi-Fi] [HA]  [=] |
+--------------------------------------------+
| Home Assistant                             |
|  Status  Not connected: invalid token      |
| +--------------------------------------+   |
| | URL                                  |   |
| | [http://homeassistant.local:8123  v] |   |
| | Token                        stored  |   |
| | [paste a new token                 ] |   |
| | [ Search ]        [ Save and connect ]|   |
| +--------------------------------------+   |
| Create a token in Home Assistant:          |
| Profile > Security > Long-lived tokens     |
+--------------------------------------------+
```

**Logs.** Viewable over the web with a filter and a debug mode. No live view: the page shows the
log as of loading, `Refresh` loads it again.

Code: `components/logbuf`, `api_log.cpp`. The last 16 KB of esp_log messages in
PSRAM, the oldest lines go first, empty after a restart. Errors and warnings are always kept,
Debug (stored) keeps info messages as well (ESP-IDF has no "notice"). The console's `log` level
only changes the serial output. Filter: Errors, Warnings and errors, Everything (the default).
Times are the device's clock once it is set, else `+h:mm:ss` since the start. Download saves the
whole log as text. The bar right of the box drags the page wider than its 800 px,
kept in the browser, a double-click goes back to the default. API: `GET /api/log` `{"debug", "capacity"}`, `POST /api/log` `debug=1|0`,
`GET /api/log.txt` one line per message, `<unix time or 0> <esp_log line>`.

```
+--------------------------------------------+
| hassctrl-1a2b            [Wi-Fi] [HA]  [=] |
+--------------------------------------------+
| Logs                         [Errors   v]  |
| +--------------------------------------+   |
| | 12:41:07 E wifi  wrong password      |   |
| | 12:41:12 I wifi  retry in 10 s       |   |
| | 12:41:22 I wifi  connected           |   |
| +--------------------------------------+   |
| [ Refresh ]  [ Download ]   Debug  [  o]   |
+--------------------------------------------+
```

**System.** Firmware and file system updates are uploaded here, see section 5. The
backup card sits above the update card, so the backup is seen before an update.

Backup: one YAML file with the sections `network`, `display`, `sound`, `alarm`,
`camera`, `hass`, `webui` (import only, section 3) and the dashboard (`defaults`, `tiles`); Export all, Settings only,
Dashboard only (`GET /api/backup.yaml[?parts=settings|dashboard]`). A section of something the
device does not have (`sound`, `alarm`, `camera`) is not written, and in a file from another
device it is ignored ("camera (not on this device)"). No password and no token is written; a
Wi-Fi `password:`, a `hass:` `token:` or a `camera:` `stream_password:` added to the file by hand
is used. `camera:` holds `mains_hz`, `auto_exposure`, `brightness`, `exposure_us`, `gain` (in
1/16), `mirror`, `flip`, `stream`, `stream_size` (full, half), `stream_fps`, `stream_quality`,
`stream_user`; another `stream_user` without a `stream_password` drops the stored password, and
the stream stays off without a login (a note says so). A trusted self-signed
certificate is written as `hass:` `certificate:` (its SHA-256) and trusted again on import. Import and Check upload the
file (`POST /api/backup/import[?check=1]`): the whole file is checked first, one problem and
nothing changes; the answer lists the sections, the problems, what to know ("Office needs its
password") and what was ignored. A network without a password is saved as "password needed" and
not tried until it gets one. Only the main firmware: the recovery app has no backup (404), and
its System page shows no Backup card.

The dashboard import (Dashboard page, Import & export) reads the same settings sections from
`hass.yaml`: checked with the dashboard, applied after it, the message names them
("settings: hass, camera"), the notes and what was ignored. `docs/dashboard-sample/hass.yaml`
shows every section, commented out.

```
+--------------------------------------------+
| hassctrl-1a2b            [Wi-Fi] [HA]  [=] |
+--------------------------------------------+
| System                                     |
|  Firmware  0.1.0      Uptime  3 d 4 h      |
|  Free memory  internal 84 KB, PSRAM 7 MB   |
| +--------------------------------------+   |
| | Backup (no passwords, no token)      |   |
| | [ Export all ] [ Settings ] [ Dash ] |   |
| | [ file ...  ]    [ Check ] [Import]  |   |
| +--------------------------------------+   |
| | Update                               |   |
| | [ Firmware ]       [ File system ]   |   |
| +--------------------------------------+   |
| | Device                               |   |
| | [ Restart ]     [ Factory reset ]    |   |
| +--------------------------------------+   |
+--------------------------------------------+
```

### 4.1 Camera stream

Only on a board with a camera (docs/architecture.md section 9.8). The stream is set on the screen
(Settings, Camera, docs/ui/lvgl/lvgl-settings.md section 3) or on the web UI's Camera page; the
picture (exposure, gain, mirror, flip, mains frequency) only on the screen, next to its preview.

**Camera page** (in the menu only when `/api/status` says `"camera": true`): one form with "Stream
on", "Size" (1288 x 728 or 644 x 364), "Frames per second (at most)" 5, 10, 15 or 30, "Quality" 30 to
95 (a slider, default 50), "Username" and "Password" (the stream's own login; the password is never shown, an
empty field keeps it), Save; then the two addresses with the browser's host name. Saving with the
stream on and no complete login is refused ("the stream needs a username and a password").
Another username drops the stored password (it needs its password again), an empty username
removes the login and the stream goes off.

* `GET /api/camera` (web UI login): `{"enabled", "size": "full"|"half", "width", "height", "fps",
  "quality", "username", "password_set"}`. `POST /api/camera` with any of `enabled=1|0`,
  `size=full|half`, `fps=5|10|15|30`, `quality=30..95`, `username`, `password` (empty or missing
  keeps the stored one); answers like GET, 400 with `error` for a wrong value. 404 without a
  camera.

* `GET /api/camera.mjpg`: the MJPEG stream (`multipart/x-mixed-replace`), at most the frame rate
  of the settings, at most two at a time (a third gets 503).
* `GET /api/camera.jpg`: one picture; when no capture runs, it starts one and waits half a second
  for the exposure.
* Both answer 404 while the stream is off and ask for the stream's own login with HTTP Basic auth
  (not the web UI login), so a player or Home Assistant's MJPEG camera can open them:
  `http://user:password@<device>/api/camera.mjpg`. Plain HTTP: the login crosses the network
  readable.

## 5. Firmware update

* **Update form**: the System page and the recovery page take a firmware image and a
  file system image, either as a file upload or as a URL the device downloads from.
* **OTA over the network**: the same authenticated endpoints take the raw
  image as the request body (`POST /api/update/firmware`, `/api/update/filesystem`), so `curl
  --data-binary` or `tools/update.py` push a build without a browser. `POST /api/update/url`
  makes the device download the image (http or https, common CA bundle, redirects followed);
  `GET /api/update` reports the app, what it can write and the progress. On a 4 MB board the
  main firmware answers 409 with `"recovery": true`; the page and the script restart the device
  into the recovery app (`POST /api/restart` with `to=recovery`) and send the image there.
* **Checks before writing**: a firmware image must be an app image for this chip whose project
  is `hassctrl` (the recovery app and other firmware are refused); a file system image must carry
  the LittleFS magic. Only then is anything erased. A firmware image must also be built for this
  board: every image carries the board name right after `esp_app_desc_t` (ESP-IDF's custom app
  description, `hc::board::ImageTag`), so a T4 image on the WT32 is refused with "the firmware is
  for the TTGO T4, this is the WT32-SC01".
* The firmware image goes to the inactive OTA partition and is booted after a check. If the new
  firmware does not confirm itself after boot, the bootloader rolls back (ESP-IDF app rollback).
* **Flash budget**: two OTA partitions on the 4 MB boards would leave about 1.9 MB per firmware
  image and little room for LittleFS, so they carry one OTA partition and the recovery app, see
  [architecture.md](../../architecture.md) section 9.5.

## 6. HTTPS

* Cancelled: there will be no HTTPS web server. The web UI is plain HTTP only, with no
  certificate and no HTTPS option in the settings.
* TLS stays in use as a client: `wss://` to Home Assistant, camera snapshots over https and the
  update download by URL. For this the main firmware keeps mbedTLS in PSRAM
  (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` in `sdkconfig.main`): a `wss://` connection with
  certificate check runs on the WT32 with 19 KB internal RAM left during the handshake. The
  recovery app keeps mbedTLS in internal RAM (it must run without PSRAM).

## 7. Open points

* Home Assistant: a test of address and token that does not store them, so a typo cannot
  replace a working token (today Save and connect stores first).
