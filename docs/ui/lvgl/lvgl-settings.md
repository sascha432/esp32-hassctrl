# Settings and first setup (LVGL)

Status: **running**. Settings on the touch screen (the status bar, the page stack, every page) and
the first setup. Reference resolution 480 x 320, landscape (WT32-SC01). Colors, fonts, sizes and
the building blocks of a page: [lvgl-design.md](lvgl-design.md). Code: `components/ui/src/settings.cpp`
(frame, stack, dialogs), `settings_pages.cpp`, `settings_sound.cpp`, `settings_alarm.cpp`,
`settings_camera.cpp`, `setup.cpp`, `keyboard.cpp`, `alarm.cpp`.

## 1. Frame and status bar

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

## 2. Navigation

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

## 3. Pages

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
|  Camera                                                 >  |
+------------------------------------------------------------+
|  Alarm           07:30, Mon-Fri                         >  |
+------------------------------------------------------------+
|  System                                                 >  |
+------------------------------------------------------------+
```

The Volume slider is only there with an audio output (`CONFIG_HC_AUDIO` on a board with an
amplifier, docs/architecture.md section 9.6), the Sound row with an audio output or a vibration
motor (section 9.7), the Camera row only with a camera whose sensor answered
(`CONFIG_HC_BOARD_HAS_CAMERA`, the JC4880P443C), the Alarm row only with the alarm
(`CONFIG_HC_AUDIO_ALARM`); the page scrolls then. The Alarm row shows the time and the days,
"Off", or "Snoozed until 07:40".

A row with a switch and no page of its own (Automatic, Hotspot on its page, the touch sounds, the
alarm) flips the switch with a tap anywhere on it; a row without any action takes no touches,
they scroll the page (2026-10-09: the switch alone was hard to hit, the row took the taps beside
it).

| Sub page | Content |
| --- | --- |
| Wi-Fi | state (network, signal, address), saved networks (up to 5, decided) in priority order (proposal), add network (the setup pages), reconnect. A saved network opens a detail page: change password, move up, move down, forget. How the order and the signal decide which network is used: [architecture.md](../../architecture.md) section 9.2 |
| Hotspot | on or off, name, password, QR code to join, new password, connected devices |
| Home Assistant | connection state, URL, last error, QR code to the web UI page where it is configured |
| Display | brightness (also on quick settings), dim after (never, 10 s to 1 h, default 5 min), dimmed brightness (its own slider, the screen shows the level while it is dragged), screen off (never, 1 min to 24 h, default 1 h); both times with `-` and `+` whose step grows with the time (10 s, 15 s ... 1 min, 2 min ... 1 h, 1 h 30 min ... 24 h), holding a button steps 4 times a second; a touch on a dimmed screen works as usual, the touch that wakes a screen that is off only wakes it (running). With a motion sensor: "Wake on motion" (on by default, in the backup `wake_on_motion`): while the sensor sees motion the screen is at the user's brightness, a screen that was off takes the next touch as usual, and the dim and off times count from the last motion as from a touch (Settings still close after their time without a touch); "Motion hold" (1 min to 1 h, default 5 min, a `-`/`+` row, in the backup `motion_hold_s`): Home Assistant's helper stays Detected until the sensor saw nothing for that long, every motion restarts it. With a light sensor: "Automatic" (on by default) with the measured lux and the backlight level; two rows, "Lowest brightness at" and "Full brightness at", take the light in the room now when tapped (shown in lx; a tap that would put the lowest point at or above the full point, or the reverse, shows "not darker" or "not brighter" for 2 s and changes nothing); the brightness then follows the room light linearly, from 1 % at the first light to the slider's value at the second and above (defaults 0 and 350 lx), smoothed over about 2 s. The sensor's own calibration belongs to the board (docs/adding-a-board.md); the two lights are the user's, stored in the display settings and in the backup (`light_min_lx`, `light_max_lx`). One "Rotation 90°" row: with an orientation sensor in charge a tap locks it ("Lock"); locked or without a sensor a tap opens a square window (200 px, always at the center of the display) with the title and the angle at the top, a square "Rotate" button (90 degrees clockwise) at its exact center and, with a sensor, "Use the sensor" at the bottom; nothing moves when the display turns. It closes 5 s after the last tap or with a tap outside (running). Later: orientation calibration |
| Sound | only with an audio output: "Volume" 0 to 100 % (default 50, also on quick settings; 0 is silent; letting go of the slider plays the click at the new volume, unless both touch sounds are off). "Touch sounds" (only with `CONFIG_HC_AUDIO_TOUCH_SOUND`), each on its own and on by default: "Click on taps", a short click for every tap on a tap target (buttons, switches, sliders, the keyboard, tiles, rows that open or do something), none for a tap beside them, the touch that only wakes the screen or the end of a drag; "Tick on sliders", soft ticks while a slider moves, more of them the faster it moves. A line under them: the touch sounds play at this volume, the alarm always at full volume. "Vibration" (only with a vibration motor, `CONFIG_HC_VIBRATION`): "Vibrate", on by default, a short pulse with every tap that would click (whatever the volume and "Click on taps" say) and pulses while the alarm rings; off means no vibration at all. With a motor and no audio the page has only this switch. Stored a few seconds after the last change (running since 2026-10-09) |
| Camera | only with a camera (2026-10-12). "Preview" opens the preview (below). "Picture": "Mains" 50 Hz or 60 Hz (default 60), the frequency of the power grid, whose light the exposure keeps from flickering in the picture. "Stream": "Stream" on or off (default off; on only with a login: without one the switch goes back and the login opens), "Login" (the username, or "not set"; opens the login form), "Size" the sensor's 1288x728 or half of it, 644x364, "Frames/s" 5, 10, 15 or 30 (at most; the stream sends no more), "Quality" of the JPEGs 30 to 95 (default 50). A line with the two addresses, `http://<device>/api/camera.mjpg` (the stream) and `/api/camera.jpg` (one picture), both behind the login (docs/ui/web/web-app.md). Each change is applied and stored at once |
| Camera login | username and password (up to 32 characters each), the eye shows the password, "Save" (OK and Enter save too, or go from the name to the password). A stored password is not shown: the field says "password (unchanged)", and an empty field keeps it. An empty username removes the login and turns the stream off |
| Camera preview | the camera's picture in the frame's shape: the whole picture at the top in portrait, beside a column of 300 px in landscape. The controls: "Auto exposure" on or off; on: "Brightness" -50 to +50 (moves the brightness the exposure aims at); off: "Exposure" 0.1 ms to the frame's longest (33 ms) and "Gain" 1x to 15.5x; "Mirror" and "Flip" (upside down), lit while on. Every change shows at once; stored a few seconds after the last one. The picture is drawn past LVGL (docs/architecture.md section 9.8): 30 fps without tearing |
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

## 4. First setup

Shown on the first boot and after a factory reset. The hotspot is on during the whole setup, and
the screen and the phone path run at the same time (decided).

**Built so far** (2026-10-07, `components/ui/src/setup.cpp`): Welcome, Choose Wi-Fi and Password
(the Settings pages, opened alone), Connecting, Connected and the failure. A network saved from
the phone also switches the screen to Connecting. `Next` on Connected goes to the dashboard;
the login and the hotspot choice are made in the web UI setup on the phone (see below).

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

**No Web UI login or Done page on the screen** (decided 2026-10-10, dropped from the design). The setup in the
browser on the phone (`web/app.js`, docs/ui/web/web-app.md section 1) has them, in three steps: Wi-Fi, Web UI
login (creating it also signs in), Done (address, `<name>.local`, "Keep the hotspot on"); tried and
confirmed by the user. The screen shows the address and `<name>.local` on Connected, and `Next` goes
to the dashboard, which shows a placeholder until Home Assistant is set up.

**Screen and phone at the same time.** There is no coordination: the first submission wins, and
the other path is not told (decided). It is an unlikely case and not worth extra code.

## 5. Open points

* None.
