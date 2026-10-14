# Panels (LVGL)

Status: **running**. The full screen views a tile opens: the light (dimmer), the climate, the
sensor, the lock and the weather. The picture tile's full screen view is in
[lvgl-camera.md](lvgl-camera.md).
Tiles and their states: [lvgl-dashboard.md](lvgl-dashboard.md); colors, fonts and the shared
parts: [lvgl-design.md](lvgl-design.md).

## 1. Frame

A tap opens the panel of a tile over the page; the back tile closes it, opening another page or a
new layout closes it, turning the display builds it again. While it is open the page's
subscription carries the panel's fields as well (`hass_protocol::fields_of`). Code: `panel.cpp`
(the frame), `light_panel.cpp`, `climate_panel.cpp`, `sensor_panel.cpp`, `lock_panel.cpp`,
`picture_panel.cpp`, `weather_panel.cpp`; the history graph of the sensor and the lock panel is
`history_view.cpp`.

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
panels of a dimmer, a climate, a sensor and a lock are a dialog of 620x420 (440x600 in portrait),
centered over the page. The page behind it is dimmed by a black veil of 50 %, the dialog is opaque,
radius 12, 1 px border. The veil darkens at once; the dialog slides down from above the screen into
the middle (250 ms, easing out) and back up when it closes (easing in). Only these boards animate
the panels; a fading veil redrew the whole screen every frame and is gone. A tap on the veil closes the dialog like the back tile. The camera image stays full
screen.

## 2. Light panel

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

## 3. Climate panel

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

## 4. Sensor panel

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
| timeline (on/off) | a binary sensor, an entity that reports `on`/`off`, or a lock (section 5): a 40 px strip of the state changes, 16 px from the card's edges left and right, centered in the card above the time labels, the legend centered in the space above it (on orange, off `0x2b3844`, unknown/unavailable the background), each pixel column taking the most important state of its time, so a short motion stays visible; the legend in the words of the device class |
| time axis | lines at whole local hours, 1 to 24 hours apart so the labels keep 44 px; `HH:00` under each line whose label fits in the card |
| message | `Loading...`, `No statistics for this sensor`, `No values in this time`, or the error of the request (`The clock is not set yet`, `Not connected to Home Assistant`); a refresh that fails keeps the graph |

The history is asked for when the panel opens, when the range changes and every 60 s while it is
open (`Backend::fetch_history`). The hass task reads the window in chunks of 6 hours, one after the
other (`recorder/statistics_during_period` with `period: 5minute`, or `history/history_during_period`
with `minimal_response`), and hands the whole window to the UI as one `core::History`; a new
request replaces one that is still running, an answer to an older one is dropped by its
generation. 48 hours are 576 points (4.6 KB on the large heap).

## 5. Lock panel

Like the light panel: two buttons in the options choose the control (`lock`) or the timeline
(`chart-timeline-variant`); the panel opens on the control. No header action.

```
+--------+------------------------+
|  back  | Front Door             |
+--------+------------------------+
| [lock] |    +---------+         |
| [time] |    | (lock)  |   (door)|   the open button only with
|        |    | Locked  |  Tap    |   supported_features OPEN;
|        |    +---------+  again  |   the note while it is armed
|        |    |         |  to open|
|        |    +---------+         |
+--------+------------------------+
```

| Part | Content | Touch |
| --- | --- | --- |
| slider | the track of the light's level slider (113 px wide at most, as large in the P4's dialog), a thumb half its height 6 px inside it: at the top while locked or locking, at the bottom while unlocked, unlocking or open, in the middle while jammed or unavailable; the glyph (`lock`, `lock-open`, `lock-alert`) and the state in words on it; orange while locked, green while unlocked, red while jammed (the tile's colors); the expected state at once | drag the thumb (it follows the finger) or tap a half: when that half differs from the state, `lock.lock` or `lock.unlock` is sent on release, else the thumb goes back |
| open | a round 64 px button with `door-open` right of the slider, only when `supported_features` has OPEN (1); subscribed only while the panel is open | the first tap arms it (orange, `Tap again to open` under it) for 3 s; a second tap sends `lock.open` |
| timeline | `history_view` with the states of the lock: `unlocked`, `unlocking`, `open`, `opening` count as on (green), `locked` and `locking` as off (orange), `jammed` as a problem (red, ranked above on so a short jam stays visible), the rest unknown; legend `Unlocked`, `Locked`, `Unknown`, and `Jammed` only while the window has a jam | the range buttons as in the sensor panel |

A lock that asks for a code (`code_format`) is not supported: HA refuses the call, and the pending
look ends with the error. The action is pending until HA reports `locked` or `unlocked` (`locking`
does not end it); `open` waits for nothing, the lock locks again by itself.

## 6. Weather panel

Added 2026-10-13 (your request: a tile and its details, no history). The panel of a `weather.*`
entity, tested with OpenWeatherMap (One Call 3.0, which has both forecasts). Like the sensor panel
it has no options: the frame is the portrait one in both orientations. Code: `weather_panel.cpp`.

**Not supported on the TTGO T4's 320x240** (decided 2026-10-13, your request): the rows do not
fit above the forecast there (only "Feels like" does), and the T4 is a test device. The panel
works on the WT32-SC01 (480x320), the JC3248W535 (320x480) and the JC4880P443C (dialog).

```
+----+ (icon) Weather
| <- |        11.8 °C   Cloudy
+----+
 (thermometer) Feels like    10.2 °C   (drop) Humidity      81%
 (gauge)       Pressure   1023 hPa     (wind) Wind  25.7 km/h (ENE)
 (eye)         Visibility  3.7 km      (cloud) Clouds         90%
 (thermometer) Dew point    8.6 °C
 Forecast                                  [Daily][Hourly]
 +---------------------------------------------------------+
 | Tue   Wed   Thu   Fri   Sat   Sun   Mon   Tue           |
 | (24)  (24)  (24)  (24)  (24)  (24)  (24)  (24)          |
 | 15°   19.3° 20.3° 17°   13.3° 10.3° 11°   15.3°          |
 | 8.5°  12.8° 13.8° 10.5° 6.8°  3.8°  4.5°  8.8°           |
 +---------------------------------------------------------+
```

| Part | Content |
| --- | --- |
| header | the condition's icon (48 px), the name (16 px) above the temperature (28 px, with its unit), the condition in words right of it |
| rows | what the entity reports, in this order: feels like, humidity, pressure, wind (speed, unit and the point of the compass of `wind_bearing`), gusts, visibility, clouds, dew point, UV index. A value the entity does not have is left out (OpenWeatherMap has no UV index, and gusts only sometimes). One column when all fit, else two (in reading order) from 400 px width. As many rows as fit above the forecast, spread over the space (gap 4 px to 18 px), placed when the values arrive. A name gives way to its value ("..."). |
| chips | `Daily` and `Hourly` like the range buttons of the sensor panel, the selected one white; only when the entity has both (`supported_features` bits 1 and 2, both offered until it arrived), else none. The one shown last is kept for the next panel. A tap subscribes the other forecast. |
| strip | a card with the columns of the forecast, scrolling sideways (drag). Daily: weekday, condition glyph (24 px), the high, the low (8 days). Hourly: `HH:00` (the weekday in white at midnight), glyph, temperature, the chance of precipitation when above 0 (the next 24 hours). Times in the device's time zone (Home Assistant's). |
| message | in the card while there is nothing to draw: `Loading...`, `No forecast`, or the error (`No forecast of this kind`, `Not connected to Home Assistant`, Home Assistant's own words). An update that fails keeps what is drawn. |

The rows and the forecast keep 12 px from the sides (24 px in the P4's dialog, also from the
bottom).

The forecast is not part of the entity's state: the panel subscribes it with
`weather/subscribe_forecast` while it is open (`Backend::subscribe_forecast`, architecture.md
section 6): the entries come about 300 ms later and again with every update of the integration
(OpenWeatherMap polls every 10 minutes), the strip keeps its scroll position. Closing the panel or
changing the chip ends the subscription; the old answers are dropped by their generation.
An unavailable entity (Home Assistant restarting) has no `supported_features`: both chips stay and
the selected one stays selected. A forecast that fails is asked for again every 10 s while the
panel is open (Home Assistant answers "not found" or "not supported" until the integration is
set up again); what is drawn stays meanwhile.
An unavailable entity (Home Assistant restarting) has no `supported_features`: both chips stay and
the selected one stays selected. A forecast that fails is asked for again every 10 s while the
panel is open (Home Assistant answers "not found" or "not supported" until the integration is
set up again); what is drawn stays meanwhile.
Checked on the WT32-SC01 (2026-10-13): the 8 days match the Home Assistant app, the hourly strip
scrolls, closing and reopening works.

## 7. Open points

* The graph of a sensor needs long term statistics (a `state_class`); a sensor without them shows
  "No statistics for this sensor". Reading its state history instead is possible, but a busy
  sensor's changes may not fit a message (16 KB per 6 hours).
* The climate panel has one setpoint; `heat_cool` with a low and a high setpoint
  (`target_temp_low`/`_high`) is not read yet and shows `--`.
* The weather panel: `twice_daily` forecasts are not read; the icon of `partlycloudy` is the day
  one at night too (the entity's state has no sun position); a forecast has at most 8 days or 24
  hours (`core::kMaxForecastEntries`); rows that do not fit are left out.
* The light panel opens only from a dimmer; a light without `dimmer` toggles on a tap. A long
  press could open it (to decide).
