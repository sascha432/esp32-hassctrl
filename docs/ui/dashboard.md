# Dashboard

Status: **first version running** (2026-10-07) in the Windows version and on the T4. Carried over
from `behavior.md` of the old firmware (`kfc_fw/.../docs/hass_layout/behavior.md`), which stays the
reference for the panels that are not built yet. Code: `components/ui/src/dashboard.cpp` (pages,
store, actions), `tiles.cpp` (one view per tile), `icons.cpp` (glyphs).

## 1. Frame and grid

The dashboard uses the whole screen; the pull-down at the top center opens Settings
([screen.md](screen.md) section 1). A page is its grid of tiles, 8 px from the screen edges, 4 px
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

## 2. Tiles

| Tile | Content, top to bottom | Tap |
| --- | --- | --- |
| switch, light without `dimmer` | icon (48 px), name | toggles |
| button, scene, script | icon (48 px), name | press / turn on |
| sensor | icon (24 px), value with unit, name | opens the sensor panel with the history |
| dimmer (light with `dimmer`) | the level as a fill from the bottom, the percentage in the middle, the name at the bottom | drag up or down sets the level on release, double tap toggles, a single tap opens the light panel (section 6) |
| climate 1x2 | `+` bar, action ("Heating", the mode when the entity has none), setpoint, room temperature, name, `-` bar | the bars step the setpoint; the steps are sent 0.7 s after the last tap; a tap beside the bars opens the climate panel |
| climate 1x1 | one line in the small font: the action and the room temperature (`Heating • 21.5°`); setpoint, name | opens the climate panel |
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
| pending (an action on its way) | a cyan (`0x00ffff`) dot with a glow and a short fading tail runs clockwise round the border, 220 px a second on every card size (`runner.cpp`; each frame redraws only the dot's stretch of the border); the expected value shown at once |
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
of `core` plus `arrow-left`, `plus` and `minus`, in 24 and 48 px (`tools/build_icons.py`, which also writes `web/icons.svg` for the
web editor from the same names).

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
times a second. The header action of a climate is shown only while it is on; without it the name
takes the whole header.

In the dialogs of the P4 (`panel_dialogs`, `Frame::large`) the touch targets are larger:

- the header row, the portrait back tile and the header action 60 px, the steppers 80 px and the
  portrait options row 55 px (a quarter larger);
- the list items in Montserrat 24 (`theme::font_item()`, a third larger than the normal 18) and
  as much taller as their font (42 px scaled by the line heights: 54 px);
- the level and color temperature sliders wider: 160 px and 80 % of the control's height,
  centered, in portrait; 140 px, 16 px short of the control's bottom and (the level) 40 px from
  the steppers in landscape. The steppers are centered on the slider.

The lists of both panels (effects, modes, presets, fan modes) have 42 px items as wide as two of
them side by side. When every item fits in one column, the list is that column, centered in the
control area; else it has two columns, centered when they fit (no room kept for a scrollbar). A
longer one scrolls, and when it opens with the value that is set out of view, that item is scrolled into the
middle (no further than the end of the list).

Boards with `Info::ui_panel_dialogs` (the JC4880P443C, `hc::ui::Options::panel_dialogs`): the
panels of a dimmer, a climate and a sensor are a dialog of 620x420 (440x600 in portrait),
centered over the page. The page behind it is dimmed by a black veil of 50 %, the dialog is opaque,
radius 12, 1 px border. The veil darkens at once; the dialog slides down from above the screen into
the middle (250 ms, easing out) and back up when it closes (easing in). Only these boards animate
the panels; a fading veil redrew the whole screen every frame and is gone. A tap on the veil closes the dialog like the back tile. The camera image stays full
screen.

### Light panel

Carried over from `behavior.md` section 7 of the old firmware, fitted to 320x240.

| Part | Content | Touch |
| --- | --- | --- |
| power (header) | power glyph, filled orange while on (no running dot in the panels) | toggles |
| buttons | level (`brightness-6`), color (`palette`), color temperature (`thermometer`), effects (`creation`); only those the light offers (`supported_color_modes`, `effect_list`), none when only the level is left; the selected one white | shows its control |
| level | vertical slider (113 px at most, the fill orange, the percentage in the middle) and the round `+`/`-` steppers (64 px) beside it | drag or tap sets the level on release; the steppers go to the next multiple of the tile's `step` (default 1 %), held 4 times a second, sent 0.5 s after the last step; 0 turns the light off |
| level of an on/off light | `On` / `Off` in large type | (the power button) |
| color | hue and saturation wheel (196 px at most; hue counterclockwise from red at the right, white in the center), the knob in the color that is set | drag, sent on release (`hs_color`) |
| color temperature | vertical slider as large as the level slider, with the colors of the range (cool at the top), a white marker (a rounded bar 12 px tall, 6 px from the sides, no border), the kelvin in the middle; empty while the light is in a color mode | drag, sent on release in steps of 10 K |
| effects | the effect list in two columns, scrolling, the running effect orange | sets the effect |

The wheel is drawn by the panel into a buffer of the large heap (`lv_colorwheel` exists only in
LVGL 8, docs/architecture.md); it is freed when another control is selected. Every action shows
at once and is pending (`core::Expectation` compares the state, the level, the color within 2, the
color temperature within 2 % and the effect) until HA reports it.

### Climate panel

Carried over from `behavior.md` section 6 of the old firmware (the Lyric thermostat card).

| Part | Content | Touch |
| --- | --- | --- |
| power (header) | power glyph, filled orange; only while the mode (the pending one first) is not `off` and the entity has `off` in its `hvac_modes` | `set_hvac_mode` `off` (not `climate.turn_off`, which needs the entity's TURN_OFF feature); turning on again is the mode pill's job |
| pills | mode, preset, fan mode: the option's name (small), its value, the glyph left of them when the pill is at least 100 px wide (mode: `fire`, `snowflake`, `sun-snowflake-variant`, `thermostat-auto`, `water-percent`, `fan`, `power`; preset `record`; fan `fan`); only the options the entity has a list for; the pill of the open list white | shows the list of the option in the place of the arc; a second tap goes back |
| arc | 270 degrees, the gap at the bottom, the ring a tenth of its diameter (at least 12 px), the track `0x2b3844`, the value orange while the mode is not `off` (grey when off), the knob white; centered on its ring, not on its box | drag on the ring sets the setpoint on release |
| inside the arc | the action (`Heating`, else the mode; orange while not off), the setpoint (28 px, cyan while pending), `Current temperature` (`Current` in an arc under 180 px) and the room temperature | |
| steppers | `+` above `-` right of the arc in landscape, `-` left of `+` under it in portrait | step the setpoint by `target_temp_step`, the tile's `step` or 0.5; sent 0.7 s after the last step |
| option list | the values in two columns, scrolling; the mode with its glyph; the value that is set orange | sets it and goes back to the arc |

While the mode is `off` the panel opens on the mode list instead of the arc (the arc does nothing
then), and the power button leads to it as well; once per opening, a tap on the mode pill goes to
the arc (2026-10-08, your request).

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
while it is shown, but not while its last request is on its way (an image that came after a newer
request was thrown away, and a page of slow cameras only grew the queue); after 60 s without an
answer it asks again. A tap shows it full screen on black, refreshed the same way. Until the first
full screen frame arrives, the tile's frame is shown enlarged where it lies in the whole picture
(it is the picture's middle in the tile's shape; `core::Image::source_width` tells the picture's
shape), so the stream does not jump when it starts.

The full screen view (`picture_panel.cpp`) shows the whole picture. At 100 % it covers the screen
(the side that sticks out is cut off), so every camera looks the same whatever its resolution;
the zoom steps are 50, 75, 100, 150 and 200 %. A swipe moves the picture, as far as its edges (a
side smaller than the screen stays centered); the position is kept as a part of the picture, so it
stays on the same spot when the zoom changes. A touch shows round buttons at the top center, "X"
(back), "+" and "-" (grayed at the ends), with "swipe to move" below them; they hide 5 s after the
last touch, and the view opens with them shown. They sit below the pull-down zone of Settings
(`overlay::kZoneHeight`), which lies above the panel and takes the taps there. A zoom step zooms the
frame on screen with LVGL at once and asks for the frames in the new size: still images anew, a
stream goes on in it without a new connection. Zoom and position are kept per camera (entity and
stream) while the device runs: the view opens as it was left. Each change of size, zoom or place
redraws the whole screen (LVGL's own invalidation of the zoomed image left parts of the larger
picture on the black after a zoom out).

The image is `GET /api/camera_proxy/<entity>?width=W&height=H` with the token, W x H the size of
the tile inside its border or of the screen at the zoom. Home Assistant scales it when it can; the device
decodes the JPEG at the smallest scale that still covers the box (1:1 to 1:8), then cuts the middle
to the box's aspect ratio for a tile; the full screen view asks for the whole picture
(`whole`: the decoder's output as it is, the view scales it to cover). The frame is RGB565 in the display's byte order on the large heap,
shared between the tile and its full screen view (480x320: 300 KB).

The images are fetched by the media tasks of `hass` (`media.cpp`, ESP-IDF's `esp_http_client`
and `esp_jpeg`), so a slow camera never holds up the values of the page. On the P4 up to three
tasks fetch at the same time, as the browser does (Home Assistant takes 2 to 5 s for some cameras);
the internal RAM of the ESP32 and the S3 has room for one (on the S3 three left 19 bytes and a
socket could not be created). A task is created when a request finds every task busy (6 KB of
stack, 8 KB with https); the images are decoded one at a time. One request per camera, stream and
size waits, the newest; one that falls out of the full queue (8) is answered with an error. A JPEG
of more than 512 KB is refused.

On the P4 the hardware decoder's frame is scaled to the box by the PPA. Its blocking call did not
come back now and then (2026-10-10, cause in the 2D-DMA not found) and the media task waited for
good: the scale now has 500 ms, after that the decoder and the PPA stay off until the restart and
the images are decoded in software (25 to 30 ms for a tile). The buffers of that scale are never
freed, the DMA may still use them.

### MJPEG stream (test, 2026-10-08)

A picture tile with `stream: http://...` (an MJPEG stream, for example of go2rtc) takes its image
from the first frame of the stream instead of Home Assistant; full screen plays the stream with the
frame rate in the upper left corner. Full screen waits 30 s for the stream's answer (go2rtc
answers once its first frame is there, which took longer than 10 s when it first had to start its
source), a tile's first frame 10 s; a missing answer says "no answer within ... s" instead of
"HTTP -1". `stream: hass` plays Home Assistant's own MJPEG stream of the
camera (`/api/camera_proxy_stream/<entity>` with the token) full screen, and the tile keeps the
scaled snapshots of the camera proxy (that stream has the camera's full size). One stream plays
at a time; the still images of the page go on in the other tasks (on the ESP32 and the S3 they
wait meanwhile). A frame is decoded once the UI took the
one before (it takes it within 10 to 15 ms); the frames in between are read and dropped, so the
picture never falls behind. A stream frame is the whole picture as the decoder made it, without a
copy (the copy of 480x320 through PSRAM cost 166 ms), and the view scales it to cover the screen
at the zoom; the software decoders' scale is the largest that still shows 3/4 of the frame
(`core::stream_shift`: 704x576 on 800x480 loses a strip, 1280x960 is decoded at 1:2). A spinner
turns until the first frame (its track at 50 % opacity, so the image shows through); turning
the display builds the view again in the new size and restarts the stream in it (with the kept
zoom and position). `hass
streamscale auto|0..3` on the console forces the decoder's scale. The decoder gets a work area of
8 KB per image (esp_jpeg's 3100 bytes were too small for the snapshots of Home Assistant's
`camera.balconycam`).

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

On the JC4880P443C (ESP32-P4) the hardware does the work (`hass/src/image_hw.cpp`): the JPEG
decoder decodes at full size (800x480 in 17 ms, 140 ms in software; pictures whose sides are not
divisible by 8, often Home Assistant's snapshots in a tile's size, and progressive JPEGs go to the
software decoder), and the PPA scales the frame to cover the request's size (`core::cover_block`,
scale in sixteenths; the whole picture for the full screen view, `core::whole_block`), so the UI
shows a stream's frame 1:1: LVGL's zoom of a smaller frame took 100 ms. The view shows a frame
up to 7 % larger than its box 1:1 (the scale rounds up to sixteenths). A whole picture is
enlarged to at most 2 M pixels (4 MB): 200 % of a 16:9 stream on the portrait screen would be
2880x1620, 9 MB a frame with two or three on the way, and every other frame failed for memory
(2026-10-11); LVGL zooms the rest (150 and 200 % in portrait: still about 14 fps for go2rtc's
800x480). A frame that already has the screen's size is not scaled. The stream reads the next frame
while the UI draws the last one (50 ms for 800x480, software rotation) and, with the hardware
decoder, decodes it then too; the software decoder waits for the UI first, so a frame that is
dropped is not decoded for nothing. The UI's take wakes the stream (a semaphore; the tick of 10 ms
was too coarse to poll). Further settings of the board: a TCP window of 64 KB (with the default
5.7 KB go2rtc waited while a frame decoded: 3.3 fps), LVGL's rotation buffer of 48 KB (a whole
band; 10 KB flushed it in 5 pieces), the copy of LVGL's bands into the frame buffer by the CPU
(with the DMA2D memory got corrupted while a stream played, `init_panel` of the board).

Measured full screen 800x480 (2026-10-08):

| Stream | source | shown | decode + scale per frame |
| --- | --- | --- | --- |
| go2rtc 800x480, `-r 15 -q:v 18` (`_xl`, 23 KB frames) | 15 fps | 14.1 to 15.8 fps | 17 ms |
| go2rtc 800x480, `-r 12 -q:v 17` (24 KB frames) | 12.4 fps | 11 to 13 fps | 17 ms |
| go2rtc 640x320, `-r 5` / `-r 3` (`_m`, `_s`, 15 KB frames) | 5 / 3 fps | 5 / 3 fps | 7 + 15 ms |
| HA `camera.doorspy_cam_axxon0` (1280x960, 34 KB frames) | 9.4 fps | 9.4 fps | 55 + 21 ms |
| HA `camera.balconycam` (704x576, 52 KB frames) | 2 fps | 2 fps | 17 + 17 ms |

Before the hardware (software decoder, LVGL's zoom): 800x480 5.5 fps, doorspy 2.5 fps. The user
measured 18 to 22 fps with go2rtc's `-r 25` and saw no tearing with the CPU copy (2026-10-08).

On the JC3248W535 (ESP32-S3, 480x320) Espressif's `esp_new_jpeg` decodes (`hass/src/new_jpeg.cpp`,
a dependency of `hass` for the S3 only; SIMD, a 640x320 frame in 41 ms instead of TJpgDec's
148 ms). Its own buffers come from `hot_alloc` (internal RAM only while 32 KB stay free; it took
them from internal RAM first, whatever was left). It scales while it decodes, to sides in multiples of 8 (`core::cover_size8`): a frame
larger than the screen is decoded at the smallest size that covers it (800x480 at 536x320), so
LVGL needs no zoom; the scaling costs time (800x480 107 ms), a stream in the screen's size is
cheaper. The media task runs on the second core of every chip that has one: on the S3's first
core with Wi-Fi and LVGL the decoding starved the idle task (task watchdog) and took 60 ms
instead of 41. TCP window 23 KB (16 segments, the Wi-Fi buffers are in PSRAM; internal RAM
stayed at 40 KB free). Measured full screen (2026-10-08):

| Stream | source | shown | decode per frame |
| --- | --- | --- | --- |
| go2rtc 640x320, `-r 10 -q:v 17` | 10.2 fps | 9.2 to 10.9 fps | 41 ms |
| go2rtc 640x320, `-r 8 -q:v 16` (`_m`) | 8.2 fps | 7.7 to 8.4 fps | 40 ms |
| go2rtc 800x480, `-r 15 -q:v 18` (`_xl`) | 15 fps | 6.4 to 6.9 fps | 107 ms (scaled) |
| HA doorspy 1280x960 | 9.4 fps | about 3 fps | 210 to 240 ms (scaled) |

Before (TJpgDec, window 5.7 KB): 640x320 4.6 fps, 800x480 2.6 fps; the window alone gave 5.9 and
3.1 fps.

With the media task on the second core the P4 is unchanged (`_xl` 13.6 to 16.6 fps, 640x320 all
10 fps, doorspy 9.4 fps) and the WT32 (TJpgDec, default window) shows 640x320 at 2.3 to 2.6 fps,
800x480 at 1.8 fps and Home Assistant's doorspy (1280x960, 1:4, about 1 s per frame) at 0.8 fps;
internal RAM 32 KB free as before (2026-10-08).

