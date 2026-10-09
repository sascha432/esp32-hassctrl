# esp32-hassctrl - architecture

Status: **proposal, nothing implemented yet.** Review this before code is written on top of it.

A Home Assistant touch panel for ESP32 / ESP32-S3 / ESP32-P4, written in C++ on native ESP-IDF
with LVGL 9. It is a port of the Home Assistant dashboard of the `weather_station2` plugin in
`kfc_fw` (Arduino, LovyanGFX, LVGL 8.4, WT32-SC01). The behavior of that dashboard carries over,
and its `hass.yaml` format stays as the import and export format; its internal structure does not.

---

## 1. Goals and non-goals

Goals:

* The same dashboard: tiles, areas, panels, history graphs, camera previews, configured in the
  web UI; the existing `hass.yaml` can be imported (section 5, "Dashboard configuration").
* Boundaries the build enforces: a module that must not know about LVGL or the network cannot
  include it.
* Every piece of mutable state has exactly one owning task. No shared state, no `volatile` flags.
* Domain logic, config parsing, template building and response decoding run on the PC as unit
  tests. The design review of the screens is done on a Windows version of the UI (decided
  2026-10-07, replaces "no UI simulator"); the final version is tested on the WT32-SC01 by the
  developer. The Windows version only shows the GUI: no network and no Home Assistant, the
  entity states come from a sample file written by the agent, and a tap or drag changes those
  sample states, so the developer can click through pages, panels and controls to judge the
  look. LVGL 8.4 with LVGL's Windows driver, window sizes of the boards (480x320, 320x480,
  320x240, later 640x480 and 480x800), a key to rotate.
* Adding a board touches one board directory and one Kconfig choice, nothing else.
* LVGL 8.4 first, because the existing UI code is LVGL 8. LVGL 9 follows; once it works well,
  LVGL 8 support is removed (see section 7.1).
* First setup and network recovery work from the touch screen and from a phone through an access
  point with a captive portal (see section 9).

Non-goals for the first version (seams are left for them, see section 9):

* BLE provisioning (decided against), web dashboard, remote debug protocol.

---

## 2. Toolchain and build

* **Native ESP-IDF with `idf.py`** (CMake + Ninja), not PlatformIO. PlatformIO lags behind
  ESP-IDF releases and needed the pioarduino fork for the P4, which is how the previous attempt
  ended up with two platforms and two LVGL versions in one matrix.
* ESP-IDF **6.1** (decided), installed with the ESP-IDF Installation Manager (EIM): the source in
  `C:\esp\v6.1\esp-idf`, the tools in `C:\Espressif\tools`. A terminal is prepared with EIM's
  activation script `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`.
* Dependencies come from the Espressif component registry, declared in each component's
  `idf_component.yml`, locked in `dependencies.lock` (committed).
* One build directory per board and application. Board selection is a Kconfig choice set by the
  board's defaults file. `tools/build.py` passes the layers and the target; see the README for the
  commands.
* One ESP-IDF project builds both applications: `HC_APP=main` or `HC_APP=recovery` excludes the
  other application's top-level component (section 9.5).
* Host builds (tests) are a plain CMake project in `host/` that compiles the pure
  components directly. They do not use the ESP-IDF build system at all. Compiler: MSVC of Visual
  Studio 2022 (decided), with the cmake and ninja that ESP-IDF installs.

---

## 3. Layers and components

Every module is an ESP-IDF **component** with its own `CMakeLists.txt`. `REQUIRES` in that file
is the dependency rule: if `core` does not list `lvgl`, `#include <lvgl.h>` in `core` fails to
compile. This is the main tool that keeps the architecture from eroding, by people or by AI
agents.

```
                 +-------------------------------------------+
                 |  main/   composition root (app_main)       |
                 |  creates everything, wires the mailboxes   |
                 +-------------------------------------------+
                    |            |              |           |
                    v            v              v           v
              +---------+  +----------+  +-----------+  +-----------+
              |   ui    |  |   hass   |  |   board   |  | platform  |
              | LVGL 9  |  | WS + API |  | lcd/touch |  | wifi, fs, |
              | screens |  | session  |  | backlight |  | time, log |
              +---------+  +----------+  +-----------+  +-----------+
                  |   \        |    \
                  |    \       |     +--> hass_protocol (pure: template, decode, commands)
                  v     v      v                |
              +--------------------+            |
              |  core  (pure C++)  | <----------+
              |  layout, store,    |
              |  intents, pending  |
              +--------------------+
                        ^
                        |
              +--------------------+
              | config (pure C++)  |  dashboard.json, hass.yaml -> core::Layout
              +--------------------+
```

| Component | Depends on | Pure (host build) | Responsibility |
| --- | --- | --- | --- |
| `memory` | nothing | yes | The heap for large data: `main` points it at PSRAM at start, the PC build uses malloc. `memory::Vector` and `memory::String` allocate from it (running) |
| `formats` | `memory` | yes | `JsonWriter` (the web server's answers and the stored dashboard) and the YAML subset reader of section 5 (running) |
| `core` | `memory` | yes | Domain model: `Layout` (pages, tiles, grid placement; running), `EntityStore` (live entity state; running), intents (running), pending/optimistic logic |
| `config` | `core`, `formats`, `cjson` | yes | Reads the stored dashboard (JSON) and the `hass.yaml` import with its includes into a `core::Layout`, validates it, reports errors with file and line, writes the JSON and the YAML export (section 5, "Dashboard configuration") |
| `hass_protocol` | `core`, `formats`, `cjson` | yes | The field table; builds `render_template` templates from the tiles of a page, builds `call_service` (running) / statistics / history commands, decodes messages into core types |
| `hass` | `hass_protocol`, `esp_websocket_client`, `esp_http_client`, `cjson` | no | The session task: connect, auth, reconnect, ping, request ids, subscriptions, chunked statistics, camera fetch |
| `ui` | `core`, `lvgl` | no | Screens, tile views, panels, theme, fonts. Turns touches into intents, store changes into widget updates |
| `board` | `esp_lcd`, panel and touch drivers | no | Brings up display, touch and backlight for the selected board, returns `esp_lcd` handles. No LVGL, so the recovery app can use it too |
| `platform` | `littlefs`, `esp_sntp`, `nvs_flash` | no | LittleFS mount, NVS, time sync, log setup |
| `network` | `esp_wifi`, `esp_netif` | no | Station and access point state machine, scans, stored credentials (section 9) |
| `webserver` | `esp_http_server`, `network` | no | Captive portal and the setup pages for the phone path (section 9) |
| `main` | everything | no | `app_main`: start order, mailboxes, error policy at boot |

Rules:

1. `memory`, `formats`, `core`, `config` and `hass_protocol` include no ESP-IDF, FreeRTOS or
   LVGL header. They are compiled by the host build, which proves it.
2. `ui` never talks to the network. It emits intents. `hass` never touches LVGL. It emits events.
3. `main` is the only place that knows every component. It contains wiring, not logic.
4. Undocumented Home Assistant commands (`render_template`, `recorder/statistics_during_period`,
   `history/history_during_period`) appear **only** in `hass_protocol`. A breaking change in
   Home Assistant touches that component and its tests.

---

## 4. Tasks, ownership and data flow

Two actors own all state. Everything between them is a message.

| Task | Created by | Owns |
| --- | --- | --- |
| **UI task** | `esp_lvgl_port` | LVGL, all widgets, `core::EntityStore`, `core::PendingTracker`, navigation state |
| **hass task** | `hass` | the WebSocket client, request ids, subscriptions, reconnect and ping timers, statistics chunking |
| WebSocket task | `esp_websocket_client` | socket I/O only; its event handler reassembles frames and forwards complete messages to the hass task |
| media worker (later) | `hass` | camera fetch and JPEG decode, so a 50 KB image never delays a state update |

```
 touch -> [UI task] --intent--> PendingTracker --HassCommand--> mailbox --> [hass task] --> esp_websocket_client
                ^                                                                  |
                |                                                                  v
          apply to store <---- mailbox <---- HassEvent (already decoded) <---- reassemble + decode
          update widgets
```

* The **core lives in the UI task.** The UI is the only consumer of entity state, so putting the
  store there removes every lock around the model. The UI task drains its inbound mailbox from an
  `lv_timer` every 20 ms.
* **Decoding happens in the hass task.** The UI task receives typed values
  (`PageValues{subscription, std::vector<EntityUpdate>}`), never JSON. Parsing a few KB never
  blocks rendering.
* **Sending never blocks the UI.** `esp_websocket_client_send_text` takes a lock and can block up
  to its timeout on a bad link, so only the hass task calls it.
* **Mailbox** is a small typed queue (`std::mutex` + ring buffer + task notification). FreeRTOS
  queues copy raw bytes, which is wrong for `std::variant` with strings and vectors in it.

```cpp
// ui -> hass
using HassCommand = std::variant<SubscribePage, CallService, FetchStatistics, FetchHistory,
                                 FetchImage, Resync>;
// hass -> ui
using HassEvent   = std::variant<ConnectionChanged, PageValues, ServiceFailed,
                                 StatisticsResult, ImageReady>;
```

### Stale results

Every subscription carries a generation number. When the visible page changes, the UI sends a new
`SubscribePage` with a new generation. The hass task unsubscribes the old template and stamps every
`PageValues` with the generation it belongs to. The UI drops results of an older generation. This
replaces the page stamp of the old client and covers the race where a result for the previous page
arrives after navigation.

---

## 5. The domain model (`core`)

The old model stored the live entity state inside the tile config (`Tile::value`) and kept the
pending flags in a parallel vector in the dashboard. Two tiles showing the same entity held two
copies. The new model separates three things with different lifetimes:

| Type | Lifetime | Content |
| --- | --- | --- |
| `Layout` | immutable after a config load | pages, grid per page, tiles with type, icon, name, position, ranges. Entities are interned: a tile holds an `EntityRef` (index), not a string |
| `EntityStore` | as long as the layout | one `EntityState` per interned entity: state text, numeric value, the projected attributes, last update. Change notification per entity |
| `PendingTracker` | per action | optimistic value, expected state, deadline. Resolves when the store reports the expected state, reverts on timeout or `ServiceFailed` |

A config reload builds a new `Layout` and `EntityStore` and replaces both. The UI rebuilds its
screens. Nothing patches the old objects in place.

### Tile type descriptors

Each tile type is described in one table entry, and every layer reads that entry:

```cpp
// core: what a tile can show
enum class Field : uint8_t { State, Rounded, Name, Icon, DeviceClass, Unit, Brightness, ... };

// hass_protocol: one entry per field
struct FieldSpec {
    Field       field;
    const char *code;       // the key in the result: "b"
    const char *attribute;  // "brightness"; nullptr for the state itself
    Kind        kind;       // text, number, list, pair
};
```

* `hass_protocol::fields_of(tile, panel)` says which fields a tile needs: the state always, the
  `friendly_name` only without a name of the tile, icon and device class only without an icon,
  the brightness only for a `dimmer`, the panel fields only while the panel is open.
* `hass_protocol` builds the template of the visible page from it, every value as
  `state_attr(e,'<attribute>')|tojson`, and decodes a result back into `EntityUpdate`s, from the
  same table.
* `ui` picks the tile view for the kind.

The old code kept the template in the client and the response mapping in the dashboard, two lists
that had to stay in sync by hand. With one table they cannot drift.

### Compact template results

Keys in the rendered JSON are the entity's position in the subscription and a one or two letter
field code, not entity ids and attribute names:

```json
{"0":{"s":"on","b":180},"1":{"s":"heat","t":21.5,"c":20.8,"a":"heating"}}
```

That keeps a full page well under one TCP segment for typical layouts, which matters for the
reassembly path described in section 6.

### Intents

The UI turns gestures into intents: `Toggle`, `Press`, `SetLevel`, `SetTemperature`,
`SetHvacMode`, `SetPreset`, `SetFanMode`, `SetEffect`, `SetColor`, `SetColorTemp`. Each holds an
`EntityRef` plus its value. `PendingTracker` records the optimistic value and `hass_protocol` turns
the intent into a `call_service` message. Gesture rules (double tap window, drag release) stay in
`ui`. What an intent means stays in `core`.

### Dashboard configuration

Decided (2026-10-07):

* The configuration holds only the dashboard: a `defaults` section and the `tiles`. `defaults`
  holds the grid of the document for now; more defaults may follow. The connection values
  (URL, token) are settings, not part of the dashboard; the YAML import accepts a `hass:` block
  for them (below). There is no compatibility with the file of the old firmware (decided
  2026-10-07): the project is in development, so one format only. `docs/dashboard-sample/` is the
  guide to it and a test file.
* The device does not have to keep the configuration as YAML. The web UI is the editor for areas
  and tiles. The screen allows only a few settings; moving tiles comes later. YAML is the import and
  export format for advanced users, and the import accepts includes.
* No reload by file size. A change made in the web UI, on the screen or by an import posts
  `ReloadConfig` to the UI mailbox.
* The full resync of the visible page is a fixed 60 seconds. After an error or a reconnect the
  page is subscribed again immediately.

Storage (decided 2026-10-07):

* One JSON file on LittleFS, `/fs/dashboard.json`, written atomically (temporary file, then
  rename). The web UI reads and writes it as it is, and the file is readable in `files.zip`.
  Reading uses cJSON, which ESP-IDF 6 no longer bundles: `espressif/cjson` from the registry is
  already in the main firmware for `hass` (about 5 KB of flash, tree in PSRAM). Writing uses the
  web server's `JsonWriter`, moved to a shared place. The dashboard API reaches `config` through
  a function table filled in `main`, like `HassAccess`, so the recovery app links no cJSON.
* The file carries a format version and a revision. Every tile gets a stable id, created by the
  device, so the web UI and the screen can name a tile. A write from the web UI sends the revision
  it read and gets `409` when the screen changed the dashboard in between.
* JSON and YAML both end in a `core::Layout`, and one validator checks it. A layout that fails is
  never stored, and the running dashboard stays.

YAML import and export (decided 2026-10-07, kept simple):

* The import runs on the device, in `config`, with a parser written new for this project. The
  old parser was AI-generated; it is a reference for the behavior and the error messages, not
  code to port. Errors name the file, the line and the include chain
  (`rooms/office.yaml:12: unknown tile key 'nmae' (included from rooms.yaml:9)`).
* All files are uploaded as one set (folder picker in the web UI, or the tool). One file at a
  time cannot work: the main file names files that have not arrived yet. A "check only" option
  validates without storing.
* An include is an entry of any tile list. The included tiles go where the entry stands, so the
  order of the files decides the placement as before:

  ```yaml
  # hass.yaml
  defaults:
    grid: 4x2
  tiles:
    - type: climate
      entity: climate.living_room
    - include: rooms.yaml
  ```

  ```yaml
  # rooms.yaml: a plain list that starts at column 0, however deep it is included
  - type: area
    name: Rooms
    grid: 4x3
    tiles:
      - include: rooms/office.yaml
  ```

* Paths are relative to the including file and cannot leave the uploaded set. Includes nest up to
  4 levels; a cycle is an error. No globs (the order would come from the file names) and no
  variables in the first version.
* The parser accepts any consistent indentation and any key order.
* A `hass:` block is accepted: `url` and `token` go to the settings, like a save on the Home
  Assistant page. A file without a token is the preferred way (the stored token stays). The
  import does not read the block itself: it hands the keys it does not know to their owners
  (`hass`, `network`, `display` of the backup file).
* The export dumps everything into one file, for backup: `hass.url` without the token, and
  without the comments of the import. Splitting it into files is up to the user. It is the same
  file as the settings backup (decided): optional sections `hass`, `network`, `display`,
  `defaults` and `tiles`, and the export offers settings, dashboard or both.

Tile format, decided (2026-10-07):

* The kind of a tile follows from the domain of its entity. `type` stays only for the tiles
  without an entity, `area` and `spacer`. The `dimmer` type is gone: a light tile has the flag
  `dimmer`, which shows the level fill, or it acts as a simple switch.
* Icons: one name per glyph, no duplicates. The names are the MDI names (`lightbulb`,
  `power-plug`).
* One notation for sizes: `cols x rows` as a string, `grid: 4x3` and `size: 1x2` (stored as
  `[4, 3]` in JSON). `position:` is dropped together with moving tiles.
* `decimals` defaults to the display precision of the entity in Home Assistant
  (`states(entity, rounded=True)`), the key overrides it.
* The web UI reads the entity from Home Assistant when a tile is created and fills in everything
  (name, icon, dimmer flag, size).

Tile format, details (decided 2026-10-07):

* Domains:

  | Domain | Tile | Tap |
  | --- | --- | --- |
  | `light` | without `dimmer`: 1x1 switch; with `dimmer: true`: level fill, 1x2 (1x1 with `size`) | toggle; with `dimmer`: drag sets the level, double tap toggles, tap opens the light panel |
  | `switch`, `input_boolean`, `fan`, `automation` | switch, 1x1 | toggle |
  | `button`, `input_button`, `scene`, `script` | button, 1x1 | press, `turn_on` for scene and script |
  | `climate` | climate, 1x2 (1x1 with `size`) | as before |
  | `camera` | picture, 2x2 (`size` 2x1 or 1x1) | as before |
  | `sensor`, `binary_sensor`, every other domain | sensor, 1x1 (read only) | opens the panel with the history |

* `type:` exists only for `area` and `spacer`; on any other tile it is an error.
* Icon order, the same as in the Home Assistant frontend or as close as the glyph subset
  allows: the `icon` of the tile; the entity's `icon`
  attribute (set by the user in HA, or by older integrations) when the glyph is in the firmware;
  domain and `device_class`, with the off glyph for on/off entities; the domain. The icons of
  integrations (`icons.json`) are not part of the state and are not used. The glyph subset grows
  to the HA defaults of these domains and device classes; its flash cost is measured for both
  icon sizes before it is fixed.
* A tile without a name (YAML import) shows the entity's `friendly_name`.
* Climate step: the entity's `target_temp_step` (a capability attribute in the state, only there
  when the integration sets it, for example `generic_thermostat` with 0.1, 0.5 or 1), else the
  `step` of the tile (0.1 or more), else 0.5 for °C and 1 for °F as in the HA frontend.
* Light step: `step` of the tile for the `+`/`-` buttons of the light panel, default 1 %,
  1 to 50. The slider stays continuous. `+`/`-` repeat while held, in the light and the climate
  panel. `min`/`max` of lights are dropped.

Orientation (decided 2026-10-07):

* As in the old firmware: the tiles fill the grid first-fit in list order in both orientations,
  and the portrait grid is the transposed one. The 180 degree turned orientations use the same
  layout as their base orientation.
* No moving of tiles for now, and no cells per orientation. A grid of its own for portrait is
  postponed as well.
* Discussed and postponed: a fixed cell per tile and orientation (`landscape:` / `portrait:`),
  placed before the tiles of the automatic order, and a move on the screen that asks "both
  orientations" or "only this orientation".

---

## 6. Home Assistant connection (`hass`, `hass_protocol`)

What is kept from the old client, because it works:

* one WebSocket connection, authenticated with a long-lived token;
* `render_template` per visible page, re-subscribed on page change, with the panel's detail fields
  added while a panel is open. Home Assistant only sends a result when the rendered text changed;
* `call_service` over the same connection, with failures reported per entity;
* statistics and history in 6 hour chunks, one request at a time;
* a full resync every `hass.poll` seconds as a safety net;
* an application-level ping and a silence timeout to detect half-open connections.

What changes:

* Transport is `esp_websocket_client` (TLS through `esp-tls`, optionally with the certificate
  bundle) instead of a hand-written framer on `WiFiClient`.
* The client delivers a large frame in several data events, and a single read can also return a
  partial frame when the next TCP segment or TLS record has not arrived yet. The event handler
  reassembles by `payload_offset` / `payload_len` / `fin` into a PSRAM buffer with a hard size cap,
  and forwards only complete messages.
* The old notes record a stall after about 8 KB of a single message. That was observed on the
  Arduino stack. Retest it on ESP-IDF; keep the 6 hour chunks either way, they cost nothing.
* JSON is parsed with cJSON from the component registry (ESP-IDF 6 no longer bundles it),
  allocating from PSRAM through `cJSON_InitHooks`. Messages are capped at 16 KB.
* Camera images use `esp_http_client` and a JPEG decoder (`esp_new_jpeg` in software, the
  hardware JPEG driver on the P4), in the media worker.

---

## 7. UI (`ui`)

* **Screens** are classes with an explicit lifecycle: `create(parent)`, `apply(changes)`,
  `destroy()`. Navigation keeps a stack: main page, area pages, panels, image viewer.
* **Tile views**: one class per tile type, one file each, behind a small interface
  (`create`, `update(const EntityState&, PendingState)`, gesture callbacks). The old screen was a
  single 5,500 line file.
* **Panels**: climate, light, sensor graph, each its own screen class.
* **Theme**: the color and font constants of `behavior.md` section 8 in one header. Fonts and
  Material Design Icons are generated with `lv_font_conv` for each supported LVGL version.
* **Updates are incremental**: the store reports which entities changed, and only their tile
  views update. Nothing rebuilds the grid because a value changed.

### 7.1 LVGL 8 and 9

Decision: LVGL 8.4 first, because the UI code being ported is LVGL 8. LVGL 9 follows as a
separate step. When LVGL 9 works well on the target boards, LVGL 8 support is removed. Both
versions are only built side by side during that transition.

What is known about performance so far:

| Source | Finding |
| --- | --- |
| [lvgl#5459](https://github.com/lvgl/lvgl/issues/5459) | about 37 % slower on an ESP32-S3 going from 8.3.9 to 9.0.0 |
| [esp_lvgl_port performance notes](https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/docs/performance.md) | ESP32-S3 RGB panel: LVGL 8 at 12 / 16 FPS (default / optimized), LVGL 9.2 at 9 / 11 FPS. The two benchmark demos measure FPS differently |
| [LVGL 9.6 release notes](https://lvgl.io/blog/release-v9-6) | rewritten software blend and transform paths: ESP32-S3-Korvo-1 4 % less render time, ESP32-P4 rotated images 33 ms down to 11 ms. No comparison with LVGL 8 |

The LVGL 9 step measures the dashboard's own screens on both versions. On the WT32-SC01 the SPI
transfer (30.7 ms per full screen) may hide render differences once render and DMA overlap.

Rules that keep the later switch cheap:

* No widget that exists in only one version. The light panel's color wheel is a custom widget,
  because `lv_colorwheel` exists only in LVGL 8. The same applies to `lv_meter` (8 only) and
  `lv_scale` (9 only).
* Byte swapping for SPI panels: with LVGL 8 it is `CONFIG_LV_COLOR_16_SWAP` in the board's
  defaults file, with LVGL 9 `esp_lvgl_port`'s `swap_bytes` flag, which esp_lvgl_port 2.9.0 only
  offers for LVGL 9. The board states the need (`Info::swap_bytes`) and `main` checks that the
  configuration agrees.
* LVGL calls that differ between the versions go through `ui/lvgl_compat.hpp`, the only file
  allowed to test the LVGL version. It is deleted together with LVGL 8 support.

---

## 8. Boards (`board`)

```cpp
namespace board {
struct Handles { esp_lcd_panel_handle_t panel; esp_lcd_touch_handle_t touch; };
esp_err_t init(Handles &out);          // panel, touch, backlight; no LVGL
void      set_backlight(uint8_t percent);
const Info &info();                    // name, resolution, has_touch, psram
}
```

* `board` knows nothing about LVGL. It brings up the panel and touch with `esp_lcd` and returns
  their handles. In the main firmware, `main` hands them to `esp_lvgl_port`, so `ui` stays plain
  LVGL, without ESP-IDF drivers. The recovery app draws on the panel directly
  (section 9.5). Both share every board driver.

* Adding a board, with every check and calibration: [adding-a-board.md](adding-a-board.md).
* One directory per board under `components/board/boards/<name>/`, compiled only when its
  Kconfig option is selected (`if(CONFIG_HC_BOARD_WT32_SC01)` in the component's CMake).
  No `#ifdef` chains inside shared sources.
* Pins and panel parameters live in the board's own source file, not in Kconfig.
* The backlight is PWM through the LEDC peripheral at 25 kHz (decided: 1, 5 and 25 kHz look the
  same on the WT32-SC01, so the highest one stays). Brightness in percent goes through a
  perceptual curve, the inverse of the CIE lightness (decided: it feels smooth down to the
  lowest levels, a linear curve gets choppy there).
* **Minimum brightness** (decided): the lowest PWM duty at which the screen is still readable is
  a calibration of each device, like the orientation sensor. 1 % is that minimum and 100 % is
  full brightness, so no setting can make the screen unreadable. It is found with the backlight
  tab of the test screen and later with the calibration in Settings, System; the screen returns
  to the normal brightness after 10 seconds without a touch while the test runs. Stored in NVS
  with the other calibration values (step 3).
* `board` also owns the shared buses, such as the I2C bus of the touch controller, and hands out
  their handles, so a sensor on the same bus does not open it a second time.

### 8.1 Orientation

* Landscape and portrait in the first version, on every board (decided).
* Where an MPU6050 accelerometer is connected, the screen rotates with the device (decided).
  Its wiring is documented on the board's device page.
* The `orientation` component reads the MPU6050 over the board's I2C bus with the defaults of the
  old firmware, which needed no calibration (decided): a sample every 100 ms, a new orientation
  is taken over when the accelerometer reading is steady (it changes less than 0.05 g between
  two samples; the gyroscope is not used, a module can read 12 deg/s at rest) and the orientation
  held for 500 ms.
* Only the part of gravity in the panel plane decides the orientation, from about 15 degrees
  away from lying flat. The old firmware treated the panel as flat as soon as the part into the
  panel was the largest, so a panel leaning back on a desk got no orientation at boot and
  started upside down (seen on the WT32-SC01: gravity x 0.66, z 0.76, about 49 degrees from
  upright). Near a diagonal one axis has to win by 30 %, so the screen does not flip. Until the UI mailbox exists, `main` applies the rotation under the LVGL lock; later it
  posts `OrientationChanged` to the UI mailbox.
* Proposal: SPI panels rotate in the panel controller (`esp_lcd_panel_swap_xy` and
  `esp_lcd_panel_mirror`), which costs nothing at runtime, and the touch coordinates rotate
  with the same flags of `esp_lcd_touch`. RGB and MIPI-DSI panels rotate in software, on the
  ESP32-P4 with its pixel processing accelerator.
* Developer calibration (decided: done when the board is added, never on the device or by the
  user): two captures instead of fixed axis settings. `orient capture 0`: the panel exactly
  upright in the base orientation. `orient capture 90`: turned 90 degrees clockwise, still
  exactly upright. The two gravity vectors, in the module's coordinates, go into the board info
  (`imu_axes`). The measurement takes five poses (0, 90, 180, 270 and flat, screen up): opposite
  poses cancel the offset of the module, which can be several tenths of a g (the module of the
  replaced WT32 reads 1.55 g standing still), so the board info also has `imu_offset`, taken off
  every reading before the orientation is decided. A reading is then the one of +g0, +g1, -g0, -g1 it points along most (largest dot
  product), which gives 0, 90, 180 and 270 degrees. That covers every mounting, also a module
  that sits at an angle to the panel. Check: the two captures have to be 85 to 95 degrees
  apart (only then the panel stood upright), otherwise the measurement is rejected. The tilt of
  the module against the panel is printed; what to do with it (for example a lock near flat) is
  the developer's decision. Without `imu_axes` the fixed mapping of the old firmware applies.
* The orientation task runs at priority 6, above LVGL (4), `hass` (4) and `network` (5). At
  priority 2 it was starved for up to a second while the screen redrew, which showed up as random
  I2C `ESP_ERR_TIMEOUT`s (about one in four seconds) and stale samples. The module is read at
  400 kHz and a timed out read is repeated twice (decided 2026-10-07).
* The UI rebuilds the current screen for the new size. The dashboard uses the transposed grid of
  the old firmware, so a page reads in the same order in both orientations (section 5,
  "Dashboard configuration").

| Board | MCU | Display | Touch | Drivers (component registry) |
| --- | --- | --- | --- | --- |
| WT32-SC01 (first target) | ESP32, 4 MB flash, 4 MB PSRAM | 480x320 ST7796S, SPI, write only | FT6336U | `esp_lcd_st7796`, `esp_lcd_touch_ft5x06` |
| JC3248W535 | ESP32-S3, 16 MB, 8 MB PSRAM | 320x480 AXS15231B, QSPI, whole frames only | integrated | `esp_lcd_axs15231b` |
| JC4880P443C | ESP32-P4 + C6, 16 MB, 32 MB PSRAM | 480x800 ST7701S, MIPI-DSI | GT911 | `esp_lcd_st7701`, `esp_lcd_touch_gt911`, `esp_wifi_remote` + `esp_hosted` |
| TTGO T4 (test device, not fully supported) | ESP32, 4 MB, PSRAM | 240x320 ILI9341, SPI | none | `esp_lcd_ili9341` |

Display flushing on the WT32-SC01: the old firmware rendered into two full-screen buffers in
PSRAM and pushed them with blocking writes, because SPI DMA on the classic ESP32 cannot read
PSRAM. The new setup uses two partial buffers in internal DMA-capable RAM (about 40 lines each),
so LVGL renders the next band while DMA sends the previous one. Measure it against the profiling
report of the old firmware: the 30.7 ms wire time of a full screen at 80 MHz stays the floor.

The AXS15231B of the JC3248W535 takes no row address over QSPI (a write starts at the first row
or continues where the last one ended) and cannot rotate a frame. The board sets
`Info::full_frames`, and `main` uses `frame_display` instead of esp_lvgl_port's display: LVGL
renders in direct mode into one frame in PSRAM, and after each refresh the frame is sent from the
top down to the lowest changed row, in bands that are turned to the panel's orientation while
they are copied into two internal DMA buffers (copying one overlaps sending the other). Rotation
is software for this board; touch still comes from esp_lvgl_port.

The ST7701S of the JC4880P443C is a MIPI-DSI video panel: the DPI controller scans a frame buffer
in PSRAM and nothing can rotate it. The board sets `Info::mipi_dsi`, and `main` uses
esp_lvgl_port's DSI display with two 50-line draw buffers in internal RAM, which the CPU copies
into the frame buffer (the DMA2D corrupted memory under a full-screen camera stream); LVGL rotates in software. The ESP32-P4 has no radio: Wi-Fi runs on the
ESP32-C6 of the board, and `network` gets the `esp_wifi` API from `esp_wifi_remote` over
`esp_hosted` (SDIO) without code changes. The device name comes from the P4's base MAC.
The C6 runs an esp_hosted coprocessor firmware of the same version as the host side, built
from `boards/jc4880p443c/c6` and sent to it over SDIO (`hc::network::update_coprocessor`,
console `c6 update <url>`); a different major version on the two chips is a mismatch.

---

## 9. Setup, settings and platform services

### 9.1 Setup and network (decided)

* No BLE. A device is set up from its touch screen or from a phone through the device's access
  point (AP) with a captive portal. First setup offers both at the same time.
* **AP password**: generated randomly on the first boot, 8 digits and 2 lowercase letters, stored
  in NVS. It stays the same until a factory reset or until the user generates a new one in
  Settings. It is shown on the screen, together with a QR code to join the AP, whenever the AP
  is active.
* **First setup**: AP and station run at the same time. The screen offers the network list,
  manual network name entry, the Wi-Fi password, and the username and password of the web UI.
  The phone path offers the same through the captive portal. The setup page has an option to
  keep the AP on afterwards.
* **After setup**: the AP stays on for a while with the same password, so the phone can see the
  result (proposal: 15 minutes), then turns off unless the user chose to keep it on or turns it
  on later in Settings.
* **Touch is a requirement** for every supported board (decided). So the AP never opens by
  itself when the Wi-Fi fails; the user turns it on in Settings. Recovery without a PIN goes
  through Settings on the screen (reset the web UI login, factory reset); no hardware fallback
  (decided).
* **Wi-Fi errors** are shown in the status bar at the top of the screen.
* **Settings, Wi-Fi page**: no PIN, reachable at any time. Select or change the network,
  reconnect, turn the AP on or off, generate a new AP password.
* **Up to 5 networks**: the first one comes from the setup, more are added in the advanced
  Wi-Fi settings.
* **Home Assistant URL and token** are entered in the web UI, which requires the username and
  password from the first setup. The screen shows the connection status.
* **QR codes** on the screen, scanned with the phone's camera: one in the standard Wi-Fi format
  to join the AP, and one with the device's address to open the web UI. Both are drawn dark on a
  light card with a quiet zone, also in the dark theme.
* **AP and station run at the same time** whenever the AP is on. A phone on the AP may lose its
  connection for a moment when the station connects or changes channel. That is accepted for
  the settings web site.
* The network pages of the first setup are the same pages as the Wi-Fi part of Settings.
* Naming: **Settings** is the settings dialog. Its first page is the quick settings page. On
  screen and in the web UI the two Wi-Fi roles are **Wi-Fi** (station) and **Hotspot** (AP).
  Code and developer docs use the ESP-IDF terms AP and STA.
* Screen and web layouts: [ui/screen.md](ui/screen.md) and [ui/web.md](ui/web.md).

### 9.2 Open points of the setup design

* **Fast reconnect** (decided): each network stores the access point (BSSID) and channel of its
  last connection. Only when waking from deep sleep the device goes straight to the network of
  the last connection without a scan (decided); only when that fails it scans. A normal boot and
  a lost connection always scan, so the network order is followed, and the 30 s signal check
  handles moving around. DHCP skips the ARP probe
  of the offered address and asks for the last address again. Measured on the T4 with a WPA3
  network: 680 to 840 ms from the start to the IP address, 3.4 s with a scan. About 500 ms of
  it is the WPA3 handshake (SAE) on the ESP32.
* A router with WPA3 and protected management frames keeps the connection of a device that
  was reset (button, power) for a few seconds and does not answer its new login in time
  (reason 2). The first retry comes after 1 s for that reason, and it goes to the same network:
  reason 2 does not count as a failed attempt the first time in a round. Without that, a power cut moved the device to the next saved
  network for good (T4, 2026-10-07: 9 of 10 hard resets got reason 2 at the first attempt, the
  second attempt always worked). A network saved without a password makes the driver look for an
  open network; against a secured one that ends in reason 210, shown as "the network needs a
  password, add it again". Passwords of 1 to 7 characters are refused when a network is saved.
* Wi-Fi power save is off for the panels, which run on mains power: with modem sleep every
  packet waits for the next beacon (ping 107 ms average, single requests up to seconds on the
  T4), without it 19 ms. A battery remote will need its own setting.
* Open: the signal check interval for a device that moves (remote) versus a stationary one;
  needs tests with a real remote.
* Open: slowing down reconnect scans while a phone is connected to the hotspot. With the fast
  reconnect most reconnects need no scan any more.
* Network selection with up to 5 saved networks, with the panel also used as a portable
  remote on a magnetic wall mount in mind:
  * **Two policies, chosen in the Wi-Fi settings** (decided): the entered order, or the strongest
    signal. With the strongest signal, the networks are sorted by signal at runtime and that order
    is never saved. The entered order is never changed automatically.
  * **On connect** (boot, or after the connection is lost): one scan. With the entered order, take
    the first saved network in range whose signal is usable; if none is usable, the strongest.
    The display shows that it is scanning and connecting, so a few extra seconds are fine
    (decided).
  * **While connected**: no periodic switching, because a switch drops the Home Assistant
    connection for a few seconds. Only when the signal stays weak for 30 seconds (decided) does a
    background scan run, and the device switches only to a clearly better network.
  * **Thresholds**, adjusted later by measuring throughput against signal strength (decided).
    Starting point taken from Apple's published roaming behavior for iPhone and iPad: roam when
    the signal is weaker than -70 dBm, and only to an access point at least 8 dB stronger
    ([Apple: Wi-Fi roaming support](https://support.apple.com/102002)). The panel needs far less
    throughput than a phone, so the trigger may end up lower.
  * **Same network, several access points** (mesh, repeaters): the connect picks the strongest
    access point (`WIFI_CONNECT_AP_BY_SIGNAL` with a full channel scan), and 802.11k/v, which
    ESP-IDF supports, helps the router steer the device while connected.

### 9.3 Storage

* **LittleFS** for files (decided). It is power-loss safe and does its own wear leveling. SPIFFS
  is in maintenance mode, and FAT with wear leveling is not power-loss safe; FAT stays only for
  SD cards.
* A LittleFS that fails to mount is damaged and never formatted automatically (decided), so its
  readable files can be exported first. A partition that holds no LittleFS at all, recognized by
  the missing "littlefs" superblock, holds nothing to lose and is formatted: a new device, or
  leftovers of another firmware (seen on the T4).
* **NVS** for secrets and small values: Wi-Fi passwords, the AP password, the web UI password
  hash and the Home Assistant token are never stored in a file that the web UI can download.
* The Wi-Fi driver's own NVS copy of the station config is turned off
  (`esp_wifi_set_storage(WIFI_STORAGE_RAM)`), so the `network` component is the only owner of
  the stored networks.
* **Settings live in NVS** (decided). Live controls such as sliders are written with a delay,
  forms with an OK or Save button are written on save. Values that change many times per second
  while a slider moves are discussed later.
* **Files on LittleFS** hold the web UI files (decided) and the dashboard configuration
  (`dashboard.json`, proposal in section 5). The dashboard is edited in the web UI or imported
  as YAML. There is no file manager (decided).
* **Export and import** of the settings are actions in the web UI, as YAML like the dashboard,
  without secrets; an import may carry passwords added by hand. There is no automatic export
  file (decided).
* **Logs** are viewable in the web UI, with a debug mode that adds notice and info messages to the
  normal error log (decided; details later).

### 9.4 Components

| Component | First version | Responsibility |
| --- | --- | --- |
| `platform` | yes | boot log, both NVS partitions, LittleFS at `/fs`, the settings class for the settings NVS, the device name (`hassctrl-` and the last two MAC bytes). SNTP and log levels later |
| `network` | yes | station and AP state machine of sections 9.1 and 9.2, up to 5 stored networks, the hotspot password, scan results, stored in the settings NVS. One task owns the driver and the state; commands are queued, changes go to a listener (later the UI mailbox), readers get copies. The network choice is plain C++ (`selection.cpp`) for host tests. mDNS (`espressif/mdns`): the device answers as `<device name>.local` and announces `_http._tcp`, in both apps |
| `webserver` | yes | captive portal DNS and redirect (running), the status API and the built-in page (running), the setup pages for the phone path, login with the web UI username and password, Home Assistant URL and token, the backup export and import (one YAML file; the sections come from a table filled in `main`, each with write, check and apply, so the web server depends on none of their owners), logs. Serves the web files from LittleFS, with a recovery page compiled in that works without the file system ([ui/web.md](ui/web.md) section 7). Later: the dashboard API (read and write the layout, YAML import and export), which posts `ReloadConfig` to the UI mailbox instead of the old "file size changed" polling |
| `ui` | yes | plain LVGL, no ESP-IDF drivers. Settings (quick settings, Wi-Fi, hotspot, Home Assistant, display, system; running), the pull-down zone and the alert strip on `lv_layer_top` (running), the dashboard later. Everything it needs from the firmware comes through `hc::ui::Backend`, a table of functions that `main/ui_backend.cpp` fills from `network`, `board`, `platform` and `webserver`. The first setup is a sequence of the same pages |
| `light_sensor` | yes, optional per device | BH1750 ambient light on the board's I2C bus (looked for at 0x23 and 0x5C), averaged over a second, for the automatic brightness of `ui` (running on the WT32-SC01) |
| `hass` | yes | the Home Assistant connection of section 6 (running, step 5 first part): URL and token in the settings NVS, one WebSocket (`espressif/esp_websocket_client`, TLS with the CA bundle), authentication, the API's ping every 25 s, 70 s silence timeout, reconnect 2 s doubling to 30 s, a rejected token is not retried. One task owns the client; the client's event handler only puts messages together (PSRAM, 16 KB cap) and queues them. The web server and the UI get its status through function tables filled in `main` (`webserver::HassAccess`, `ui::Backend::hass_info`), so the recovery app links neither it nor cJSON. URL parsing (`url.cpp`) is plain C++ for host tests |
| `ota` | yes | firmware and file system images from an upload, a tool or a URL download, app rollback ([ui/web.md](ui/web.md) section 8), restart into the recovery app and back, partition layout of section 9.5 (running) |
| `devconsole` | yes, removable | serial command line for development (`CONFIG_HC_DEV_CONSOLE`): Wi-Fi networks, hotspot, file system, restart, factory reset. Used until the setup screens exist |
| `uidebug` | yes, removable | screenshots of the screen and `lv_layer_top` as run-length encoded RGB565 over the serial console (`tools/screenshot.py`), and a virtual pointer as a second `lv_indev` for `tap`, `press`, `release` and `swipe` in `devconsole`. Over the network later |

### 9.5 Partition layout and the recovery app (proposal)

Two equal OTA partitions on a 4 MB board leave about 1.9 MB per firmware image. The proposal is a
second, small firmware: the **recovery app**. It is the ESP-IDF factory app, built from the same
repository and the same components (`platform`, `network`, `webserver`, `ota`), without LVGL and
without Home Assistant. It reads the same NVS, so Wi-Fi, hotspot and web UI login are the same as
in the main firmware.

| Partition | 4 MB boards | 16 MB boards |
| --- | --- | --- |
| nvs, otadata, phy | 36 KB | 36 KB |
| factory: recovery app | 1 MB | 1 MB |
| ota_0: main firmware | about 2.4 MB | 6 MB |
| ota_1: main firmware | none | 6 MB |
| LittleFS | about 512 KB | the rest |

* **Update on a 4 MB board**: the main firmware cannot overwrite itself, so it reboots into the
  recovery app, which receives the new image, writes it to ota_0 and boots it. On a 16 MB board
  the main firmware writes the other OTA partition directly. `esp_ota_get_next_update_partition()`
  tells the two cases apart, so the update code is the same.
* **Recovery for free**: when the main firmware is broken, does not confirm itself after an update,
  or an update was interrupted by a power loss, the bootloader starts the recovery app. Its web
  page is the recovery page of [ui/web.md](ui/web.md) section 7, so there is one recovery page
  for both cases.
* **Cost**: during an update on a 4 MB board the old firmware is gone once writing starts. If the
  upload fails, the device stays in the recovery app until a good image arrives. A second
  application is built and tested with every release.
* **Only one app runs at a time.** The recovery app is a separate firmware, so the LVGL of the
  main firmware is not running while it runs. It recovers from three situations: an update on a
  4 MB board (the main firmware is being replaced), a broken main firmware (crash loop, failed
  update, interrupted update), and a request from the user. A broken file system alone does not
  need it: the main firmware handles that with its own recovery page.
* **Screen in the recovery app** (decided: small, nothing fancy): no LVGL, which would cost a few hundred KB of the
  1 MB. The app uses the same `board` drivers and draws directly on the panel: a small bitmap font
  for a few status lines and a QR code with the address of its web page, drawn with Espressif's
  `qrcode` component. For example "Recovery mode", "Updating 42 %", "Open http://192.168.1.57".
* **Alternative**: compressed or delta OTA updates exist for ESP-IDF, but they need a custom
  bootloader or a patch component. The recovery app is simpler and also covers the recovery case.

---

## 10. Repository layout

```
esp32-hassctrl/
  CMakeLists.txt                 ESP-IDF project
  sdkconfig.defaults             shared Kconfig defaults
  partitions/                    4mb.csv, 16mb.csv
  boards/<name>/sdkconfig.defaults   target, flash, PSRAM, board choice
  boards/jc4880p443c/c6/         ESP-IDF project of the board's ESP32-C6 (esp_hosted coprocessor)
  main/                          app_main, wiring only
  recovery/                      the recovery app (section 9.5): its own app_main, same components
  components/
    memory/      include/hc/memory/*.hpp src/
    formats/     include/hc/formats/*.hpp src/
    core/        include/hc/core/*.hpp   src/
    config/      include/hc/config/*.hpp src/
    hass_protocol/ ...
    hass/        ...
    ui/          screens/ tiles/ panels/ settings/ theme/ fonts/
    board/       include/hc/board.hpp  boards/<name>/
    platform/    ...
    network/     ...
    webserver/   ...
  host/
    CMakeLists.txt  the host build (MSVC), python tools/host_test.py builds and runs it
    tests/       unit tests for formats, core, config, hass_protocol; data/ holds sample files
    sim/         the Windows version of the UI (section 1): its own CMake project (build/sim), the
                 stub Backend with the scenarios, lv_conf.h, the Windows driver; python tools/sim.py
  tools/build.py, host_test.py, sim.py, msvc.py (finds MSVC, cmake, ninja for the last two)
  docs/
    architecture.md              this file
    ui/                          screen and web UI layouts, design system (see ui/README.md)
    adding-a-board.md            bring-up procedure for a new board
    hass_config.md               user reference of the hass.yaml import (carried over, updated)
    behavior.md                  tile behavior spec (carried over)
    devices/<board>/             pins and datasheets
```

---

## 11. Conventions

* C++23 as supported by the ESP-IDF toolchain. No exceptions, no RTTI. Errors cross ESP-IDF
  boundaries as `esp_err_t` and pure code returns `std::expected<T, Error>`.
* One namespace per component (`hc::core`, `hc::hass`, ...). Public headers in
  `include/hc/<component>/`, everything else private to the component.
* Files stay small. A file over about 600 lines is a design smell to discuss, not a target.
* Memory: large buffers (message reassembly, images, store) in PSRAM via `heap_caps_malloc`;
  LVGL draw buffers in internal DMA RAM. No allocation per frame in the render path.
* The dashboard lives in PSRAM: layout, entity states and the LVGL objects of the dashboard.
  Small values that are read often (flags, state bits, pending marks) sit in a separate compact
  array in internal RAM, indexed like the tiles or entities. The old firmware worked this way.
  (Decided 2026-10-07.)
* Comments explain why, not what. Behavior is specified in `docs/`, not in header comments.

---

## 12. Porting map from `kfc_fw/src/plugins/weather_station2`

| Old | New | How |
| --- | --- | --- |
| `hass_config.{h,cpp}` YAML parser and grid placement | `config`, `core::Layout` | rewrite (the old code is AI-generated): a new parser for the import with includes, relative indentation and `defaults`; the old code was a reference for behavior and error messages; host tests with `docs/dashboard-sample/` |
| `Tile::value` / `TileValue` | `core::EntityStore` | redesign: keyed by entity, not by tile |
| `hass_dashboard.{h,cpp}` | `core` (store, pending, intents) + `ui` (status line) | split |
| `hass_client.{h,cpp}` template builder | `hass_protocol` + tile descriptors | redesign: one table drives template and decoding |
| `hass_client.{h,cpp}` task, queue, images, stats | `hass` session task + media worker | rewrite on mailboxes |
| `hass_socket.{h,cpp}` framing, auth, statistics parser | `esp_websocket_client` + `hass_protocol` | replace the framing, port the parsers |
| `ws2_screen_hass.cpp` | `ui/screens`, `ui/tiles`, `ui/panels` | split by screen, tile type and panel; LVGL 9 API |
| `lvgl_ui.{h,cpp}` theme and widgets | `ui/theme`, `ui/widgets` | port |
| `docs/hass_config.md`, `docs/hass_layout/behavior.md` | `docs/` | copy, keep as the spec |

---

## 13. Implementation order

Each step ends with something that runs and is tested on hardware. No Home Assistant code is
ported before step 5.

1. **Toolchain**: ESP-IDF 6.x via EIM, empty `app_main` builds and boots on the WT32-SC01.
2. **Board**: display, touch and backlight through `esp_lvgl_port`, an LVGL 8.4 test screen.
   Measure render and flush time against the old profiling report.
3. **Platform and network**: LittleFS, NVS, the station and AP state machine, up to 5 networks,
   the AP password, the alert strip messages. The partition tables with two OTA partitions are
   fixed here, and the firmware size is watched from now on.
4. **First setup, Settings and web server**: on-screen setup pages, QR codes, captive portal, web
   UI login, the Wi-Fi part of Settings, firmware and file system update, the recovery page.
   Measure the memory cost of HTTPS. Tested end to end: factory state, setup by screen, setup by
   phone, network loss, AP on and off from Settings, an update, a broken file system.
5. **Home Assistant connection**: URL and token from the web UI, `hass` and `hass_protocol`,
   connect, authenticate, subscribe, `call_service`. Tested with logs and the status line
   before any dashboard UI exists. Host tests for template building and decoding.
   Running since 2026-10-07: URL and token, connect, authenticate, ping, reconnect (`hass`).
   Since 2026-10-07 also `hass_protocol`, the subscription of a page and `call_service`
   (tested from the console, before the dashboard screen).
6. **Dashboard**: `core` and `config` (JSON storage, YAML import and export, placement, host
   tests), then the `ui` grid, tiles, panels, graph and picture tiles, and the dashboard editor
   of the web UI.
7. **LVGL 9**: build and measure against LVGL 8.4, then remove LVGL 8 if LVGL 9 works well.
8. **More boards**, then the later components of section 9.

---

## 14. Open questions

* The dashboard configuration: what is decided and what is proposed is in section 5,
  "Dashboard configuration". Open: the details of the JSON storage.
* The open points of the setup design, listed in section 9.2.
