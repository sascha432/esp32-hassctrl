# Screen layout

Status: **proposal**. Reference resolution 480 x 320, landscape (WT32-SC01). Terms, colors and
sizes are in [design-system.md](design-system.md).

## 1. Dashboard frame and the pull-down gesture

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
| "Swipe down from the top for Settings" | after the first setup | the first pull-down (decided). A tap leaves it in place |

The strip is as wide as its message needs, so it can get large. It keeps a small gap to the
left and right screen edges, so the rounded corners and the transparency stay visible (decided).
Proposals: a long message wraps to at most 3 lines; one message at a time, errors before warnings
before hints.

### 1.2 Start screen

Running on the JC4880P443C (user, 2026-10-09). From the start until Home Assistant has sent the values of the dashboard's first page, the
dashboard is covered by the Home Assistant logo (160 x 160, at the center, on the dashboard
background). The runner of an action on its way (docs/ui/dashboard.md section 3) goes round the
house, 5 px outside its edge, clockwise from the top. The dashboard is built under the logo, so it
is complete when the logo goes. The alert strip and the pull-down work as usual (both lie above
it); the first setup opens over it as before. A touch on the logo does nothing.

* The logo goes when Home Assistant is connected and the values of the page came, or 10 s after
  the connection when they do not come (a page without entities).
* Without a dashboard nothing more comes: once Home Assistant is connected the runner stops and a
  line below the logo says "No dashboard yet. Create one in the web UI at http://...". The logo
  goes once a dashboard is saved and its values came.
* It is shown once per start: a lost connection later shows the alert strip over the dashboard.

The backlight stays off from the start until LVGL has drawn the first frame, then fades up in
0.5 s (power.hpp); before it the panel would show what it held before the start. Before every
restart (board, `esp_restart()` shutdown handler) the backlight turns off and the panel is
switched off and put to sleep: on the JC4880P443C the backlight pin is undriven for about 1.3 s
until the next start takes it, and the ST7701S showed cyan without a signal.

The logo comes from the icon of dashboardicons.com, rendered by `tools/build_logo.py` into two
4-bit alpha masks (the white tree, the blue face; 25 KB) and the runner's track.

## 2. Settings frame and status bar

Settings and the first setup have the status bar (36 px) and the content area below it
(480 x 284). The dashboard has none (decided).

```
+------------------------------------------------------------+
| < Settings                               [wifi] [HA] 12:41 |
+------------------------------------------------------------+
|                                                            |
|                                                            |
|                 content area, 480 x 284 px                 |
|                                                            |
|                                                            |
+------------------------------------------------------------+
```

| Part | Content |
| --- | --- |
| left | `<` back (decided: left of the title) and the title of the page |
| right | state icons for Wi-Fi and Home Assistant (always shown, the glyph or color shows the state), Hotspot (only while it is on), the time |

* Only the time, no date (decided). Without time sync it shows `--:--` (decided). A screensaver
  with date and time may come later.

```
+------------------------------------------------------------+
| < Wi-Fi                                  [wifi] [HA] --:-- |
+------------------------------------------------------------+
```

A Wi-Fi or Home Assistant problem replaces the title with a short error in the alert color. A tap
on the error opens the matching page.

## 3. Navigation

* The dashboard is the home screen. Settings opens on top of it with the pull-down gesture.
* Settings is a stack: quick settings, then a sub page, then a detail page. `<` in the status
  bar goes one level back. From quick settings it closes Settings. There is no swipe up to
  close: it fired while scrolling (decided).
* Settings closes after 5 minutes without a touch (decided). Every touch, including typing,
  restarts the timer. While a form has unsaved changes, the timeout is off (decided).
* Two kinds of controls (decided):
  * **live controls** such as sliders and switches apply immediately and are saved with a delayed
    write;
  * **forms** such as a Wi-Fi password have a Cancel and a Save button and are saved on Save.
* Destructive actions (factory reset, forget network, reset web UI login) ask in a dialog. Cancel
  is on the left, the destructive button on the right in the error color.

## 4. First setup

Shown on the first boot and after a factory reset. The hotspot is on during the whole setup, and
the screen and the phone path run at the same time (decided).

**Built so far** (2026-10-07, `components/ui/src/setup.cpp`): Welcome, Choose Wi-Fi and Password
(the Settings pages, opened alone), Connecting, Connected and the failure. A network saved from
the phone also switches the screen to Connecting. `Next` on Connected goes to the dashboard;
the Web UI login and Done pages below are not built yet: the login is created in the web UI,
and the hotspot goes off by its own timer.

**Welcome.** Both paths on one screen: the left column starts the setup on the screen, the right
column has the QR code that joins the hotspot, plus its name and password. The QR code is about
164 px with its quiet zone.

```
+------------------------------------------------------------+
| Setup                                            [hotspot] |
+-----------------------------+------------------------------+
|                             | With your phone              |
|  Set up on this screen      |  +--------+                  |
|                             |  |  QR    |  1. Scan to join |
|  +----------------------+   |  |  join  |     the hotspot  |
|  |     Choose Wi-Fi     |   |  +--------+                  |
|  +----------------------+   |  Hotspot   hassctrl-1a2b     |
|                             |  Password  48213907kx        |
|                             |  2. The setup page opens,    |
|                             |     or http://192.168.4.1    |
+------------------------------------------------------------+
```

**Choose Wi-Fi.** Scan results sorted by signal strength, `Other network ...` for a hidden network
or manual entry. A tap on a network opens the password page.

```
+------------------------------------------------------------+
| < Choose Wi-Fi                                    [rescan] |
+------------------------------------------------------------+
|  Home-Net                                 [lock] ||||   >  |
+------------------------------------------------------------+
|  Neighbor                                 [lock] |||    >  |
+------------------------------------------------------------+
|  Guest                                           ||     >  |
+------------------------------------------------------------+
|  Other network ...                                      >  |
+------------------------------------------------------------+
```

**Password.** The LVGL keyboard takes the lower half. `show` toggles the password visibility,
which replaces a confirmation field.

```
+------------------------------------------------------------+
| < Home-Net                                                 |
+------------------------------------------------------------+
|  Password  [**************        ]  [show]  [Connect]     |
+------------------------------------------------------------+
|   q   w   e   r   t   y   u   i   o   p      [<-]          |
|   a   s   d   f   g   h   j   k   l      [Enter]           |
|  [^]  z   x   c   v   b   n   m   ,   .      [?123]        |
|  [ABC]      [          space          ]      [hide]        |
+------------------------------------------------------------+
```

**Connecting, then the result.** Success shows the address and the mDNS name:

```
+------------------------------------------------------------+
| Setup                                     [hotspot] [wifi] |
+------------------------------------------------------------+
|                                                            |
|            Connected to Home-Net                           |
|            Address  192.168.1.57                           |
|                     hassctrl-1a2b.local                    |
|                                                            |
|                                         +----------+       |
|                                         |   Next   |       |
|                                         +----------+       |
+------------------------------------------------------------+
```

A failure names the reason from the Wi-Fi driver (wrong password, network not found, no answer):

```
+------------------------------------------------------------+
| Setup                                            [hotspot] |
+------------------------------------------------------------+
|                                                            |
|            Could not connect to Home-Net                   |
|            The password is wrong.                          |
|                                                            |
|     +-----------------+           +-----------------+      |
|     |  Other network  |           |    Try again    |      |
|     +-----------------+           +-----------------+      |
+------------------------------------------------------------+
```

**Web UI login.** The username is prefilled with `admin`. The keyboard opens below while a field is
active.

```
+------------------------------------------------------------+
| < Web UI login                                             |
+------------------------------------------------------------+
|  The web UI is where Home Assistant is connected.          |
|                                                            |
|  Username  [admin                 ]                        |
|  Password  [**********            ]  [show]   [Next]       |
|                                                            |
|  (the keyboard opens below while a field is active)        |
+------------------------------------------------------------+
```

**Done.** The option to keep the hotspot on (decided), and the QR code with the address of the web
UI, where Home Assistant is connected. Without the option, the hotspot still stays on for a while
with the same password, so a phone can see the result (decided; proposal: 15 minutes).

```
+------------------------------------------------------------+
| Setup done                                          [wifi] |
+-----------------------------+------------------------------+
|                             |                              |
|  Hotspot after setup        |  +--------+                  |
|  [ ] Keep the hotspot on    |  |  QR    |  Open the web UI |
|                             |  |  web   |  to connect      |
|                             |  +--------+  Home Assistant  |
|  +--------+                 |  http://192.168.1.57         |
|  |  Done  |                 |  hassctrl-1a2b.local         |
|  +--------+                 |                              |
+------------------------------------------------------------+
```

After `Done` the dashboard shows a placeholder until Home Assistant is set up: a short text, the
same QR code, and the hint about the pull-down gesture.

**Screen and phone at the same time.** There is no coordination: the first submission wins, and
the other path is not told (decided). It is an unlikely case and not worth extra code.

## 5. Settings

**Quick settings**, the first page. Six rows of 44 px fit without scrolling.

```
+------------------------------------------------------------+
| < Settings                               [wifi] [HA] 12:41 |
+------------------------------------------------------------+
|  Brightness   [=============o----------------]   60 %      |
+------------------------------------------------------------+
|  Volume       [=========o--------------------]   50 %      |
+------------------------------------------------------------+
|  Wi-Fi           Home-Net, 192.168.1.57                 >  |
+------------------------------------------------------------+
|  Hotspot         Off                                [  o]  |
+------------------------------------------------------------+
|  Home Assistant  Connected                              >  |
+------------------------------------------------------------+
|  Display                                                >  |
+------------------------------------------------------------+
|  Sound                                                  >  |
+------------------------------------------------------------+
|  Alarm           07:30, Mon-Fri                         >  |
+------------------------------------------------------------+
|  System                                                 >  |
+------------------------------------------------------------+
```

The Volume slider and the Sound row are only there with an audio output (`CONFIG_HC_AUDIO` on a
board with an amplifier, docs/architecture.md section 9.6), the Alarm row only with the alarm
(`CONFIG_HC_AUDIO_ALARM`); the page scrolls then. The Alarm row shows the time and the days,
"Off", or "Snoozed until 07:40".

A row with a switch and no page of its own (Automatic, Hotspot on its page, the touch sounds, the
alarm) flips the switch with a tap anywhere on it; a row without any action takes no touches,
they scroll the page (2026-10-09: the switch alone was hard to hit, the row took the taps beside
it).

| Sub page | Content |
| --- | --- |
| Wi-Fi | state (network, signal, address), saved networks (up to 5, decided) in priority order (proposal), add network (the setup pages), reconnect. A saved network opens a detail page: change password, move up, move down, forget. How the order and the signal decide which network is used: [architecture.md](../architecture.md) section 9.2 |
| Hotspot | on or off, name, password, QR code to join, new password, connected devices |
| Home Assistant | connection state, URL, last error, QR code to the web UI page where it is configured |
| Display | brightness (also on quick settings), dim after (never, 10 s to 1 h), dimmed brightness (its own slider, the screen shows the level while it is dragged), screen off (never, 1 min to 24 h); both times with `-` and `+` whose step grows with the time (10 s, 15 s ... 1 min, 2 min ... 1 h, 1 h 30 min ... 24 h), holding a button steps 4 times a second; a touch on a dimmed screen works as usual, the touch that wakes a screen that is off only wakes it (running). With a light sensor: "Automatic" (on by default) with the measured lux and the backlight level; the brightness then follows the room light linearly, from 1 % in the dark to the slider's value at 350 lx and more (175 lx gives half of it), smoothed over about 2 s. The sensor's calibration belongs to the board (docs/adding-a-board.md), the user only has the slider. One "Rotation 90°" row: with an orientation sensor in charge a tap locks it ("Lock"); locked or without a sensor a tap opens a square window (200 px, always at the center of the display) with the title and the angle at the top, a square "Rotate" button (90 degrees clockwise) at its exact center and, with a sensor, "Use the sensor" at the bottom; nothing moves when the display turns. It closes 5 s after the last tap or with a tap outside (running). Later: orientation calibration |
| Sound | only with an audio output: "Volume" 0 to 100 % (default 50, also on quick settings; 0 is silent; letting go of the slider plays the click at the new volume). "Touch sounds" (only with `CONFIG_HC_AUDIO_TOUCH_SOUND`), each on its own and on by default: "Click on taps", a short click for every tap on a tap target (buttons, switches, sliders, the keyboard, tiles, rows that open or do something), none for a tap beside them, the touch that only wakes the screen or the end of a drag; "Tick on sliders", soft ticks while a slider moves, more of them the faster it moves. A line under them: the touch sounds play at this volume, the alarm always at full volume. Stored a few seconds after the last change (running since 2026-10-09) |
| Alarm | only with the alarm: "Alarm" on or off; "Next" (in 7 h 12 min, or the day and time when it is more than a day away; "Waiting for the clock" without SNTP); the time on two rollers (hour, minute; setting it turns the alarm on); the days as seven buttons Mon to Sun (none: once, then it turns itself off); "Snooze" 5, 10, 15, 20 or 30 min (a tap steps); the sound: "Beeps" (built in) and the MP3 files of `/fs/sounds/alarms`, a tap selects it and plays it once at full volume, leaving the page stops it; a line: it rings at full volume until Stop or Snooze, at most 10 minutes. Every change is stored at once (running since 2026-10-09) |
| System | device name, firmware version, address, MAC, uptime, memory; reset web UI login; restart; factory reset |

**Wi-Fi:**

```
+------------------------------------------------------------+
| < Wi-Fi                                       [wifi] 12:41 |
+------------------------------------------------------------+
|  Connected to Home-Net    signal good    192.168.1.57      |
+------------------------------------------------------------+
|  Saved networks (2 of 5)                                   |
|  1  Home-Net                                connected   >  |
|  2  Office                                              >  |
+------------------------------------------------------------+
|  [  Add network  ]    [  Reconnect  ]                      |
+------------------------------------------------------------+
```

**Hotspot:** the password is shown whenever the hotspot is on (decided).

```
+------------------------------------------------------------+
| < Hotspot                                  [hotspot] 12:41 |
+-----------------------------+------------------------------+
|  Hotspot             [o  ]  |  +--------+                  |
|                             |  |  QR    |  Scan to join    |
|  Name      hassctrl-1a2b    |  |  join  |                  |
|  Password  48213907kx       |  +--------+                  |
|                             |                              |
|  2 devices connected        |                              |
|  [  New password  ]         |                              |
+------------------------------------------------------------+
```

**Alarm screen:** over everything (`lv_layer_top`), the screen wakes and stays on while it rings.
Snooze is the large target; both end the sound. 10 minutes without a tap end it like Stop.

```
+------------------------------------------------------------+
|                         (bell) Alarm                        |
|                            07:30                            |
|      +----------------------------------------------+      |
|      |                 Snooze 10 min                |      |
|      +----------------------------------------------+      |
|      |                     Stop                     |      |
|      +----------------------------------------------+      |
+------------------------------------------------------------+
```

**Confirmation dialog:**

```
+------------------------------------------------------------+
| < System                                      [wifi] 12:41 |
+------------------------------------------------------------+
|                                                            |
|        +--------------------------------------------+      |
|        |  Factory reset?                            |      |
|        |  All settings, networks and the Home       |      |
|        |  Assistant token are deleted.              |      |
|        |                                            |      |
|        |   [  Cancel  ]            [   Reset   ]    |      |
|        +--------------------------------------------+      |
+------------------------------------------------------------+
```

## 6. Other resolutions

The layout adapts to the resolution instead of being drawn per board. Sizes in use or planned:
480 x 320, 320 x 480, 640 x 480, 480 x 800, each in landscape and portrait. More can be
added later.

* **Landscape and portrait** on every board (decided). Where an MPU6050 is connected, the screen
  rotates with the device (decided), see [architecture.md](../architecture.md) section 8.1.
* **Portrait**: two-column pages (welcome, done, hotspot) stack their columns, QR code on top.
  Lists and forms stay one column. The keyboard is full width. The dashboard uses the transposed
  grid of the old firmware; tiles moved in edit mode keep a cell of their own per orientation
  ([architecture.md](../architecture.md) section 5, "Orientation").
* **Higher DPI**, such as 480 x 800 at 4.3 inch: sizes scale with the DPI (see
  [design-system.md](design-system.md) section 6).
* **240 x 320 without touch (TTGO T4, test device only)**: everything is shown as on the other
  boards, touch just does nothing (decided). Touch is a requirement for supported boards
  (decided).

## 7. Open points

* Whether a swipe from the left edge also goes back.
* The height of the pull-down start zone, the line limit and the message order of the alert
  strip (section 1).
