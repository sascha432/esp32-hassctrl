# esp32-hassctrl - architecture

A Home Assistant touch panel for ESP32 / ESP32-S3 / ESP32-P4, written in C++ on native ESP-IDF
with LVGL 8.4 (section 7.1). It runs on the WT32-SC01, the JC3248W535 and the
JC4880P443C; the TTGO T4 is a test device.

This file describes the firmware as it is built. Something that is not built says so where it
stands ("not built"). Open work is in [TODO.md](TODO.md), the history in
[CHANGELOG.md](CHANGELOG.md). The firmware began as a port of the dashboard of the
`weather_station2` plugin in `kfc_fw` (Arduino, LovyanGFX, LVGL 8.4); its behavior was the
reference, its code was not carried over.

---

## 1. Principles and non-goals

* Boundaries the build enforces: a module that must not know about LVGL or the network cannot
  include it (section 3).
* Every piece of mutable state has exactly one owning task. No shared state, no `volatile` flags
  (section 4).
* Domain logic, config parsing, template building and response decoding run on the PC as unit
  tests (`host/tests`, `tools/host_test.py`).
* Screens are designed in the Windows version of the UI (`host/sim`, `tools/sim.py`) and then
  tested on the boards. The Windows version only shows the GUI: no network and no Home
  Assistant; the entity states come from a sample backend (`components/sample_backend`, also the
  firmware's benchmark mode), and a tap or drag changes them, so pages,
  panels and controls can be clicked through. LVGL 8.4 with LVGL's Windows driver, the window
  sizes of the boards (480x320, 320x480, 480x800), a key to rotate.
* Adding a board touches one board directory and one Kconfig choice, nothing else (section 8).
* First setup and network recovery work from the touch screen and from a phone through an access
  point with a captive portal (section 9).
* A dashboard is configured in the web UI or on the screen; YAML is the import and export format
  (section 5).

Not planned: BLE provisioning, a dashboard in the web browser, a remote debug protocol, a
`media_player` entity, battery operation (deep sleep; cancelled 2026-10-11).

---

## 2. Toolchain and build

* **Native ESP-IDF with `idf.py`** (CMake + Ninja), not PlatformIO. PlatformIO lags behind
  ESP-IDF releases and needed the pioarduino fork for the P4, which is how the previous attempt
  ended up with two platforms and two LVGL versions in one matrix.
* ESP-IDF **6.1**, installed with the ESP-IDF Installation Manager (EIM): the source in
  `C:\esp\v6.1\esp-idf`, the tools in `C:\Espressif\tools`. A terminal is prepared with EIM's
  activation script `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`.
* Dependencies come from the Espressif component registry, declared in each component's
  `idf_component.yml`, locked in `dependencies.lock` (committed).
* One build directory per board and application. Board selection is a Kconfig choice set by the
  board's defaults file. `tools/build.py` passes the layers and the target; see the README for the
  commands. The layers, a later one wins: `sdkconfig.defaults`, `boards/<board>/sdkconfig.defaults`,
  `sdkconfig.<app>`, then the `sdkconfig.defaults` of a variant (section 8, "Variants") when the
  build is for one (`<board>.<variant>`, build directory `build/<board>.<variant>/<app>`).
* One ESP-IDF project builds both applications: `HC_APP=main` or `HC_APP=recovery` excludes the
  other application's top-level component (section 9.5). The version is `PROJECT_VER` in the root
  `CMakeLists.txt`; the build time is the app descriptor's `__DATE__`/`__TIME__`, which the root
  `CMakeLists.txt` recompiles with every build (the IDF alone only does it after a dependency
  changed). `platform::build_time()` formats it for the boot log, `/api/status` and the System pages.
* FreeRTOS tick 1 ms (`CONFIG_FREERTOS_HZ=1000`, `sdkconfig.main`). esp_lvgl_port sleeps at
  least 5 ms between `lv_timer_handler()` calls; at the default 100 Hz that is 0 ticks, so the
  LVGL task only yielded and starved IDLE0 (task watchdog) whenever its flush did not block, as
  with the P4's MIPI-DSI panel. Durations are converted with `portTICK_PERIOD_MS` /
  `pdMS_TO_TICKS`, never with a fixed 10 ms tick.
* Host builds (tests) are a plain CMake project in `host/` that compiles the pure
  components directly. They do not use the ESP-IDF build system at all. Compiler: MSVC of Visual
  Studio 2022, with the cmake and ninja that ESP-IDF installs.

---

## 3. Layers and components

Every module is an ESP-IDF **component** with its own `CMakeLists.txt`. `REQUIRES` in that file
is the dependency rule: if `core` does not list `lvgl`, `#include <lvgl.h>` in `core` fails to
compile. This is the main tool that keeps the architecture from eroding, by people or by AI
agents.

```
                 +-------------------------------------------+
                 |  main/   composition root (app_main)       |
                 |  creates everything, wires the components |
                 +-------------------------------------------+
                    |            |              |           |
                    v            v              v           v
              +---------+  +----------+  +-----------+  +-----------+
              |   ui    |  |   hass   |  |   board   |  | platform  |
              | LVGL    |  | WS + API |  | lcd/touch |  | wifi, fs, |
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
| `memory` | nothing | yes | The heap for large data: `main` points it at PSRAM at start, the PC build uses malloc. `memory::Vector` and `memory::String` allocate from it |
| `formats` | `memory` | yes | `JsonWriter` (the web server's answers and the stored dashboard) and the YAML subset reader of section 5 |
| `core` | `memory` | yes | Domain model: `Layout` (pages, tiles, grid placement), `EntityStore` (live entity state), intents, pending/optimistic logic (`PendingTracker`); the time zones: the IANA names with their POSIX rules (`timezones.inc`, built by `tools/build_timezones.py` from the tzdata release on PyPI) and `local_time()`, the rule evaluated in plain C++ for a clock tile with a zone of its own (the C library's TZ is one global, set to the device's zone) |
| `config` | `core`, `formats`, `cjson` | yes | Reads the stored dashboard (JSON) and the `hass.yaml` import with its includes into a `core::Layout`, validates it, reports errors with file and line, writes the JSON and the YAML export (section 5, "Dashboard configuration") |
| `hass_protocol` | `core`, `formats`, `cjson` | yes | The field table; builds `render_template` templates from the tiles of a page, builds `call_service` / statistics / history commands, decodes messages into core types |
| `hass` | `hass_protocol`, `esp_websocket_client`, `esp_http_client`, `cjson` | no | The session task: connect, auth, reconnect, ping, request ids, subscriptions, chunked statistics, camera fetch, the sounds of the hassctrl_say event |
| `ui` | `core`, `lvgl` | no | Screens, tile views, panels, theme, fonts. Turns touches into intents, store changes into widget updates |
| `board` | `esp_lcd`, panel and touch drivers | no | Brings up display, touch and backlight for the selected board, returns `esp_lcd` handles. No LVGL, so the recovery app can use it too |
| `platform` | `littlefs`, `esp_sntp`, `nvs_flash` | no | LittleFS mount, NVS, time sync, log setup |
| `network` | `esp_wifi`, `esp_netif` | no | Station and access point state machine, scans, stored credentials (section 9) |
| `webserver` | `esp_http_server`, `network` | no | Captive portal and the setup pages for the phone path (section 9) |
| `dashboard` | `config`, `core`, `platform` | no | The stored dashboard (`/fs/dashboard.json`): loads it, hands out the current `core::Layout`, saves with a revision check, imports and exports (section 5, "Storage") |
| `main` | everything | no | `app_main`: start order, the function tables between the components, error policy at boot |

The other components (`light_sensor`, `orientation`, `touch_input`, `frame_display`, `ota`,
`audio`, `vibration`, `camera`, `relay`, `motion`, `logbuf`, `devconsole`, `uidebug`,
`esp_driver_ppa`) are listed in section 9.4.

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
| **hass task** | `hass` | the WebSocket client, request ids, subscriptions, reconnect and ping timers, history chunking |
| WebSocket task | `esp_websocket_client` | socket I/O only; its event handler reassembles frames and forwards complete messages to the hass task |
| media tasks | `hass` (`media.cpp`; HTTP and MJPEG in `media_http.cpp`, the decoders in `media_decode.cpp`) | camera fetch and JPEG decode, so an image never delays a state update; created with the first request, up to three on the P4, one on the ESP32 and the S3 |

```
 touch -> [UI task] --intent--> PendingTracker --Backend::call_service--> queue --> [hass task] --> esp_websocket_client
                ^                                                                        |
                |                                                                        v
          apply to store <-- take_event (lv_timer, 20 ms) <-- HassEvent (decoded) <-- reassemble + decode
          update widgets
```

* The **core lives in the UI task.** The UI is the only consumer of entity state, so putting the
  store there removes every lock around the model. The dashboard screen takes the waiting events
  from an `lv_timer` every 20 ms (`Backend::take_event`). At most 16 wait; when the UI was busy
  (a screen animation) and they are full, the oldest page values or image make room. An answer
  (a media folder, the entity search, a history) is asked for once and waited for: it is dropped
  only when nothing else is waiting (logged).
* **Decoding happens in the hass task.** The UI task receives typed values
  (`PageValues{generation, updates}`), never JSON. Parsing a few KB never blocks rendering.
* **Sending never blocks the UI.** `esp_websocket_client_send_text` takes a lock and can block up
  to its timeout on a bad link, so only the hass task calls it.
* **To the hass task**: `subscribe_page`, `call_service`, `fetch_history` and the entity queries
  (the web UI's picker, which waits for the answer, and the edit mode's search, whose answer comes
  back as a `HassEvent`) post a command to the task's FreeRTOS queue (16 entries), which carries the event type
  and a pointer to the heap-allocated `std::variant` (a FreeRTOS queue copies raw bytes, which is
  wrong for a variant with strings in it). The socket's events (opened, message, closed), the network's
  and a changed URL or token go through the same queue, so the task waits on one thing. Images go to the media tasks' own
  queue (`fetch_image`, `play_stream`).
* **To the UI**: a `std::deque` of `HassEvent` under a mutex, at most 16; when nobody takes them
  the oldest go. The connection state is not an event: the UI reads it with `Backend::hass_info`
  (the status bar and Settings).
* The **dashboard** does not send a message either: `Backend::dashboard()` returns the current
  layout (a `shared_ptr` to an immutable `core::Layout`), and the screen checks it on its timer.
  A new pointer is a new dashboard (a save in the web UI, an import, an edit on the screen).
* Other tasks that call the UI (the orientation sensor, the backup import) take the LVGL lock
  (`lvgl_port_lock`).

```cpp
// ui -> hass (hass.cpp, private to the task)
using Command   = std::variant<Subscribe, Call, FetchHistory, Query, SubscribeForecast, Motion>;
// hass -> ui (core/entity.hpp)
using HassEvent = std::variant<PageValues, ServiceFailed, History, Image, Forecast, EntityList, EntityDetails>;
```

### Stale results

Every subscription carries a generation number. When the visible page changes, the UI sends a new
`SubscribePage` with a new generation. The hass task unsubscribes the old template and stamps every
`PageValues` with the generation it belongs to. The UI drops results of an older generation. This
covers the race where a result for the previous page arrives after navigation.

---

## 5. The domain model (`core`)

Three things with different lifetimes are kept apart, so two tiles of the same entity share one
state:

| Type | Lifetime | Content |
| --- | --- | --- |
| `Layout` | immutable after a config load | pages, grid per page, tiles with kind, icon, name, size and cell per orientation, ranges. Entities are interned: a tile holds an `EntityIndex`, not a string |
| `EntityStore` | as long as the layout | one `EntityState` per interned entity: state text, numeric value, the projected attributes, last update. Change notification per entity |
| `PendingTracker` | per action | optimistic value, expected state, deadline. Resolves when the store reports the expected state, reverts on timeout or `ServiceFailed` |

A config reload builds a new `Layout` and `EntityStore` and replaces both. The UI rebuilds its
screens. Nothing patches the old objects in place.

### Tile type descriptors

Each tile type is described in one table entry, and every layer reads that entry:

```cpp
// core: what a tile can show (a 64 bit FieldMask since the weather fields: 42 fields)
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
  the brightness and the color modes for a light, the panel fields only while the panel is open.
* `hass_protocol` builds the template of the visible page from it, every value as
  `state_attr(e,'<attribute>')|tojson`, and decodes a result back into `EntityUpdate`s, from the
  same table.
* `ui` picks the tile view for the kind.
* A weather entity (`TileKind::Weather`, `weather.*`) has 15 more fields: `temperature` (its own
  field, not `Target`: on a climate it is the setpoint), `apparent_temperature`, `humidity`,
  `pressure`, `wind_speed`, `wind_gust_speed`, `wind_bearing`, `visibility`, `cloud_coverage`,
  `dew_point`, `uv_index` and the units of four of them. The tile needs the temperature, its unit
  and the humidity, the panel the rest and `supported_features`. The condition is the state
  (`core/weather.hpp`: words, MDI names, compass).
* A timer entity (`TileKind::Timer`, `timer.*`) has 3 more fields: `duration` and `remaining`
  (Home Assistant's `0:30:00`, decoded to seconds by `core::duration_seconds`) and `finishes_at`.
  The template renders that one as epoch seconds, kept as an `int64_t` (`Kind::Timestamp`:
  `as_timestamp(state_attr(e,'finishes_at'),none)`), so the device parses no ISO time; the
  countdown runs from it by the device's clock (docs/ui/lvgl/lvgl-panels.md section 7).
* An automation or a script (`TileKind::Automation`, `automation.*`, `script.*`) has one more
  field, `last_triggered`, rendered as epoch seconds (`Kind::Timestamp`:
  `as_timestamp(state_attr(e,'last_triggered'),none)`, null while it never ran, kept as an
  `int64_t`, 0 then), so the device parses no ISO time; the tile and the panel show it as a time
  of day, "Yesterday" or a date.

The template and the decoding come from the same table, so they cannot drift apart.

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
`SetHvacMode`, `SetPreset`, `SetFanMode`, `SetEffect`, `SetColor`, `SetColorTemp`, `SetLock`
(lock, unlock or open), `SetTimer` (start with an optional duration, pause, cancel or finish), `Run` (the
actions of an automation with `skip_condition`, a script; `kStop` turns a running script off). Each holds an
`EntityIndex` plus its value. `PendingTracker` records the optimistic value and `hass_protocol` turns
the intent into a `call_service` message. Gesture rules (double tap window, drag release) stay in
`ui`. What an intent means stays in `core`.

### Dashboard configuration

* The configuration holds only the dashboard: a `defaults` section (the grid of the document)
  and the `tiles`. The connection values (URL, token) are settings, not part of the dashboard;
  the YAML import accepts them in a `hass:` block (below). There is one format, no compatibility
  with the file of the old firmware. `docs/dashboard-sample/` is the guide to it and a test file.
* The device keeps the dashboard as JSON (Storage, below). The web UI is the editor for areas and
  tiles. The screen has an edit mode that moves, removes and adds tiles
  ([ui/lvgl/lvgl-dashboard.md](ui/lvgl/lvgl-dashboard.md) section 7). YAML is the import and
  export format for advanced users, and the import accepts includes.
* **The web UI goes with the firmware** (rule). The web editor (`web/dashboard.js`)
  copies rules of `core` and `config`: the placement (`place()`, `has_back_tile()`, `placeable()`
  in `layout.cpp`), the kinds of the domains (`kind_of_entity()`), the default sizes, the keys of
  the format. Any change to the dashboard (format, rules, placement, API) changes the
  editor in the same commit, and the web image goes onto the devices together with the firmware
  (README, "Flash"). The device's check stays the truth: a forgotten copy shows as a problem in
  the editor, not as a broken dashboard.
* A change made in the web UI, on the screen or by an import makes a new layout, which the screen
  picks up (section 4).
* The full resync of the visible page is a fixed 60 seconds. After an error or a reconnect the
  page is subscribed again immediately.

Storage:

* One JSON file on LittleFS, `/fs/dashboard.json`, written atomically (temporary file, then
  rename). The web UI reads and writes it as it is, and the file is readable in `files.zip`.
  Reading uses cJSON, which ESP-IDF 6 no longer bundles: `espressif/cjson` from the registry is
  in the main firmware for `hass` too (about 5 KB of flash, tree in PSRAM). Writing uses
  `formats::JsonWriter`. The dashboard API reaches `config` through
  a function table filled in `main`, like `HassAccess`, so the recovery app links no cJSON.
* The file carries a format version and a revision. Every tile gets a stable id, created by the
  device, so the web UI and the screen can name a tile. A write from the web UI sends the revision
  it read and gets `409` when the screen changed the dashboard in between.
* **Format version 2.** The firmware reads its own version only and refuses an older file with a
  message that names `tools/convert_dashboard.py`, the one-time conversion of an older file or
  folder (JSON or YAML, standard library only; from version 1 it writes out the old default sizes
  of dimmer, climate and camera tiles, so a converted dashboard looks as before). **No
  conversion code in the firmware**: a new version adds its step to the tool.
* JSON and YAML both end in a `core::Layout`, and one validator checks it. A layout that fails is
  never stored, and the running dashboard stays.

YAML import and export:

* The import runs on the device, in `config`, with a YAML subset parser of its own
  (`formats`). Errors name the file, the line and the include chain
  (`rooms/office.yaml:12: unknown tile key 'nmae' (included from rooms.yaml:9)`).
* All files are uploaded as one set (folder picker in the web UI, or the tool). One file at a
  time cannot work: the main file names files that have not arrived yet. A "check only" option
  validates without storing.
* An include is an entry of any tile list. The included tiles go where the entry stands, so the
  order of the files decides the placement:

  ```yaml
  # hass.yaml
  defaults:
    grid: 4x2
  tiles:
    - entity: climate.living_room
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
  variables.
* The parser accepts any consistent indentation and any key order.
* The main file may carry the settings sections of the backup file (`hass`, `network`,
  `display`, `sound`, `alarm`, `camera`, `time`, `webui`), with the tokens and passwords added by hand
  (`hass:` `token:`, a network's `password:`, `camera:` `stream_password:`, `webui:` `user:` and
  `password:`, the web UI login, which no export writes). The dashboard import does not
  read them itself: `main` hands the main file to the backup's readers
  (`config::backup::read_settings`, the section table of `main/backup.cpp`). They are checked
  with the dashboard, a problem in either refuses the import, and applied after the dashboard
  was stored. The answer names the
  applied sections, the notes and the ignored keys. A file without a token is fine: the stored
  token stays.
* The export dumps everything into one file, for backup: `hass.url` without the token, and
  without the comments of the import. Splitting it into files is up to the user. It is the same
  file as the settings backup: optional settings sections (those above, without `webui`),
  `defaults` and `tiles`, and the export offers settings, dashboard or both.

Tile format:

* The kind of a tile follows from the domain of its entity. `type` is only for the tiles
  without an entity: `area`, `spacer`, `clock` and `media`. A light tile with the flag `dimmer` shows the
  level fill; without it, it acts as a simple switch.
* Icons: one name per glyph, no duplicates. The names are the MDI names (`lightbulb`,
  `power-plug`).
* One notation for sizes: `cols x rows` as a string, `grid: 4x3` and `size: 1x2` (stored as
  `[4, 3]` in JSON). The size and the cell of a tile live per orientation under `landscape:` and
  `portrait:` ("Orientation" below).
* `decimals` defaults to the display precision of the entity in Home Assistant
  (`states(entity, rounded=True)`), the key overrides it.
* The web UI reads the entity from Home Assistant when a tile is created and fills in everything
  (name, icon, dimmer flag, size).

Tile format, details:

* Domains:

  | Domain | Tile | Tap |
  | --- | --- | --- |
  | `light` | without `dimmer`: switch; with `dimmer: true`: level fill | toggle after the double-tap window, double tap opens the timeline (the light panel when the light dims); with `dimmer`: drag sets the level, double tap toggles, tap opens the light panel (with a history button) |
  | `switch`, `input_boolean`, `fan` | switch | toggle |
  | `automation`, `script` | automation: the time of the last run (`Disabled`, `Running`) | double tap runs it (`automation.trigger` with the conditions skipped, `script.turn_on`; a running script is stopped), tap opens the automation panel (enabled switch, last run, run, timeline) |
  | `button`, `input_button`, `scene` | button | press, `turn_on` for a scene |
  | `climate` | climate; tall or wide it has the `+`/`-` bars; in `heat_cool` both setpoints | the bars step the setpoint, a tap elsewhere opens the climate panel |
  | `camera` | picture, the image fills the tile | shows the image or the stream full screen |
  | `lock` | lock (the state in words) | opens the lock panel (lock, unlock, open, the timeline); never unlocks by itself |
  | `weather` | weather: the condition, the temperature, the humidity | opens the forecast panel |
  | `type: clock` (no entity) | clock: the time, digital or analog, in the device's zone or its own `zone` ([ui/lvgl/lvgl-dashboard.md](ui/lvgl/lvgl-dashboard.md) section 3) | nothing |
  | `type: media` (no entity) | media: the icon `music` and its name (default "Media") | opens the media browser at its `folder`, a folder of Home Assistant's media library or of the SD card; without one both tops ([ui/lvgl/lvgl-settings.md](ui/lvgl/lvgl-settings.md) section 3) |
  | `sensor`, `binary_sensor`, every other domain | sensor (read only) | opens the panel with the history |

  Every tile is 1x1 unless its `landscape:` / `portrait:` size says otherwise. Any kind takes
  any size up to the grid; the look of a tile follows
  its shape ([ui/lvgl/lvgl-dashboard.md](ui/lvgl/lvgl-dashboard.md) section 3).
* `type:` exists only for `area`, `spacer`, `clock` and `media`; on any other tile it is an
  error. The clock's keys (`zone`, `hours`, `seconds`, `date`, `style`, `border`, `ticks`) are
  only for clocks, `border` and `ticks` only for analog ones; `zone` is an IANA name the firmware
  knows (`core::time_zone_rule`), anything else is refused at import. The media tile's key is
  `folder`, where its browser starts: a `media-source://` id of Home Assistant's media library
  (at most 255 bytes; `media-source://` alone is its top) or a folder of the SD card relative to
  it (no `..`, no `\`, no `/` at the ends, at most 96 bytes; `/` alone is its root); without it
  the browser shows both tops. Only for media tiles. The web editor picks it with a folder
  browser (`GET /api/hass/media`, `GET /api/sd/list`).
* Icon order, the same as in the Home Assistant frontend or as close as the glyph subset
  allows: the `icon` of the tile; the entity's `icon`
  attribute (set by the user in HA, or by older integrations) when the glyph is in the firmware;
  domain and `device_class`, with the off glyph for on/off entities; the domain. The icons of
  integrations (`icons.json`) are not part of the state and are not used. The glyph subset is
  the list in `components/core/src/icons.cpp` (and `UI_GLYPHS` of `tools/build_icons.py`).
* A tile without a name (YAML import) shows the entity's `friendly_name`.
* Climate step: the entity's `target_temp_step` (a capability attribute in the state, only there
  when the integration sets it, for example `generic_thermostat` with 0.1, 0.5 or 1), else the
  `step` of the tile (0.1 or more), else 0.5 for °C and 1 for °F as in the HA frontend.
* Light step: `step` of the tile for the `+`/`-` buttons of the light panel, default 1 %,
  1 to 50. The slider stays continuous. `+`/`-` repeat while held, in the light and the climate
  panel. Lights have no `min`/`max`.

Orientation:

* The portrait grid is the transposed landscape grid, so a page reads in the same order in both
  orientations. The 180 degree turned orientations use the layout of their base orientation. A
  grid of its own for portrait is not built.
* Everything about where and how large a tile is lives per orientation, as a map `landscape:` /
  `portrait:` with `size` (default 1x1; portrait: the landscape size turned), `cell` and
  `hidden`. Per orientation the tiles with a cell take it first, the others fill the free blocks
  first-fit in list order. Moving a tile (the edit mode of the screen, the web editor) gives
  every tile of its page its current cell in that orientation only, and pushes the tiles in its
  way aside (`core::move_tile`). Details: [ui/lvgl/lvgl-dashboard.md](ui/lvgl/lvgl-dashboard.md)
  section 6.
* **Placement is never an error**: a tile that cannot take its cell (outside the grid, on the
  back tile, over another tile) or finds no free block is not shown in that orientation
  (`Placement::shown`), the device draws nothing for it, the web editor lists it under the
  preview and a drag brings it back. Only the data (entity ids, icons, ranges) is checked.
* `swipe: true` on an area tile takes the back tile
  off its page, a swipe right goes back and dots show the depth
  ([ui/lvgl/lvgl-dashboard.md](ui/lvgl/lvgl-dashboard.md) section 9). The subscription of a
  page covers only the tiles shown in the current orientation.

---

## 6. Home Assistant connection (`hass`, `hass_protocol`)

* One WebSocket connection, authenticated with a long-lived token.
* `render_template` per visible page, re-subscribed on page change, with the panel's detail
  fields added while a panel is open. Home Assistant only sends a result when the rendered text
  changed.
* `call_service` over the same connection, with failures reported per entity.
* Statistics and history in 6 hour chunks, one request at a time.
* The forecast of a weather entity while its panel is open (below).
* A full resync every 60 seconds as a safety net (section 5).
* The API's ping every 25 s and a 70 s silence timeout detect half-open connections (section
  9.4, `hass`).
* Transport is `esp_websocket_client`, TLS through `esp-tls` with the certificate bundle.
* A self-signed certificate is trusted on first use (`hass/src/tls.cpp`): the connections to
  Home Assistant (WebSocket and camera images) attach the bundle and wrap its verify callback.
  The server's certificate passes when its SHA-256 equals the trusted one (checked first) or the
  bundle trusts the chain. A refused certificate's SHA-256 goes to the status, and the user
  trusts it in the web UI after comparing it with the browser's. One fingerprint in the settings
  NVS (`hass`/`cert`), forgotten with a new address, in the backup. Not a CA: a Home Assistant
  behind a private CA is trusted by its server certificate as well, and a renewed certificate
  has to be trusted again. The bundle has cross-signed verification on
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`): google.com sends its root
  cross-signed by a root the common bundle lacks.
* The client delivers a large frame in several data events, and a single read can also return a
  partial frame when the next TCP segment or TLS record has not arrived yet. The event handler
  reassembles by `payload_offset` / `payload_len` / `fin` into a PSRAM buffer with a hard size cap,
  and forwards only complete messages.
* JSON is parsed with cJSON from the component registry (ESP-IDF 6 no longer bundles it),
  allocating from PSRAM through `cJSON_InitHooks`, set first thing in `app_main` (the stored
  dashboard is parsed before `hass` starts). The buffer of a connection is 32 KB of PSRAM (the
  hourly forecast: 48 entries of about 320 bytes); a larger message grows it up to 256 KB (a
  folder of the media library: about 300 bytes a file), and it goes back to 32 KB after it.
* The forecast of a weather entity is not in its state. The weather panel subscribes it with
  `weather/subscribe_forecast` (`{"type":"weather/subscribe_forecast","entity_id":...,
  "forecast_type":"daily"|"hourly"}`, Home Assistant 2023.9 and later): an empty result, then an
  event `{"type":"daily","forecast":[...]}` now and with every update of the integration. The
  hass task owns one such subscription (`SubscribeForecast` command, `ForecastState`): a new
  command ends the old one with `unsubscribe_events` and subscribes the wanted one, type `None`
  only ends it; it is subscribed again after a reconnect (after `auth_ok`, next to the page).
  `hass_protocol::decode_forecast` keeps the first 24 entries (8 days or 24 hours) as
  `core::Forecast` (time, condition, temperature, low, chance of precipitation), delivered to the
  UI as a `HassEvent` stamped with the panel's generation. An error (`forecast_not_supported`
  when the entity lacks the type) comes as a `Forecast` with `error`.
* Camera images use `esp_http_client` and a JPEG decoder (on the ESP32 `esp_jpeg`, the TJpgDec
  of the ROM, which reads no progressive JPEGs; on the S3 `esp_new_jpeg`; on the P4 the hardware
  JPEG driver), in the media worker: three tasks on the P4, one on the ESP32
  and the S3 (internal RAM), see ui/lvgl/lvgl-camera.md "Camera image".
* Sounds from Home Assistant: on a board with MP3 (`set_sound_player`) the hass task subscribes
  the event `hassctrl_say` after `auth_ok` (`subscribe_events`; Home Assistant allows a custom
  event to administrators only, a refusal is logged). The rest is in section 9.6.
* The media library (Media in Home Assistant's sidebar, `CONFIG_HC_UI_PLAYER`):
  `media_source/browse_media` lists a folder (`core::MediaList`: title, and per entry the
  `media_content_id`, title, MIME type, folder, playable), `media_source/resolve_media` gives the
  address of a file, signed by Home Assistant for 24 hours (`/media/local/...?authSig=...`).
  Messages and decoders in `hass_protocol/src/media_source.cpp` (host tested). The hass task
  keeps one browse and one resolve on their way (`MediaState`); one on its way is never
  replaced, newer ones wait behind it (the waited ones in order, the UI's as the newest only),
  each with its timeout from when it was asked, so the player's folder walk and the browser do
  not cancel each other. A browse for the UI comes back through `take_event` stamped with its
  generation, the others (the console, the music download, the walk) wait on a semaphore. The
  music itself is section 9.6.

---

## 7. UI (`ui`)

`components/ui/src` is flat: one namespace and one file pair per part, with module state inside
the `.cpp`, no class hierarchy. Every part runs in the UI task (section 4).

* **Dashboard** (`dashboard.cpp`): owns the current layout, the `EntityStore` and the
  `PendingTracker`, the page that is shown and the one open panel. It takes the events of the hass
  task, applies them to the store and redraws only the tiles and the panel whose entity changed;
  nothing rebuilds the grid because a value changed. A new page, a new layout or a turn of the
  display builds the page again. Pages slide in; `swipe.cpp` adds the swipe navigation.
* **Tile views** (`tiles.cpp`): one `tiles::View` per tile, built for its kind and shape and
  drawn by `tiles::update(view, layout, state, pending)`; the gestures of a tile become intents
  through the dashboard. All kinds are built and drawn in `tiles.cpp` (824 lines); the gestures
  are in `tiles_gestures.cpp`, the clock in `tiles_clock.cpp`, the texts that panels use too
  (`sensor_text`, `when_text`, ...) in `tiles_text.cpp`, the shared parts in `tiles_parts.hpp`.
* **Panels**: `panel.cpp` builds the frame (back tile, header, options, control; a dialog on the
  P4), and each kind has its own file with `build(frame, tile)`, `update()` and `forget()`:
  light, climate, sensor, lock, timer, automation, weather and the picture (full screen camera).
  The graph and the timeline are one part (`history_view`), used by the sensor, lock, timer and
  automation panels and behind the history buttons of the light and the climate panel.
* **Around the dashboard**: the start screen (`splash`), the pull-down zone and the alert strip on
  `lv_layer_top` (`overlay`), Settings and the first setup (`settings*`, `setup`), the edit mode
  (`edit_mode`, `add_dialog`, `entity_picker`), screen power (`power`), rotation (`rotation`),
  the alarm screen (`alarm`), the touch sounds (`sound`).
* **Theme**: the color and font constants of [ui/lvgl/lvgl-design.md](ui/lvgl/lvgl-design.md) in
  one place (`theme.hpp`). Fonts and Material Design Icons are generated with `lv_font_conv`
  (`tools/build_icons.py`).

### 7.1 LVGL 8.4

The firmware runs on LVGL 8.4, the version of the code it was ported from, and stays on it. LVGL
9.6 was built side by side and measured with the rendering benchmark on all three boards
(2026-10-17, [benchmark.md](benchmark.md) section 8): 2 to 3 times slower on the ESP32, 25 to 75 %
slower on whole frames on the ESP32-S3, 5 to 15 % slower on the ESP32-P4 with 150 KB less internal
RAM; its fast functions did not fit the IRAM of the ESP32 and the S3, and it nearly used up the
LVGL task's stack. It is not an option for these boards, and the code for it was removed.

* The analog clock's face is drawn by hand (`tiles_clock.cpp`, a draw event on a plain object),
  not an `lv_meter`: the same look, and a tick redraws only the strips of the hands that moved.
* Byte swapping for SPI panels is `CONFIG_LV_COLOR_16_SWAP` in the board's defaults file. The
  board states the need (`Info::swap_bytes`) and `main` checks that the configuration agrees.
* Changes that may cost render time are measured with `tools/bench.py` before they stay, on the
  JC3248W535 or the P4: the WT32's times move by 20 to 30 % between builds of the same code
  ([benchmark.md](benchmark.md) section 6).

---

## 8. Boards (`board`)

```cpp
namespace hc::board {
struct Handles { esp_lcd_panel_io_handle_t io; esp_lcd_panel_handle_t panel;
                 esp_lcd_touch_handle_t touch; i2c_master_bus_handle_t i2c; };
esp_err_t init(Handles &out);          // panel, touch, backlight; no LVGL
void      set_backlight(uint8_t percent, BacklightCurve curve = BacklightCurve::Perceptual);
const Info &info();                    // the board's constants: size, pins, add-ons, calibration
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
* **Variants**: the add-ons of one unit (an LDR, a motor, relays, the calibration of
  its MPU6050, a config option) are a directory `boards/<board>/variants/<name>/` in git. Its `hc_board_variant.hpp` is a copy of `components/board/src/variant/hc_board_variant.hpp`
  whose constexpr `variant::apply(Info)` changes the fields of the board's `Info`; every board file
  builds its `kInfo` through it, and `tools/build.py <board>.<name>` puts the variant's directory
  on the include path instead of the default, which changes nothing. An `sdkconfig.defaults` in
  the directory is the last config layer. The board file in the repository keeps an example of
  every add-on (the developer's unit). `Info::variant` names it in the boot log and on Settings,
  System; the image tag (`ImageTag`) stays the board type, so every image of the type is accepted
  by the update check. Steps: [adding-a-board.md](adding-a-board.md) "Variants".
* The backlight is PWM through the LEDC peripheral at 25 kHz (1, 5 and 25 kHz look the
  same on the WT32-SC01, so the highest one stays). Brightness in percent goes through a
  perceptual curve, the inverse of the CIE lightness (it feels smooth down to the
  lowest levels, a linear curve gets choppy there).
* **Minimum brightness**: the lowest PWM duty at which the screen is still readable is
  a constant of the board type (`Info::backlight_min_duty`), like the orientation sensor's
  calibration: found by the developer when the board is added, never on the device or by the
  user. 1 % is that minimum and 100 % is full brightness, so no setting can make the screen
  unreadable. It is found with the backlight tab of the test screen
  ([adding-a-board.md](adding-a-board.md)); the screen returns to the normal brightness after 10
  seconds without a touch while the test runs.
* A speaker amplifier on I2S is part of the board too: its pins are in `Info` (`audio_*`), and the
  board's Kconfig option selects `HC_BOARD_HAS_AUDIO`, which offers the `audio` component
  (section 9.6).
* A vibration motor works the same way: its GPIO is `Info::vibration_gpio`, and the board's
  Kconfig option selects `HC_BOARD_HAS_VIBRATION`, which offers the `vibration` component
  (section 9.7).
* A MIPI-CSI camera: the board's Kconfig option selects `HC_BOARD_HAS_CAMERA`, which builds the
  `camera` component; the sensor is found on the board's I2C bus (`Handles::i2c`, section 9.8).
* Relays (`Info::relay_gpios`, section 9.7a) and a motion sensor (`Info::motion_gpio`, section
  9.7b) are plain GPIOs named in `Info`; their components are always built and idle without a pin.
* **Touch input** (`touch_input`): a task samples the touch controller every
  10 ms (priority 5) and queues every change; LVGL's read takes them in order, all waiting ones in
  one read (`continue_reading`). esp_lvgl_port read the controller in the LVGL task every 30 ms
  and not while LVGL rendered, so quick taps during a long render were lost. Points outside the
  panel are dropped. A controller that reports less often gets a longer period
  (`Info::touch_sample_ms`): the AXS15231B of the JC3248W535 makes a new report about every 17 to
  22 ms and answers "no finger" to a read without one, so read every 10 ms a held finger was
  lifted and put down again and again (one tap became several). It is read every 20 ms, and its
  board file takes a single "no finger" during a touch for a misread. Console
  `touch log` prints the samples.
  Up to two fingers are read: LVGL's pointer follows the one that came down first (by the
  controller's track id), `touch_input::fingers()` gives both to a two-finger gesture (the camera's
  pinch to zoom). The JC3248W535's driver reads one finger only and takes misread answers as
  touches; its board file reads two, takes a bad header for no finger and drops shifted records.
  Measured 2026-10-11: the GT911 and the AXS15231B keep two fingers apart down to 15 and 7 mm, the
  FT6336U of the WT32-SC01 merges them into one point below about 3 cm.
* `board` also owns the shared buses, such as the I2C bus of the touch controller, and hands out
  their handles, so a sensor on the same bus does not open it a second time.

### 8.1 Orientation

* Landscape and portrait on every board.
* Where an MPU6050 accelerometer is connected, the screen rotates with the device; without one the rotation is set in Settings, Display.
  Its wiring is documented on the board's device page.
* The `orientation` component reads the MPU6050 over the board's I2C bus with the defaults of the
  old firmware: a sample every 100 ms, a new orientation
  is taken over when the accelerometer reading is steady (it changes less than 0.05 g between
  two samples; the gyroscope is not used, a module can read 12 deg/s at rest) and the orientation
  held for 500 ms.
* Only the part of gravity in the panel plane decides the orientation, from about 15 degrees
  away from lying flat. The old firmware treated the panel as flat as soon as the part into the
  panel was the largest, so a panel leaning back on a desk got no orientation at boot and
  started upside down (seen on the WT32-SC01: gravity x 0.66, z 0.76, about 49 degrees from
  upright). Near a diagonal one axis has to win by 30 %, so the screen does not flip. `main` hands a new
  orientation to the UI under the LVGL lock (section 4).
* SPI panels (WT32-SC01, T4) rotate in the panel controller: esp_lvgl_port sets
  `esp_lcd_panel_swap_xy` and `esp_lcd_panel_mirror` on a turn, which costs nothing at runtime.
  The AXS15231B (`frame_display`) and the MIPI-DSI panel cannot rotate and turn in software
  (section 8, below the table). `touch_input` turns the controller's coordinates to the
  display's rotation.
* Developer calibration (done when the board is added, never on the device or by the user): two captures instead of fixed axis settings. `orient capture 0`: the panel exactly
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
  400 kHz and a timed out read is repeated twice.
* The UI rebuilds the current screen for the new size. The dashboard uses the transposed grid of
  the old firmware, so a page reads in the same order in both orientations (section 5,
  "Dashboard configuration").

| Board | MCU | Display | Touch | Drivers (component registry) |
| --- | --- | --- | --- | --- |
| WT32-SC01 | ESP32, 4 MB flash, 4 MB PSRAM | 480x320 ST7796S, SPI, write only | FT6336U | `esp_lcd_st7796`, `esp_lcd_touch_ft5x06` |
| JC3248W535 | ESP32-S3, 16 MB, 8 MB PSRAM | 320x480 AXS15231B, QSPI, whole frames only | integrated | `esp_lcd_axs15231b` |
| JC4880P443C | ESP32-P4 + C6, 16 MB, 32 MB PSRAM | 480x800 ST7701S, MIPI-DSI | GT911 | `esp_lcd_st7701`, `esp_lcd_touch_gt911`, `esp_wifi_remote` + `esp_hosted` |
| TTGO T4 (test device, not fully supported) | ESP32, 4 MB, PSRAM | 240x320 ILI9341, SPI | none | `esp_lcd_ili9341` |

Display flushing on the SPI panels: two partial buffers in internal DMA-capable RAM (about 40
lines each), so LVGL renders the next band while DMA sends the previous one; SPI DMA on the
classic ESP32 cannot read PSRAM. On the WT32-SC01 a full screen redraw takes 35 to 45 ms against
the 30.7 ms wire time at 80 MHz, partial redraws 4 to 9 ms (TODO.md, Step 2).

The AXS15231B of the JC3248W535 takes no row address over QSPI (a write starts at the first row
or continues where the last one ended) and cannot rotate a frame. The board sets
`Info::full_frames`, and `main` uses `frame_display` instead of esp_lvgl_port's display: LVGL
renders in direct mode into one frame in PSRAM, and after each refresh the frame is sent from the
top down to the lowest changed row, in bands that are turned to the panel's orientation while
they are copied into two internal DMA buffers (copying one overlaps sending the other). Rotation
is software for this board; `touch_input` reads its touch every 20 ms (section 8).

The ST7701S of the JC4880P443C is a MIPI-DSI video panel: the DPI controller scans a frame buffer
in PSRAM and nothing can rotate it. The board sets `Info::mipi_dsi`, and `main` uses
esp_lvgl_port's DSI display with two 50-line draw buffers in internal RAM, which the CPU copies
into the frame buffer (the DMA2D corrupted memory under a full-screen camera stream); LVGL rotates in software. The ESP32-P4 has no radio: Wi-Fi runs on the
ESP32-C6 of the board, and `network` gets the `esp_wifi` API from `esp_wifi_remote` over
`esp_hosted` (SDIO) without code changes. The device name comes from the P4's base MAC.
The C6 runs an esp_hosted coprocessor firmware of the same version as the host side, built
from `boards/jc4880p443c/c6` and sent to it over SDIO (`hc::network::update_coprocessor`,
console `c6 update <url>`), or, while the C6 cannot hold Wi-Fi, from the P4's free app slot
written over USB (`update_coprocessor_from_slot`, console `c6 update slot`, Wi-Fi stopped); a
different major version on the two chips is a mismatch.

---

## 9. Setup, settings and platform services

### 9.1 Setup and network

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
  result (15 minutes), then turns off unless the user chose to keep it on or turns it on later
  in Settings.
* **Touch is a requirement** for every supported board. So the AP never opens by
  itself when the Wi-Fi fails; the user turns it on in Settings. Recovery without a PIN goes
  through Settings on the screen (reset the web UI login, factory reset); no hardware fallback.
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
* Screen and web layouts: [ui/README.md](ui/README.md) (the LVGL and the web documents and their design guides).

### 9.2 Network selection and reconnect

* **Up to 5 saved networks, two policies** chosen in the Wi-Fi settings: the entered order, or
  the strongest signal. With the strongest signal the networks are sorted by signal at runtime
  and that order is never saved. The entered order is never changed automatically. The choice is
  plain C++ (`network/src/selection.cpp`, host tested).
* **On connect** (boot, or after the connection is lost): one scan. With the entered order, the
  first saved network in range whose signal is usable (-75 dBm or better); if none is, the
  strongest. Of several access points with the network's name (mesh, repeaters) the strongest is
  taken, and the connect names its BSSID. The display shows that it is scanning and connecting.
* **While connected**: no periodic switching, because a switch drops the Home Assistant
  connection for a few seconds. Only when the signal stays below -70 dBm for 30 s does a
  background scan run, and the device switches only to an access point at least 8 dB stronger
  (`hc/network/network.hpp`). The numbers are Apple's published roaming behavior for iPhone and
  iPad ([Apple: Wi-Fi roaming support](https://support.apple.com/102002)); 802.11k/v are not
  used.
* **Fast reconnect**: each network stores the access point (BSSID) and channel of its last
  connection. Only when waking from deep sleep would the device go straight to it without a
  scan; nothing sleeps (battery operation was cancelled), so this path is kept but not taken.
  DHCP skips the ARP probe
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
* Wi-Fi power save is off while the screen is on: with modem sleep every packet waits for the
  next beacon (ping 107 ms average, single requests up to seconds on the T4), without it 19 ms.
  While the screen is off (backlight 0, `ui/src/power.hpp`) `main` turns modem sleep on
  (`WIFI_PS_MIN_MODEM`, `network::set_power_save`) and off again with the first step of the
  fade up; Home Assistant's updates and the web UI are slower then, nobody looks at the screen.
  JC3248W535 with the screen off: 120 mA without, 93 mA with modem sleep (2026-10-23).

### 9.3 Storage

* **LittleFS** for files. It is power-loss safe and does its own wear leveling. SPIFFS
  is in maintenance mode, and FAT with wear leveling is not power-loss safe; FAT stays only for
  SD cards.
* A LittleFS that fails to mount is damaged and never formatted automatically, so its
  readable files can be exported first. A partition that holds no LittleFS at all, recognized by
  the missing "littlefs" superblock, holds nothing to lose and is formatted: a new device, or
  leftovers of another firmware (seen on the T4).
* **NVS** for secrets and small values: Wi-Fi passwords, the AP password, the web UI password
  hash and the Home Assistant token are never stored in a file that the web UI can download.
* The Wi-Fi driver's own NVS copy of the station config is turned off
  (`esp_wifi_set_storage(WIFI_STORAGE_RAM)`), so the `network` component is the only owner of
  the stored networks.
* **Settings live in NVS**, written by this policy (`hc/platform/settings.hpp`):

  | Kind of value | Written | Examples |
  | --- | --- | --- |
  | changed by a live control or by using the device, in bursts | `platform::set_blob_later`: 10 s after the last change, at most 60 s after the first, and at a restart | `ui/display`, `ui/sounds`, `ui/vibrate`, `camera/picture`, `camview/*` |
  | saved by an explicit action (OK, Save, Apply), or a secret | at once, with commit; a failure is reported | Wi-Fi networks, policy and hotspot, web UI login, Home Assistant address, token and certificate, camera stream login, fixed time zone and SNTP servers (`time/fixed`, `time/server1`, `time/server2`) |
  | has to survive a power loss right after it was set | at once | `ui/alarms` |
  | device state that changes rarely | at once, when it changed | `network/last_seen` (another access point or channel), `time/zone` (Home Assistant's), `ui/hint` (once) |
  | developer and calibration | at once | `console/*`, `logbuf/debug`, `light/*` |

  Nothing writes per frame or per touch: a value that can change more often than about once a
  minute takes the delayed write. Its owner reads it from its RAM copy, not back from NVS. NVS
  skips a value that did not change, so writing the same one again costs no flash. The delayed
  table has 8 keys; a ninth writes the waiting ones at once. A factory reset drops the waiting
  values, so none of them comes back with the restart.
* **Files on LittleFS** hold the web UI files and the dashboard configuration
  (`dashboard.json`, section 5). The dashboard is edited in the web UI or imported
  as YAML. There is no file manager for them; the card has one in the web UI and on the screen.
* **Export and import** of the settings are actions in the web UI, as YAML like the dashboard,
  without secrets; an import may carry passwords added by hand. The password of a camera
  stream's address (`http://user:password@host/`) stays in `dashboard.json`; the export leaves
  it out. There is no automatic export file.
* **Logs** are viewable in the web UI (`logbuf`): errors and warnings always, info messages as
  well in the debug mode (stored).
* **SD card** (`components/sdcard`, `CONFIG_HC_SDCARD`, default on where the board selects
  `HC_BOARD_HAS_SDCARD`: the JC4880P443C and the JC3248W535). FAT at `/sd` with `esp_vfs_fat_sdmmc_mount`, the pins
  and the supply in `board::Info::sd_*`.
  * **Off, nothing is built**: the component has no sources, and every use (`main`, the
    webserver's `api_sd.cpp` and `/api/status`, the console, the UI's System row and Save
    button, `board::Info`'s fields, the httpd handler count) is inside `#if CONFIG_HC_SDCARD`.
    ESP-IDF reads the requirements before Kconfig, so `fatfs` is always required, but nothing
    references it. Checked on `jc4880p443c.mic`: every section of `idf.py size` the same as
    before the card (2026-10-23). The recovery app turns it off (`sdkconfig.recovery`).
  * **The controller is shared** with the C6's SDIO link (esp_hosted, slot 1); the card takes
    slot 0 and its IO MUX pins. In IDF 6.1 `sdmmc_host_init()` returns the controller that exists
    and `sdmmc_host_deinit_slot()` removes it only with the last slot, so the card mounts at
    boot (after `board::init`, before the audio and the UI, which list its alarm sounds) whether
    or not the C6 came first, and unmounting leaves Wi-Fi alone (an IDF error line "host
    controller with slot registered" is that check, harmless).
  * Power: the JC4880P443C's card and pins are on LDO channel 4 (`sd_pwr_ctrl_new_on_chip_ldo`),
    3.3 V, through a switch that is always on (the LDO code is built only where the chip has
    one, `SOC_GP_LDO_SUPPORTED`). 40 MHz when the card takes it.
  * **1 bit** (`sd_data_gpios[1]` = -1): the JC3248W535's slot is wired for SPI (CS 10, MOSI 11,
    CLK 12, MISO 13, 10k pullups, 3.3 V rail) and runs as SDMMC 1 bit through the ESP32-S3's GPIO
    matrix: CMD on MOSI, D0 on MISO. ESP-IDF leaves D3 alone in 1 bit mode, a low D3 at CMD0
    would select SPI mode, so `start()` drives the D3 given (CS) high. 40 MHz.
  * **No card detect** on the JC4880P443C: mounted at boot, then by request (console `sd mount`,
    the web UI's Mount); unmount before taking a card out. A card without a FAT, or with a FAT
    that does not mount, is reported ("not formatted") and **never formatted by itself**; the web
    UI formats it after the login's password (one FAT over the whole card, 32 KB clusters).
  * Users: alarm sounds (`main/ui_sound.cpp`: the list merges `/fs/sounds/alarms` and
    `/sd/sounds/alarms`, a name on both once; a stored name stays bare, the ring takes the
    file system's file first, then the card's, the beeps when neither opens), the microphone's
    recording (`sdcard::save_recording`: `/sd/recordings/rec-<date>-<time>.wav`, from the LVGL
    task in 45 ms or from the console), the web UI page (ui/web/web-app.md section 4, SD card:
    list, streamed upload and download, delete, folders, rename/move, a download from a URL in a
    task of its own like the OTA download, format), the screen's file manager (Settings, System,
    SD card; ui/lvgl/lvgl-settings.md section 3) and its player (section 9.6), console `sd`.
  * **One set of file operations** (`sdcard::list`, `remove`, `make_dir`, `rename`, `files.cpp`)
    for the web UI, the screen (through `ui::Backend::sd_*`, `main/ui_files.cpp`) and the
    console: paths relative to the card through `make_path`, the root never removed or renamed,
    a rename never over a name that exists (FatFs would refuse it too) nor a folder into itself.
    The screen lists on the LVGL task (a folder of a few hundred entries in a few ms), up to 200
    entries into a buffer that lives as long as the page.
  * Free space: FAT counts it once after mounting (a full FAT scan on a card without FSInfo),
    then keeps it. `status()` waits at most 20 ms for the lock, so the screen and the web server
    do not wait for a format.
  * **RAM: FATFS buffers in PSRAM.** In IDF 6.1, `ff_memalloc()` tries PSRAM only when
    `CONFIG_FATFS_ALLOC_PREFER_ALIGNED_WORK_BUFFERS` is set; `CONFIG_FATFS_ALLOC_PREFER_EXTRAM`
    alone falls through to `MALLOC_CAP_DEFAULT`, which keeps small blocks internal (our
    `SPIRAM_MALLOC_ALWAYSINTERNAL` is 16384 B on the P4, 512 B on the ESP32 and the S3). The defaults are
    right and stay as they are: `FATFS_ALLOC_PREFER_EXTRAM=y`,
    `FATFS_ALLOC_PREFER_ALIGNED_WORK_BUFFERS=y`, `FATFS_PER_FILE_CACHE=y`, `FATFS_USE_DYN_BUFFERS=y`.
    The volume's work buffer, the table of open files (`max_files` = 4) and each file's sector
    cache are in PSRAM; the P4's PSRAM is DMA-capable, so the SDMMC controller reads and writes
    them directly. Long names on the heap (the default), UTF-8 names
    (`CONFIG_FATFS_API_ENCODING_UTF_8`, the JC4880P443C's `sdkconfig.defaults`).
  * Other boards (not built): the TTGO T4's slot is on SPI (CS 13, MOSI 15, SCK 14, MISO 2, a
    strapping pin that needs the card's pullup); the WT32-SC01 has none.

### 9.4 Components

| Component | Responsibility |
| --- | --- |
| `platform` | boot log, both NVS partitions, LittleFS at `/fs`, the settings class for the settings NVS, the device name (`hassctrl-` and the last two MAC bytes), SNTP with two servers (the web UI's Time page, else pool.ntp.org and time.cloudflare.com; a change restarts SNTP) and the time zone (`time_sync.cpp`; the zone's name from Home Assistant unless a name or a POSIX rule is fixed on the Time page, its POSIX rule from the zone table of `core`) |
| `network` | station and AP state machine of sections 9.1 and 9.2, up to 5 stored networks, the hotspot password, scan results, stored in the settings NVS. One task owns the driver and the state; commands are queued, readers get copies (`status()`); a change listener exists but `main` passes none. Files: `network.cpp` (the state, the task, start), `station.cpp` (scan, connect, retry, roam), `commands.cpp` (the saved networks), `api.cpp` (the calls of other tasks), shared through `task.hpp`. The network choice is plain C++ (`selection.cpp`) for host tests. mDNS (`espressif/mdns`): the device answers as `<device name>.local` and announces `_http._tcp`, in both apps |
| `webserver` | captive portal DNS and redirect, the status API and the built-in page, the setup pages for the phone path, login with the web UI username and password, Home Assistant URL and token, the time zone and SNTP servers (`api_time.cpp`, main firmware only: `Config::time`), the backup export and import (one YAML file; the sections come from a table filled in `main`, each with write, check and apply, so the web server depends on none of their owners), logs. Serves the web files from LittleFS, with a recovery page compiled in that works without the file system ([ui/web/web-recovery.md](ui/web/web-recovery.md)). The dashboard API (`/api/dashboard`: read and write with the revision, YAML import and export) through the `dashboard` component |
| `ui` | plain LVGL, no ESP-IDF drivers. Settings (quick settings, Wi-Fi, hotspot, Home Assistant, display, sound, camera, alarm, system), the pull-down zone and the alert strip on `lv_layer_top`, the dashboard with its tiles, panels and edit mode. Everything it needs from the firmware comes through `hc::ui::Backend`, a table of functions that `main/ui_backend.cpp` fills from `network`, `board`, `platform` and `webserver`. The first setup is a sequence of the same pages |
| `light_sensor` | ambient light for the automatic brightness of `ui`, optional per device: a BH1750 on the board's I2C bus (looked for at 0x23 and 0x5C), averaged over a second, or an LDR on an ADC pin (`ldr.hpp`, the WT32-SC01's variant) |
| `hass` | the Home Assistant connection of section 6: URL and token in the settings NVS, one WebSocket (`espressif/esp_websocket_client`, TLS with the CA bundle), authentication, the API's ping every 25 s, 70 s silence timeout, reconnect 2 s doubling to 30 s, a rejected token is not retried. One task owns the client; the client's event handler only puts messages together (PSRAM, 32 KB, up to 256 KB for a large one) and queues them. The web server and the UI get its status through function tables filled in `main` (`webserver::HassAccess`, `ui::Backend::hass_info`), so the recovery app links neither it nor cJSON. The stored URL, token and certificate, their checks and the token test are in `config.cpp` (runs in the caller's task); the session stays in one file, `hass.cpp`, because pages, history, forecast, queries and the media library share its state and task; the music's download is `media_audio.cpp`. URL parsing (`url.cpp`) is plain C++ for host tests |
| `ota` | firmware and file system images from an upload, a tool or a URL download, app rollback ([ui/web/web-app.md](ui/web/web-app.md) section 5), restart into the recovery app and back, partition layout of section 9.5 |
| `devconsole` | serial command line for development (`CONFIG_HC_DEV_CONSOLE`): Wi-Fi networks, hotspot, file system, restart, factory reset, and the board, Home Assistant and UI commands of the bring-up and of debugging (`help` lists them) |
| `audio` | I2S output to the board's amplifier, the volume, the touch sounds, MP3 files, the alarm sound and the music of the player with its spectrum and tags (section 9.6, `CONFIG_HC_AUDIO`) |
| `player` | the media player's queue, independent of the screen: SD files, playlists and the media library, next, previous, shuffle, repeat (section 9.6, `CONFIG_HC_UI_PLAYER`) |
| `vibration` | a vibration motor on a GPIO: a pulse when a finger touches a tap target, a pattern while the alarm rings (section 9.7, `CONFIG_HC_VIBRATION`) |
| `sdcard` | the microSD card: FAT at `/sd` on SDMMC slot 0, mount, unmount, format, list, delete, rename/move, a download from a URL onto it, WAV files, console `sd` (section 9.3, `CONFIG_HC_SDCARD`) |
| `camera` | the MIPI-CSI camera of the JC4880P443C: capture, its settings, the web stream (section 9.8, `CONFIG_HC_BOARD_HAS_CAMERA`) |
| `touch_input` | the touch screen as LVGL's pointer, sampled by a task of its own (section 8) |
| `orientation` | the MPU6050 on the board's I2C bus, the screen's rotation (section 8.1) |
| `frame_display` | the LVGL display of a panel that takes whole frames only (the AXS15231B, section 8) |
| `logbuf` | the log in a ring buffer for the web UI's log page |
| `uidebug` | screenshots of the screen and `lv_layer_top` as run-length encoded RGB565 over the serial console (`tools/screenshot.py`; two `lv_snapshot`s blended, an alpha of `LV_OPA_MAX` or more counts as opaque as on the panel), and a virtual pointer as a second `lv_indev` for `tap`, `press`, `release` and `swipe` in `devconsole`. Removable, not over the network |
| `relay` | up to four relays on GPIOs, switched by Home Assistant entities found by the device's name (section 9.7a) |
| `motion` | a motion sensor on a GPIO, reported to a Home Assistant helper, and wake on motion (section 9.7b) |
| `esp_driver_ppa` | replaces ESP-IDF's PPA driver with a fix for the ESP32-P4's silicon bug DIG-734 (its README: the numbers, the diff to check after an IDF update, when to drop it); the build of the P4 stops if `ppa_srm.c` lacks the patch or differs from the IDF's file by more |

### 9.5 Partition layout and the recovery app

Two equal OTA partitions on a 4 MB board would leave about 1.9 MB per firmware image. Instead
every board carries a second, small firmware: the **recovery app**. It is the ESP-IDF factory
app, built from the same
repository and the same components (`platform`, `network`, `webserver`, `ota`), without LVGL and
without Home Assistant. It reads the same NVS, so Wi-Fi, hotspot and web UI login are the same as
in the main firmware.

| Partition | 4 MB boards | 16 MB boards |
| --- | --- | --- |
| nvs, otadata, phy | 28 KB | 28 KB |
| factory: recovery app | 1.06 MB | 1.25 MB |
| ota_0: main firmware | about 2.4 MB | 5.75 MB |
| ota_1: main firmware | none | 6 MB |
| LittleFS | 448 KB | 2.9 MB |
| settings (the firmware's NVS) | 64 KB | 64 KB |

* **Update on a 4 MB board**: the main firmware cannot overwrite itself, so it reboots into the
  recovery app, which receives the new image, writes it to ota_0 and boots it. On a 16 MB board
  the main firmware writes the other OTA partition directly. `esp_ota_get_next_update_partition()`
  tells the two cases apart, so the update code is the same.
* **Recovery for free**: when the main firmware is broken, does not confirm itself after an update,
  or an update was interrupted by a power loss, the bootloader starts the recovery app. Its web
  page is the recovery page of [ui/web/web-recovery.md](ui/web/web-recovery.md), so there is one recovery page
  for both cases.
* **Cost**: during an update on a 4 MB board the old firmware is gone once writing starts. If the
  upload fails, the device stays in the recovery app until a good image arrives. A second
  application is built and tested with every release.
* **Only one app runs at a time.** The recovery app is a separate firmware, so the LVGL of the
  main firmware is not running while it runs. It recovers from three situations: an update on a
  4 MB board (the main firmware is being replaced), a broken main firmware (crash loop, failed
  update, interrupted update), and a request from the user. A broken file system alone does not
  need it: the main firmware handles that with its own recovery page.
* **Screen in the recovery app**: no LVGL, which would cost a few hundred KB.
  The app uses the same `board` drivers and draws directly on the panel, band by band from a list
  of text lines and one QR code (no frame buffer, so it works for the SPI panels, the AXS15231B
  that only takes whole frames from the top and the DSI panel): the 8x8 font of
  `recovery/font8x8.inc` (from LVGL's unscii-8, `tools/gen_recovery_font.py`) and Nayuki's
  `qrcodegen.c` (copied from LVGL, MIT). It shows the board, the firmware in flash, the Wi-Fi
  state and address, the hotspot name and password while it is on, and the QR code of
  the hotspot, which is always on in this app (`network::force_hotspot`, not stored) with its name and
  password and a QR code that joins it (the phone then opens the captive portal at
  `http://192.168.4.1/`); with a Wi-Fi connection its IP is a line of text. An update in progress
  adds a line with its state and percent (`ota::status()`), and the reason when it failed. Redrawn
  when something changes (checked once a second). The drivers made the app 45 KB larger, so the factory partition of the 4 MB layout is
  1.06 MB (ota_0 2.4 MB). Measured 2026-10-17 on the WT32-SC01: recovery 1067 KB (21 KB free; without WPA2/3 Enterprise and SAE-PK,
  5.7 KB less, `sdkconfig.recovery`), main 2155 KB (277 KB free); a build with debug off (log level error,
  silent asserts, no error names) would be 96 KB smaller, not done; IDF's build warns that the main image does not fit `factory`, which it never goes to.
* **The recovery app changes only over USB** (`tools/build.py <board> --app recovery -p COMx flash`, which also
  writes the bootloader and the partition table): no update writes `factory`, the main firmware's update only
  `ota_0` and `storage`.
* **Web pages in the recovery app**: the built-in page (always at `/recovery`, at `/` without web
  files) with first setup, login, status, firmware and file system update by file or URL, the
  files as .zip, format, restart and "Start the firmware"; and the web UI from LittleFS when it
  is there, without Home Assistant, dashboard and backup (their routes are not registered: 404,
  the pages say so or leave the card out), with "Start the firmware" on its System page. See [ui/web/web-recovery.md](ui/web/web-recovery.md).
* **Not used**: compressed or delta OTA updates exist for ESP-IDF, but they need a custom
  bootloader or a patch component. The recovery app is simpler and also covers the recovery case.

### 9.6 Audio

Sound through an I2S amplifier and a speaker, on the boards that have one: the JC3248W535 has an
NS4168 amplifier (I2S in, no setup), the JC4880P443C an ES8311 codec with an amplifier; the WT32-SC01 and the T4 have none and build without audio.

* **Features that can be left out** (menu "hassctrl audio"), each on by default where the board
  offers audio:
  * `CONFIG_HC_AUDIO`: the component `audio`. A board offers it by `select HC_BOARD_HAS_AUDIO` in
    its Kconfig choice entry and by the pins in its `board::Info` (`audio_bclk_gpio`,
    `audio_ws_gpio`, `audio_dout_gpio`; pins stay in the board source, section 8). Without it the
    component has no sources and `main` and `devconsole` leave their calls out (`#if`).
  * `CONFIG_HC_AUDIO_TOUCH_SOUND`: the click and the slider tick.
  * `CONFIG_HC_AUDIO_MP3`: MP3 files (the alarm sounds), with the decoder below.
  * `CONFIG_HC_AUDIO_ALARM`: the alarm clock of Settings.
  * `CONFIG_HC_MIC`: the microphone (below), only where the board has one.
  * `CONFIG_HC_UI_PLAYER`: the media player (below): the media library of Home Assistant on
    every board with MP3, with `CONFIG_HC_SDCARD` also the card's files and playlists. It
    selects LVGL's PNG decoder (`LV_USE_PNG`) for the covers.

  The UI, which is shared with the Windows version, has no `#if` for the audio options: the
  Backend entries of a missing feature are nullptr (`main/ui_sound.cpp`), and Settings leave its
  page or switch out. The SD card's parts and the player are the exception: their Backend entries
  and pages exist only with `CONFIG_HC_SDCARD` and `CONFIG_HC_UI_PLAYER` (the simulator defines
  both).
* **A codec belongs to the board**, like the panel and touch drivers: `board::Info::audio_codec`
  says the sound goes through one, `board::audio_codec()` hands out `start(sample_rate)` and
  `stop()` (nullptr when it did not answer: then main starts no audio). `main` passes them with
  the MCLK pin to `audio::Config`; the audio task calls `start()` whenever its channel starts or
  changes the rate (the clock is running then) and `stop()` before it stops it. The JC4880P443C's
  ES8311 (`boards/jc4880p443c/es8311.cpp`) uses `espressif/esp_codec_dev` (Apache 2.0) for its
  control part only: I2S slave, MCLK 256 x the rate from the ESP32-P4, DAC at 0 dB, the volume in
  software as everywhere; the driver switches the amplifier (GPIO 11) with the output. The DAC's
  level is a board constant (`kDacDb`, 0 dB).
* **One task owns the I2S channel** (priority 5, above LVGL; 5 KB internal stack with MP3, of
  which decoding used 2.1 KB, 3 KB without): sounds are posted to a queue of four; the task takes
  the newest, which replaces the one that plays (quick taps do not queue up clicks). The task
  reads 128 samples at a time from a `Source` (memory, tone, the built-in beeps, an MP3 file or
  buffer)
  and looks for a newer sound between them. On both boards the channel runs all the time
  (`Info::audio_always_on`); between sounds the DMA sends zeros (`auto_clear`). Stopping it a
  second after the last sound made the amplifier's wake-up audible: the P4's first click after a
  pause was lost, the JC3248W535's click about a second after the last one was quieter. A board
  without the flag still stops it after a second, with 20 ms of silence before the first sound.
  Every sound is padded to whole DMA buffers, and the first one after a pause gets a buffer of
  silence before it: after a pause the I2S driver hands out the buffer the DMA is sending at that
  moment, and a sound written there lost its start. 16 bit mono, written to both slots, so the amplifier
  plays it whichever channel its CTRL pin selects. Eight DMA buffers of 128 frames (2 KB): a
  click starts within about 16 ms (between sounds the buffers are free, it goes into the next
  one), and music at 48 kHz has 21 ms to spare: with three (8 ms) a download's burst left the
  DMA without data a few times on the JC3248W535, with eight none in a minute with bursts.
* **Sample rate**: the built-in sounds are 16 kHz; a file plays at its own rate, the I2S clock
  follows it (`i2s_channel_reconfig_std_clock()` with the channel stopped). Stereo is mixed to
  mono.
* **Volume** in software, 0 to 100 % on a square curve (the NS4168 has no volume control); 0 is
  silent. On quick settings and on Settings, Sound. Stored in the settings NVS (`ui` / `sounds`)
  by `main`, which sets the audio volume at start and on every change.
* **An alarm sound comes first**: it plays at full volume whatever the volume is, and only
  another alarm sound or `stop_alarm()` replaces it; other sounds are dropped while it plays.
* **Touch sounds** (decided by the UI, played by `audio`), each with its own switch, both on by
  default; they play at the volume and are silent at 0 %:
  * the click, a 6 ms tick at 3 kHz, once per tap on a *tap target*: LVGL's input widgets
    (button, switch, slider, arc, button matrix, keyboard) and the plain objects marked with
    `sound::tappable()` (tiles, tile bars, rows with an action, the panel buttons, the light
    panel's bar and wheel). A tap on a container, a backdrop, the dashboard between tiles or on the
    waking touch, and the end of a drag (more than 10 px) make none;
  * the slider tick, 3 ms at 4.5 kHz and softer: a slider or arc ticks at every 1/25 of its range,
    a plain drag control marked with `sound::draggable()` (the light panel's bar) every 14 px, never
    two within 25 ms. The faster it moves, the more ticks (sim: 9 per second for a slow drag,
    33 for a fast one).

  The UI hooks LVGL's input device feedback (the input driver's `feedback_cb`, `sound.cpp`), which reports every
  event a touch sends; the sounds are decided there, not in each widget.
* **MP3** (`CONFIG_HC_AUDIO_MP3`): `espressif/esp_audio_codec` 2.6.2, its simple decoder with only
  the MP3 decoder registered. Chosen over `minimp3`: optimized for the ESP32-S3 and P4, and
  minimp3 decodes with its scratch (`mp3dec_scratch_t`, size not measured) on the stack, which
  has to be internal RAM here (the task reads files, and the flash cache is off while LittleFS
  reads). License: "Espressif Modified
  MIT", prebuilt libraries, allowed only on Espressif chips, which this firmware is. The ESP32-P4
  has no audio decoder in hardware (its codecs are JPEG and H.264); this library has optimized
  builds for it.
  * The file is read 2 KB at a time with `open`/`read` (a `FILE` would add stdio's buffer in
    internal RAM) into a PSRAM buffer; the decoder and the buffers exist only while a file is
    open. The code and tables are in flash, except 1.1 KB of Espressif's assembly hot loops in
    IRAM on the ESP32-S3.
  * Memory: the library allocates through weak `media_lib_module_malloc/calloc/realloc` and
    `media_lib_free`, which call `malloc`; every block under 16 KB went to internal RAM
    (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`, the default on the P4, on the S3 until 2026-10-25), 25 KB of the 33 KB left. `mp3_source.cpp` defines
    them and prefers PSRAM: while a file plays the internal RAM stays where it was (39 KB on the
    JC3248W535).
  * The dependency is fetched for the targets of the boards with audio (`target in [esp32s3,
    esp32p4]`), not by the option: the component manager fails on a `$CONFIG{...}` that a board
    without audio does not have. Without the option nothing of it is linked. It needs an ESP32-P4
    of revision 3.0 or later (the JC4880P443C is 3.01).
* **Music** (`CONFIG_HC_UI_PLAYER`, `play_media`, `play_stream`): the audio task has two
  places, a sound (click, tone, alarm, announcement) and the music, each with its own sources
  (two `Mp3Source`s, so an alarm's MP3 does not close the music's). A sound plays over the music,
  which waits meanwhile and goes on where it was when the sound ended (`MediaState::Interrupted`):
  an alarm or an announcement interrupts it, the touch clicks are dropped while there is music,
  playing or paused (a click would switch the I2S clock from 48 to 16 kHz and back). The music
  is a WAV file (16 bit PCM, mono or stereo, 8 to 48 kHz; `wav_source.cpp`), an MP3 file, or an
  MP3 that is downloaded while it plays, at the volume. Pause keeps it open and writes nothing
  (the DMA sends zeros). When it ends by itself the player is told (`set_media_ended`, on the
  audio task); a stop or a newer file does not. A recording of the microphone is refused while
  there is music (its rate is 16 kHz).
  * **Download** (`hass/src/media_audio.cpp`): a task per file (6 KB of stack, 8 KB with https,
    in PSRAM: it touches no flash) resolves the `media-source://` id (the address is kept until a
    connection fails), GETs it and writes what arrives into a `core::ByteRing` (1 MB of PSRAM,
    32 s at 256 kbit/s; one writer, one reader, atomics, host tested). It downloads in bursts:
    when the ring is full the connection is closed, and a new one (`Range`) fetches the next
    half once half of the ring was played, every 16 s at 256 kbit/s. A connection kept open
    with a full ring held its whole TCP window in Wi-Fi's receive buffers, internal RAM: 76 KB
    on the P4, and on the JC3248W535 it left 7 KB of internal RAM and no DMA memory (2026-10-23).
    With the bursts: 192 of 219 KB free on the P4, 14 to 15 of 16 KB on the JC3248W535. The audio task reads the ring and never waits on the network: it plays from
    32 KB on, and when the ring runs dry it waits again for 32 KB (`MediaState::Buffering`, the
    DMA sends zeros meanwhile, the task looks every 10 ms). A seek goes through the ring to the
    download, which fetches the file again from there (`Range: bytes=N-`, HTTP 206; a server
    that ignores the range has the bytes before read past); a connection that broke off is
    tried again from where it was, five times 2 s apart, then the music fails. The task lives
    until the audio task cancels the ring (the music ends or is replaced), also after the file
    is complete, for a seek back.
  * **Tags** (`formats/src/mp3_tags.cpp`, host tested): `formats::Mp3Reader` is fed the first
    bytes of a file or a download as they are read: the ID3v2 tag (2.2 to 2.4, ISO-8859-1,
    UTF-16 and UTF-8 texts; title, artist, album, track and the cover, the front cover first, up
    to 1 MB of tag) and the Xing/Info or VBRI header of the first frame. The tag's bytes are not
    handed to the decoder: a 70 KB PNG cover held bytes it took for frames. The length is the
    header's frame count, else the size without the tag over the first frame's bitrate
    (esp_audio_codec reports it in kbit/s, not bit/s as its header says); a seek uses the
    header's table, else the bitrate. The tags go to the screen once read (`media_tags`).
  * **Covers**: JPEG with the decoders of the camera images (`hass::decode_jpeg`), PNG with
    LVGL's lodepng (`main/ui_player.cpp`), cut to a square of the screen's size.
  * The state and the position are atomics the screen reads (`media_status`); `media_debug()`
    gives the channel's rate and the DMA underruns while music played (i2s `on_send_q_ovf`).
* **Player** (`components/player`): the queue, independent of the screen, so the next item
  starts also with no screen open. Items are a file of the card, a `media-source://` id or an
  address (a playlist's line). A file of the card plays its folder's MP3 and WAV files from it
  on (in the order of the names: without case, numbers by value, `core::natural_less`, host
  tested), a playlist (`.m3u`, `.m3u8`, `formats/src/m3u.cpp`, host tested: `#EXTINF` titles,
  paths relative to the playlist, `\` as `/`) its entries, an MP3 of the media library its
  folder's MP3s from it on (in the order the browser shows: folders first, then the names). A
  playlist of the library (`audio/x-mpegurl`, `.m3u`, `.m3u8`; `player::play_media_playlist`)
  is downloaded by the folder task below with `hass::stream_media` into a 64 KB ring (15 s at
  most): its paths are relative to the playlist's folder and become `media-source://` ids, its
  addresses stream as they are.
  * **A folder with its subfolders** (the play button of a folder in the media browser,
    `player::play_folder` / `play_media_folder`, console `player folder`): `player/src/tree.cpp`
    (host tested) walks depth first, each folder's files before its subfolders, both in the
    order of the names. A folder with a playlist gives the playlist's entries (the first one by
    name) in place of its files, its subfolders follow; a playlist that cannot be read leaves the
    files. What plays stops at once; a task (8 KB stack in PSRAM) lists the folders (the card's
    with `sdcard::list`, the library's with the blocking `hass::browse_media`, one request a
    folder, 10 s each) while the player says Buffering with the folder's name, then the queue
    starts at its first item. At most 1000 items, 8 levels below the folder, 100 folders of the
    library; a newer play or folder makes the walk's result void, an empty one is reported. Next, previous (within the first 3 s the one
  before, else the start of the item), shuffle (the item that plays first, the rest in a random
  order) and repeat off, all or one (`player::Queue`, host tested); shuffle and repeat are stored
  (`ui` / `player`, `set_blob_later`). An item that cannot be played is skipped with a message
  (the alert strip, or the player's page in Settings), three in a row stop the queue. Console
  `player`.
  * **Spectrum** (`spectrum.cpp`): the task copies every chunk of the player's file, before the
    volume, into a ring of 2048 samples in PSRAM; `media_spectrum()` takes the last 1024 on the
    caller's task (the screen, about 30 times a second), a Hann window, a radix-2 float FFT of
    its own (no esp-dsp), and the peaks of 16 bands spaced in octaves from 40 Hz to half the
    rate, -60 to 0 dB as 0 to 1. A bin is 43 Hz at 44.1 kHz, narrower than the lowest bands:
    every band gets at least one bin of its own, so these step a bin at a time. Tables and work
    arrays (16 KB) in PSRAM, made on first use.
* **Sounds in the file system image**: `sounds/` of the repository goes to `/fs/sounds/` of
  boards built with `CONFIG_HC_AUDIO_MP3` (`tools/build_fs.py`; a device's own files outside
  `/www` and `/sounds` are kept). The alarm sounds `sounds/alarms/*.mp3` are six alarm sounds of
  the Android Open Source Project (Apache License 2.0, `sounds/NOTICE`), converted by
  `tools/build_sounds.py` to mono 22.05 kHz 48 kbit/s MP3, 206 KB together.
* **Alarm clock** (`CONFIG_HC_AUDIO_ALARM`): up to 5 alarms (`ui::Alarms`), each with its time,
  days (none: once, then it turns itself off), snooze time, sound and Vibrate switch, stored
  together in the settings NVS (`ui` / `alarms`) at once. The single alarm of the firmware before
  2026-10-21 (`ui` / `alarm`) is read once into the first one, with the Vibrate switch of the
  Sound page. When an alarm's minute comes (`core::alarm_due`, plain C++ with host tests), the UI wakes the screen
  (from off or dimmed to the user's brightness, and the first touch is not swallowed; `power::wake()`
  again on every check while it rings, so the screen neither dims nor turns off), shows the alarm screen over everything and plays the sound in a loop at full volume until Stop
  or Snooze, at most 10 minutes. It needs the clock (SNTP); the time zone comes from Home
  Assistant or is fixed on the web UI's Time page. Two alarms in the same minute ring once, as the first of the list (every one of
  them without days turns off). One snooze at a time: it rings the snoozed alarm again, and the
  next alarm that rings ends it. A snooze lives in RAM only: a restart while snoozed forgets it.

* **Sounds from Home Assistant** (with `CONFIG_HC_AUDIO_MP3`): an automation fires
  the event `hassctrl_say` with a `message` and a text-to-speech `engine` (a tts entity;
  `language` and the engine's `options` map optional, e.g. Google Translate's `tld`; none of
  Google's sets the speed), or a `url` of an MP3; `device` (the host name, or a list of them) picks
  the devices, without it every one plays (`hass_protocol::decode_say`, host tested). Chosen over
  a `media_player` entity (it would need a DLNA renderer or an integration of its own; not planned) and over
  a web API on the device (Home Assistant would need its login); the event reuses the
  connection and the token. `docs/dashboard-sample/hass.yaml` shows the Home Assistant side,
  `docs/home-assistant/say.md` is the guide.
  * `hass/src/say.cpp`, a task that exists only while a sound is on its way (6 KB of stack, 8 KB
    with https or another server's address): a message goes to `POST /api/tts_get_url` with
    the event's options and `preferred_format: mp3` and one channel over them (Home Assistant
    converts what the engine makes), the
    path of the answer is fetched from the device's own address of Home Assistant (the `url` of
    the answer is built from HA's configured address). A path of the event is Home Assistant's,
    with the token; another server's address gets no token, also after a redirect (up to three,
    a new client each: `esp_http_client` kept its transport and sent plain HTTP to the https of
    a redirect). The whole MP3 goes into PSRAM, at most 1 MB (a sentence of Google's TTS is
    20 to 30 KB, under a second on the JC3248W535), so the audio task never waits on the network.
    A newer sound stops a download between two reads. A WAV is refused with a log line (no WAV support).
  * `audio::play_mp3` takes the buffer; the MP3 source decodes it in place and frees it at the
    end. It is an announcement: only an alarm or another announcement replaces it, the touch
    sounds are dropped while it plays, music waits and goes on after it; it plays at the volume
    (0: silent).
  * Console: `sound say <tts entity> <text>` and `sound url <url>` do what the event does.

**Microphone** (`CONFIG_HC_MIC`, menu "hassctrl audio", on by default where the board selects
`HC_BOARD_HAS_MIC`; the JC4880P443C): the ES8311's ADC with the analog MEMS microphone
MSM381A3729H9CP on MIC1P/N. For now only its settings and level, for Home Assistant Assist and an
intercom later.

* **Codec**: `esp_codec_dev` in ADC + DAC mode; `AudioCodec::set_mic(gain_db, auto_gain)`. A gain
  of 0 to 60 dB goes into the PGA first (REG14, 0 to 30 dB in 3 dB steps, the least noise), then
  the digital ADC scale (REG16, 6 dB steps), the half steps into the ADC volume (REG17)
  (`es8311_gain.hpp`, host test `mic_gain_*`; the steps are those of Linux's es8311 driver).
  Auto gain is the codec's ALC (REG18/19): it holds the output between -18.1 and -14.5 dBFS,
  0.25 dB per 64 LRCK (about 60 dB/s; at 256 LRCK the first second of a sound clipped), but it only turns the gain down from the one set (measured 2026-10-22: over
  30 dB a tone at -26 dBFS stayed there, over 54 dB one at -2 dBFS came down to -15). So it starts
  from 54 dB, which lifts a quiet room to about -46 dBFS; board constants in `es8311.cpp`. Fixed
  gain measured with the same tone: 0 / 15 / 30 / 42 / 54 dB gave -56 / -40 / -26 / -14 / -2 dBFS. The driver's start writes
  REG14 and REG17, so `start()` writes the microphone's registers again; a mutex keeps the audio
  task's start/stop and the UI's `set_mic` apart.
* **I2S**: a receive channel next to the speaker's on the same port (`i2s_new_channel` with both,
  `Config::din`), the left slot (REG44 puts the DAC's signal in the right one, for echo
  cancellation later). It runs with the speaker's clock and rate, so it needs
  `Config::keep_running` (the P4's `audio_always_on`); an MP3 at another rate restarts both.
* **Level**: a task `mic` (priority 5 like the audio task: the receive DMA holds only 24 ms, the
  speaker's buffers; 2.5 KB) reads 512 samples at a time only while someone
  listens (`mic_listen`), and keeps the largest one; `mic_peak_dbfs` hands it out and starts over.
  The meter of Settings, Sound listens while the page is open; console `mic [<s>|record|gain
  <db>|auto on|off|regs]`.
* **Test recording** (`mic_record_start/stop/cancel/state`): up to 5 s at 16 kHz into a 160 KB
  PSRAM buffer, allocated with the first recording and kept (the audio task plays from it). The
  mic task copies what it reads (not the first 200 ms, nor the last 150 ms after Stop: the tap's
  click was in the recording), and at the end posts the samples as an announcement, so a tap's
  click does not cut them off; it plays at the volume (none at 0, so the start is refused). A
  rate change of the speaker's channel (an MP3) ends the recording there. The state is Playing
  until the audio task has taken the samples and played them (`g_record_posted`,
  `g_record_plays`).
* **Settings**: `MicSettings {gain_db, auto_gain}` under `ui/mic` (default 42 dB, auto gain on),
  stored 10 s after the last change (section 9.3), applied at boot after the volume; backup
  `sound: mic_gain_db, mic_auto_gain`.

### 9.7 Vibration

A small vibration motor (ERM) for tactile feedback; for testing on the
JC4880P443C, GPIO 28 of the expansion header.

* **Wiring**: the motor through a transistor or a MOSFET with a flyback diode, the GPIO on its
  gate or base; high = on. A GPIO cannot drive a motor alone. The P4's test motor: 3 V, 70 mA,
  on 3.3 V through a 2N2222 with 470 Ohm at the base. The kitchen variant: the same motor through an
  AO3400 (N-MOSFET) with 300 Ohm at the gate and 6.2 kOhm from the gate to GND, which keeps it off
  while the pin floats at boot.
* **Option**: `CONFIG_HC_VIBRATION` (menu "hassctrl vibration"), on by default where the board
  offers it (`select HC_BOARD_HAS_VIBRATION`, the pin in `board::Info::vibration_gpio`, like the
  LDR of the WT32-SC01). Without it the component has no sources; the Backend entries are
  nullptr (`main/ui_sound.cpp`) and the UI leaves the switch out.
* **No task**: `vibration::pulse(ms)` turns the GPIO on and starts a one-shot `esp_timer` that
  turns it off; a newer pulse restarts it. A mutex guards the state between the callers and the
  timer's task. Each start notes when the timer is due; a callback that comes before that (it
  waited for the lock while a newer pulse restarted the timer) does nothing. `esp_timer_is_active()`
  cannot tell this: in ESP-IDF 6.1 it is also true inside the timer's own callback, and the first
  version, which asked it, never turned the motor off.
* **Taps**: the UI's touch feedback (section 9.6, touch sounds) also calls
  `Backend::vibrate_click` when a finger touches a tap target (LV_EVENT_PRESSED, once per touch),
  a pulse of `Info::vibration_click_ms`. The click sound comes when the finger lifts; the pulse
  does not wait for it, so a press that turns into a scroll or a drag vibrates too.
  Motors differ a lot (the 3 V test motor on the P4 does not move below 70 ms, it has 75 ms), so the
  length is a constant of the board, measured by the developer when the motor is added
  ([adding-a-board.md](adding-a-board.md) step 10), never a user setting. It follows
  neither the volume nor "Click on taps"; slider ticks do not vibrate (a motor cannot tick that
  fast).
* **Alarm**: `main` starts the pattern with the alarm sound (`play_alarm`, when the alarm's own
  Vibrate switch is on) and stops it with the sound: 500 ms on, 500 ms off. Pulses are dropped while it runs. The looping alarm runs until
  `stop_alarm()`. The preview of Settings plays once, so `start_alarm(active)` gets
  `audio::alarm_playing`: before each new pulse the motor asks it and ends when the sound has
  ended. A pulse that began is always played in full.
* **Setting**: "Vibrate" on Settings, Sound (on by default) for the taps; each alarm has a
  Vibrate switch of its own (on by default), which alone decides for it. Stored in the settings NVS (`ui` / `vibrate`) a few seconds
  after the change. The Sound page is there with audio or a motor.

### 9.7a Relays

Component `relay`: relay n (1 to 4) is a GPIO of the board (`Info::relay_gpios`, -1: none;
high = on, through a transistor or a relay board). Home Assistant switches it with an entity found by its
name, nothing to configure: `input_boolean.<device>_relay_<n>` or `switch.<device>_relay_<n>`, `<device>`
the host name with `_` (`hassctrl-7e7d` is `input_boolean.hassctrl_7e7d_relay_1`).

* **Subscription**: after `auth_ok` the hass task subscribes one `render_template` (hass_protocol
  `relay_template`): it reads the states of the named entities (relays 1 to 4 in both domains) as
  `{"1":"on","2":"off"}`; an entity that does not exist is left out. The ids are listed, not searched
  with `states.<domain>`: Home Assistant delays a template that watches a whole domain by up to a
  second (found 2026-10-15, a toggle took about 500 ms), one that names its entities renders at once.
  The result also is the initial state. A normal token is enough.
* **Handler**: `hass::set_relay_handler` (set by `main` when the board has a relay) hears of every relay
  that is new or changed; after a reconnect each entity is told once. Any state but `on`
  (`off`, `unavailable`, `unknown`) is off. A relay whose entity disappears keeps its state.
* **Pins**: low before they become outputs, so a relay is off while the board starts and until Home
  Assistant has answered. A relay with `Info::relay_pullup` (a module without its own pullup, closed
  while its input is high) is open drain with the internal pullup instead: closed while the board
  starts, low opens it. Console `relay [status|<n> on|off]` switches one by hand; Home Assistant's
  next change overrides it.
* Two entities with the same number (a switch and an input_boolean): the last one listed wins.

### 9.7b Motion sensor

Component `motion`: a PIR or radar on a GPIO of the board (`Info::motion_gpio`, -1: none;
`Info::motion_active_high`: the level of its output while it sees motion). The other way round
from the relays: the device tells Home Assistant. It turns the toggle helper
`input_boolean.<device>_motion` (`input_boolean.hassctrl_7e7d_motion`, the host name with `_`) on or
off; the user creates the helper (Settings, Devices, Helpers, Toggle), nothing is configured on the
device. No words of a language: a template binary sensor of device class motion on the helper (a UI
helper of Home Assistant) shows Detected / Clear in HA's language. The JC4880P443C: GPIO 34 of the expansion header, active low; its kitchen variant a radar on
GPIO 49, active high, its hallway variant a motion sensor on GPIO 28, active high. Setup in Home
Assistant: [home-assistant/motion-sensor.md](home-assistant/motion-sensor.md).

* **Pin**: an input with the internal pull toward the level that means clear (pull-down for an
  active high sensor, pull-up for active low), so an unplugged sensor reads clear. An esp_timer
  samples it every 100 ms; a level that holds for two samples is the state (a PIR holds its output
  for seconds, an interrupt would add nothing). That is the sensor's state (`motion::occupied`,
  what wake on motion uses).
* **Hold**: the detected state (`motion::detected`) turns on with the sensor and off
  only when the sensor saw nothing for the hold time; every motion restarts it, so a sensor that
  resets after 1-2 s does not flicker the helper. 1 to 60 min, default 5 min
  (`DisplaySettings::motion_hold_s`, Settings, Display, "Motion hold"; `ui_backend` hands it to
  `motion::set_hold_s` on loading and on every change, `main` loads it before the sensor starts).
  Every change of the detected state, and the first one, goes to the handler `main` gave it:
  `hass::publish_motion`.
* **Publishing**: a command of the hass task (`Motion`); the task remembers the last state, sends
  `input_boolean.turn_on` / `turn_off` (hass_protocol `motion_message`, `target.entity_id`) when it is
  connected and the state changed, and sends the last state again after every `auth_ok`, so the
  helper follows the sensor across reconnects. A normal token is enough.
* **Is the helper there**: Home Assistant answers the call with success also for an
  entity it does not have. So with the first state of a connection the task subscribes
  `{{ states('input_boolean.<device>_motion') }}` (render_template, hass_protocol `motion_template`):
  `unknown` is missing (logged once each time it goes missing, with the entity ID to create), a
  helper that appears gets the current state at once. `hass::Status::motion_helper` (no sensor,
  unknown, missing, found) and the entity ID go to `GET /api/hass` (`motion_helper`,
  `motion_entity`) and the web UI's Home Assistant page ("Motion helper"). A call that fails all
  the same (no permission) is logged once per connection.
* Console `motion [status|detected|clear]`: the sensor's and the held state with the hold time, or a state sent by hand to try the helper
  without the sensor; the sensor's next change overrides it.
* On the dashboard the tile goes on the template binary sensor: Detected / Clear by its device
  class and the timeline in its panel. A tile on the helper itself would be a switch tile.
* **Wake on motion** (Settings, Display, `DisplaySettings::wake_on_motion`, on by
  default): the UI's power check (every 200 ms) asks `Backend::motion` and counts motion as a
  touch for the dim and off times: a dimmed screen brightens, a screen that is off turns on without
  the wake-up blocker, and it stays on while the sensor sees motion. Its own time stamp, not LVGL's
  activity, so Settings still close after their time without a touch. The display settings blob
  grew to its fifth layout (older ones are converted, `wake_on_motion` keeps the default), with the
  motion hold to its sixth (`motion_hold_s` keeps the default).

### 9.8 Camera

The JC4880P443C's camera, an OV02C10 (2 MP) on the 15-pin MIPI-CSI connector
([devices/JC4880P443C](devices/JC4880P443C/JC4880P443C.md), "Camera"). Board option `HC_BOARD_HAS_CAMERA` (selected by the JC4880P443C); without
it the component has no sources and the Backend entries are nullptr.

* **Capture**: `esp_video` (V4L2) with the CSI controller and the ISP, which turns the sensor's
  RAW10 into RGB565 (1288x728, 30 fps), and esp_ipa's exposure, white balance and colour control
  with the OV02C10's tuning. The sensor driver is the vendor demo's (`components/camera/ov02c10`,
  Apache-2.0, its README lists the changes). A task on core 0 (the CSI's DMA interrupt shares the
  DW-GDMA interrupt group with the display's DPI channel, created on core 0) runs while someone
  holds the capture (`acquire` / `release`): three buffers, the newest frame kept dequeued so
  readers use it where the ISP wrote it. A frame that does not come for 1 s starts the capture
  again.
* **Settings** (`camera` namespace of the settings NVS): the picture (auto exposure, brightness,
  manual exposure and gain, mirror, flip, mains frequency), stored a few seconds after the last
  change, and the stream (on, size, frame rate, quality, login), stored at once. Mirror and flip
  move the sensor's readout window, so they are written only in standby: a change while it
  streams restarts the capture. The brightness and the mains frequency change a copy of the
  exposure control's tuning (`esp_video_isp_pipeline_set_ipa_config`). In the backup file as
  `camera:` (`main/backup.cpp`, only on a device with a camera; the stream's password is read,
  never written).
* **Web**: `/api/camera.jpg` and `/api/camera.mjpg` (HTTP Basic auth with the stream's login, 404
  while the stream is off; [ui/web/web-app.md](ui/web/web-app.md) section 4), registered through
  `webserver::Config::add_routes`, so the web server does not depend on the camera. The web UI's
  Camera page sets the stream through `/api/camera` and `webserver::CameraAccess` (filled in
  `main`, nullptr without a camera); the picture's settings stay on the screen. JPEGs come
  from the P4's encoder, made at most once per frame and shared; at half size the CPU shrinks the
  frame first. An MJPEG stream runs in a task of its own (`httpd_req_async_handler_begin`), at
  most two.
* **Preview** (Settings, Camera, Preview; `main/camera_preview.cpp`): LVGL redraws a full screen
  picture in bands with software rotation, about 50 ms into the frame buffer the panel scans, so a
  moving picture tore. The DPI panel has two frame buffers; while the preview runs, LVGL's flush
  writes its bands into both (around the picture's box) and a task scales and turns each camera
  frame in one pass into the box of the buffer not scanned and hands it over with `draw_bitmap`,
  which the DPI driver takes at the end of a frame. 30 fps without tearing; LVGL keeps running
  around it (the controls).
* **Console** (developer): `camera [mirror|flip on|off]`, `camreg <reg> [<value>]` (a sensor
  register, hex).

---

## 10. Repository layout

```
esp32-hassctrl/
  CMakeLists.txt                 ESP-IDF project
  sdkconfig.defaults             shared Kconfig defaults
  sdkconfig.main, sdkconfig.recovery (+ .esp32, .esp32p4)   per application and target
  bench/                         the benchmark dashboard and its version (docs/benchmark.md)
  partitions/                    4mb.csv, 16mb.csv
  boards/<name>/sdkconfig.defaults   target, flash, PSRAM, board choice
  boards/<name>/variants/<variant>/  one unit's add-ons: hc_board_variant.hpp, sdkconfig.defaults
  boards/jc4880p443c/c6/         ESP-IDF project of the board's ESP32-C6 (esp_hosted coprocessor)
  main/                          app_main and the wiring: ui_backend, ui_sound, backup, camera_preview,
                                 bench_mode (the benchmark's boot)
  recovery/                      the recovery app (section 9.5): its own app_main, same components
  components/
    <name>/      include/hc/<name>/*.hpp  src/   one per component of sections 3 and 9.4
    ui/          src/ flat: one file per screen, tile view, panel, Settings page; src/fonts/
    board/       include/hc/board/board.hpp  boards/<name>/  src/variant/ the variant that changes nothing
    camera/      ov02c10/ the vendor's sensor driver with its changes (README)
    esp_driver_ppa/  overrides the IDF's PPA driver: its sources, ppa_srm.c with the DIG-734 flag forced (README)
  host/
    CMakeLists.txt  the host build (MSVC), python tools/host_test.py builds and runs it
    tests/       unit tests for formats, core, config, hass_protocol, network's choice, the backup;
                 stubs/ of ESP-IDF headers
    sim/         the Windows version of the UI (section 1): its own CMake project (build/sim), the
                 stub Backend with the scenarios, lv_conf.h, the Windows driver; python tools/sim.py
  web/                           the web UI (index.html, app.js, dashboard.js, icons.svg)
  sounds/                        the alarm sounds (MP3, AOSP, Apache 2.0, NOTICE)
  tools/
    build.py, build_fs.py, update.py   firmware, file system image, update over the network
    build_web.py                       the web files for one firmware (board-only pages left out, Bootstrap purged, minified)
    host_test.py, sim.py, msvc.py      host tests, Windows version (msvc.py finds MSVC, cmake, ninja)
    test_dashboard.py, test_web.py     tests against a device
    screenshot.py                      the screen over the serial console (uidebug)
    build_icons.py, build_logo.py, build_sounds.py, build_timezones.py, gen_recovery_font.py
                                       generated files, committed
  docs/
    architecture.md              this file
    TODO.md, CHANGELOG.md
    ui/                          screen and web UI layouts, design system (see ui/README.md)
    adding-a-board.md            bring-up procedure for a new board
    dashboard-sample/            the dashboard format as YAML, a guide and a test file
    home-assistant/              guides for the Home Assistant side (timer, motion sensor, sounds)
    devices/<board>/             pins and datasheets
```

---

## 11. Conventions

* C++23 as supported by the ESP-IDF toolchain. No exceptions, no RTTI. Errors cross ESP-IDF
  boundaries as `esp_err_t` and pure code returns `std::expected<T, Error>`.
* One namespace per component (`hc::core`, `hc::hass`, ...). Public headers in
  `include/hc/<component>/`, everything else private to the component.
* Files stay small. A file over about 600 lines is a design smell to discuss, not a target.
  Split where code changes for another reason and needs none of the file's private state;
  accepted: `hass.cpp` (one session on one task), `tiles.cpp` (create and update of every tile
  kind, one switch each), `devconsole.cpp` (a development tool), and the files of one subject at
  612 to 745 lines (`layout.cpp`, `panel.cpp`, `dashboard.cpp`, `backup.cpp`, `history_view.cpp`,
  `picture_panel.cpp`, `orientation.cpp`, `main.cpp`, `edit_mode.cpp`).
* Memory: large buffers (message reassembly, images, store) in PSRAM via `heap_caps_malloc`;
  LVGL draw buffers in internal DMA RAM. No allocation per frame in the render path. On the
  ESP32 and the S3 a plain `malloc` up to 512 bytes goes to internal RAM first, anything larger to
  PSRAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` in `sdkconfig.main.esp32` and `.esp32s3`; the
  ESP-IDF default 16 KB filled the WT32's internal RAM, and the JC3248W535's: 10 KB left with Home
  Assistant connected, 42 KB with 512). On the S3 the IRAM code counts too: IRAM and DRAM are one
  SRAM, every byte of code in IRAM (Wi-Fi's `IRAM_OPT`, LVGL's fast functions) is a byte less of
  heap. Memory that must be internal (DMA, code that runs with the cache off) is asked for with
  `heap_caps_malloc`. A static scratch buffer that is used now and then (an import, the Wi-Fi
  page) is `EXT_RAM_BSS_ATTR`, in PSRAM (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`). The
  console command `mem` shows what is free.
  A buffer that is only faster in internal RAM (a JPEG decoder's tables and lines) takes it only
  while 32 KB stay free (`hass::detail::hot_alloc`), else PSRAM: Wi-Fi, lwIP and TLS allocate at
  any moment and fail where nobody looks. The same goes for libraries that take internal RAM
  first on their own: `esp_new_jpeg`'s buffers (29 KB for a 176x144 image) are linked through
  `hot_alloc` with `--wrap` (`hass/CMakeLists.txt`). Short peaks count too: the cJSON tree of the
  stored dashboard (147 tiles) took all internal 8-bit RAM at boot until cJSON went to PSRAM.
* The dashboard lives in PSRAM: layout, entity states and the LVGL objects of the dashboard.
  Small values that are read often (flags, state bits, pending marks) sit in a separate compact
  array in internal RAM, indexed like the tiles or entities.
* Comments explain why, not what. Behavior is specified in `docs/`, not in header comments.
