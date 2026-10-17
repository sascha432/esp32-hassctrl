# Web files and recovery

Status: **running**. Where the web files live, what happens when they are missing or the file
system is broken, the built-in recovery page (`components/webserver/src/builtin_page.cpp`) and
the web UI in the recovery app. The recovery app itself (partitions, what it runs):
[architecture.md](../../architecture.md) section 9.5. Firmware and file system updates:
[web-app.md](web-app.md) section 5.

## 1. Web files and recovery

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
  The settings export is not built yet ([TODO](../../TODO.md)).
* **In the recovery app** (2026-10-12): the same rules, `/` serves the web UI from LittleFS when
  it is there. Its Home Assistant and Dashboard pages say "not in the recovery app", the System
  page has no Backup card; Status, Wi-Fi, Hotspot, Logs, update, files, format, restart and
  factory reset work. "Start the firmware" is on the built-in page (`/recovery`) and, since
  2026-10-17, on the System page (Restart and reset) when a firmware is in flash.
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

## 2. The built-in page's look

The built-in page has barely any design on purpose (decided): one HTML string in the firmware, no
Bootstrap, no external files, about the size of a small icon font. It follows the system's light
or dark mode (`prefers-color-scheme`) with the screen's colors in dark (background `#0b0f14`, card
`#182029`, label `#7fa8c9`) and the orange accent `#ff8000` for buttons and messages. One column of
at most 480 px, cards as `<section>` with radius 12, facts as a two-column `<dl>`. A change to its
look stays in that file; it is not a second web UI.

## 3. Open points

* None.
