# UI documentation

Two user interfaces, each in its own folder; the file names say which one they belong to.

**LVGL**, the touch screen ([lvgl/](lvgl/)):

| Document | Contents |
| --- | --- |
| [lvgl-design.md](lvgl/lvgl-design.md) | the design guide: principles, colors, fonts, sizes, icons, resolutions and board options, the building blocks of a page, texts, checklist |
| [lvgl-touch.md](lvgl/lvgl-touch.md) | touch targets, the gestures of every screen, click, ticks and vibration, waking the screen |
| [lvgl-dashboard.md](lvgl/lvgl-dashboard.md) | the dashboard: pull-down, alert strip, start screen, grid, tiles, states, icons, arrangement per orientation, edit mode |
| [lvgl-panels.md](lvgl/lvgl-panels.md) | the panels a tile opens: frame, light (dimmer), climate, sensor, lock |
| [lvgl-camera.md](lvgl/lvgl-camera.md) | the camera image, its full screen view and MJPEG streams, with measurements |
| [lvgl-settings.md](lvgl/lvgl-settings.md) | Settings: status bar, navigation, every page, the alarm screen, dialogs; the first setup |

**Web**, the web UI in the browser ([web/](web/)):

| Document | Contents |
| --- | --- |
| [web-design.md](web/web-design.md) | the design guide: principles, Bootstrap look, page frame, how a page is built, texts, checklist |
| [web-app.md](web/web-app.md) | the web app: site map, setup, login, the main pages and their API, firmware update, HTTPS |
| [web-dashboard-editor.md](web/web-dashboard-editor.md) | the dashboard editor: what it does, data flow, the entity picker, page layout; [demo](web/web-dashboard-editor-demo.html) |
| [web-recovery.md](web/web-recovery.md) | web files on LittleFS, the built-in recovery page, the web UI in the recovery app |

[images/](images/) holds pictures of the screens for the README.

## Terms

Shared by the touch screen and the web UI. New terms go here first.

| Term | Meaning | Status |
| --- | --- | --- |
| **Wi-Fi** | the connection of the device to the home network (ESP-IDF: station, STA) | decided |
| **Hotspot** | the network the device opens itself, for setup and recovery (ESP-IDF: access point, AP) | decided |
| **Settings** | the settings on the screen | decided |
| **Quick settings** | the first page of Settings | decided |
| **Web UI** | the web site of the device | decided |
| **Dashboard** | the Home Assistant tiles, the main screen of the device | decided |
| **Page**, **tile**, **panel** | a page of the dashboard, one cell of it, the view a tile opens | from the old firmware |
| **Status bar** | the bar at the top of Settings and the setup screens. The dashboard has none | decided |
| **Pull-down** | the swipe down from the top center that opens Settings | decided |
| **Alert strip** | the message panel at the top center of the dashboard, for errors, warnings and hints | decided |
| **Edit mode** | moving and removing tiles on the screen | decided |
| **Recovery page**, **recovery app** | the web page built into the firmware; the small firmware in its own partition | decided |

Code and developer docs use the ESP-IDF terms (AP, STA). Users never see them. The UI is English
only (decided).

## How to read the wireframes

* Wireframes are ASCII at a fixed scale: one column is 8 px, one line is about 16 px. The screen
  reference is 480 x 320 (60 columns), the web reference is a phone at 360 px (44 columns).
* `[text]` is a button or a field, `[  o]` / `[o  ]` a switch (off / on), `>` opens a sub page,
  `<` in the bar goes back, `||||` is the signal strength, `[wifi]` an icon.
* Every value in a wireframe is an example.
* Each document ends with its open points. Decided items are marked as decided; everything else
  is a proposal.

## Previews

The screens are reviewed in the Windows version of the UI (`python tools/sim.py`, see the README),
with the real fonts, in every window size and rotation, and rendered into PNG files by script. On
a board, `tools/screenshot.py` takes a screenshot over the serial console. The web UI is its own
preview; the dashboard editor also has a demo page that runs without a device.

## Keeping this current

A change of a screen or page is not done until its document is updated, in the same commit as the
code. A new color, font or size goes into the design guide of its side first.
