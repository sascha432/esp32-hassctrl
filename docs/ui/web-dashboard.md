# Dashboard editing: web UI editor and screen edit mode

Status: **decided** (2026-10-08; push aside is judged on the device, section 1). Built
(2026-10-08/09): the format (section 1), the edit mode of the screen (section 3, approved), the
API (section 2.2) and the web editor (`web/dashboard.js`, typed entity ids); the icon file and
the entity picker are not. The look can be tried in a browser:
[dashboard-editor-demo.html](dashboard-editor-demo.html) (open the file directly, no device
needed; mock data, nothing is saved). The screen's edit mode is not in it: it is tested on the
device.

Three parts:

1. **Positions per orientation** in the dashboard format (section 1). Decided 2026-10-08 (your
   request): a tile keeps its own cell in landscape and in portrait. This replaces "`position:` is
   dropped" and "no moving of tiles for now" (architecture.md section 5, TODO "Order per
   orientation", both 2026-10-07); those texts change when this is built.
2. **Web UI editor** on the Dashboard page, next to the import and export of the backup
   (section 2).
3. **Edit mode on the screen**: long press starts it, tiles are moved by drag, a long press on a
   tile offers to remove it. No adding of tiles on the screen yet (section 3).

## 1. Positions in the format

* A tile has two optional cells, `landscape` and `portrait` (column, row from 0). Portrait keeps
  the transposed grid (4x3 becomes 3x4, no portrait grid of its own) and the swapped span, as
  today; its cell is a cell of that transposed grid.
* Placement of a page, per orientation: first every tile **with** a cell takes it; then the tiles
  **without** one fill the first free block in list order (today's first-fit). A page where no tile
  has a cell is placed exactly as today, so existing files do not change.
* Rules (`core::finish`): a given cell must lie inside the grid with the whole span, must not
  overlap another tile, and must not be the back tile's cell of an area page. Each is an error in
  both orientations, naming the tile. A tile without a cell that finds no room is an error in
  landscape and falls back to the first cell in portrait (as today).
* **Moving writes the whole page.** When a tile is moved (screen or web), every tile of that page
  gets its current cell in that orientation, not only the moved one. The page then stays as it was
  arranged, also when a tile is added later (it takes the first free block). The other orientation
  is not touched.
* **A drop onto other tiles pushes them aside** (decided 2026-10-08 for a first try, judged on
  the device; the alternative is a swap of two tiles of the same span). The moved tile takes its
  block; every tile it covers takes the first free block in reading order from the drop cell on,
  wrapping to the start of the page, in their old order. When one of them finds no block, the drop
  is refused (red outline). The same rule (`core::apply`) serves the screen and the web editor.
* **Reset arrangement** (web and screen): removes the cells of a page in one orientation, back to
  automatic order.
* A grid change that leaves a cell outside the grid: the web editor offers to reset the
  arrangement of that page; the device refuses a file with such a cell (error, as above).
* Spacers stay: in automatic order they keep a cell free; in an arranged page an empty cell needs
  none, but a spacer still works there.
* JSON: `"landscape": [2, 1], "portrait": [0, 3]` (like `size`). YAML: `landscape: 2,1`
  (column,row), to keep sizes `cols x rows` and positions apart at a glance.
* `core::Tile`: `has_landscape`, `has_portrait` next to the `Cell`s that `finish()` fills, and
  `portrait_fallback` (no block in portrait, drawn at the first cell). The edits are pure functions
  of a finished layout: `move_tile`, `reset_arrangement` and `remove_tile` (host tests in
  `host/tests/layout_test.cpp`). `remove_tile` keeps the entity table, so the entity store of the
  screen stays valid while editing; the next read of the stored file drops unused entities.

## 2. Web UI editor

The Dashboard page has two tabs: **Editor**, and **Import & export** (the YAML import with
includes, check only, and the YAML export: what the page has today). The backup of settings and
dashboard as one file is on the System page (web.md section 2), not here (decided 2026-10-08).

### 2.1 What it does

* **Pages.** A tree of the main page and its areas (up to `core::kMaxAreaDepth` deep). Add an area,
  rename, delete (with its tiles, after a confirm that names the count), move a tile to another
  page by dragging it onto the tree.
* **Grid.** `defaults.grid`, and per area page its own grid or the default. 1x1 to 8x8.
* **Tiles.** Add from a Home Assistant entity, add an area or a spacer, remove, duplicate, move to
  another page, reorder. Every key of the tile format; the keys a kind does not have are hidden.
* **Preview.** The page as the screen draws it (colors of design-system.md section 2), the back
  tile, landscape or portrait, in the aspect of a board. A tile that does not fit shows the error.
  The preview shows whether the page is in automatic order or arranged in that orientation.
* **Drag in the preview**: in automatic order a drag reorders the list (the tile takes the place of
  the tile it is dropped on: after it when dragged forward, before it when dragged back); in an
  arranged page it moves the tile to the cell and pushes the tiles there aside (section 1). A drag in an automatic page onto an empty cell
  arranges the page (the rule of section 1). Up/Down buttons do the same without dragging, for
  phones.
* **Save** of the whole document with its revision. Unsaved changes are marked, Undo within the
  session, a warning before leaving the page.

### 2.2 Data flow

```
GET /api/dashboard ---> editor model (the JSON as it is) ---> edits in the browser
                                                                  |
        409: reload or overwrite  <---  POST /api/dashboard?revision=N  (whole document)
        400: problem on tile/page  <---           |
        200: new revision, cells   <---           v
                                        config::read_json -> core::finish -> store -> ReloadConfig
```

* **The browser edits the JSON document, the device validates it.** No per-tile API: one
  validator (`core::finish`) for every path, and the device never merges partial changes.
* **New tiles have no `id`.** `finish()` gives them one; the save answer returns the stored
  document, so the editor goes on with the device's ids and revision.
* **The placement in the browser is a preview.** A copy of the placement of `layout.cpp` in JS
  (about 40 lines) redraws at once on a drag. `POST ...?check=1` validates without storing; the
  editor calls it 1 s after the last change and shows the device's problem. A difference is a bug
  in the copy.
* **The editor copies rules of the firmware** (the placement, the sizes of a kind, the kinds of the
  domains): a change to the dashboard in the firmware changes `web/dashboard.js` in the same
  commit, and the web image goes onto the devices with the firmware (docs/architecture.md
  section 5, README "Flash").
* **Answers** (built): save and check answer `{"checked", "revision", "dashboard"}`; the dashboard
  is the one stored (or checked) with the ids of new tiles, and every tile carries
  `"cells": [landscape column, row, portrait column, row, portrait fallback 0/1]`. The reader skips
  `cells`, so an answer can be sent back as it is. A problem answers `{"error", "path"}`: the path
  is the tile's index in each level of tile lists (`config::tile_path`), because a new tile has no
  id the editor could match. `GET /api/dashboard/editor` gives the device's screen in landscape
  (name, width, height, the gap between tiles) and the icon names.
* **The preview is the device's own screen** (2026-10-09, your review): no choice of board; the
  screen comes from `GET /api/dashboard/editor`.
* **No live values in the preview** (decided 2026-10-09): it shows the icons and names.
* **Conflicts.** The screen's edit mode changes the dashboard too. A `409` shows "The dashboard
  was changed on the device" with Reload (drops the local changes) and Overwrite.

### 2.3 Entities from Home Assistant

The web UI never talks to Home Assistant itself (the token is write-only). The device answers from
its WebSocket session.

* **Not `get_states`.** A large house sends 1 MB and more; the ESP32 boards have about 55 KB of
  internal RAM free. The list comes from a `render_template` that filters on Home Assistant's side:
  `[["light.kitchen","Kitchen","light",null,"%"], ...]` (id, friendly name, domain, device class,
  unit), filtered by domain and search text, at most 200 rows, on the large heap.
* **Details of one entity** for a new tile: icon, device class, unit, display precision, whether
  a light dims (`supported_color_modes` other than `onoff`), a climate's `target_temp_step`. The
  editor sets `dimmer: true` for a dimmable light; keys equal to the automatic value are left out
  so the tile follows Home Assistant (no `name` when it is the friendly name).
* Without Home Assistant the picker takes a typed entity id (`core::valid_entity_id`).

### 2.4 Page layout

Phone first like the rest of the web UI, but the editor uses the full width from 800 px.

```
+--------------------------------------------------------------------------------------+
| hassctrl-1a2b                                                  [Wi-Fi] [HA]          |
+-----------+--------------------------------------------------------------------------+
| Status    | Dashboard   [Editor] [Import & export]   rev 12 · unsaved [Undo][Save]  |
| Home Ass. +--------------+------------------------------------+----------------------+
| Wi-Fi     | Pages        | Main page   grid [4]x[3]           | Tile                 |
| Hotspot   | > Main       | [Landscape|Portrait] [WT32 480x320]| Ceiling      light   |
| Dashboard |   > Rooms    | Automatic order   [Reset]          | Entity [light.aur..] |
| Logs      |     Workshop | +------+------+------+------+      | Name   [Ceiling    ] |
| System    | [+ Area]     | |      |Hearth| (o)  | (o)  |      | Icon   [(o) auto   ] |
|           |              | | 24 % |21.5° |Crystal Breeze|      | Size   [1x2 auto v] |
|           |              | |######|      +------+------+      | [x] Dimmer  Step 5   |
|           |              | +------+------+ 25.4 |      |      | Position  auto / 0,0 |
|           |              | | Owl (2x1)   |Rooms |Movie |      | [Up][Down][Dup][Del] |
|           |              | +-------------+------+------+      |                      |
+-----------+--------------+------------------------------------+----------------------+
```

Below 800 px the columns stack: the page as a dropdown, the preview, the tile form.

## 3. Edit mode on the screen

```
+------------------------------------------------------------+
|  Editing Main page · landscape   [Reset]  [Cancel] [Done]  |   edit bar instead of the pull-down
| +-----------+ +-----------+ +-----------+ +-----------+    |
| |  ·    ·   | |           | |  (icon)   | |  (icon)   |    |   tiles dimmed, dashed border,
| |  Ceiling  | |  Hearth   | |  Crystal  | |  Breeze   |    |   no values, no actions
| |           | |           | +-----------+ +-----------+    |
| |           | |           | +- - - - - -+ +-----------+    |   the target block of a drag:
| |           | |           | |  (free)   | | Moonbase  |    |   green when it fits, red when not
| +-----------+ +-----------+ +- - - - - -+ +-----------+    |
| +-------------------------+ +-----------+ +-----------+    |
| |       Owl               | |  Rooms >  | |  Movie    |    |
| +-------------------------+ +-----------+ +-----------+    |
+------------------------------------------------------------+
```

* **Start**: a long press (1000 ms, decided 2026-10-08, without moving more than 10 px) anywhere on a dashboard page,
  on a tile or on an empty cell. No tile uses a long press today (only the steppers of the panels
  repeat on hold), and a dimmer's drag starts with movement, so they do not collide. The tile under
  the finger does not act (no toggle, no panel). The bar and the outlines of the tiles show the
  start.
* **While editing**: the tiles keep their look with an outline; taps do nothing except on an
  area tile, which opens its page in edit mode too, and the back tile, which goes back. The
  pull-down for Settings is off; the edit bar (44 px) takes the top and the grid shrinks by its
  height while editing, so no tile is covered. Pages slide as usual.
* **Move**: drag a tile; it follows the finger, the block under it is outlined, green when the
  tile can go there (the tiles in the way are pushed aside, section 1), red when not. Release on
  green moves it, on red it snaps back. The page becomes arranged in the current orientation.
* **Remove**: a long press on a tile while editing (without moving) asks "Remove Ceiling?" with
  Remove and Cancel. An area names the tiles of its page that go with it. The back tile cannot be moved
  or removed.
* **Reset** sets the page back to automatic order in this orientation (after a confirm).
* **Done** saves; **Cancel** drops all changes of this edit session. The changes of every page
  visited are saved together, one write.
* **Leaving** (decided 2026-10-08): only a tap on Done or Cancel ends edit mode. There is no
  timeout, and turning the display keeps it open (the edit goes on in the new orientation). The
  session lives in RAM until Done: a reset or a power loss drops it. A new layout from the web UI
  or an import while editing drops the session too (it would conflict) and says so in the alert
  strip.
* No adding of tiles and no change of sizes on the screen yet.

### 3.1 Where the code goes

Built differently from the first proposal (a list of operations replayed by `dashboard`): the
session is a **copy of the layout**, edited by the pure functions of `core`, and Done stores the
copy. Same behavior, one data path fewer.

* `ui/edit_mode.cpp`: the long press that starts it (`edit::watch` on every card, its step bars and
  the page), the edit layer over the page (a transparent object on the screen that takes every
  touch, so the tiles' own gestures need no changes), the bar, the drag with its outline (green
  0x2ecc71 or the error red), the remove and reset dialogs (`settings::confirm`).
* `ui/dashboard.cpp`: `start_editing` keeps the layout the edit started from and draws a copy;
  `set_edited` swaps in the next copy; `save_editing` (Done) and `stop_editing` (Cancel). The grid
  starts below the bar while editing; the cards get an outline (`apply_look` sets the border on
  every update, the outline stays). A new layout from outside while editing drops the copy and
  says so ("Edit dropped"). Done reopens the stored dashboard on the same page.
* When edit mode starts, the pressed tile is deleted with its page; LVGL then ends that press, so
  the tile does not act on the release, and a finger that stays down goes on as a press on the
  edit layer: hold and drag in one move works.
* `core`: `move_tile`, `reset_arrangement`, `remove_tile` (section 1).
* `dashboard::save_layout` writes the copy through `save_json` (JSON out and in again, so the
  stored dashboard is what the file holds); `Backend::save_dashboard` calls it.
* **The write runs in the LVGL task** (changed from the proposal "off the UI task"): a worker task
  needs a stack in internal RAM (a LittleFS write cannot run from a PSRAM stack), which is the
  scarce memory. It is one write of the JSON (about 10 KB for a large house) per edit session;
  to be measured on the boards.
* Spacers are not drawn on the screen, also not while editing: they are pushed aside like any tile
  but cannot be picked up or removed there (web editor only).
* The tiles keep their look while editing (values included), with an outline; the bar is 44 px.

## 4. Firmware work

| Where | What |
| --- | --- |
| `core` | `landscape`/`portrait` cells and their rules in `finish()`; `move_tile`, `reset_arrangement`, `remove_tile` (built) |
| `config` | the two keys in JSON and YAML (read, write, export); the sample in `docs/dashboard-sample/` documents them (built) |
| `dashboard` | `save_layout()` (built) |
| `ui` | edit mode (section 3, built) |
| `api_dashboard.cpp` | `POST /api/dashboard?check=1`; answers `{"checked", "revision", "dashboard"}` with cells per tile, on 400 `{"error", "path"}` (built) |
| `api_hass.cpp` | `GET /api/hass/entities?domain=&q=`, `GET /api/hass/entity?id=` |
| `api_dashboard.cpp` | `GET /api/dashboard/editor`: the screen of the device and `core::icon_names()` (built) |
| `tools/build_icons.py` | also writes `web/icons.svg`, one `<symbol>` per icon name (about 6 KB gzipped) |
| `web/` | `dashboard.js`, loaded only by the Dashboard page, so the setup on a phone stays small |
| tests | host tests for the placement and `apply()`; `tools/test_dashboard.py` for check=1, cells, the problem's tile id |

## 5. Steps

1. **Format**: positions in `core` and `config`, host tests, sample.
2. **Screen edit mode**: `apply()`, `save_edit()`, the gestures. Reviewed in the Windows version
   first (`tools/sim.py`), then on the boards.
3. **API**: check=1, cells, structured problem, icons list.
4. **Web editor without Home Assistant**: pages, grid, tiles with a typed entity id, preview with
   both orientations, save, 409, undo; the Import & export tab moves in.
5. **Icons** (`web/icons.svg`) and the **entity picker**.
6. Later: adding tiles and sizes on the screen. Not planned: live values in the web preview
   (decided 2026-10-09).

## 6. Open points

* **Push aside or swap**: push aside is built first; decided after trying it on the device. On
  a small page (4x2) it moves tiles the user did not touch.
* **Moving between pages on the screen**: not proposed (drag onto the back tile could move a tile
  to the parent page later).
* **Save model of the web editor**: explicit Save (proposed, one flash write per session) or save
  2 s after each change.
* **Validation in the browser**: only what the forms enforce (ranges, entity id characters, name
  length); every other rule only on the device through `check=1`, so the rules are not written
  twice.
