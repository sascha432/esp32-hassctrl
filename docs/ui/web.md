# Web UI layout

Status: **proposal**, partly running (2026-10-07): setup, login, status, Wi-Fi, hotspot and
system pages in `web/`; Home Assistant, dashboard and logs follow with their firmware steps.
Terms and colors are in [design-system.md](design-system.md).

## 1. Principles

* **Phone first.** The first contact is a phone in the captive portal. One column, at most
  480 px wide, centered on large screens.
* **Works without internet.** Everything is served by the device: no CDN, no web fonts. In the
  captive portal there is no internet.
* **Bootstrap look with light and dark mode** (decided), the full Bootstrap CSS file (decided),
  see [design-system.md](design-system.md) section 7. No JavaScript framework.
* **Secrets are write-only.** The web UI can set Wi-Fi passwords and the Home Assistant token, but
  never reads them back. It shows whether a secret is stored.
* **Always recoverable.** Login and firmware update work even when the file system is broken
  (decided), see section 7.
* **Same terms** as the screen. English only (decided).

## 2. Site map

```
Setup                     only while the device is not set up, no login
  Wi-Fi  ->  Web UI login  ->  Done
Login
Main pages                after login
  Status                  device, Wi-Fi, hotspot, Home Assistant, memory, uptime
  Home Assistant          URL, token, test connection
  Wi-Fi                   saved networks (up to 5), add, forget, reconnect
  Hotspot                 on or off, name, password, new password
  Dashboard               areas and tiles editor, YAML import (with includes) and export,
                          validation errors
  Logs                    view and filter, debug mode
  System                  backup (export and import), firmware and file system update,
                          restart, factory reset
Recovery                  compiled into the firmware: login, update, format
```

The dashboard is edited here (decided). YAML import and export are for advanced users, also
with a tool. There is no file manager (decided).
Settings are exported and imported on the System page as YAML, without secrets; an import may
carry passwords added by hand (decided).

## 3. Page frame

A header with the device name, the state icons and the menu button, then the page title and cards.

```
+--------------------------------------------+
| hassctrl-1a2b            [Wi-Fi] [HA]  [=] |
+--------------------------------------------+
| Page title                                 |
|                                            |
| +--------------------------------------+   |
| | card                                 |   |
| +--------------------------------------+   |
| +--------------------------------------+   |
| | card                                 |   |
| +--------------------------------------+   |
+--------------------------------------------+
```

The menu opens over the page on phones. On screens wider than 800 px it is a sidebar instead. It
also holds the light, dark or automatic mode.

```
+--------------------------------------------+
| hassctrl-1a2b            [Wi-Fi] [HA]  [x] |
+--------------------------------------------+
|  Status                                    |
|  Home Assistant                            |
|  Wi-Fi                                     |
|  Hotspot                                   |
|  Dashboard                                 |
|  Logs                                      |
|  System                                    |
+--------------------------------------------+
|  Light / dark / auto                       |
|  Log out                                   |
+--------------------------------------------+
```

## 4. Setup pages

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

When the device joins the home network, the phone may lose the hotspot for a moment (decided:
accepted). The page keeps polling and shows the result as soon as the phone is back. The hotspot
stays on for a while after the setup with the same password, so there is no hurry (decided;
proposal: 15 minutes).

If the screen and a phone submit setup data at the same time, the first submission wins and the
other side is not told (decided).

## 5. Login

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

* **Remember me** stores a persistent cookie that lasts 90 days (decided). Without it the
  cookie ends with the browser session.
* The cookie is a signed token holding the username and the expiry, so no session table is
  needed.
* **Anything that changes ends all tokens** (decided): the username, the password, or a counter
  stored in NVS. The counter is increased by "log out everywhere" in the menu.
* How (decided): the signing key is derived from the stored login record and the counter,
  `key = HMAC(stored password hash, counter)`. The stored hash is `hash(salt, username,
  password)` with PBKDF2. A new password or username gives a new hash, a new counter gives a new
  key, and either way every old token fails the check. The counter is kept out of the password
  hash itself, because the device stores only the hash: increasing a counter inside it would
  need the plain password. So "log out everywhere" works without asking for the password.
* Failed logins are slowed down (1 s). Log out is in the menu.
* PBKDF2-HMAC-SHA256 with 1000 iterations: 10000 took 5.3 s on the ESP32 (PSA crypto) and
  blocked the web server for every login; 1000 take about 0.4 s.

## 6. Main pages

**Home Assistant.** URLs found by mDNS are offered in the list, a URL can also be typed. The token
field is empty. `stored` says that a token exists.

Running since 2026-10-07 (`components/hass`, `api_hass.cpp`, the `ha` view of `web/app.js`):

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
* Changed from the sketch below: **Save and connect** instead of "Test connection" and "Save".
  The status shows the result within seconds. Open: a separate test that does not store the
  token, so a typo cannot replace a working token ([TODO](../TODO.md)).
* The header's `HA` is green while connected, red for a rejected token, yellow otherwise.
* API: `GET /api/hass`, `POST /api/hass` (`url`, `token`; an empty token keeps the stored one,
  an empty url removes both), `POST /api/hass/reconnect`, `GET /api/hass/discover`. The token
  is write-only. The recovery app has none of them (404).

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
| | [ Test connection ]        [ Save ]  |   |
| +--------------------------------------+   |
| Create a token in Home Assistant:          |
| Profile > Security > Long-lived tokens     |
+--------------------------------------------+
```

**Logs.** Viewable over the web with a filter, plus a debug mode that adds notice and info
messages to the normal error log (decided; details are discussed later). No live view: the page
shows the log as of loading, `Refresh` loads it again (decided).

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

**System.** Firmware and file system updates are uploaded here (decided), see section 8. The
backup card sits above the update card, so the backup is seen before an update (decided).

```
+--------------------------------------------+
| hassctrl-1a2b            [Wi-Fi] [HA]  [=] |
+--------------------------------------------+
| System                                     |
|  Firmware  0.1.0      Uptime  3 d 4 h      |
|  Free memory  internal 84 KB, PSRAM 7 MB   |
| +--------------------------------------+   |
| | Backup (settings without secrets)    |   |
| | [ Export ]           [ Import ]      |   |
| +--------------------------------------+   |
| | Update                               |   |
| | [ Firmware ]       [ File system ]   |   |
| +--------------------------------------+   |
| | Device                               |   |
| | [ Restart ]     [ Factory reset ]    |   |
| +--------------------------------------+   |
+--------------------------------------------+
```

## 7. Web files and recovery

The web files live on LittleFS (decided). LittleFS survives power loss well, but a corrupted file
system has happened on earlier devices, mostly with the files still readable. So:

* **Recovery page in the firmware** (decided): a page with barely any design, compiled into the
  firmware. It is served when the file system cannot be mounted or the web files are missing. It
  offers login, export of the settings and of every file that can still be read, firmware
  update, file system update, format and restart. Login works without the file system, because
  the credentials are in NVS.
* **Running** (2026-10-07): `/recovery` always serves the built-in page, `/` serves it while the
  web files are missing. `GET /api/fs/files.zip` streams every file that can still be read as
  one .zip (stored, not compressed; files that cannot be read are listed in `unreadable.txt`
  inside), `POST /api/fs/format` with `confirm=yes` erases the file system. Both need the login.
  The settings export is not built yet ([TODO](../TODO.md)).
* **No automatic format** (decided): a LittleFS that fails to mount starts the recovery mode
  instead of being formatted, so files that are still readable can be exported first. Only a
  partition without any LittleFS (new device, leftovers of another firmware) is formatted.
* **Defensive reading** (proposal): a file that cannot be parsed is reported and skipped, never a
  reason to crash.
* **Atomic install** (proposal): an upload goes into a temporary directory and replaces the old
  files only when it is complete. An interrupted upload leaves the old UI in place.
* **Version check** (proposal): the web files carry the API version they were built for. On a
  mismatch the firmware shows "web UI outdated" with an upload button.
* The firmware does **not** embed a second copy of the web files (decided). Updates are a firmware
  image and a file system image.

```
+--------------------------------------------+
| hassctrl-1a2b  RECOVERY                    |
+--------------------------------------------+
| The file system could not be read.         |
|                                            |
| Username [admin       ]                    |
| Password [**********  ]                    |
| [Log in]                                   |
|                                            |
| [Export settings and files]                |
| Firmware     [choose file]  [Upload]       |
| File system  [choose file]  [Upload]       |
| [Format file system]   [Restart]           |
+--------------------------------------------+
```

## 8. Firmware update

* **Update form** (decided): the System page and the recovery page take a firmware image and a
  file system image, either as a file upload or as a URL the device downloads from.
* **OTA over the network** (decided, running): the same authenticated endpoints take the raw
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
* **Flash budget**: two OTA partitions on the 4 MB boards leave about 1.9 MB per firmware image
  and little room for LittleFS. The proposal is a small recovery app in its own partition, see
  [architecture.md](../architecture.md) section 9.5.

## 9. HTTPS

* HTTPS is an option in the web UI settings, plain HTTP is the default (decided). There is no
  certificate upload (decided): the device creates its own self-signed certificate. The browser
  warning is acceptable when the device is opened by its IP address (decided).
* It must not cost too much internal RAM. PSRAM is fine (decided). Done for the main firmware
  (2026-10-07, `sdkconfig.main`): `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`. A `wss://` connection
  with certificate check runs on the WT32 with 19 KB internal RAM left during the handshake.
  The recovery app keeps mbedTLS in internal RAM (it must run without PSRAM).
* Espressif's figures for one TLS connection: about 42 KB of heap with the default buffers, about
  22 KB with dynamic buffers. `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` moves all mbedTLS allocations
  to PSRAM. The real cost on the device is measured in step 4 of the implementation order.
* Proposal: the certificate is created when HTTPS is turned on and kept in NVS. While HTTPS is
  on, plain HTTP redirects to it. The captive portal and the setup pages stay plain HTTP, because
  phones detect captive portals over HTTP. At most 2 TLS connections at a time.

## 10. Open points

* The OTA push method in detail.
