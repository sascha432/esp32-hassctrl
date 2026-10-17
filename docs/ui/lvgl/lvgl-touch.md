# Touch (LVGL)

Status: **running**. Everything a finger does on the screen: touch targets, the gestures of each
screen and how they keep out of each other's way, the click and the vibration, and the touch that
wakes the screen. The screens are described in [lvgl-dashboard.md](lvgl-dashboard.md),
[lvgl-panels.md](lvgl-panels.md), [lvgl-camera.md](lvgl-camera.md) and
[lvgl-settings.md](lvgl-settings.md). The touch controllers per board:
[adding-a-board.md](../../adding-a-board.md). Touch is a requirement for a supported board
(decided).

## 1. Touch targets

* At least 44 x 44 px (about 7 mm at 165 DPI): rows, buttons, the back tile, the steppers (64 px),
  the panel option buttons. In the P4's dialogs the header, the steppers and the option row are a
  quarter larger ([lvgl-panels.md](lvgl-panels.md) section 1).
* **A row is the target, not its widget.** A row with a switch and no page of its own flips the
  switch with a tap anywhere on it (`theme::toggle()`); a row without any action takes no touches,
  so they scroll the page (2026-10-09: the switch alone was hard to hit).
* A slider takes touches 16 px past its ends (`theme::slider()`): at 0 and 100 % half of the knob
  lies outside the slider, where a drag did nothing.
* Taps beside a target do nothing and make no sound.

## 2. Gestures

| Where | Gesture | Effect | Values |
| --- | --- | --- | --- |
| dashboard, top center | drag down from the pull-down zone | opens Settings | zone 160 x 40 px (`overlay::kZoneHeight`; in a full-screen panel only between its header buttons), 24 px of drag; the alert strip's area starts it too |
| alert strip | tap | a state message opens Settings at the matching page; "⟳ Stream failed" tries again; another event message closes | |
| switch, button, scene tile | tap | toggles, presses | |
| automation, script tile | double tap; single tap | runs it (stops a running script); opens the automation panel (after the double tap time) | 300 ms |
| dimmer tile | drag up or down | sets the level on release | starts after 10 px |
| dimmer tile | double tap | toggles | 300 ms |
| dimmer tile | single tap | opens the light panel (after the double tap time) | |
| climate tile | tap on a `+`/`-` bar | steps the setpoint, sent 0.7 s after the last tap | |
| other tiles with a panel | tap | opens the panel | |
| area tile, back tile | tap | opens the page, goes to the parent | |
| swipe page (an area with `swipe`), any tile | drag to the right | back to the parent page ([lvgl-dashboard.md](lvgl-dashboard.md) section 9) | the direction is decided after 10 px, 40 px sideways swipe; the tile under the finger is not tapped |
| dot band (swipe page) | tap on the left half | back | the band is 24 px high, the dots 9 px (32 and 12 on the P4) |
| dashboard page | long press, moving less than 10 px | starts the edit mode | 1000 ms |
| edit mode | drag a tile; long press on a tile; tap on a tile; tap on an empty cell; drag to the right on an empty cell | moves it; asks to remove it; its title, entity and options; a "+" cell: the steps of adding a tile, area or clock there; back from a swipe page | [lvgl-dashboard.md](lvgl-dashboard.md) section 7 |
| panel steppers | tap; hold | one step; a step every 250 ms | `panel::kStepRepeatMs` |
| panel sliders, color wheel, arc | drag | sent on release | |
| P4 panel dialog | tap on the dimmed page | closes the dialog | |
| camera full screen | swipe; pinch | moves the picture; zooms | 8 px to start a move |
| Settings | `<` in the status bar | one page back; from quick settings closes Settings | |
| popup | tap outside | closes it | |

Rules that keep the gestures apart:

* **A drag that starts on a control stays with that control.** The level drag of a dimmer stays a
  tile gesture, a panel slider (`panel::slider_track()`) is dragged without starting a gesture of
  the screen. A new drag control does the same.
* **No swipe to close** (decided): a swipe up to close Settings fired while scrolling. Back is `<`.
* **No long press on a tile** except to start the edit mode; a dimmer's drag starts with movement,
  so the two do not collide.
* **The first 10 px decide between sideways and up or down** (2026-10-14): a sideways drag is a
  swipe of the page wherever it starts (it acts only on a swipe page), an up or down drag belongs
  to the dimmer under the finger or the pull-down zone. A swipe takes the rest of the press
  (`lv_indev_wait_release`), so the tile it started on is not tapped on release.
* What lies at the top center under the pull-down zone does not get taps there: the camera view's
  buttons sit below it (`overlay::kZoneHeight`). While a full-screen panel is open the zone keeps
  between the back button and the two header buttons (`overlay::fit_zone()`, 94 px wide at 320 px):
  on the JC3248W535 it covered most of the history button and took its taps (2026-10-17).
* While a full-screen panel slides in, the page beside it takes no taps (a clear cover under the
  panel until the slide ends); before, a tap there opened the tile under it (2026-10-17).
* While the dashboard is edited the pull-down is off; the alert strip shows only event messages
  and a drag on it does not open Settings.
* The camera view's buttons move down with the alert strip (`overlay::on_strip_moved()`), so a
  message of two lines never covers them.

## 3. Feedback: click, ticks and vibration

Each with its own switch in Settings, Sound ([lvgl-settings.md](lvgl-settings.md) section 3);
code: `sound.hpp`/`sound.cpp`, played through the `Backend`.

* **Click** for every tap on a tap target, when the finger lifts. Tap targets are LVGL's input
  widgets (button, switch, slider, arc, button matrix, keyboard) and plain objects marked with
  `sound::tappable()` (tiles, rows, custom buttons). A tap on a container, a backdrop or the
  screen, the end of a drag and the touch that only wakes the screen make none. **A custom tap
  target must call `sound::tappable()`**, or it is silent.
* **Ticks** while a slider or an arc moves, one per 1/25 of its range, and while a drag control
  marked with `sound::draggable()` moves (the dimmer's level), one per 14 px: the faster it moves,
  the more ticks.
* **Vibration** pulses when a finger touches a tap target, not when it lifts like the click: also
  for a press that turns into a scroll or a drag. It follows neither the volume nor the click's
  switch. Ticks do not vibrate.

## 4. Waking the screen

* A touch on a dimmed screen works as usual (and brings the brightness back).
* The touch that wakes a screen that is off only wakes it: a blocker on `lv_layer_sys()` takes it
  (`power.cpp`), so nothing under the finger acts.
* Every touch, typing included, restarts the dim and off timers and Settings' 5 minute timeout.

## 5. Controllers

The board reads its touch controller and hands LVGL the points; `ui` sees only LVGL's input
device. Two points (pinch) where the controller has them; the WT32-SC01's FT6336U makes one point
of two fingers closer than about 3 cm. The Windows version has one pointer (the mouse): no pinch.

## 6. Open points

* Whether a swipe from the left edge also goes back on a page with a back tile. On a swipe page
  (an area with `swipe`, 2026-10-14) a swipe right goes back from anywhere.
