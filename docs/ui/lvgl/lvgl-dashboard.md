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
the alert strip is shown, its whole area also starts the pull-down (built).

### 1.1 Alert strip

Messages on the dashboard appear in the **alert strip** (decided): a panel at the top center,
touching the top edge, with rounded bottom corners and a background at 90 % opacity (60 % until 2026-10-13) over the
dashboard. The same style is used for every error, warning and hint on the screen, also over the
panels and the camera view (decided 2026-10-12: no popups for messages; the rules are in
[lvgl-design.md](lvgl-design.md) section 8). It **slides down from the top** when it appears
(250 ms, easing out) and back up when it goes (2026-10-12). The pull-down gesture works through
it. A tap on a state message opens Settings at the matching page; a tap on an event message
closes it.

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
| Event messages of `overlay::show()`, one short line, the reason in the log: "Stream stopped, retrying", "Stream loading ..." and "⟳ Stream failed: <reason>" (the camera view, [lvgl-camera.md](lvgl-camera.md) section 2), "Changes not saved", "Tile not removed", "Edit dropped" (the edit mode, section 7) | 5 s; the stream's until its state changes, "Stream failed" until a try (every 10 s, or a tap) reaches the server; before the state messages, a newer one replaces it | the time is over, or a tap (on "Stream failed": one more try) |

The strip is as wide as its message needs, so it can get large. It keeps a small gap to the
left and right screen edges, so the rounded corners and the transparency stay visible (decided).
A long message wraps (two lines in portrait are common), at most 3 lines, then it ends in "..."
(decided 2026-10-14). One message at a time: an event message,
then a Wi-Fi problem, connecting, a Home Assistant problem, the hint (built). While the dashboard
is edited the strip shows only event messages, over the edit bar; a drag on it does not open
Settings then.

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
(section 1). A page is its grid of tiles (1x1 to 32x32, `core::kMaxGrid`; 2026-10-14, your request, was 8x8), 8 px from the screen edges, 4 px
between tiles (7 px on the JC4880P443C's 800x480, `Options::tile_gap`, 2026-10-09), radius 8. Portrait uses the transposed grid and the tile spans turned with it
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

An area page has the back tile (arrow) in its first cell; a tap goes to the parent page. An
area with `swipe` has none: a swipe to the right goes back (section 9).

An area page slides in from the right over 250 ms (ease out) and pushes the page that was shown
out to the left; the back tile and the swipe to the right slide the parent in from the left
(section 9). The leaving page
takes no taps while it moves. Every board slides since 2026-10-14 (the swipe fires the slide;
`Info::ui_slide_pages = false` switches without the animation); before, only the JC4880P443C
did. Any other page change (a new layout, turning the display) has no slide.

## 3. Tiles

| Tile | Content, top to bottom | Tap |
| --- | --- | --- |
| switch, light without `dimmer` | icon (48 px), name; a light that is on and reports a level shows it under the icon ("44 %", the icon then 24 px) | toggles once the double-tap window (300 ms) has passed; a double tap opens the light panel when the light dims (a color mode other than `onoff`, or a reported level; [lvgl-panels.md](lvgl-panels.md) section 2), else the sensor panel with the on/off timeline (section 4) |
| button, scene, script | icon (48 px), name | press / turn on |
| sensor | icon (24 px), value with unit, name | opens the sensor panel with the history |
| lock | icon (24 px), the state in words (`Locked`, `Unlocking`, `Jammed`), name | a double tap locks an unlocked or open lock and unlocks a locked one (2026-10-16, your request; was: never from the tile); jammed or unavailable it opens the panel instead; a single tap opens the lock panel once the double-tap window (300 ms) has passed |
| dimmer (light with `dimmer`) | the level as a fill from the bottom, the percentage in the middle, the name at the bottom; off or unavailable the icon over the name, as a light without `dimmer`; the same in every shape | drag up or down sets the level on release, double tap toggles, a single tap opens the light panel ([lvgl-panels.md](lvgl-panels.md) section 2) |
| climate, tall | `+` bar, action ("Heating", the mode when the entity has none), setpoint, room temperature, name, `-` bar | the bars step the setpoint; the steps are sent 0.7 s after the last tap; a tap beside the bars opens the climate panel |
| climate, wide | the `-` bar down the left, the `+` bar down the right; between them the stack of the tall look when the card is at least 110 px high inside the padding, else the one-cell line | as the tall one |
| climate, one cell | one line in the small font: the action and the room temperature (`Heating • 21.5°`); setpoint, name | opens the climate panel |
| weather, one cell | icon (24 px) beside the temperature, the condition and the humidity in one line (`Cloudy • 81%`), name | opens the weather panel ([lvgl-panels.md](lvgl-panels.md) section 6) |
| weather, tall | icon (48 px), the temperature, the condition, the humidity with its drop glyph, name | opens the weather panel |
| weather, wide | the large icon left; right of it the temperature (28 px), the condition and the humidity in one line, the name | opens the weather panel |
| switch, button, sensor, lock, area, wide | the icon left (48 px on a large cell, else 24), right of it the value (sensor, lock) and the name | as the one-cell tile |
| picture (camera) | the camera image over the whole card, the name centered on a dark band at its bottom; the camera icon and the name until the first image arrives; any shape | shows the image full screen |
| area | folder (or the tile's icon), name | opens the page |
| spacer | nothing | |
| clock, digital | the icon when the tile has one (24 px), the time in the largest digit font that fits the width and leaves the lines below their room (72 or 48 px digits, `theme::font_digits_*`, then the text fonts; chosen once, so the digits stay put), with `hours: 12` AM/PM beside it, with `seconds` the seconds; the date under it (`date: true`, "Fri 16 Oct"); the name when the tile has one | nothing |
| clock, analog | the face (an `lv_meter`: hour and minute hand with round ends, with `seconds` a second hand in the active color, `ticks` none / quarter / hour (default) / minute as its scales, `border: true` a rim), the date and the name below it; wide: the face as tall as the card at the left, name and date right of it. A face that would be under 40 px drops the date, then the name; under 30 px of radius the minute ticks become hour ticks | nothing |

A clock has no entity (`type: clock`, [architecture.md](../../architecture.md) section 5). It ticks
by a timer of its own, once a second (every 250 ms with seconds), and redraws only when the shown
minute or second changed. `zone` (an IANA name, checked at import) is evaluated by
`core::local_time`; without it the clock shows the device's zone through the backend, the same
time as the status bar. While the clock is not set: `--:--`, the hands at twelve with a muted hub.

Sizes and shapes (decided 2026-10-14, built the same day, `tiles::create` takes the span):
every tile is 1x1 unless its size says otherwise, any kind takes any size up to the grid, per
orientation (section 6). The look follows the shape in the shown orientation: more rows than
columns and at least 180 px high is **tall**, more columns than rows and at least 180 px wide
is **wide**, everything else (one cell, 2x2, a 1x2 too small for the tall look) is the one-cell
look in the card. A portrait screen turns a 1x2 climate into a wide 2x1 with the bars left and
right by itself. The fonts follow the size of the card (your report 2026-10-14: a 2x2 area on an
8x7 grid of the P4 had the small glyph): a card at least 90 px wide and tall (a tall one twice
as tall) gets the large fonts (value 28 px, name 16 px, icon 48 px), smaller cells the small ones (value
20 px, name 12 px, icon 24 px). A value that is too wide steps down through the font sizes
(`612 ppm`, `Detected` in a 74 px cell). A name that is too long ends in `...`. Reviewed in the
sim at 480x320, 320x480 and 800x480 (every kind in the three shapes). Open: a 2x2 switch on the
P4 looks empty around its 48 px glyph (no larger glyph size).

## 4. States and colors

| State | Look |
| --- | --- |
| on (switch, light, binary sensor `on`) | filled with the active color (`0xff8000`), white icon and name |
| off | card `0x182029`, border `0x2b3844`, icon and name `0x7fa8c9`; the off glyph of the domain |
| pending (an action on its way) | a cyan (`0x00ffff`) dot with a glow and a short fading tail runs clockwise round the border, 220 px a second on every card size (`runner.cpp`; each frame redraws only the dot's stretch of the border); the expected value shown at once |
| unavailable (`unavailable`, `unknown`) | card `0x161d25`, value `--` |
| no value yet | value `...` |
| dimmer | the fill is the indicator, the card stays dark; the name is white on the fill |
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
...; `icons.cpp`; a weather entity: the glyph of its condition, as the Home Assistant app has
them: `weather-sunny`, `weather-partly-cloudy`, `weather-pouring`, ...). The glyphs are the 60 names of `core` plus the ones the screens draw themselves
(`UI_GLYPHS`: `arrow-left`, `plus`, `minus`, `chart-timeline-variant`, ...), in 24 and 48 px (`tools/build_icons.py`, which also writes `web/icons.svg` for the
web editor from the same names).

## 6. Placement per orientation

Decided 2026-10-08 (cells per orientation) and 2026-10-14 (sizes and hiding per orientation,
overflow instead of errors; your request), built in `core` and `config`: everything about where
and how large a tile is lives per orientation. Tiles are moved in the edit mode (section 7) and
in the web editor ([web-dashboard-editor.md](../web/web-dashboard-editor.md)).

* A tile has two optional maps, `landscape` and `portrait` (`core::Placement`), each with
  `size` (columns x rows, 1x1 to 8x8, `core::kMaxTileSize`; default 1x1 in landscape, in portrait the landscape size
  turned, so a tile keeps its cell count and turns with the grid unless told otherwise), `cell`
  (column, row from 0; without it the tile takes the first free block) and `hidden` (the tile is
  not placed, drawn or subscribed in that orientation). Portrait keeps the transposed grid (4x3
  becomes 3x4, no portrait grid of its own); its cells are cells of that transposed grid.
* Placement of a page, per orientation (`core::finish`): first every tile **with** a cell takes
  it; then the tiles **without** one fill the first free block in list order (first-fit). A page
  where no tile has a cell is in automatic order.
* **Nothing about placement is an error.** A tile whose cell lies outside the grid, on the back
  tile or over another tile (the earlier tile in list order keeps the cell), a tile without a
  free block, or one larger than the grid is not shown in that orientation
  (`Placement::shown == false`): the device draws nothing for it, the page's subscription
  leaves it out, the web editor lists it under the preview ("no room") and a drag onto a cell
  brings it back. Room made later (a tile removed, hidden or moved) brings an automatic tile back
  by itself. Before 2026-10-14 every one of these refused the whole file, and a portrait tile
  without room was drawn over the first cell; both are gone.
* **Moving writes the whole page.** When a tile is moved (screen or web), every shown tile of
  that page gets its current cell in that orientation, not only the moved one. The page then
  stays as it was arranged, also when a tile is added later (it takes the first free block). The
  other orientation is not touched. Hidden tiles and tiles without room keep no cell.
* **A drop onto other tiles pushes them aside** (decided 2026-10-08 for a first try, judged on
  the device; the alternative is a swap of two tiles of the same span). The moved tile takes its
  block; every tile it covers takes the first free block in reading order from the drop cell on,
  wrapping to the start of the page, in their old order. When one of them finds no block, the drop
  is refused (red outline): nothing vanishes into the overflow by a move. The same rule
  (`core::move_tile`) serves the screen and the web editor; the editor also drops a tile that is
  not on the screen onto a cell (hidden: shown again).
* **Reset arrangement** (web and screen): removes the cells of a page in one orientation, back to
  automatic order. It cannot fail: a tile without room in list order is left out.
* A grid change that leaves a cell outside the grid puts that tile into the editor's tray;
  nothing else changes, the cell stays for a grid that fits it again.
* Spacers stay: in automatic order they keep a cell free; in an arranged page an empty cell needs
  none, but a spacer still works there. A hidden spacer is allowed.
* The page of an area with `swipe` (section 9) has no back tile: its first cell is free.
* YAML and JSON (`docs/dashboard-sample/`, format version 2):

  ```yaml
  - entity: climate.hearth
    landscape:
      size: 1x2
      cell: 2,1
    portrait:
      hidden: true
  ```

  `"landscape": {"size": [1, 2], "cell": [2, 1]}, "portrait": {"hidden": true}` in JSON. The
  keys of version 1 (`size: 1x2`, `landscape: 2,1`) are refused with a message that says where
  they went; `tools/convert_dashboard.py` converts a version 1 file once.
* `core::Tile::sides[2]`, one `Placement` per orientation (`tile.in(o)`): `has_size`, `has_cell`,
  `hidden` from the file, `size`, `cell` and `shown` filled by `finish()`. The edits are pure
  functions of a finished layout: `move_tile`, `reset_arrangement` and `remove_tile` (host tests
  in `host/tests/layout_test.cpp`). `remove_tile` keeps the entity table, so the entity store of
  the screen stays valid while editing; the next read of the stored file drops unused entities.
* The web API's answers carry `"cells": {"landscape": [column, row] or null, "portrait": the
  same}` per tile, where the device draws it (null: not shown there).
* `swipe: true` on an area tile: section 9.

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
  start, and a click and a pulse (`sound::confirm()`) confirm it. The hold that started it does
  not count as a remove press; the finger can drag the tile it is on straight away.
* **While editing**: the tiles keep their look with an outline; a tap on the back tile goes back,
  on a tile opens its title and options (an area too: it does not open), on a "+" cell adds a tile
  (section 7.2). Areas do not open, there is no swipe and the dots are hidden (2026-10-14). The
  pull-down for Settings is off; the edit bar (44 px) takes the top and the grid shrinks by its
  height while editing, so no tile is covered. Pages slide as usual.
* **Press**: the tile under the finger is outlined orange until the finger lifts (confirmed by you on the P4, 2026-10-14).
* **Move**: drag a tile; it follows the finger, the block under it is outlined, green when the
  tile can go there (the tiles in the way are pushed aside, section 6), red when not. Release on
  green moves it, on red it snaps back. Every cell the tile snaps to ticks like a slider
  (`sound::snap()`, follows the slider sound switch). The page becomes arranged in the current orientation.
* **Remove**: a long press on a tile while editing (without moving) asks "Remove Ceiling?" with
  Remove and Cancel; the dialog opens with a click and a pulse. An area names the tiles of its page that go with it. The back tile cannot be moved
  or removed.
* **Add**: every free cell shows a dashed border with a "+" (the web editor's look; not on a
  spacer's cell or the back tile); a tap on one adds a tile, an area or a clock there in three
  steps (section 7.2).
* **Reset** sets the page back to automatic order in this orientation (after a confirm); a tile
  without room in list order is left out of the screen, not refused (2026-10-14).
* **Done** saves; **Cancel** drops all changes of this edit session. A save or a remove that
  fails says so in the alert strip ("Changes not saved"), over the edit bar; the reason is in
  the log. The changes of every page
  visited are saved together, one write.
* **Leaving** (decided 2026-10-08): only a tap on Done or Cancel ends edit mode. There is no
  timeout, and turning the display keeps it open (the edit goes on in the new orientation). The
  session lives in RAM until Done: a reset or a power loss drops it. A new layout from the web UI
  or an import while editing drops the session too (it would conflict) and says so in the alert
  strip.
* No change of sizes on the screen yet. Tiles hidden or without room in
  the shown orientation are not part of the edit: a tile cannot be moved into or out of the
  overflow on the screen (web editor only). A sideways drag on an empty cell goes back from a
  swipe page (section 9).

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
  the tile does not act on the release. LVGL may send the edit layer nothing more of that finger
  (it waits for the release; seen on the WT32, 2026-10-16), so a timer reads the input device
  until the finger is up (`compat::pressed`); until then a release is no tap.
* `ui/add_dialog.cpp`: the steps of adding (section 7.2); `ui/entity_picker.cpp`: the entity search
  and the page frame of the steps, children of the edit layer. The "+"
  cells are one object of the edit layer under the bar that draws every free cell itself
  (`LV_EVENT_DRAW_MAIN`: four dashed lines and a "+"; LVGL 8 has no dashed border, and a 32x32
  grid would be a thousand objects).
* `core`: `move_tile`, `reset_arrangement`, `remove_tile` (section 6), `add_tile` (`NewTile`: an
  entity's tile, a clock, or an area with its first tile), `change_tile`.
* `dashboard::save_layout` writes the copy through `save_json` (JSON out and in again, so the
  stored dashboard is what the file holds); `Backend::save_dashboard` calls it.
* **The write runs in the LVGL task** (changed from the proposal "off the UI task"): a worker task
  needs a stack in internal RAM (a LittleFS write cannot run from a PSRAM stack), which is the
  scarce memory. It is one write of the JSON (about 10 KB for a large house) per edit session;
  to be measured on the boards.
* Spacers are not drawn on the screen, also not while editing: they are pushed aside like any tile
  but cannot be picked up or removed there (web editor only).
* The tiles keep their look while editing (values included), with an outline; the bar is 44 px.

### 7.2 Adding a tile

Built 2026-10-16 and confirmed by you in landscape and portrait on every board (your requests: a
search field to find an entity; the "+" cells of the web editor;
"ask first for the title and type, if a tile is added a second dialog comes up for the entity,
after selection a third dialog for options"). Every free cell of the page shows a dashed border
with a "+"; a tap on one opens the steps, each a page over the edit layer with the edit bar's look:

```
1  New tile                  [Cancel] [Next]     2  Entity of the tile    [Back]
   (Tile) ( Area ) ( Clock )                        [ ceil              ] [kbd]
   [ title               ] [kbd]                    | Attic Ceiling   light.attic_ceiling |
   q w e r t y u i o p ...  (OK: Next)              | Kitchen Ceiling ✓ light.kitchen_... |

3  Options of Kitchen        [Back] [Add]        4  An area: First tile of Kitchen [Back]
   Size in landscape (1x1) (2x1) (1x2) (2x2)        the search of step 2; a row adds
   Light:  Dimmer               [on]                the area with that tile
   Sensor: Decimals (Auto) (0) (1) (2)
   Clock:  Analog / 12 hours / Seconds / Date
   Area:   Swipe navigation     [off]   [Next]
```

* **Start**: a tap on a "+" cell (not in the gap between cells). No steps when the cell cannot
  take a 1x1 tile: a spacer there says "A spacer keeps this cell free" (spacers are not drawn),
  a full page "No room for a tile here". In the alert strip.
* **1. Title and type**: Tile (of an entity), Area or Clock, and the title (the tile's `name`;
  empty: Home Assistant's friendly name, or no name for a clock; an area needs one). OK on the
  keyboard is Next. Tile and Area need the Home Assistant connection for the search; without it
  the step says so.
* **2. Entity** (a tile): the search. Home Assistant filters with the template of the web
  editor's picker (`hass_protocol/picker.cpp`, [web-dashboard-editor.md](../web/web-dashboard-editor.md)
  section 3), at most 30 rows (about 60 LVGL objects); every word in the id or the friendly name,
  so "light kitchen" filters by domain (no domain filter, decided with you). The field starts
  empty: the first entities by id. A search goes out 400 ms after the last key, or at once on OK.
  A ✓ marks an entity a tile shows already. A tap on a row asks for its details (a light's color
  modes) and goes on.
* **3. Options** (decided with you, the rest stays in the web editor: icon, unit, step, stream,
  time zone, the clock's border and ticks): the size in the shown orientation (the sizes the grid
  has room for), a light's `dimmer` (preset from Home Assistant: more color modes than `onoff`),
  a sensor's decimals, the clock's style, 12 hours, seconds and date, an area's swipe navigation.
  Add puts it into the edited copy; a problem (no room for 2x2 there) stays on the step.
* **4. First tile** (an area): an area page needs a tile (`finish()`), so the area comes with
  its first one, found by the same search. The page stays where it is.
* **Placement**: `core::add_tile` appends the tile to the page and moves it to the tapped cell
  like a drag (`move_tile`): the other tiles keep their cells, the page becomes arranged in this
  orientation. The new tile shows its values at once: the screen's entity store grows with the
  edited copy's entity table.
* **Changing a tile** (2026-10-16, your request: "when I tap a tile I need to adjust title and
  options and entity"): a tap on a tile in edit mode opens the same steps with its values. Step 1
  is the title (the placeholder is the name it shows now; the type stays, an entity's domain
  decides it), then the options with Save, and for an entity's tile an **Entity** row on top that
  opens the search; the entity picked comes back to the options. Another domain changes the kind
  (`core::change_tile`: the keys the new kind does not have go). A size or decimals beyond the
  usual choices (set in the web editor) is one more choice, so it is not lost unseen; the keys the
  screen does not offer stay as they are. A larger size keeps the cell and pushes the tiles in the
  way aside; "no room for 2x2 there" stays on the step.
* **Back** goes one step back with what was chosen; **Cancel** (step 1) drops it all. The keyboard
  button beside a field hides the keyboard (your request: landscape has little room), the field or
  the button brings it back.
* **Answers** come back through `take_event` as `core::EntityList` and `core::EntityDetails`,
  stamped with a generation; the screen drops older ones. The search shares the one query slot of
  the hass task with the web editor's picker: a newer request of either replaces the one on its way.
* **Memory**: the answer is parsed in the hass task into PSRAM; on the WT32 the internal heap was
  the same with the steps open (38 KB free, 2026-10-16).

## 8. Open points

* Numeric sensors are never highlighted; the old rule ("on while a number is reported") would
  make every sensor orange. Proposal: keep it so.
* The climate default step is 0.5 for both units until the unit of the HA configuration is read.
* A failed service call ends the pending look; it is not shown in the alert strip yet.
* The height of the pull-down start zone (section 1).
* **Push aside or swap**: push aside is built first; decided after trying it on the device. On
  a small page (4x2) it moves tiles the user did not touch.
* **Moving between pages on the screen**: not proposed (drag onto the back tile could move a tile
  to the parent page later).
* **Dimmer options** (your request 2026-10-14, later): `+`/`-` bars on a dimmer card large
  enough (like the climate bars), stepping by the tile's `step`, with a key to turn them off;
  `toggle_on_tap: true` makes a tap toggle at once, details then open on a hold of about 500 ms
  (which cancels the edit watch, so the 1000 ms edit long press never fires on such a dimmer);
  the double tap goes for that dimmer, it would either toggle first and then open, or delay every
  toggle by the double-tap window (which is what a switch tile does since 2026-10-15: its double
  tap opens the timeline, your request).

## 9. Swipe navigation and the dots

Decided and built 2026-10-14 (your request: enabled per area, "disable the back button and use
swipe navigation", dots at the bottom; the first build as a strip of the main page was wrong and
is gone). An area tile with `swipe: true` (YAML and JSON; the checkbox of the area in the web
editor's inspector):

* **Its page has no back tile.** Its first cell is a cell like any other. The area tile itself
  stays on the parent page: a tap opens the page as always.
* **A swipe to the right goes back** to the parent page (the page slides out as with the back
  tile). A swipe to the left does nothing. Pages without `swipe` keep the back tile and take no
  swipe.
* **The dots** show how deep the page is: a band of 24 px at the bottom of a swipe page (32 px
  on the JC4880P443C with its large fonts; your request 2026-10-14: 50 % and 100 % larger than
  the first 16 px), the grid shrinks by it (like it shrinks below the edit bar). One 9 px dot
  (12 px on the P4, the gap two thirds of a dot) per page
  of the chain: the page the swipe areas were entered from, then every swipe area down to the
  shown one, which is lit (`kText`; the others `kTextLabel` at 50 %). Rooms (back tile) > Workshop
  (`swipe`) shows two dots on Workshop; Rooms with `swipe` as well shows three on Workshop
  (`core::swipe_chain`). A tap on the left half of the band goes back; the right half does
  nothing. Pages without a swipe area have no band.
* **The gesture**: after 10 px the direction of a drag is decided (`swipe::kSlop`); a sideways
  drag of 40 px (`kSwipeStart`) swipes at once, and the rest of the press goes nowhere
  (`lv_indev_wait_release`: the tile under the finger is not tapped, it gets a press lost). A drag
  that goes more up or down than sideways is the dimmer's level drag or the pull-down; the dimmer
  starts its drag only then (`View::press_x`). In edit mode a sideways drag that starts on an
  empty cell or the band goes back as well (the layer's own detector); a drag on a tile stays a
  move, and the band takes no taps there.
* The slide of a page runs on every board now (`Info::ui_slide_pages` defaults to true; before,
  the JC4880P443C only); a swipe fires it, the page does not follow the finger (two pages exist
  only during the 250 ms slide, decided for the WT32's RAM).
* Code: `swipe.cpp` (the detector, the band), `dashboard::swiped()`, `core::swipe_page`,
  `has_back_tile`, `swipe_chain`; the web editor mirrors them (the preview draws the back tile
  and the dots by the same rules).
* Tried in the sim with the sample (Workshop has `swipe`): Workshop without a back tile and with
  two dots, a swipe right back to Rooms, a tap on the band's left half back to Rooms, and with
  Rooms set to `swipe` too three dots on Workshop and two on Rooms.

```
+------------------------------------------------------------+
|  +-----------+ +-----------+ +-----------+ +-----------+   |
|  |   tile    | |   tile    | |   tile    | |   tile    |   |   the Workshop page: no back tile,
|  +-----------+ +-----------+ +-----------+ +-----------+   |   a swipe right goes back to Rooms
|  +-----------+ +-----------+                               |
|  |   tile    | |   tile    |                               |
|  +-----------+ +-----------+                               |
|                         o  •                               |   the dots: Rooms, Workshop
+------------------------------------------------------------+
```
