# Dashboard (LVGL)

Status: **running** on every board. The main screen: the frame with the pull-down and the alert
strip, the grid of tiles, their states, the arrangement per orientation and the edit mode. The
panels a tile opens are in [lvgl-panels.md](lvgl-panels.md), the camera image and streams in
[lvgl-camera.md](lvgl-camera.md), the gestures in [lvgl-touch.md](lvgl-touch.md). Colors, fonts
and sizes: [lvgl-design.md](lvgl-design.md). Carried over from `behavior.md` of the old firmware
(`kfc_fw/.../docs/hass_layout/behavior.md`). Code: `components/ui/src/dashboard.cpp` (pages,
store, actions), `tiles.cpp` (one view per tile), `icons.cpp` (glyphs), `overlay.cpp` (pull-down
and alert strip), `splash.cpp` (start screen), `edit_mode.cpp`.

## 1. Frame, pull-down and alert strip

The dashboard has no status bar and uses the whole screen (decided). Settings opens with a swipe
down that starts at the top center of the screen, like the Home Assistant app (decided). A
vertical drag that starts on a tile, such as the level drag of a dimmer, stays a tile gesture.

```
+------------------------------------------------------------+
|           . . . . swipe down from here . . . .             |
|                                                            |
|  +-----------+ +-----------+ +-----------+ +-----------+   |
|  |   tile    | |   tile    | |   tile    | |   tile    |   |
|  |           | |           | |           | |           |   |
|  +-----------+ +-----------+ +-----------+ +-----------+   |
|  +-----------+ +-----------+ +-----------+ +-----------+   |
|  |   tile    | |   tile    | |   tile    | |   tile    |   |
|  +-----------+ +-----------+ +-----------+ +-----------+   |
+------------------------------------------------------------+
```

The start zone is 160 px wide at the top center and 40 px high (80 x 24 was hard to hit, 2026-10-07). While
the alert strip is shown, its whole area also starts the pull-down (proposal).

### 1.1 Alert strip

Messages on the dashboard appear in the **alert strip** (decided): a panel at the top center,
touching the top edge, with rounded bottom corners and a background at 60 % opacity over the
dashboard. The same style is used for every error, warning and hint. The pull-down gesture works
through it, and a tap on it opens Settings at the matching page.

```
+------------------------------------------------------------+
|        |  ! Wi-Fi: wrong password - tap to fix  |          |
|        \________________________________________/          |
|  |   tile    | |   tile    | |   tile    | |   tile    |   |
|  |           | |           | |           | |           |   |
|  +-----------+ +-----------+ +-----------+ +-----------+   |
|  +-----------+ +-----------+ +-----------+ +-----------+   |
|  |   tile    | |   tile    | |   tile    | |   tile    |   |
|  +-----------+ +-----------+ +-----------+ +-----------+   |
+------------------------------------------------------------+
```

| Message | Shown while | Removed when |
| --- | --- | --- |
| Wi-Fi error, such as "Wi-Fi: wrong password" | the error exists | the error is gone |
| "Scanning for Wi-Fi ..." / "Connecting to Home-Net ..." | the device scans or connects (decided: shown on the display) | connected, or replaced by an error |
| Home Assistant error, such as "Home Assistant: the host does not answer" (running: after Wi-Fi problems, once it lasts 10 s, so a reconnect or a restart of Home Assistant does not flash it; a tap opens its Settings page) | the error exists | connected again |
| "Swipe down from the top for Settings" | after the first setup | the first pull-down once the onboarding is complete: Wi-Fi up and Home Assistant connected (decided 2026-10-10: while the start screen offers "Set up Home Assistant" the hint stays). Stored, so it does not come back after a restart. A tap leaves it in place |

The strip is as wide as its message needs, so it can get large. It keeps a small gap to the
left and right screen edges, so the rounded corners and the transparency stay visible (decided).
Proposals: a long message wraps to at most 3 lines; one message at a time, errors before warnings
before hints.

### 1.2 Start screen

Running on the JC4880P443C (user, 2026-10-09). From the start until Home Assistant has sent the values of the dashboard's first page, the
dashboard is covered by the Home Assistant logo (160 x 160, at the center, on the dashboard
background). The runner of an action on its way (docs/ui/lvgl/lvgl-dashboard.md section 4) goes round the
house, 5 px outside its edge, clockwise from the top. The dashboard is built under the logo, so it
is complete when the logo goes. The alert strip and the pull-down work as usual (both lie above
it); the first setup opens over it as before. A touch on the logo does nothing.

* The logo goes when Home Assistant is connected and the values of the page came, or 10 s after
  the connection when they do not come (a page without entities).
* Without a dashboard nothing more comes: once Home Assistant is connected the runner stops and a
  line below the logo says "No dashboard yet. Create one in the web UI at http://...". The logo
  goes once a dashboard is saved and its values came.
* Without a Home Assistant address and token (a factory reset, a backup carries no token) there
  is nothing to wait for: once Wi-Fi is connected the runner stops and a button "Setup Home
  Assistant" below the logo opens Settings at the Home Assistant page (the QR code of the web UI);
  `<` leads to quick settings as usual. When the token is saved the button goes and the runner
  starts again. Without Wi-Fi the runner keeps going.
* It is shown once per start: a lost connection later shows the alert strip over the dashboard.

The backlight stays off from the start until LVGL has drawn the first frame, then fades up in
0.5 s (power.hpp); before it the panel would show what it held before the start. Before every
restart (board, `esp_restart()` shutdown handler) the backlight turns off and the panel is
switched off and put to sleep: on the JC4880P443C the backlight pin is undriven for about 1.3 s
until the next start takes it, and the ST7701S showed cyan without a signal.

The logo comes from the icon of dashboardicons.com, rendered by `tools/build_logo.py` into two
4-bit alpha masks (the white tree, the blue face; 25 KB) and the runner's track.

## 2. Grid

The dashboard uses the whole screen; the pull-down at the top center opens Settings
(section 1). A page is its grid of tiles, 8 px from the screen edges, 4 px
between tiles (8 px on the JC4880P443C's 800x480, `Options::tile_gap`, 2026-10-09), radius 8. Portrait uses the transposed grid and the tile spans turned with it
(docs/architecture.md section 5, "Orientation"). Turning the display builds the page again.

```
+------------------------------------------------------------+
| +-----------+ +-----------+ +-----------+ +-----------+    |
| |           | | [+ + + +] | |  (icon)   | |  (icon)   |    |
| |           | |  Heating  | |  Crystal  | |  Breeze   |    |
| |   24 %    | |   21.5°   | +-----------+ +-----------+    |
| |###########| | (t) 20.8° | +-----------+                  |
| |##Ceiling##| |  Hearth   | |  (icon)   |     spacer       |
| +-----------+ | [- - - -] | |  25.4 °C  |                  |
|               +-----------+ +-----------+                  |
| +-------------------------+ +-----------+ +-----------+    |
| |     (camera)  Owl       | | (folder)  | |  (icon)   |    |
| +-------------------------+ +-----------+ +-----------+    |
+------------------------------------------------------------+
```

An area page has the back tile (arrow) in its first cell; a tap goes to the parent page.

Boards with `Info::ui_slide_pages` (the JC4880P443C, `hc::ui::Options::slide_pages`): an area page
slides in from the right over 250 ms (ease out) and pushes the page that was shown out to the left;
the back tile slides the parent in from the left. The leaving page takes no taps while it moves.
Any other page change (a new layout, turning the display) has no slide.

## 3. Tiles

| Tile | Content, top to bottom | Tap |
| --- | --- | --- |
| switch, light without `dimmer` | icon (48 px), name | toggles |
| button, scene, script | icon (48 px), name | press / turn on |
| sensor | icon (24 px), value with unit, name | opens the sensor panel with the history |
| lock | icon (24 px), the state in words (`Locked`, `Unlocking`, `Jammed`), name | opens the lock panel; a tap on the tile never locks or unlocks |
| dimmer (light with `dimmer`) | the level as a fill from the bottom, the percentage in the middle, the name at the bottom | drag up or down sets the level on release, double tap toggles, a single tap opens the light panel ([lvgl-panels.md](lvgl-panels.md) section 2) |
| climate 1x2 | `+` bar, action ("Heating", the mode when the entity has none), setpoint, room temperature, name, `-` bar | the bars step the setpoint; the steps are sent 0.7 s after the last tap; a tap beside the bars opens the climate panel |
| climate 1x1 | one line in the small font: the action and the room temperature (`Heating • 21.5°`); setpoint, name | opens the climate panel |
| picture (camera) | the camera image over the whole card, the name centered on a dark band at its bottom; the camera icon and the name until the first image arrives | shows the image full screen |
| area | folder (or the tile's icon), name | opens the page |
| spacer | nothing | |

Sizes: a cell at least 90 px tall and wide gets the large fonts (value 28 px, name 16 px, icon
48 px), smaller cells the small ones (value 20 px, name 12 px, icon 24 px). A value that is too
wide steps down through the font sizes (`612 ppm`, `Detected` in a 74 px cell). A name that is too
long ends in `...`.

## 4. States and colors

| State | Look |
| --- | --- |
| on (switch, light, binary sensor `on`) | filled with the active color (`0xff8000`), white icon and name |
| off | card `0x182029`, border `0x2b3844`, icon and name `0x7fa8c9`; the off glyph of the domain |
| pending (an action on its way) | a cyan (`0x00ffff`) dot with a glow and a short fading tail runs clockwise round the border, 220 px a second on every card size (`runner.cpp`; each frame redraws only the dot's stretch of the border); the expected value shown at once |
| unavailable (`unavailable`, `unknown`) | card `0x161d25`, value `--` |
| no value yet | value `...` |
| dimmer | the fill is the indicator, the card stays dark |
| climate | the bars are orange while the mode is not `off`, the action is orange |
| lock | filled in the color of its state (your choice, 2026-10-11): unlocked, unlocking, open, opening green (`0x4caf50`), locked and locking orange (`0xff8000`), jammed red (`0xff0000`); `theme::kLock*` |

A light that is switched on shows the level it had before (HA turns it on at that level), not
100 %; the store keeps the last level while the light is off. A light not seen on since the start
shows `On` until HA reports the level.

A pending action ends when the entity reports the expected state (a level within 3 of 255, a
setpoint within 0.05), when the call fails, or after 5 s; then the tile shows the entity's state
again.

Texts: binary sensors in the words of their device class (`Open`/`Closed`, `Detected`/`Clear`,
`Wet`/`Dry`, `Unlocked`/`Locked`), other states with a capital and spaces (`heat_cool` is `Heat cool`), numbers with
the tile's `decimals` or HA's display precision. A state of several lines shows its first line.

## 5. Icons

The tile's `icon`; else the entity's `icon` attribute when the glyph is in the firmware; else the
default of the domain and device class (light `lightbulb`/`lightbulb-off`, outlet `power-plug`,
motion `motion-sensor`, temperature `thermometer`, lock `lock`/`lock-open`/`lock-alert` (jammed),
...; `icons.cpp`). The glyphs are the 44 names of `core` plus the ones the screens draw themselves
(`UI_GLYPHS`: `arrow-left`, `plus`, `minus`, `chart-timeline-variant`, ...), in 24 and 48 px (`tools/build_icons.py`, which also writes `web/icons.svg` for the
web editor from the same names).

## 6. Arrangement per orientation

Decided 2026-10-08 (your request), built: a tile keeps its own cell in landscape and in portrait.
Tiles are moved in the edit mode (section 7) and in the web editor
([web-dashboard-editor.md](../web/web-dashboard-editor.md)).

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

## 7. Edit mode

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
  tile can go there (the tiles in the way are pushed aside, section 6), red when not. Release on
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

### 7.1 Where the code goes

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
* `core`: `move_tile`, `reset_arrangement`, `remove_tile` (section 6).
* `dashboard::save_layout` writes the copy through `save_json` (JSON out and in again, so the
  stored dashboard is what the file holds); `Backend::save_dashboard` calls it.
* **The write runs in the LVGL task** (changed from the proposal "off the UI task"): a worker task
  needs a stack in internal RAM (a LittleFS write cannot run from a PSRAM stack), which is the
  scarce memory. It is one write of the JSON (about 10 KB for a large house) per edit session;
  to be measured on the boards.
* Spacers are not drawn on the screen, also not while editing: they are pushed aside like any tile
  but cannot be picked up or removed there (web editor only).
* The tiles keep their look while editing (values included), with an outline; the bar is 44 px.

## 8. Open points

* Numeric sensors are never highlighted; the old rule ("on while a number is reported") would
  make every sensor orange. Proposal: keep it so.
* The climate default step is 0.5 for both units until the unit of the HA configuration is read.
* A failed service call ends the pending look; it is not shown in the alert strip yet.
* The height of the pull-down start zone, the line limit and the message order of the alert
  strip (section 1).
* **Push aside or swap**: push aside is built first; decided after trying it on the device. On
  a small page (4x2) it moves tiles the user did not touch.
* **Moving between pages on the screen**: not proposed (drag onto the back tile could move a tile
  to the parent page later).
