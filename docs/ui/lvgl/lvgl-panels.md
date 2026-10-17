# Panels (LVGL)

The full screen views a tile opens: the light (dimmer), the climate, the
sensor, the lock, the weather, the timer and the automation. The picture tile's full screen view is in
[lvgl-camera.md](lvgl-camera.md).
Tiles and their states: [lvgl-dashboard.md](lvgl-dashboard.md); colors, fonts and the shared
parts: [lvgl-design.md](lvgl-design.md).

## 1. Frame

A tap opens the panel of a tile over the page; the back tile closes it, opening another page or a
new layout closes it, turning the display builds it again. While it is open the page's
subscription carries the panel's fields as well (`hass_protocol::fields_of`). Code: `panel.cpp`
(the frame), `light_panel.cpp` (color wheel and color temperature in `light_color.cpp`),
`climate_panel.cpp` (the arc in `climate_arc.cpp`), `sensor_panel.cpp`, `lock_panel.cpp`,
`picture_panel.cpp`, `weather_panel.cpp`, `timer_panel.cpp`, `automation_panel.cpp`; the history graph of
the sensor, the lock, the timer and the automation panel is `history_view.cpp`.

```
landscape (480x320)                     portrait (320x480)
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
centered over the page. The page behind it is dimmed by a black veil of 80 %, the dialog is
opaque in `theme::kDialog` (0x161d25, a step lighter than the page so it stands out of the dimmed
page, a step darker than its cards), radius 12, 1 px border. The veil darkens at once; the dialog slides down from above the screen into
the middle (250 ms, easing out) and back up when it closes (easing in). The full-screen panel of
the other boards slides in from the right and out to the right like an area page. The veil does not fade (a
fading veil redrew the whole screen every frame). A tap on the veil closes the dialog like the back tile. The camera image stays full
screen.

## 2. Light panel

| Part | Content | Touch |
| --- | --- | --- |
| power (header) | power glyph, filled orange while on (no running dot in the panels) | toggles |
| history (header, left of power) | `chart-timeline-variant`, white while the history is shown | shows the on/off timeline of the light (section 4) in the place of the control; a second tap, or a mode button, goes back |
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

Modeled on the Lyric thermostat card of Home Assistant.

| Part | Content | Touch |
| --- | --- | --- |
| power (header) | power glyph, filled orange; only while the mode (the pending one first) is not `off` and the entity has `off` in its `hvac_modes` | `set_hvac_mode` `off` (not `climate.turn_off`, which needs the entity's TURN_OFF feature); turning on again is the mode pill's job |
| history (header, left of power; in its place while the power button is hidden) | `chart-timeline-variant`, white while the history is shown | shows the room temperature over time (section 4: the curve of `current_temperature` from the state history, `HistoryKind::Temperature`, each change holding until the next, the unit `°`) in the place of the arc or the list; a second tap goes back, a pill opens its list |
| pills | mode, preset, fan mode: the option's name (small), its value, the glyph left of them when the pill is at least 100 px wide (mode: `fire`, `snowflake`, `sun-snowflake-variant`, `thermostat-auto`, `water-percent`, `fan`, `power`; preset `record`; fan `fan`); a value too wide for the pill takes the small font before it is cut with "..." ("Heat cool" at 480x320); only the options the entity has a list for; the pill of the open list white | shows the list of the option in the place of the arc; a second tap goes back |
| arc | 270 degrees, the gap at the bottom, the ring a tenth of its diameter (at least 12 px), the track `0x2b3844`, the value orange while the mode is not `off` (grey when off), the knob white; centered on its ring, not on its box | drag on the ring sets the setpoint on release |
| inside the arc | the action (`Heating`, else the mode; orange while not off), the setpoint (28 px, cyan while pending), `Current temperature` (`Current` in an arc under 180 px) and the room temperature | |
| steppers | `+` above `-` right of the arc in landscape, `-` left of `+` under it in portrait | step the setpoint by `target_temp_step`, the tile's `step` or 0.5; sent 0.7 s after the last step |
| option list | the values in two columns, scrolling; the mode with its glyph; the value that is set orange | sets it and goes back to the arc |
| range (heat_cool) | while the entity reports `target_temp_low` and `target_temp_high` (the pending ones first): two knobs on the ring, the indicator orange between them; inside the arc the two setpoints side by side (28 px, 20 px in an arc under 170 px inside), the selected one white (cyan while pending), the other and its knob `0x7fa8c9` | a drag on the ring moves the nearer knob, not past the other one; on release `set_temperature` with `target_temp_low` and `target_temp_high`. A tap on a setpoint selects it for the steppers; the steps go out together, 0.7 s after the last |

While the mode is `off` the panel opens on the mode list instead of the arc (the arc does nothing
then), and the power button leads to it as well; once per opening, a tap on the mode pill goes to
the arc.

The range is `min_temp`..`max_temp` of the entity (7..35 until they arrived); the arc is built
again when the range or the step arrives with the panel's fields, and when a mode change brings or
takes the two setpoints (most integrations report `temperature` in heat and cool and the range,
`temperature` null, in heat_cool). The Windows version's sample `climate.*heat_pump*` is in
heat_cool.

## 4. Sensor panel

Modeled on the entity card of the Home Assistant app. No options: the
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
| timeline (on/off) | a binary sensor, an entity whose state is one of two words (`core::on_off_words`: on/off, Detected/Clear, Occupied/Clear, Open/Closed, any case; the legend then uses those words), or a lock (section 5): a 40 px strip of the state changes, 16 px from the card's edges left and right, centered in the card above the time labels, the legend centered in the space above it (on orange, off `0x2b3844`, unknown/unavailable the background), each pixel column taking the most important state of its time, so a short motion stays visible; the hour lines run from the strip's bottom to the time labels (a time without history leaves the strip empty, a line through it would show); the legend in the words of the device class |
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

The panel of a `weather.*` entity (no history), tested with OpenWeatherMap (One Call 3.0, which has both forecasts). Like the sensor panel
it has no options: the frame is the portrait one in both orientations. Code: `weather_panel.cpp`.

The panel works on the WT32-SC01 (480x320), the JC3248W535 (320x480) and the JC4880P443C (dialog).

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

## 7. Timer panel

The panel of a `timer.*` entity (a timer helper of Home Assistant). Like the
lock panel: two buttons in the options choose the control (`timer-outline`) or the timeline
(`chart-timeline-variant`); the panel opens on the control. No header action. Code:
`timer_panel.cpp`. The helper and an automation that switches a fan with it:
[timer-helper.md](../../home-assistant/timer-helper.md).

```
+--------+------------------------------+
|  back  | Fan Timer                    |
+--------+------------------------------+
| [timer]|            29:07             |   the countdown (72 px digits when they fit,
| [time] |         Ends at 23:34        |   else 48, 28, 20); the state under it
|        |     [00] : [30] : [00]       |   the duration on three rollers (h, min, s)
|        |   (>)   (||)   ([])   (v)    |   Start  Pause  Cancel  Finish
|        |  Restart Pause Cancel Finish |
+--------+------------------------------+
```

| Part | Content | Touch |
| --- | --- | --- |
| countdown | the time left (`29:07`, `1:02:15`), counting down once a second from `finishes_at` by the device's clock while the timer runs (the remaining time reported last while the clock is not set), the remaining time while paused, the duration (muted) while idle; cyan while an action is on its way | |
| state | `Ends at HH:MM` (orange) while it runs, `Paused`, `Idle`, `Unavailable` | |
| duration | three rollers as the alarm's time (`theme::roller`): hours 0-23, minutes, seconds; the entity's `duration` (a longer one as 23:59:59), or the one the rollers chose (no color of its own: cyan meant nothing to you); three rows seen; a quick drag moves them up to 4x the finger, a fling 3x LVGL's throw (`theme::roller`) | a drag changes the duration of the next start; 0:00:00 turns Start off |
| Start | `play`, `restart` while it runs; the word is `Start` while idle, `Resume` while paused (a start resumes a paused timer with its rest), `Restart` while it runs (a start restarts it) | `timer.start`, with `duration` when the rollers changed it (`0:45:00`), else without (the entity's duration, or the rest of a paused timer); the chosen duration stays shown, as the countdown too (`Expectation::duration`), until Home Assistant reports it |
| Pause | `pause`, only while it runs | `timer.pause` |
| Cancel | `stop`, while it runs or is paused | `timer.cancel` |
| Finish | `check`, while it runs or is paused | `timer.finish` (fires the finished event as if the time were up) |
| timeline | `history_view` with the states of the timer: `active` counts as on (orange), `idle` as off, `paused` takes the third color (`theme::kTimerPaused`, the label blue; shown in the legend only while the window has one), the rest unknown | the range buttons as in the sensor panel |

A button the state does not allow is drawn at 40 % and does nothing. The expected state shows at
once (`core::Expectation` compares the state: `active`, `paused`, `idle`) until Home Assistant
reports it. The rows are centered in the control; the countdown takes the largest digit font that
leaves the buttons at least 36 px, the buttons grow up to 64 px (80 px in the P4's dialog) with
the room that is left.

The tile and the panel read three attributes (`hass_protocol::kTimer`): `duration` and `remaining`
as Home Assistant writes them (`0:30:00`, `1 day, 2:00:00`; `core::duration_seconds`) and
`finishes_at`, which the template renders as epoch seconds
(`as_timestamp(state_attr(e,'finishes_at'),none)`, null while the timer does not run; kept as an
`int64_t`, 0 while it does not run) so the device parses no ISO time. `remaining` is the rest at the last change (the duration at a start,
the rest at a pause); Home Assistant does not update it while the timer runs, so the countdown
comes from `finishes_at` and the device's clock.

## 8. Automation panel

The panel of
an `automation.*` or a `script.*` entity, as Home Assistant's dialog. Like the lock panel: two
buttons in the options choose the control (`robot`, `script-text` for a script) or the timeline
(`chart-timeline-variant`); the panel opens on the control. No header action. Code:
`automation_panel.cpp`.

```
+--------+------------------------------+
|  back  | Gate Alarm                   |
+--------+------------------------------+
| [robot]|  [ Enabled            (o ) ] |   an automation only
| [time] |         Last triggered       |
|        |        Yesterday 23:29       |
|        |             (>)              |   Run actions / Run script / Stop
|        |          Run actions         |
+--------+------------------------------+
```

| Part | Content | Touch |
| --- | --- | --- |
| Enabled | a card row with the theme's switch, 320 px wide at most; only for an automation (a script has no enabled state). The switch shows the expected state at once, the row's border is cyan while the action is on its way; at 40 % while the entity is unavailable | a tap anywhere on the row or on the switch: `automation.toggle` (the expected state `on`/`off`) |
| Last triggered | the caption, and under it `last_triggered` as `Today 10:52`, `Yesterday 18:12` or `14 Oct 09:00` (`Never` while it never ran, `Unavailable`); while the device's clock is not set every day is a date | |
| Run | a round button (64 px, 80 in the P4's dialog, at least 44) with `play` and the word `Run actions` (an automation) or `Run script`; while a script runs, `stop` and `Stop`; at 40 % and dead while unavailable | `automation.trigger` with `skip_condition: true` (what "Run actions" in Home Assistant does: the actions run whatever the conditions say; a disabled automation runs too); `script.turn_on`, `script.turn_off` while it runs. A script expects `on` (cyan border until Home Assistant reports it, or the 5 s pass: a short script is off again before the next values come); an automation expects nothing, its tile shows the new time |
| timeline | `history_view` with the states: an automation's is whether it is enabled (`Enabled` orange, `Disabled`), a script's whether it runs (`Running`, `Idle`); not the runs of the automation (Home Assistant records those in the logbook, not as states) | the range buttons as in the sensor panel |

The rows are centered in the control; the Run button takes the room that is left between 44 px
and the stepper size.

The tile and the panel read one attribute, `last_triggered`, rendered as epoch seconds like the
timer's `finishes_at` (`as_timestamp(state_attr(e,'last_triggered'),none)`, null while it never
ran; kept as an `int64_t`, 0 then).

## 9. Open points

* The graph of a sensor needs long term statistics (a `state_class`); a sensor without them shows
  "No statistics for this sensor". Reading its state history instead is possible, but a busy
  sensor's changes may not fit a message (32 KB per 6 hours).
* The range of a climate (`heat_cool`) keeps the low setpoint at or below the high one; a
  minimum gap (`target_temp_low` + deadband) is left to the integration, which may refuse the call.
* The weather panel: `twice_daily` forecasts are not read; the icon of `partlycloudy` is the day
  one at night too (the entity's state has no sun position); a forecast has at most 8 days or 24
  hours (`core::kMaxForecastEntries`); rows that do not fit are left out.
* The light panel opens from a dimmer on a tap, from a light without `dimmer` that dims on a
  double tap (a light that only switches gets the sensor panel with its timeline).
* The automation panel's timeline is the enabled state, not the runs; the runs are in Home
  Assistant's logbook, which the device does not read. `last_triggered` is the last one.
* The timer panel offers no `timer.change` (adding or taking time while it runs); the rollers
  set the duration of the next start. The countdown is as right as the two clocks agree (SNTP on
  the device, Home Assistant's clock for `finishes_at`).
