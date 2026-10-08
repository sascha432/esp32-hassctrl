# Dashboard

Status: **first version running** (2026-10-07) in the Windows version and on the T4. Carried over
from `behavior.md` of the old firmware (`kfc_fw/.../docs/hass_layout/behavior.md`), which stays the
reference for the panels that are not built yet. Code: `components/ui/src/dashboard.cpp` (pages,
store, actions), `tiles.cpp` (one view per tile), `icons.cpp` (glyphs).

## 1. Frame and grid

The dashboard uses the whole screen; the pull-down at the top center opens Settings
([screen.md](screen.md) section 1). A page is its grid of tiles, 8 px from the screen edges, 4 px
between tiles, radius 8. Portrait uses the transposed grid and the tile spans turned with it
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

## 2. Tiles

| Tile | Content, top to bottom | Tap |
| --- | --- | --- |
| switch, light without `dimmer` | icon (48 px), name | toggles |
| button, scene, script | icon (48 px), name | press / turn on |
| sensor | icon (24 px), value with unit, name | opens the sensor panel with the history |
| dimmer (light with `dimmer`) | the level as a fill from the bottom, the percentage in the middle, the name at the bottom | drag up or down sets the level on release, double tap toggles, a single tap opens the light panel (section 6) |
| climate 1x2 | `+` bar, action ("Heating", the mode when the entity has none), setpoint, room temperature, name, `-` bar | the bars step the setpoint; the steps are sent 0.7 s after the last tap; a tap beside the bars opens the climate panel |
| climate 1x1 | action, setpoint, name | opens the climate panel |
| picture (camera) | the camera image over the whole card, the name centered on a dark band at its bottom; the camera icon and the name until the first image arrives | shows the image full screen |
| area | folder (or the tile's icon), name | opens the page |
| spacer | nothing | |

Sizes: a cell at least 90 px tall and wide gets the large fonts (value 28 px, name 16 px, icon
48 px), smaller cells the small ones (value 20 px, name 12 px, icon 24 px). A value that is too
wide steps down through the font sizes (`612 ppm`, `Detected` in a 74 px cell). A name that is too
long ends in `...`.

## 3. States and colors

| State | Look |
| --- | --- |
| on (switch, light, binary sensor `on`) | filled with the active color (`0xff8000`), white icon and name |
| off | card `0x182029`, border `0x2b3844`, icon and name `0x7fa8c9`; the off glyph of the domain |
| pending (an action on its way) | cyan border (`0x00ffff`, 2 px), the expected value shown at once |
| unavailable (`unavailable`, `unknown`) | card `0x161d25`, value `--` |
| no value yet | value `...` |
| dimmer | the fill is the indicator, the card stays dark |
| climate | the bars are orange while the mode is not `off`, the action is orange |

A light that is switched on shows the level it had before (HA turns it on at that level), not
100 %; the store keeps the last level while the light is off. A light not seen on since the start
shows `On` until HA reports the level.

A pending action ends when the entity reports the expected state (a level within 3 of 255, a
setpoint within 0.05), when the call fails, or after 5 s; then the tile shows the entity's state
again.

Texts: binary sensors in the words of their device class (`Open`/`Closed`, `Detected`/`Clear`,
`Wet`/`Dry`), other states with a capital and spaces (`heat_cool` is `Heat cool`), numbers with
the tile's `decimals` or HA's display precision. A state of several lines shows its first line.

## 4. Icons

The tile's `icon`; else the entity's `icon` attribute when the glyph is in the firmware; else the
default of the domain and device class (light `lightbulb`/`lightbulb-off`, outlet `power-plug`,
motion `motion-sensor`, temperature `thermometer`, ...; `icons.cpp`). The glyphs are the 43 names
of `core` plus `arrow-left`, `plus` and `minus`, in 24 and 48 px (`tools/build_icons.py`).

## 5. Open points

* Camera images are decoded by the TJpgDec of the ESP32's ROM, which does not read progressive
  JPEGs; such a camera keeps showing its icon (the log says why). A failed image keeps the last one.
* The graph of a sensor needs long term statistics (a `state_class`); a sensor without them shows
  "No statistics for this sensor". Reading its state history instead is possible, but a busy
  sensor's changes may not fit a message (16 KB per 6 hours).
* The climate panel has one setpoint; `heat_cool` with a low and a high setpoint
  (`target_temp_low`/`_high`) is not read yet and shows `--`.
* The light panel opens only from a dimmer; a light without `dimmer` toggles on a tap. A long
  press could open it (to decide).
* Numeric sensors are never highlighted; the old rule ("on while a number is reported") would
  make every sensor orange. Proposal: keep it so.
* The climate default step is 0.5 for both units until the unit of the HA configuration is read.
* Montserrat 28 costs 37 KB of flash for the values; a subset (digits, units, the state words) is
  possible if flash gets tight.
* A failed service call ends the pending look; it is not shown in the alert strip yet.

## 6. Panels

A tap opens the panel of a tile over the page; the back tile closes it, opening another page or a
new layout closes it, turning the display builds it again. While it is open the page's
subscription carries the panel's fields as well (`hass_protocol::fields_of`). Code: `panel.cpp`
(the frame), `light_panel.cpp`, `climate_panel.cpp`, `sensor_panel.cpp`, `picture_panel.cpp`.

```
landscape (480x320, 320x240)            portrait (320x480)
+--------+------------------+----+      +----+---------------+----+
|  back  | name             | on |      | <- | name          | on |
+--------+------------------+----+      +----+---------------+----+
| level  |                       |      |                         |
| color  |   control of the      |      |   control of the        |
| temp   |   selected button     |      |   selected button       |
| effect |                       |      |                         |
+--------+-----------------------+      +-------------------------+
                                        | level|color|temp|effect |
                                        +-------------------------+
```

The left column is one cell of a 4-column grid, the back tile at most a third of the height, 8 px
between it and the control (twice the grid's gap: with 4 px the arc and the lists looked glued to
the buttons); the header row is 48 px, the name 12 px right of the back tile. Portrait: back tile 48 px, the buttons in a row of 44 px at the bottom. The
round steppers of both panels are 64 px (smaller when the control area is short), held they step 4
times a second. A climate has no header action, its name takes the whole header.

The lists of both panels (effects, modes, presets, fan modes) have 42 px items as wide as two of
them side by side. When every item fits in one column, the list is that column, centered in the
control area; else it has two columns, centered when they fit (no room kept for a scrollbar). A
longer one scrolls, and when it opens with the value that is set out of view, that item is scrolled into the
middle (no further than the end of the list).

Boards with `Info::ui_panel_dialogs` (the JC4880P443C, `hc::ui::Options::panel_dialogs`): the
panels of a dimmer, a climate and a sensor are a dialog of 620x420 (440x600 in portrait),
centered over the page. The page behind it is dimmed by a black veil of 50 %, the dialog is opaque,
radius 12, 1 px border. Veil and dialog fade in when the dialog opens and out when it closes
(200 ms). A tap on the veil closes the dialog like the back tile. The camera image stays full
screen.

### Light panel

Carried over from `behavior.md` section 7 of the old firmware, fitted to 320x240.

| Part | Content | Touch |
| --- | --- | --- |
| power (header) | power glyph, filled orange while on, cyan border while pending | toggles |
| buttons | level (`brightness-6`), color (`palette`), color temperature (`thermometer`), effects (`creation`); only those the light offers (`supported_color_modes`, `effect_list`), none when only the level is left; the selected one white | shows its control |
| level | vertical slider (113 px at most, the fill orange, the percentage in the middle) and the round `+`/`-` steppers (64 px) beside it | drag or tap sets the level on release; the steppers go to the next multiple of the tile's `step` (default 10 %), held 4 times a second, sent 0.5 s after the last step; 0 turns the light off |
| level of an on/off light | `On` / `Off` in large type | (the power button) |
| color | hue and saturation wheel (196 px at most; hue counterclockwise from red at the right, white in the center), the knob in the color that is set | drag, sent on release (`hs_color`) |
| color temperature | vertical slider with the colors of the range (cool at the top), a white marker, the kelvin in the middle; empty while the light is in a color mode | drag, sent on release in steps of 10 K |
| effects | the effect list in two columns, scrolling, the running effect orange | sets the effect |

The wheel is drawn by the panel into a buffer of the large heap (`lv_colorwheel` exists only in
LVGL 8, docs/architecture.md); it is freed when another control is selected. Every action shows
at once and is pending (`core::Expectation` compares the state, the level, the color within 2, the
color temperature within 2 % and the effect) until HA reports it.

### Climate panel

Carried over from `behavior.md` section 6 of the old firmware (the Lyric thermostat card).

| Part | Content | Touch |
| --- | --- | --- |
| pills | mode, preset, fan mode: the option's name (small), its value, the glyph left of them when the pill is at least 100 px wide (mode: `fire`, `snowflake`, `sun-snowflake-variant`, `thermostat-auto`, `water-percent`, `fan`, `power`; preset `record`; fan `fan`); only the options the entity has a list for; cyan border while a change is pending; the pill of the open list white | shows the list of the option in the place of the arc; a second tap goes back |
| arc | 270 degrees, the gap at the bottom, the ring a tenth of its diameter (at least 12 px), the track `0x2b3844`, the value orange while the mode is not `off` (grey when off), the knob white; centered on its ring, not on its box | drag on the ring sets the setpoint on release |
| inside the arc | the action (`Heating`, else the mode; orange while not off), the setpoint (28 px, cyan while pending), `Current temperature` (`Current` in an arc under 180 px) and the room temperature | |
| steppers | `+` above `-` right of the arc in landscape, `-` left of `+` under it in portrait | step the setpoint by `target_temp_step`, the tile's `step` or 0.5; sent 0.7 s after the last step |
| option list | the values in two columns, scrolling; the mode with its glyph; the value that is set orange | sets it and goes back to the arc |

The range is `min_temp`..`max_temp` of the entity (7..35 until they arrived); the arc is built
again when the range or the step arrives with the panel's fields.

### Sensor panel

Carried over from the old firmware (the entity card of the Home Assistant app). No options: the
frame is the portrait one in both orientations, the control takes the whole width below the
header.

```
+----+ (icon) Name
| <- |        1236 W
+----+
History                         [48 h][24 h][12 h]
+------------------------------------------------+
| W                                              |
| 1400 -----|--------|--------|--------|-------- |
|           |   /\   |        |   /\   |         |
| 1200 ----/|---  ---|-\  /---|--/  ---|-------- |
|         / |        |  \/    |        |         |
| 1000 -----|--------|--------|--------|-------- |
|         00:00    06:00    12:00    18:00       |
+------------------------------------------------+
```

| Part | Content |
| --- | --- |
| header | the tile's icon (48 px; orange while an on/off entity is on), the name (16 px) above the live value (28 px, the tile's text) |
| range buttons | 48, 24 and 12 hours, round 52x26 px, the selected one white; the panel opens with the range used last |
| graph (statistics) | the 5 minute means of the long term statistics as a 2 px line in the accent color (`0x00ffff`), a bucket without a value is a gap; three value lines with round labels (steps of 1, 2, 2.5 or 5 times a power of ten) and the unit above them |
| timeline (on/off) | a binary sensor, or an entity that reports `on`/`off`: a 40 px strip of the state changes, 16 px from the card's edges left and right, centered in the card above the time labels, the legend centered in the space above it (on orange, off `0x2b3844`, unknown/unavailable the background), each pixel column taking the most important state of its time, so a short motion stays visible; the legend in the words of the device class |
| time axis | lines at whole local hours, 1 to 24 hours apart so the labels keep 44 px; `HH:00` under each line whose label fits in the card |
| message | `Loading...`, `No statistics for this sensor`, `No values in this time`, or the error of the request (`The clock is not set yet`, `Not connected to Home Assistant`); a refresh that fails keeps the graph |

The history is asked for when the panel opens, when the range changes and every 60 s while it is
open (`Backend::fetch_history`). The hass task reads the window in chunks of 6 hours, one after the
other (`recorder/statistics_during_period` with `period: 5minute`, or `history/history_during_period`
with `minimal_response`), and hands the whole window to the UI as one `core::History`; a new
request replaces one that is still running, an answer to an older one is dropped by its
generation. 48 hours are 576 points (4.6 KB on the large heap).

### Camera image

A picture tile asks for its image when the page opens and every `refresh` seconds (default 60)
while it is shown; a tap shows it full screen on black, refreshed the same way, and any tap goes
back. Until the first full screen frame arrives, the tile's frame is shown enlarged.

The image is `GET /api/camera_proxy/<entity>?width=W&height=H` with the token, W x H the size of
the tile inside its border or of the screen. Home Assistant scales it when it can; the device
decodes the JPEG at the smallest scale that still covers the box (1:1 to 1:8), then cuts the middle
to the box's aspect ratio. The frame is RGB565 in the display's byte order on the large heap,
shared between the tile and its full screen view (480x320: 300 KB).

The images are fetched one at a time by the media task of `hass` (`media.cpp`, ESP-IDF's
`esp_http_client` and `esp_jpeg`), so a slow camera never holds up the values of the page. The
task is created with the first image (6 KB of stack, 8 KB with https); one request per camera
waits, the newest. A JPEG of more than 512 KB is refused.

### MJPEG stream (test, 2026-10-08)

A picture tile with `stream: http://...` (an MJPEG stream, for example of go2rtc) takes its image
from the first frame of the stream instead of Home Assistant; full screen plays the stream with the
frame rate in the upper left corner. One stream plays at a time; the still images of the page
wait meanwhile. A frame is decoded once the UI took the one before (it takes it within 10 to
15 ms); the frames in between are read and dropped, so the picture never falls behind. A stream
frame is shown as the decoder made it, centered, without a copy (the copy of 480x320 through PSRAM
cost 166 ms). A spinner turns until the first frame; any tap or swipe goes back (on the release);
turning the display builds the view again in the new size and restarts the stream in it.
`hass streamscale auto|0..3` on the console forces the decoder's scale.

Measured on the WT32-SC01 (ESP32, PSRAM at 40 MHz) with go2rtc, after the first 10 to 15 s (ffmpeg
starts with large frames, 60 to 130 KB, which decode slower):

| Stream | read | shown | decode per frame |
| --- | --- | --- | --- |
| 800x480, 8 fps | 1.3 to 1.4 fps | 1.3 to 1.4 fps | 575 ms (1:2: 450 ms) |
| 640x320, 4 fps | 2.3 to 2.4 fps | 2.3 to 2.4 fps | 322 ms |
| 640x320, 2 fps | 2 fps | 2 fps (all) | 330 ms |
| 640x320, `-q:v 7` (35 KB frames) | 1.5 fps | 1.5 fps | 365 ms |
| 800x480, `-q:v 7` (67 KB frames) | 0.9 fps | 0.9 fps | 645 ms |

The decode time follows the number of pixels (about 1.5 us each) and the size of the frame; 1:2
saves little, the Huffman decoding is most of it. Reading and decoding run one after the other
in the media task (20 to 30 % of its time waits for data), so the two could overlap on the second
core. Memory while playing: internal RAM 32 to 33 KB free (unchanged), 3.8 KB of the 6 KB stack
unused, about 1 MB of PSRAM for the 800x480 frames.

