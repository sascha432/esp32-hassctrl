# Adding a board

The procedure for bringing up a new board, with every check and calibration that was done for the
TTGO T4 and the WT32-SC01. Work through it in order: each step relies on the one before. The test
screen of the main firmware (`components/ui`, tabs "Display" and "Backlight") is the tool for
steps 4 to 7. It is only in the firmware while the board is in bring-up:
`CONFIG_HC_BOARD_BRING_UP=y` in `boards/<name>/sdkconfig.defaults` (step 2) shows it over the
start screen until a dashboard is drawn; step 12 removes the line again.

## 1. Collect the facts

Create `docs/devices/<NAME>/<NAME>.md` with the datasheets next to it. If the board already ran
another firmware, take its working settings as the starting point (the WT32-SC01 settings came
from kfc_fw's LovyanGFX configuration) and note where they came from.

| Fact | Where it ends up |
| --- | --- |
| module, chip, flash size, PSRAM | `boards/<name>/sdkconfig.defaults` |
| console port: a USB-UART bridge or the chip's USB-Serial/JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) | `boards/<name>/sdkconfig.defaults` |
| panel controller, interface (SPI, QSPI, RGB, MIPI-DSI), resolution, pins, clock | board source file |
| color order (RGB or BGR), inversion, mounting (which way is up) | board source file, `Info` |
| touch controller, bus, address, INT and RST pins (and whether INT is usable) | board source file |
| backlight pin and polarity | board source file |
| Wi-Fi in the chip, or on a companion chip (ESP32-P4 + C6: esp_hosted, its bus and pins, and the companion's firmware version, which has to match the host side) | `boards/<name>/sdkconfig.defaults`, `components/network/idf_component.yml` |
| shared I2C bus and the sensors on it (MPU6050 address) | board source file, `Info::imu_address` |
| speaker amplifier on I2S (often missing from the datasheet: look at working configurations of the module) | `Info::audio_*`, `select HC_BOARD_HAS_AUDIO` (step 9) |
| vibration motor and its GPIO | `Info::vibration_gpio`, `select HC_BOARD_HAS_VIBRATION` (step 10) |
| relays for Home Assistant | `Info::relay_gpios` (up to 4, all four named: `{32, -1, -1, -1}`; -1 = none), see architecture.md 9.7a |
| camera connector (MIPI-CSI): lanes, SCCB bus and address, power enable and reset pins, a clock from the chip or on the module, the MIPI PHY supply; the sensor on the module (chip ID) | device page, `select HC_BOARD_HAS_CAMERA` (step 11) |

## 2. Create the files

| File | Content |
| --- | --- |
| `boards/<name>/sdkconfig.defaults` | `CONFIG_IDF_TARGET`, `CONFIG_HC_BOARD_<NAME>=y`, `CONFIG_HC_BOARD_BRING_UP=y` (until step 12), flash size, partition table (`partitions/4mb.csv` or `16mb.csv`), PSRAM, `CONFIG_LV_COLOR_16_SWAP=y` for SPI panels with LVGL 8, options for the flash chip if the boot log asks for them |
| `components/board/Kconfig` | a `HC_BOARD_<NAME>` entry in the board choice, `depends on IDF_TARGET_...` |
| `components/board/CMakeLists.txt` | `if(CONFIG_HC_BOARD_<NAME>)` adds `boards/<name>/<name>.cpp` |
| `components/board/boards/<name>/<name>.cpp` | `Info`, `init()` and the backlight functions, like the existing boards |
| `components/board/idf_component.yml` | the panel and touch drivers from the component registry |

The `Info` fields:

| Field | How to find it |
| --- | --- |
| `width`, `height` | the resolution in the base orientation, the one LVGL sees at rotation 0 |
| `swap_xy`, `mirror_x`, `mirror_y` | step 4, "base orientation" |
| `swap_bytes` | `true` for SPI panels; must match `CONFIG_LV_COLOR_16_SWAP` (checked at boot) |
| `draw_buffer_lines` | 40 to start with; the SPI bus's largest transfer is sized from it |
| `full_frames` | `true` for a panel that cannot take a window, only whole frames from the top (AXS15231B over QSPI); LVGL then renders full frames into PSRAM |
| `mipi_dsi` | `true` for a MIPI-DSI video panel (ESP32-P4): esp_lvgl_port's DSI display, LVGL rotates in software |
| `imu_address` | 0x68 for an MPU6050 with AD0 low, 0 without a sensor |
| `backlight_hz` | step 6, "frequency" (25000 to start with) |
| `backlight_min_duty` | step 6, "minimum brightness" (0 to start with) |

## 3. Build, flash and first boot

Check which port the board is on before flashing: flashing replaces whatever runs on it. The
recovery app comes first, it also writes the bootloader and the partition table.

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
python tools/build.py <name> --app recovery -p COMx flash
python tools/build.py <name> -p COMx flash monitor
```

Check in the boot log:

- [ ] chip, flash size and PSRAM size match the device page (a PSRAM that is not found aborts the
      main firmware; the recovery app keeps running)
- [ ] the recovery app runs from `factory`, the main firmware from `ota_0`
- [ ] no warnings about the flash chip; add the option the log names to the board's defaults
- [ ] the bootloader fits before the partition table at 0x8000: on the ESP32-P4 it starts at
      0x2000 and has only 24 KB (the JC4880P443C needs bootloader log level "warning")
- [ ] with a Wi-Fi companion chip: the log shows matching esp_hosted versions on both chips

## 4. Display (tab "Display")

- [ ] **Colors**: the bars read red, green, blue, white from the left. Red and blue swapped:
      change the color order. Noise colors: the byte order (`swap_bytes` and
      `CONFIG_LV_COLOR_16_SWAP`).
- [ ] **No borders or offsets**: the screen is filled to every edge. An offset needs
      `esp_lcd_panel_set_gap()` in the board's `init()`.
- [ ] **Base orientation**: at rotation 0 the text is upright in the orientation the board is
      meant to be used in. Otherwise change `swap_xy`, `mirror_x`, `mirror_y`.
- [ ] **Rotation**: without an orientation sensor the screen turns every ten seconds. All four
      orientations upright, no tearing while it switches.
- [ ] **Speed**: note the full screen and partial redraw times from the log on the device page.
      The full screen time should be close to the wire time of the bus (pixels x 16 bits / clock).

## 5. Touch (tab "Display")

- [ ] The dot sits under the finger in every orientation. Otherwise change the `swap_xy`,
      `mirror_x` and `mirror_y` flags and `x_max`/`y_max` of the touch configuration: they map
      the controller's coordinates to the base orientation; LVGL turns them with the screen.
- [ ] Touches arrive when the INT line is used. If they do not, poll instead (INT not
      connected), as on the WT32-SC01.

## 6. Backlight (tab "Backlight")

- [ ] **Curve**: the perceptual curve feels even over the whole slider, the linear one gets
      choppy at the bottom. Perceptual is the default; this is a sanity check of the driver.
- [ ] **Frequency**: compare 1, 5 and 25 kHz below about 20 % brightness. Take the highest
      frequency that still behaves like the lower ones (a slow backlight driver swallows short
      pulses at high frequencies) and write it to `Info::backlight_hz`.
- [ ] **Minimum brightness**: step the raw duty down with "-" until the text is just readable,
      then "Use as minimum". Write the logged duty to `Info::backlight_min_duty`. While the test
      runs, the normal brightness comes back after ten seconds without a touch.
- [ ] **Contrast**: all 16 steps of the gray ramp and the color ramps stay distinguishable at
      low and at full brightness.

## 7. Orientation sensor (if the board has one)

- [ ] The boot log shows `MPU6050 at 0x..` and `gravity at start`.
- [ ] Turn the board through all four positions: upright every time, about half a second after
      it comes to rest.
- [ ] Reboot with the board leaning back on a desk: upright within about a second, without
      turning it.
- [ ] If the module is mounted differently from the old WT32-SC01, or reads wrong, the fixed
      mapping does not fit. Measure it once (developer step, nothing is stored on the device).
      Plain `orient` first: lying flat a healthy module reads about 1.00 g in length; the module
      of the new WT32 reads 1.55 g, with an offset of about (-0.12, -0.52, +0.23) g, and still
      rotated correctly with the fixed mapping, except near the 45 degree diagonals. Then, each
      pose held still for a second, panel standing exactly upright on a flat surface against
      something square (let go of it, do not hold it): `orient capture 0` in the base orientation (the way up the screen text reads at rotation 0, which
      is not necessarily what you would call landscape: lock the rotation to 0 in Settings, Display,
      or watch the test screen first; the first WT32 measurement was taken the other way up and
      showed landscape upside down),
      `orient capture 90` turned 90 degrees clockwise, `orient capture 180`, `orient capture 270`,
      and `orient capture flat` lying on the table with the screen up. Opposite poses cancel the
      offset. The console then prints the gain, the angle between 0 and 90 (refused unless 85 to
      95), the tilt of the module, and the lines `.imu_axes = {...}` and `.imu_offset = {...}` for
      the board info. Rest is decided from the accelerometer alone (the reading changes less than
      0.05 g between two samples, `orient` shows the largest change of the last second, about 0.01
      at rest); the gyroscope is not used, because a module can read 12 deg/s at rest, near the
      old limit of 15, and noise on it delayed a new orientation at random. Rebuild, check all four
      positions, and `orient` shows the reading without the
      offset (length about 1.00 g). A capture is refused while the device moves or the readings
      vary by more than 0.1 g in the last second.
- [ ] **I2C timeouts of the MPU6050** (found 2026-10-07 on the WT32): about one `ESP_ERR_TIMEOUT` in
      four seconds, also untouched. Cause: the orientation task ran at priority 2, below LVGL (4),
      hass (4) and network (5), and was starved for up to a second while the screen redrew. A
      transfer that is interrupted that long times out, samples go stale, and `orient capture`
      finds "no fresh reading". Keep the task above them (priority 6, it runs a few milliseconds
      every 100 ms). The module is read at 400 kHz like the other devices (it was 100 kHz for a while, a guess before the priority was found; stable on the T4 at 400), a timed out read is repeated twice, a gap
      over 400 ms in the samples logs `slow: ...`, and `orient` counts failed and retried reads
      (both 0 once the priority is right). If a new board shows failures again, look at the task
      priorities before the wiring.

- [ ] Settings, Display, the row "Rotation": with the sensor in charge a tap locks the rotation;
      locked or without a sensor a tap opens the rotation window ("Rotate" turns the screen 90
      degrees clockwise, "Use the sensor" hands it back; it closes 5 s after the last tap or with
      a tap outside). The rotation survives a restart.

## 8. Light sensor (BH1750, if the board type has one built in)

A step for the developer, not the user: the user only has the brightness slider, which sets how
bright the screen is at 350 lx and more. Two values go into the board info, both found with the
console command `light` (Automatic off on the Display page while measuring):

- `light_sensor_scale`: how much of the room light reaches the sensor where the board type has
  it (behind a window or a diffuser it sees less: the scale is above 1).
- `light_sensor_backlight_lux`: what the sensor sees of the screen itself at full brightness.
  The firmware takes it off in proportion to the backlight, so the screen does not brighten
  itself up.

Steps:

- [ ] The boot log shows `BH1750 at 0x..` with the scale in use; `light` prints the values.
- [ ] Scale: in a lit room, a light meter (a phone app will do) right next to the sensor
      opening: `light calibrate <lux of the meter>`. It prints the line for the board info.
- [ ] Screen light: in the dark, `light backlight` measures with the backlight off and at full
      and prints the line for the board info.
- [ ] Put both into the board file, build, and `light reset` (the device then uses the board
      values like every other device of the type).
- [ ] Check: Automatic on, cover the sensor: the screen dims within a few seconds; in the dark it
      does not climb by its own light.
- [x] Unplug test (a sensor on a cable): passed on the WT32; the read failures came from the task priority, see docs/TODO.md "MPU6050 I2C timeouts".

## 9. Speaker (if the board has an I2S amplifier)

- [ ] The amplifier's I2S pins (BCLK, WS, DOUT; MCLK and an enable pin if it has them) go into
      the board's `Info` (`audio_bclk_gpio`, `audio_ws_gpio`, `audio_dout_gpio`,
      `audio_mclk_gpio`), and its Kconfig choice entry gets `select HC_BOARD_HAS_AUDIO`
      (components/board/Kconfig). The `audio` component is then built by default
      (`CONFIG_HC_AUDIO`).
- [ ] A codec in front of the amplifier (ES8311 and the like) is set up by the board in `init()`
      on its I2C bus: `Info::audio_codec = true` and `audio_codec()` returning its start/stop
      (the JC4880P443C's `es8311.cpp` with `esp_codec_dev`). Boards without one return nullptr.
- [ ] The boot log shows `audio: I2S out BCLK .., WS .., DOUT ..`.
- [ ] Console `sound tone 1000 800`: a clean tone, no crackle; `sound volume 100` and
      `sound volume 5`: loud and quiet but audible; `sound click`: a short tick.
- [ ] Settings, Sound appears; tapping a tile clicks, tapping beside it does not.
- [ ] Listen for a pop when the first sound starts and a second after the last one (the channel
      stops then). Note it on the device page.

## 10. Vibration motor (if the board has one)

- [ ] The motor's GPIO goes into `Info::vibration_gpio` (high = on, through a transistor), and the
      board's Kconfig choice entry gets `select HC_BOARD_HAS_VIBRATION`.
- [ ] The boot log shows `vibration: motor on GPIO .., click .. ms`. `vibrate 300`, then
      `vibrate status` right away (`on`) and a second later (`off`).
- [ ] **The click length, measured by the developer**: try `vibrate <ms>` from short to long
      (e.g. 20, 40, 60, 80) and find the shortest pulse that gives a clear tap; shorter ones do
      nothing, and motors differ a lot (the JC4880P443C's 3 V test motor: nothing below 70 ms).
      A little above it goes into `Info::vibration_click_ms` (75 ms there). The firmware does not
      start the motor without it. It is a constant of the board type, not a setting.
- [ ] `vibrate alarm`: on and off every half second, `vibrate stop` ends it. Watch the boot log
      for a brownout while the motor starts (section Power below).
- [ ] Settings, Sound shows "Vibrate"; taps vibrate, the switch turns it off.

## 11. Camera (if the board has a MIPI-CSI connector)

Only the ESP32-P4 has a CSI controller and an ISP. The `camera` component knows one sensor, the
OV02C10 of the JC4880P443C (docs/devices/JC4880P443C, "Camera", and architecture.md section 9.8).

- [ ] From the schematic, on the device page: the connector's pinout, the lanes, where SCCB goes
      (the JC4880P443C: the touch bus), whether the module's power enable and reset reach the
      chip, whether the chip supplies a clock, and which LDO channel feeds the MIPI PHY (the
      JC4880P443C: channel 3 at 2.5 V, held by the display, so `dont_init_ldo`). Read the
      sensor's chip ID over SCCB before anything else; a sensor other than the OV02C10 needs
      its driver in `components/camera` (esp_cam_sensor's, or the vendor's with its changes in
      a README) and its tuning file.
- [ ] The board's Kconfig choice entry gets `select HC_BOARD_HAS_CAMERA`, and its
      `sdkconfig.defaults` the esp_video options of the JC4880P443C's (ISP pipeline controller
      on, DVP off). `board::Handles::i2c` must be the bus the sensor is on; `main` starts the
      camera with it.
- [ ] The boot log names the sensor and its format; console `camera` shows the capture.
      Measure the frame rate (30 fps on the OV02C10) and that it keeps it with the screen busy.
- [ ] The capture task runs on the core that created the display's DMA channels (core 0 on
      the JC4880P443C: they share the DW-GDMA interrupt group; from core 1 the allocation
      fails). Check this first if the capture fails to start.
- [ ] Settings, Camera, Preview: the picture fills the box, colors right (a wrong Bayer order
      gives magenta and green), no tearing. The preview draws into the panel's second frame
      buffer: a DPI panel needs `num_fbs = 2`; an SPI or QSPI panel has none, the preview then
      needs another way.
- [ ] Mirror and flip on the sensor (`camera mirror on`, `camera flip on`): the picture turns,
      the colors stay right (a window moved by one pixel keeps the Bayer order), the frames keep
      coming. Auto exposure: point it at a lamp and away, the picture settles within a second,
      no flicker at the mains frequency.
- [ ] The web stream: a login on Settings, Camera, then `/api/camera.jpg` and
      `/api/camera.mjpg` in a browser (frame rate, both sizes). Watch the boot log for a
      brownout while the camera and the Wi-Fi run (section Power below).

## 12. Record the results

- [ ] Every constant found above is in the board's `Info`; remove `CONFIG_HC_BOARD_BRING_UP=y`
      from `boards/<name>/sdkconfig.defaults`, rebuild: the firmware starts with the start screen
      and the test screen is no longer in it (the map file has no `show_test_screen`).
- [ ] Device page: verified settings, where they came from, the measurements of steps 4 and 6.
- [ ] `docs/architecture.md` section 8: the board in the board table.
- [ ] `docs/TODO.md`: what is still open for the board.

## Power: restarts with "power on" or "brownout"

A board that restarts with the reset reason "power on" or "brownout" (boot log `system: reset
reason`) has lost its supply, it did not crash. Before anything is changed in the firmware to
avoid it (a lower volume, a slower clock), discuss with the developer whether the power supply is
the cause: a USB port of a PC gives 0.5 A (USB 2.0), a hub shares its power, and a worn cable drops
voltage. The JC4880P443C recommends 5 V at 2 to 3 A (an AI summary on Reddit says at least 1.5 to
2 A for loud audio or a camera); its speaker restarted it on USB (2026-10-09)
and both boards restarted at random until their USB cables were replaced; with the new cable the
P4 plays at the DAC's full scale on a hub with 2.4 A (2026-10-14). Test with a proper
5 V supply and a short cable first.
