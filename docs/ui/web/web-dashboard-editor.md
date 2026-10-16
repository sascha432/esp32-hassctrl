# Dashboard editor (web)

Status: **decided and built** (2026-10-08/09): the API, the web editor (`web/dashboard.js`) and
the entity picker; 2026-10-14: sizes and hiding per orientation, the tray of the tiles not on the
screen, the swipe checkbox of an area (the mock-up page of the first design is gone, the editor on a
device is the reference).

Dashboard editing has three parts:

1. **Positions per orientation** in the dashboard format: [lvgl-dashboard.md](../lvgl/lvgl-dashboard.md)
   section 6.
2. **Web UI editor** on the Dashboard page, next to the import and export of the backup (this
   document).
3. **Edit mode on the screen**: [lvgl-dashboard.md](../lvgl/lvgl-dashboard.md) section 7.

## 1. What it does

The Dashboard page has two tabs: **Editor**, and **Import & export** (the YAML import with
includes, check only, and the YAML export: what the page has today). The backup of settings and
dashboard as one file is on the System page (web-app.md section 1), not here (decided 2026-10-08).

* **Pages.** A tree of the main page and its areas (up to `core::kMaxAreaDepth` deep). Add an area
  (its page opens at once, 2026-10-09: an empty area is an error, and a "+ Tile" after "+ Area"
  meant the new area, not the page it is on), rename, delete (with its tiles, after a confirm that names the count), move a tile to another
  page by dragging it onto the tree.
* **Grid.** `defaults.grid`, and per area page its own grid or the default. 1x1 to 32x32 (2026-10-14, your request; was 8x8); a tile is 1x1 to 8x8.
* **Tiles.** Add from a Home Assistant entity, add an area, a spacer or a clock, remove, duplicate, move to
  another page, reorder. A clock (2026-10-16) has no entity: style (digital, analog), 24 or 12 h,
  seconds, date, rim and ticks of an analog face, and the time zone (a text field with the
  device's list of IANA names as suggestions; an unknown name is refused by the device's check).
  The preview shows a fixed time or the clock glyph. Every key of the tile format; the keys a kind does not have are hidden.
  A new tile is 1x1. The size is set per orientation, in the orientation the preview shows
  (1x1 up to the grid of the page; portrait offers "landscape turned" as the default), and a
  tile can be hidden per orientation (two checkboxes). A weather entity (`weather.*`) is a
  `weather` tile with the icon of the weather by default (the preview cannot show the condition:
  no live values).
* **Swipe navigation** is a checkbox of an area tile: its page loses the back tile, a swipe
  right goes back, dots show the depth ([lvgl-dashboard.md](../lvgl/lvgl-dashboard.md)
  section 9). The preview draws the back tile and the dots by the same rules as the screen.
* **Preview.** The page as the screen draws it (colors of lvgl-design.md section 2), the back
  tile where the navigation rule puts one, the dots of a swipe strip page, landscape or
  portrait, in the aspect of a board. A tile wider than tall shows the icon beside the text (the
  wide shape of the screen). The preview shows whether the page is in automatic order or
  arranged in that orientation.
* **The tray** under the preview, "Not on the screen in landscape" (or portrait): every tile of
  the page that is not drawn there, with the reason: **hidden** there, or **no room** (no free
  block, a size larger than the grid, a cell outside the grid, on the back tile or over another
  tile; the device leaves such a tile out instead of refusing the file, decided 2026-10-14).
  A tile dragged into the tray is hidden in that orientation; a chip dragged onto a cell is shown
  again and takes that cell (the tiles in the way are pushed aside). A click on a chip selects
  the tile; the inspector's Position line says why it is out and offers the other orientation.
* **A size change** keeps the cell; a tile that no longer fits appears in the tray, nothing is
  shifted (before 2026-10-14 `refit()` tried to keep it on the page and the asymmetry of the
  rules showed in your reports of 2026-10-13). The same for a grid change: a cell outside the
  new grid puts its tile into the tray, the cell stays for a grid that fits it again.
* **Drag in the preview**: in automatic order a drag reorders the list (the tile takes the place of
  the tile it is dropped on: after it when dragged forward, before it when dragged back); in an
  arranged page it moves the tile to the cell and pushes the tiles there aside ([lvgl-dashboard.md](../lvgl/lvgl-dashboard.md) section 6). A drag in an automatic page onto an empty cell
  arranges the page (the same rule). Up/Down buttons do the same without dragging, for
  phones.
* **Save** of the whole document with its revision. The screen stays on the page it shows (the
  page of the same area tile, else its nearest parent that is left; 2026-10-09, also for an
  import and the screen's Done). Unsaved changes are marked, Undo within the
  session, a warning before leaving the page.

## 2. Data flow

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
  `"cells": {"landscape": [column, row] or null, "portrait": the same}`, where the device draws
  it (null: hidden or no room). The reader skips `cells`, so an
  answer can be sent back as it is. A problem answers `{"error", "path"}` for a data error (the
  placement never is one): the path is the tile's index in each level of tile lists
  (`config::tile_path`), because a new tile has no id the editor could match.
  `GET /api/dashboard/editor` gives the device's screen in landscape (name, width, height, the
  gap between tiles) and the icon names.
* **The preview is the device's own screen** (2026-10-09, your review): no choice of board; the
  screen comes from `GET /api/dashboard/editor`. Its name and size are not shown (2026-10-09,
  your request).
* **No live values in the preview** (decided 2026-10-09): it shows the icons and names.
* **Conflicts.** The screen's edit mode changes the dashboard too. A `409` shows "The dashboard
  was changed on the device" with Reload (drops the local changes) and Overwrite.

## 3. Entities from Home Assistant

The web UI never talks to Home Assistant itself (the token is write-only). The device answers from
its WebSocket session.

* **Not `get_states`.** A large house sends 1 MB and more; the ESP32 boards have about 55 KB of
  internal RAM free. The list comes from a `render_template` that filters on Home Assistant's side:
  `GET /api/hass/entities?domain=&q=` answers
  `{"more": false, "rows": [["light.kitchen","Kitchen","light",null,"%"], ...]}` (id, friendly
  name up to 40 characters, domain, device class, unit), in order of the id. Every word of `q` has
  to be in the id or the name (case-insensitive). At most 100 rows and about 10 KB (built: 200 rows
  would not fit the 16 KB a message of the connection may have); `more` says rows were left out.
* **One-shot templates** (built): the hass task subscribes the template, takes the first result
  and ends the subscription; the web server's task waits for it, 6 s at most (503 with the reason
  otherwise: no connection, template error, timeout). One at a time, a newer one replaces it; the
  screen's entity search (edit mode, [lvgl-dashboard.md](../lvgl/lvgl-dashboard.md) section 7.2)
  shares the slot, its answer goes to the UI instead of a waiting task. The
  search text and the entity id go in as `variables` of `render_template`, so nothing is escaped
  into the template. Template and result handling are in `hass_protocol/picker.cpp` (host tests).
* **Details of one entity** for a new tile: `GET /api/hass/entity?id=` answers `{"id", "name",
  "domain", "state", "icon", "device_class", "unit", "color_modes", "temp_step"}` or
  `{"id", "missing": true}`. The editor sets `dimmer: true` for a light whose
  `supported_color_modes` has more than `onoff`, and nothing else: icon, device class, unit,
  rounding (`states(e, rounded=True)`, the display precision) and a climate's step already follow
  Home Assistant at run time, so keys equal to the automatic value are left out (no `name` when it
  is the friendly name). Not built from the proposal: the display precision (it is in the entity
  registry, not in the state; the screen rounds with it already).
* **The picker** (built): the Add dialog and the Pick button of a tile's Entity field. A domain
  filter, the search (300 ms after the last key), the rows with friendly name, id, device class,
  unit, the kind and whether the entity is on the dashboard already. A typed valid entity id is
  offered as a row of its own, also when Home Assistant does not have it (the tile is added with a
  warning). Without a connection the list holds the entities of the dashboard, with the reason.
  The friendly names of the answers name the tiles in the preview and the inspector while the page
  is open (they are not stored).

## 4. Page layout

Phone first like the rest of the web UI, but the editor uses the full width from 800 px.

```
+--------------------------------------------------------------------------------------+
| hassctrl-1a2b                                                  [Wi-Fi] [HA]          |
+-----------+--------------------------------------------------------------------------+
| Status    | Dashboard   [Editor] [Import & export]   rev 12 · unsaved [Undo][Save]  |
| Home Ass. +--------------+------------------------------------+----------------------+
| Wi-Fi     | Pages        | Main page  grid [4]x[3]            | Tile                 |
| Hotspot   | > Main       | [Landscape|Portrait]               | Ceiling      light   |
| Dashboard |   > Rooms    | Automatic order   [Reset]          | Entity [light.aur..] |
| Logs      |     Workshop | +------+------+------+------+      | Name   [Ceiling    ] |
| System    | [+ Area]     | |      |Hearth| (o)  | (o)  |      | Icon   [(o) auto   ] |
|           |              | | 24 % |21.5° |Crystal Breeze|      | Size in landscape    |
|           |              | |######|      +------+------+      |        [1x2        v]|
|           |              | +------+------+ 25.4 |      |      | [ ] Hidden landscape |
|           |              | | Owl (2x1)   |Rooms |Movie |      | [x] Hidden portrait  |
|           |              | +-------------+------+------+      | [x] Dimmer  Step 5   |
|           |              | Not on the screen in landscape     | Position  auto / 0,0 |
|           |              | [ (o) Pantry  no room ] [ Fan hid.]| [Up][Down][Dup][Del] |
+-----------+--------------+------------------------------------+----------------------+
```

Below 800 px the columns stack: the page as a dropdown, the preview, the tile form.

## 5. Firmware work

| Where | What |
| --- | --- |
| `core` | `landscape`/`portrait` placements (size, cell, hidden) and the overflow rule in `finish()`; `move_tile`, `reset_arrangement`, `remove_tile`; `swipe` of an area and its helpers (built) |
| `config` | the two maps and `swipe` in JSON and YAML (read, write, export), format version 2; the sample in `docs/dashboard-sample/` documents them (built); `tools/convert_dashboard.py` for version 1 files |
| `dashboard` | `save_layout()` (built) |
| `ui` | edit mode ([lvgl-dashboard.md](../lvgl/lvgl-dashboard.md) section 7, built) |
| `api_dashboard.cpp` | `POST /api/dashboard?check=1`; answers `{"checked", "revision", "dashboard"}` with the cells per tile (null where a tile is not drawn), on 400 `{"error", "path"}` (built) |
| `api_hass.cpp` | `GET /api/hass/entities?domain=&q=`, `GET /api/hass/entity?id=` (built) |
| `api_dashboard.cpp` | `GET /api/dashboard/editor`: the screen of the device, `core::icon_names()` and the time zone names for the clock tile (built) |
| `tools/build_icons.py` | also writes `web/icons.svg`, one `<symbol>` per icon name (about 6 KB gzipped) |
| `web/` | `dashboard.js`, loaded only by the Dashboard page, so the setup on a phone stays small |
| tests | host tests for the placement and `apply()`; `tools/test_dashboard.py` for check=1, cells, the problem's tile id |

## 6. Steps

1. **Format**: positions in `core` and `config`, host tests, sample.
2. **Screen edit mode**: `apply()`, `save_edit()`, the gestures. Reviewed in the Windows version
   first (`tools/sim.py`), then on the boards.
3. **API**: check=1, cells, structured problem, icons list.
4. **Web editor without Home Assistant**: pages, grid, tiles with a typed entity id, preview with
   both orientations, save, 409, undo; the Import & export tab moves in.
5. **Icons** (`web/icons.svg`) and the **entity picker** (built 2026-10-09).
6. **Version 2** (2026-10-14): sizes and hiding per orientation, overflow instead of errors,
   the tray, the swipe checkbox of an area; the mock-up page removed.
7. Adding tiles, areas and clocks on the screen (2026-10-16, [lvgl-dashboard.md](../lvgl/lvgl-dashboard.md) section
   7.2, with the picker's template). Later: sizes on the screen. Not planned: live values in the web preview
   (decided 2026-10-09).

## 7. Open points

* **Save model of the web editor**: explicit Save (proposed, one flash write per session) or save
  2 s after each change.
* **Validation in the browser**: only what the forms enforce (ranges, entity id characters, name
  length); every other rule only on the device through `check=1`, so the rules are not written
  twice.
