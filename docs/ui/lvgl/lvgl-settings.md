# Settings and first setup (LVGL)

Settings on the touch screen (the status bar, the page stack, every page) and
the first setup. Reference resolution 480 x 320, landscape (WT32-SC01). Colors, fonts, sizes and
the building blocks of a page: [lvgl-design.md](lvgl-design.md). Code: `components/ui/src/settings.cpp`
(frame, stack, dialogs), `settings_pages.cpp` (quick settings, Home Assistant, System, the shared
parts), `settings_wifi.cpp` (Wi-Fi, Hotspot), `settings_display.cpp`, `settings_sound.cpp`, `settings_alarm.cpp`,
`settings_camera.cpp`, `setup.cpp`, `keyboard.cpp`, `alarm.cpp`.

## 1. Frame and status bar

Settings and the first setup have the status bar (36 px) and the content area below it
(480 x 284). The dashboard has none.

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
| left | `<` back (left of the title) and the title of the page |
| right | state icons for Wi-Fi and Home Assistant (always shown, the glyph or color shows the state), Hotspot (only while it is on), the time |

* Only the time, no date. Without time sync it shows `--:--`.

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
  close: it fired while scrolling.
* Settings closes after 5 minutes without a touch. Every touch, including typing,
  restarts the timer. While a form has unsaved changes, the timeout is off.
* Two kinds of controls:
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
(`CONFIG_HC_AUDIO_ALARM`); the page scrolls then. The Alarm row shows the time and the days of
the alarm that rings next, "Off" (none is on), "None" (no alarm), or "Snoozed until 07:40".

A row with a switch and no page of its own (Automatic, Hotspot on its page, the touch sounds, the
alarm) flips the switch with a tap anywhere on it; a row without any action takes no touches,
they scroll the page (the switch alone was hard to hit).

| Sub page | Content |
| --- | --- |
| Wi-Fi | state (network, signal, address), saved networks (up to 5) in their order, add network (the setup pages), reconnect. A saved network opens a detail page: change password, move up, move down, forget. How the order and the signal decide which network is used: [architecture.md](../../architecture.md) section 9.2 |
| Hotspot | on or off, name, password, QR code to join, new password, connected devices |
| Home Assistant | connection state, URL, last error, QR code to the web UI page where it is configured |
| Display | brightness (also on quick settings), dim after (never, 10 s to 1 h, default 5 min), dimmed brightness (its own slider, the screen shows the level while it is dragged), screen off (never, 1 min to 24 h, default 1 h); both times with `-` and `+` whose step grows with the time (10 s, 15 s ... 1 min, 2 min ... 1 h, 1 h 30 min ... 24 h), holding a button steps 4 times a second; a touch on a dimmed screen works as usual, the touch that wakes a screen that is off only wakes it (running). With a motion sensor: "Wake on motion" (on by default, in the backup `wake_on_motion`): while the sensor sees motion the screen is at the user's brightness, a screen that was off takes the next touch as usual, and the dim and off times count from the last motion as from a touch (Settings still close after their time without a touch); "Motion hold" (1 min to 1 h, default 5 min, a `-`/`+` row, in the backup `motion_hold_s`): Home Assistant's helper stays Detected until the sensor saw nothing for that long, every motion restarts it. With a light sensor: "Automatic" (on by default) with the measured lux and the backlight level; two rows, "Lowest brightness at" and "Full brightness at", take the light in the room now when tapped (shown in lx; a tap that would put the lowest point at or above the full point, or the reverse, shows "not darker" or "not brighter" for 2 s and changes nothing); the brightness then follows the room light linearly, from 1 % at the first light to the slider's value at the second and above (defaults 0 and 350 lx), smoothed over about 2 s. The sensor's own calibration belongs to the board (docs/adding-a-board.md); the two lights are the user's, stored in the display settings and in the backup (`light_min_lx`, `light_max_lx`). One "Rotation 90°" row: with an orientation sensor in charge a tap locks it ("Lock"); locked or without a sensor a tap opens a square window (200 px, always at the center of the display) with the title and the angle at the top, a square "Rotate" button (90 degrees clockwise) at its exact center and, with a sensor, "Use the sensor" at the bottom; nothing moves when the display turns. It closes 5 s after the last tap or with a tap outside (running). Later: orientation calibration |
| Sound | only with an audio output: "Volume" 0 to 100 % (default 50, also on quick settings; 0 is silent; letting go of the slider plays the click at the new volume, unless both touch sounds are off). "Touch sounds" (only with `CONFIG_HC_AUDIO_TOUCH_SOUND`), each on its own and on by default: "Click on taps", a short click for every tap on a tap target (buttons, switches, sliders, the keyboard, tiles, rows that open or do something), none for a tap beside them, the touch that only wakes the screen or the end of a drag; "Tick on sliders", soft ticks while a slider moves, more of them the faster it moves. A line under them: the touch sounds play at this volume, the alarm always at full volume. "Vibration" (only with a vibration motor, `CONFIG_HC_VIBRATION`): "Vibrate", on by default, a short pulse with every tap that would click (whatever the volume and "Click on taps" say) and nothing else: each alarm has a Vibrate switch of its own. With a motor and no audio the page has only this switch. "Microphone" (only with one, `CONFIG_HC_MIC`), between the touch sounds and Vibration: "Level", a meter of the loudest sample of the last moments from -60 dBFS to full scale (a falling peak, orange above -6 dBFS) with a mark at -18 dBFS where speech should be, read only while the page is open; "Record" under it records up to 5 s, from 200 ms after the tap so its click is not in it (the button counts down as "Stop (n s)", a tap ends it early), then plays it once at the volume ("Playing...", no taps until it ends; "Volume is 0" for 2 s when the volume is 0); leaving the page ends a recording; "Save" beside it (only with `CONFIG_HC_SDCARD`) writes the last recording as a WAV file to the SD card's `recordings` folder, named by date and time, and says "Saved", "Record first", "No SD card" or "Save failed" for 2 s (45 ms for 5 s of sound); "Gain" 0 to 60 dB (default 42), greyed out without touches while Auto gain is on; "Auto gain", on by default, the codec's level control holds a voice at the mark; a line under them explains both. Stored a few seconds after the last change |
| Camera | only with a camera. "Preview" opens the preview (below). "Picture": "Mains" 50 Hz or 60 Hz (default 60), the frequency of the power grid, whose light the exposure keeps from flickering in the picture. "Stream": "Stream" on or off (default off; on only with a login: without one the switch goes back and the login opens), "Login" (the username, or "not set"; opens the login form), "Size" the sensor's 1288x728 or half of it, 644x364, "Frames/s" 5, 10, 15 or 30 (at most; the stream sends no more), "Quality" of the JPEGs 30 to 95 (default 50). A line with the two addresses, `http://<device>/api/camera.mjpg` (the stream) and `/api/camera.jpg` (one picture), both behind the login (docs/ui/web/web-app.md). Each change is applied and stored at once |
| Camera login | username and password (up to 32 characters each), the eye shows the password, "Save" (OK and Enter save too, or go from the name to the password). A stored password is not shown: the field says "password (unchanged)", and an empty field keeps it. An empty username removes the login and turns the stream off |
| Camera preview | the camera's picture in the frame's shape: the whole picture at the top in portrait, beside a column of 300 px in landscape. The controls: "Auto exposure" on or off; on: "Brightness" -50 to +50 (moves the brightness the exposure aims at); off: "Exposure" 0.1 ms to the frame's longest (33 ms) and "Gain" 1x to 15.5x; "Mirror" and "Flip" (upside down), lit while on. Every change shows at once; stored a few seconds after the last one. The picture is drawn past LVGL (docs/architecture.md section 9.8): 30 fps without tearing |
| Alarm | only with the alarm: "Next" (the alarm that rings next: in 7 h 12 min, or the day and time when it is more than a day away; "Waiting for the clock" without SNTP; "Off"); "Alarms (2 of 5)" with a row per alarm (the time, the days, its on/off switch; a tap opens its page); "Add alarm" (up to 5) adds 07:00 Mon-Fri, on, the beeps, Vibrate on, and opens its page; a line: an alarm rings at full volume until Stop or Snooze, at most 10 minutes |
| Alarm (one) | "Alarm" on or off; "Next" of this alarm; the time on two rollers (hour, minute; setting it turns the alarm on; a quick drag moves them up to 4x the finger, a fling further); the days as seven buttons Mon to Sun (none: once, then it turns itself off); "Snooze" 5, 10, 15, 20 or 30 min (a tap steps); "Vibrate" on or off (only with a vibration motor; the motor's pattern while this alarm rings); the sound: "Beeps" (built in) and the MP3 files of `/fs/sounds/alarms`, a tap selects it and plays it once at full volume (with the motor when Vibrate is on), leaving the page stops it; "Delete alarm" (red) after a question. Every change is stored at once |
| System | device name, firmware version, address, MAC, uptime, memory, "SD card" (only with `CONFIG_HC_SDCARD`: "7.4 of 29.0 GB free", MB below 1 GB, or no card, not formatted, unmounted); reset web UI login; restart; factory reset |

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

**Hotspot:** the password is shown whenever the hotspot is on.

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

**Alarm:**

```
+------------------------------------------------------------+
| < Alarm                                       [wifi] 10:29 |
+------------------------------------------------------------+
|  Next                                      in 0 h 1 min    |
|  Alarms (2 of 5)                                           |
|  07:00                              Mon-Fri  >  [  o]      |
|  10:30                              Mon-Fri  >  [o  ]      |
|  [  Add alarm  ]                                           |
+------------------------------------------------------------+
```

**Alarm screen:** over everything (`lv_layer_top`), the screen wakes and stays on while it rings.
Snooze is the large target; both end the sound. 10 minutes without a tap end it like Stop. The
snooze time, the sound and the motor are the ringing alarm's.

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
the screen and the phone path run at the same time.

**The screens** (`components/ui/src/setup.cpp`): Welcome, Choose Wi-Fi and Password
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

**No Web UI login or Done page on the screen.** The setup in the
browser on the phone (`web/app.js`, docs/ui/web/web-app.md section 1) has them, in three steps: Wi-Fi, Web UI
login (creating it also signs in), Done (address, `<name>.local`, "Keep the hotspot on"). The screen shows the address and `<name>.local` on Connected, and `Next` goes
to the dashboard, whose start screen offers "Set up Home Assistant" until it is
([lvgl-dashboard.md](lvgl-dashboard.md) section 1.2).

**Screen and phone at the same time.** There is no coordination: the first submission wins, and
the other path is not told. It is an unlikely case and not worth extra code.

## 5. Open points

* None.
