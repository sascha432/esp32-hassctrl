# Rendering benchmark

How fast the screens draw on a board, measured the same way on every board: a fixed dashboard
on fixed sample data, fixed scenes, every frame timed. It answers "is this change faster or slower
on the boards" with numbers that can be compared across builds and boards. It decided against
LVGL 9 (section 7). The result files are in [benchmarks/](benchmarks/).

## 1. Running it

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1 | Out-Null
python tools/bench.py wt32_sc01 -p COM9 --runs 3 --label lvgl8.4
python tools/bench.py jc4880p443c -p COM11 --runs 3 --label my-change
python tools/bench.py --compare docs/benchmarks/wt32_sc01-lvgl8.4.json docs/benchmarks/wt32_sc01-my-change.json
```

* The board needs the firmware of the commit to measure and the development console
  (`CONFIG_HC_DEV_CONSOLE`). `<board>` is the name given to `tools/build.py` (variant included);
  it names the result file and the build directory the firmware size is read from.
* A pass takes about three minutes: the restart, about 25 s for the boot and the sample values, two
  minutes of scenes, the output. Three passes (`--runs 3`, the default) give the medians; a pass
  whose output arrives incomplete (a line lost on the serial port, a board still booting) is
  repeated, up to `--runs` times.
* Nothing on the board changes. Each pass restarts it into benchmark mode; after the last one it
  restarts into its normal mode with its own dashboard and Home Assistant. Benchmark mode writes
  nothing to flash.
* `--compare A B` prints the scenes of two results side by side with the change of B against A,
  the memory figures and the firmware sizes. Results of another benchmark version are refused.
* The Windows version runs the same scenes (`python tools/sim.py --size wt32 --time 1770026910
  --dashboard bench/dashboard/hass.yaml --do "wait 1500" --do bench`): it checks that every scene
  works after a change of the screens; its times are the PC's and mean nothing.

## 2. Benchmark mode

The console command `bench start` restarts the board into benchmark mode (`main/bench_mode.cpp`,
a flag in RTC memory that only a software restart keeps; the boot after it is a normal one again).
In that boot:

* the screens run on the **sample backend** (`components/sample_backend`, the same data the
  Windows version shows) instead of `main/ui_backend.cpp`: every entity has a value made up from its
  domain and name, the history, the forecast and a drawn camera image included;
* the dashboard is the **benchmark dashboard** built into the firmware (`bench/dashboard/hass.yaml`),
  not the stored one;
* there is **no network** (no Wi-Fi, no Home Assistant, no web server) and no orientation sensor:
  nothing but the screens runs;
* the **clock starts at a fixed time** (Mon 2 Feb 2026, 10:08:30 UTC) and runs on with LVGL's tick,
  so the clocks and the camera's sun look the same on every run;
* the sample backend prints nothing, the backlight is the board's.

`bench run` then runs the scenes. While they run the ESP-IDF log is down to errors and the
performance monitor in the corner is hidden; the result lines are collected in PSRAM and printed
after the last scene, line by line with a pause (a USB console drops what does not fit its buffer),
ending with `BENCH-END <lines>`, which `bench.py` checks.

## 3. The benchmark dashboard

`bench/dashboard/hass.yaml`, the same on every board (its `landscape`/`portrait` sizes fit
480x320, 320x480 and 480x800):

| Page | Grid | Tiles |
| --- | --- | --- |
| main | 4x3 | dimmer light 1x2, thermostat 1x2, switch, temperature sensor, lock, timer, camera 2x1, the areas Dense and Large |
| Dense | 6x4 | 23 small tiles: switches, lights, sensors, binary sensors, a fan, a scene, a lock, and the back tile |
| Large | 4x2 | analog clock 1x2 (minute ticks, rim, seconds, date), digital clock 2x1 (seconds, date), weather, automation, back tile |

It is frozen. `bench/VERSION` is the version of the dashboard and of the sample data,
`kScenesVersion` in `components/ui/include/hc/ui/bench.hpp` the version of the scenes; every result
records both, and `--compare` refuses two results whose versions differ. A change to any of them
raises its version and needs a new baseline on every board.

## 4. The scenes

`components/ui/src/bench.cpp`, in this order. A **redraw** scene invalidates the whole screen 20
times, each time after the frame before was drawn; the others run for a fixed time and measure the
frames that come.

| Scene | What it does | Draws |
| --- | --- | --- |
| `main_redraw` | the main page, 20 full frames | every tile kind, the camera image |
| `dense_slide_in`, `dense_redraw`, `dense_back` | the dense page slides in, is redrawn, slides back | many small cards, labels, icons |
| `light_panel_open`, `_redraw`, `_close` | the light panel over the page (a dialog on the P4) | slider, buttons, the panel's animation |
| `climate_panel_*` | the climate panel | the arc, rollers |
| `sensor_panel_*` | the sensor panel | the history chart |
| `lock_panel_*`, `timer_panel_*` | lock and timer panels | buttons, the timeline |
| `picture_panel_*` | the camera full screen | a whole-screen image, zoom |
| `pending_runner` | a switch waits for its state: the glow dot runs round its card | small moving areas with alpha |
| `alert_strip` | a message in the strip at the top for 1.5 s | the top layer, a fade |
| `large_slide_in`, `clocks_tick` | the large page slides in; its clocks tick for 4 s | the hand-drawn analog face, big digits |
| `weather_panel_*`, `automation_panel_*` | the weather panel (forecast) and the automation panel | lists, icons |
| `settings_open`, `settings_scroll`, `settings_close` | Settings, Display opens (Alarm until 2026-10-21, a list since); a finger drags up the right margin in 300 ms and lets go; Settings closes | a long scrolling page, the fling |
| `rotate_90`, `main_redraw_90` | the screen turns by 90° (the page is built again), the main page is redrawn 20 times, then it turns back | the software rotation where the board has it |

## 5. What it measures

Per frame, a refresh of LVGL's display timer that flushed something:

| Value | What |
| --- | --- |
| `total` | the whole refresh, from the timer's start to its end: layout, rendering, flushing, waiting |
| `flush` | of it: the time in the driver's flush callback and in waiting for the band before it |
| `render` | `total` minus `flush` |
| `pixels` | the pixels flushed |

The times come from `esp_timer` around LVGL's refresh timer (its callback is wrapped) and around
the display driver's `flush_cb` and `wait_cb` (the wait is done inside `wait_cb`, so one call is one
wait). Per scene the result has the frames, the time, the frames per second, and min, median, 95th
percentile and max of each time in ms; before and after the scenes the free internal, DMA and
PSRAM memory, the largest blocks and the LVGL task's stack left (`lvgl_stack_free`). `bench.py`
keeps the median of every value over the runs and `spread.total.med`, how far the median frame
time of a scene differed between the runs.

## 6. Reading the numbers

* The **redraw** scenes are the steadiest: their median moves by well under 1 % between runs of
  one firmware. The open and close scenes have few frames that differ from each other (the first
  frame builds the panel); their medians are coarser, compare their `fps` and `p95` instead.
* `fps` of a scene that waits (an animation, the clocks, the runner) is how many frames it needed,
  not how many the board could draw. `clocks_tick` depends on where the seconds fall.
* On the WT32 the **SPI transfer overlaps rendering**: `flush` is small (3 to 5 ms) because the two
  partial buffers are sent while the next band renders; on the JC3248W535 `flush` is the frame sent
  to the panel in bands (about 16 ms); on the P4 the CPU copies the bands into the frame buffer
  (about 10 ms, 25 ms with the software rotation).
* **The ESP32 (WT32-SC01) depends on the build.** It runs LVGL out of a 32 KB flash cache, and where
  the linker places the code decides how often it misses. Four builds that differ only in code that
  is not drawn gave 91, 104, 108 and 132 ms for the same full redraw of the main page (each steady
  within 1 % over its runs, `wt32_sc01-check-same.json` repeated one of them), and every scene moved
  together. A difference under about 30 % between two WT32 builds says nothing about the code;
  measure such a change on the JC3248W535 or the P4, which do not show it (31.4 and 31.4 ms on two
  P4 builds, 50.7 and 51.2 ms on two JC3248W535 builds).

* **Flash and PSRAM speeds** of the boards, which the cache misses pay for:

  | Board | Flash | PSRAM | CPU |
  | --- | --- | --- | --- |
  | WT32-SC01 (ESP32) | QIO, 80 MHz | quad, 80 MHz | 240 MHz |
  | JC3248W535 (ESP32-S3) | QIO, 80 MHz | octal, 80 MHz | 240 MHz |
  | JC4880P443C (ESP32-P4) | QIO, 80 MHz | hex, 200 MHz | 400 MHz |

  Until 2026-10-30 the WT32 ran at ESP-IDF's defaults, DIO 40 MHz flash and 40 MHz PSRAM, a
  quarter of the others' flash speed; QIO at 80 MHz for both halved its frame times (section 7).
  Its flash (XMC XM25QH32, ID 0x20 0x4016) and PSRAM take it, and the PSRAM test at boot passes.
  The flash mode is in the bootloader: after a change, flash the recovery app (it brings the
  bootloader) and then the main firmware. The build spread above was measured at 40 MHz. Flash at 120 MHz on the
  JC3248W535 does not build: its octal PSRAM runs in DDR, so at 80 MHz the MSPI core clock both
  share is 160 MHz while 120 MHz flash needs 240 MHz (`mspi_timing_tuning_configs.h` asserts they
  match). Only octal PSRAM at 120 MHz would match it, an experimental setting that ESP-IDF says
  crashes when the chip's temperature drifts by about 20 °C. Espressif's esp_lvgl_port notes reach
  120 MHz on an S3 with quad PSRAM (SDR, 120 MHz for both), and gained little from it (12 to 12
  FPS on average, 8 to 9 weighted).

## 7. Results

Measured 2026-10-17 on the boards of the developer, benchmark version 1 (data 1, scenes 1), three
runs each; ms per frame (`total`, `render`, `flush` are medians).

| File | Firmware |
| --- | --- |
| `<board>-lvgl8.4.json` | the baseline: LVGL 8.4 before the benchmark's own changes to the screens (the clock was an `lv_meter`) |
| `<board>-lvgl8.4-same.json` | LVGL 8.4 with the hand-drawn clock, the firmware as it is (the tables below) |
| `<board>-lvgl9.6.json` | the same source on LVGL 9.6 (section 8) |
| `wt32_sc01-lvgl8.4-clock.json`, `wt32_sc01-check-same.json` | WT32 builds of the build spread above |
| `wt32_sc01-dio40.json`, `wt32_sc01-qio80.json` | WT32, one source (2026-10-30): flash DIO 40 MHz and PSRAM 40 MHz, then QIO 80 MHz and 80 MHz |

The hand-drawn clock face (instead of `lv_meter`) costs 4.7 instead of 22 ms a tick on the WT32
and 0.7 instead of 0.8 ms on the P4. Everything else is the same as the baseline within 2 % on the
JC3248W535 and the P4, but for the P4's weather panel redraw (78 to 60 ms, not explained by the
change).

### WT32-SC01 (ESP32, SPI, 480x320)

Measured with flash DIO 40 MHz and PSRAM 40 MHz. Since 2026-10-30 the WT32 runs at QIO 80 MHz
and 80 MHz, which halves the times (`wt32_sc01-dio40.json` against `wt32_sc01-qio80.json`, one
source):

| Scene | total med, 40 MHz | total med, 80 MHz | fps, 40 / 80 MHz |
| --- | --- | --- | --- |
| `main_redraw` | 135.64 | 62.54 | 7.0 / 14.8 |
| `dense_redraw` | 193.87 | 88.29 | 5.0 / 10.7 |
| `light_panel_redraw` | 79.97 | 36.41 | 11.5 / 24.1 |
| `weather_panel_redraw` | 170.40 | 72.87 | 5.6 / 12.8 |
| `pending_runner` | 7.34 | 3.12 | 72.5 / 99.0 |

| Scene | fps | total med | total p95 | render med | flush med | pixels/frame |
| --- | --- | --- | --- | --- | --- | --- |
| `main_redraw` | 8.8 | 107.51 | 109.86 | 102.52 | 4.43 | 153600 |
| `dense_slide_in` | 2.0 | 187.76 | 187.76 | 182.62 | 5.20 | 153600 |
| `dense_redraw` | 6.5 | 148.89 | 150.29 | 143.82 | 4.41 | 153600 |
| `dense_back` | 5.0 | 97.12 | 126.39 | 93.02 | 4.11 | 98826 |
| `light_panel_open` | 5.0 | 67.68 | 132.41 | 63.73 | 3.99 | 91866 |
| `light_panel_redraw` | 13.8 | 65.80 | 68.13 | 61.46 | 4.01 | 153600 |
| `light_panel_close` | 5.0 | 50.56 | 135.32 | 48.06 | 2.52 | 82500 |
| `climate_panel_open` | 4.0 | 105.99 | 178.34 | 100.77 | 4.54 | 114552 |
| `climate_panel_redraw` | 9.3 | 101.16 | 102.68 | 96.85 | 4.52 | 153600 |
| `climate_panel_close` | 5.9 | 14.40 | 126.98 | 13.84 | 0.58 | 59576 |
| `sensor_panel_open` | 4.0 | 103.45 | 141.34 | 98.99 | 4.21 | 112792 |
| `sensor_panel_redraw` | 11.0 | 84.20 | 87.57 | 79.71 | 4.40 | 153600 |
| `sensor_panel_close` | 7.0 | 21.89 | 143.79 | 20.84 | 1.11 | 56209 |
| `lock_panel_open` | 6.0 | 9.06 | 132.14 | 8.43 | 0.64 | 77457 |
| `lock_panel_close` | 7.0 | 23.32 | 144.94 | 22.20 | 1.11 | 62289 |
| `timer_panel_open` | 5.0 | 104.27 | 180.49 | 99.84 | 4.16 | 94810 |
| `timer_panel_close` | 7.0 | 13.88 | 118.41 | 13.31 | 0.56 | 52872 |
| `picture_panel_open` | 6.0 | 12.01 | 125.42 | 11.38 | 0.75 | 55701 |
| `picture_panel_redraw` | 11.8 | 78.92 | 80.67 | 73.44 | 4.75 | 153600 |
| `picture_panel_close` | 6.0 | 24.34 | 139.06 | 23.24 | 1.09 | 66489 |
| `pending_runner` | 78.0 | 6.36 | 10.48 | 5.76 | 0.53 | 1648 |
| `alert_strip` | 15.6 | 11.79 | 13.55 | 11.11 | 0.57 | 5211 |
| `large_slide_in` | 5.0 | 96.65 | 97.75 | 72.40 | 4.36 | 124410 |
| `clocks_tick` | 3.0 | 4.73 | 33.44 | 3.79 | 0.55 | 11530 |
| `weather_panel_open` | 4.7 | 53.46 | 119.62 | 51.23 | 1.98 | 73388 |
| `weather_panel_redraw` | 8.3 | 114.13 | 120.66 | 109.83 | 4.34 | 153600 |
| `weather_panel_close` | 5.0 | 26.96 | 140.88 | 23.03 | 2.84 | 78495 |
| `automation_panel_open` | 6.0 | 55.09 | 98.52 | 47.56 | 4.22 | 98982 |
| `automation_panel_close` | 5.0 | 75.34 | 110.57 | 70.53 | 4.35 | 90106 |
| `settings_open` | 2.7 | 5.35 | 110.93 | 4.75 | 0.60 | 79656 |
| `settings_scroll` | 16.7 | 45.14 | 74.38 | 39.07 | 6.09 | 138870 |
| `settings_close` | 6.0 | 50.59 | 96.98 | 45.05 | 4.11 | 105889 |
| `rotate_90` | 3.5 | 7.50 | 98.28 | 6.94 | 0.59 | 47112 |
| `main_redraw_90` | 10.0 | 92.29 | 95.10 | 88.00 | 4.63 | 153600 |

Internal RAM free 127 KB (largest block 72 KB), PSRAM free 3946 KB; the LVGL task's stack had 3424 bytes left after the scenes. Firmware 2.30 MB.

### JC3248W535 (ESP32-S3, QSPI full frames, 320x480)

| Scene | fps | total med | total p95 | render med | flush med | pixels/frame |
| --- | --- | --- | --- | --- | --- | --- |
| `main_redraw` | 17.9 | 50.68 | 51.23 | 34.37 | 16.26 | 153600 |
| `dense_slide_in` | 5.0 | 69.73 | 80.60 | 53.28 | 16.26 | 153600 |
| `dense_redraw` | 13.9 | 66.48 | 67.41 | 50.22 | 16.32 | 153600 |
| `dense_back` | 8.0 | 49.47 | 61.22 | 33.08 | 16.26 | 153600 |
| `light_panel_open` | 8.0 | 39.62 | 67.85 | 23.25 | 16.25 | 153600 |
| `light_panel_redraw` | 22.6 | 38.97 | 39.75 | 22.63 | 16.33 | 153600 |
| `light_panel_close` | 8.0 | 24.58 | 73.76 | 8.55 | 16.23 | 172800 |
| `climate_panel_open` | 7.0 | 67.03 | 80.57 | 50.72 | 16.28 | 175542 |
| `climate_panel_redraw` | 19.1 | 46.96 | 47.83 | 30.65 | 16.28 | 153600 |
| `climate_panel_close` | 7.0 | 39.56 | 81.16 | 23.59 | 16.26 | 175542 |
| `sensor_panel_open` | 8.0 | 46.21 | 68.35 | 29.81 | 16.25 | 326400 |
| `sensor_panel_redraw` | 21.4 | 41.88 | 42.52 | 25.58 | 16.29 | 153600 |
| `sensor_panel_close` | 7.0 | 42.26 | 76.42 | 26.17 | 16.22 | 175542 |
| `lock_panel_open` | 7.0 | 52.10 | 66.86 | 35.75 | 16.28 | 153600 |
| `lock_panel_close` | 8.0 | 24.54 | 71.28 | 8.46 | 16.21 | 172800 |
| `timer_panel_open` | 7.0 | 49.39 | 82.53 | 33.18 | 16.21 | 153600 |
| `timer_panel_close` | 8.0 | 23.53 | 82.61 | 7.23 | 16.18 | 172800 |
| `picture_panel_open` | 8.0 | 20.15 | 136.89 | 3.99 | 16.13 | 153600 |
| `picture_panel_redraw` | 22.0 | 40.32 | 41.58 | 24.03 | 16.19 | 153600 |
| `picture_panel_close` | 7.0 | 40.99 | 75.33 | 24.68 | 16.25 | 153600 |
| `pending_runner` | 42.2 | 18.39 | 20.58 | 2.29 | 16.11 | 162635 |
| `alert_strip` | 11.6 | 20.23 | 21.29 | 4.19 | 16.07 | 153600 |
| `large_slide_in` | 8.0 | 45.37 | 52.47 | 29.09 | 16.25 | 192000 |
| `clocks_tick` | 3.0 | 18.67 | 26.21 | 2.69 | 16.04 | 256000 |
| `weather_panel_open` | 6.0 | 43.86 | 64.83 | 27.53 | 16.30 | 238933 |
| `weather_panel_redraw` | 18.8 | 47.98 | 48.95 | 31.67 | 16.31 | 153600 |
| `weather_panel_close` | 6.0 | 38.32 | 77.66 | 22.25 | 16.22 | 230400 |
| `automation_panel_open` | 8.0 | 41.53 | 60.46 | 25.23 | 16.22 | 192000 |
| `automation_panel_close` | 7.0 | 47.41 | 61.75 | 31.26 | 16.24 | 219428 |
| `settings_open` | 2.7 | 49.14 | 50.57 | 32.88 | 16.25 | 153600 |
| `settings_scroll` | 11.3 | 38.82 | 45.44 | 22.40 | 16.34 | 162635 |
| `settings_close` | 8.0 | 31.56 | 56.83 | 15.19 | 16.22 | 153600 |
| `rotate_90` | 4.0 | 19.25 | 47.52 | 2.88 | 16.40 | 153600 |
| `main_redraw_90` | 17.5 | 52.46 | 53.10 | 35.88 | 16.58 | 153600 |

Internal RAM free 144 KB (largest block 96 KB), PSRAM free 7741 KB; the LVGL task's stack had 3208 bytes left after the scenes. Firmware 2.40 MB.

### JC4880P443C (ESP32-P4, MIPI DSI, 480x800, variant kitchen)

| Scene | fps | total med | total p95 | render med | flush med | pixels/frame |
| --- | --- | --- | --- | --- | --- | --- |
| `main_redraw` | 27.8 | 31.36 | 31.68 | 20.94 | 10.42 | 384000 |
| `dense_slide_in` | 8.0 | 42.74 | 47.21 | 32.16 | 10.54 | 289912 |
| `dense_redraw` | 21.1 | 42.66 | 42.94 | 32.26 | 10.40 | 384000 |
| `dense_back` | 11.0 | 30.41 | 38.79 | 20.01 | 10.39 | 286423 |
| `light_panel_open` | 10.0 | 31.25 | 47.51 | 25.40 | 5.86 | 188390 |
| `light_panel_redraw` | 16.5 | 55.77 | 55.99 | 45.32 | 10.38 | 384000 |
| `light_panel_close` | 10.0 | 28.32 | 46.82 | 21.13 | 6.03 | 182801 |
| `climate_panel_open` | 8.0 | 42.14 | 58.11 | 35.61 | 6.45 | 205675 |
| `climate_panel_redraw` | 13.8 | 67.37 | 67.68 | 56.63 | 10.51 | 384000 |
| `climate_panel_close` | 8.0 | 31.98 | 58.18 | 21.39 | 7.45 | 199926 |
| `sensor_panel_open` | 10.0 | 27.30 | 45.95 | 21.06 | 6.29 | 206839 |
| `sensor_panel_redraw` | 13.8 | 67.57 | 67.84 | 57.01 | 10.52 | 384000 |
| `sensor_panel_close` | 8.0 | 32.04 | 59.33 | 21.59 | 7.28 | 198970 |
| `lock_panel_open` | 10.0 | 28.79 | 45.90 | 23.25 | 5.54 | 185870 |
| `lock_panel_close` | 11.0 | 28.19 | 45.43 | 21.25 | 6.64 | 178250 |
| `timer_panel_open` | 9.0 | 47.32 | 58.08 | 40.35 | 6.97 | 188622 |
| `timer_panel_close` | 10.0 | 24.68 | 56.36 | 19.38 | 5.33 | 173265 |
| `picture_panel_open` | 19.3 | 1.54 | 34.73 | 1.47 | 0.07 | 55866 |
| `picture_panel_redraw` | 29.4 | 29.11 | 29.32 | 18.20 | 10.83 | 384000 |
| `picture_panel_close` | 10.0 | 7.05 | 54.86 | 4.81 | 2.24 | 178106 |
| `pending_runner` | 99.5 | 0.49 | 0.59 | 0.46 | 0.03 | 1748 |
| `alert_strip` | 22.8 | 1.38 | 1.75 | 1.27 | 0.10 | 7167 |
| `large_slide_in` | 10.0 | 32.12 | 37.40 | 21.60 | 10.47 | 308730 |
| `clocks_tick` | 3.0 | 0.73 | 6.11 | 0.49 | 0.24 | 15119 |
| `weather_panel_open` | 7.3 | 36.49 | 50.71 | 30.18 | 6.11 | 180286 |
| `weather_panel_redraw` | 15.4 | 60.18 | 60.38 | 49.78 | 10.39 | 384000 |
| `weather_panel_close` | 7.0 | 45.47 | 55.82 | 37.78 | 8.31 | 241215 |
| `automation_panel_open` | 9.0 | 42.21 | 44.97 | 34.08 | 8.04 | 229610 |
| `automation_panel_close` | 8.0 | 32.12 | 44.01 | 25.18 | 8.31 | 237198 |
| `settings_open` | 4.7 | 32.59 | 34.44 | 21.87 | 10.56 | 330233 |
| `settings_scroll` | 11.3 | 29.10 | 30.73 | 19.32 | 9.71 | 364769 |
| `settings_close` | 9.0 | 31.04 | 33.90 | 20.54 | 10.45 | 303072 |
| `rotate_90` | 4.0 | 1.80 | 64.91 | 1.46 | 0.35 | 111715 |
| `main_redraw_90` | 12.8 | 73.54 | 73.85 | 48.83 | 24.46 | 384000 |

Internal RAM free 293 KB (largest block 244 KB), PSRAM free 31050 KB; the LVGL task's stack had 4416 bytes left after the scenes. Firmware 3.04 MB.

## 8. LVGL 9.6 against 8.4 (2026-10-17, not adopted)

The move to LVGL 9 was measured with this benchmark and dropped: LVGL 9.6 is slower on every board,
much slower on the ESP32 and the S3, and it is not an option for these boards (your decision,
2026-10-17). The code that built both versions was removed again; the results stay here.

**How it was built.** LVGL 9.6.0~1 with esp_lvgl_port 2.9.0, the same source as `lvgl8.4-same`:
the component manifests picked the version by an environment variable, LVGL 9 had its own lock
file, config layer and build directory; a compatibility header carried the calls that differ
(images, canvas, spinner, tab view, QR code, draw calls, event targets, refresh monitor, touch
feedback); the drivers (`frame_display`, `touch_input`, `uidebug`) and `main` had LVGL 9 branches
(display and input device creation, `swap_bytes` in the port and in the frame display's bands, the
snapshot as `lv_draw_buf_t`, `LV_STDLIB_CUSTOM` for the PSRAM allocator). What it took:

* The ESP32 did not take LVGL 9's `LV_ATTRIBUTE_FAST_MEM` functions in IRAM (74 KB over; LVGL
  8.4's fit), and on the ESP32-S3 the link failed with them ("dangerous relocation: j: cannot
  encode: xt_debugexception", the IRAM grew past the reach of the exception vectors). On both, all of
  LVGL ran from flash.
* The WT32's firmware fit its 2.4 MB app partition only without the widgets the screens do not use
  (LVGL 9 builds most of them by default): 2.46 MB against 2.30 MB on 8.4.
* The LVGL task's 7 KB stack (esp_lvgl_port's default) was nearly used up on every board: 32 bytes
  left on the JC3248W535, 596 on the P4, 664 on the WT32 (LVGL 8.4: 3.2 to 4.4 KB).
* LVGL 9.6 deprecates `lv_obj_add_flag`, `lv_obj_remove_flag` and `lv_obj_has_flag` in favour of
  one setter per flag: the calls the screens use most would all have to change.
* The P4's camera preview (drawn past LVGL into the frame buffers) was not ported: LVGL 9 rotates
  the bands in esp_lvgl_port's flush, not before it.

**Results**, the same source on both, ms per frame (median of three runs):

| Scene | WT32-SC01 8.4 | 9.6 | JC3248W535 8.4 | 9.6 | JC4880P443C 8.4 | 9.6 |
| --- | --- | --- | --- | --- | --- | --- |
| `main_redraw` | 108 | 280 (+160 %) | 51 | 73 (+44 %) | 31.4 | 36.2 (+15 %) |
| `dense_redraw` | 149 | 410 (+175 %) | 66 | 110 (+65 %) | 42.7 | 46.4 (+9 %) |
| `light_panel_redraw` | 66 | 175 (+166 %) | 39 | 53 (+35 %) | 55.8 | 59.7 (+7 %) |
| `climate_panel_redraw` | 101 | 230 (+127 %) | 47 | 59 (+25 %) | 67.4 | 70.7 (+5 %) |
| `sensor_panel_redraw` | 84 | 224 (+166 %) | 42 | 55 (+31 %) | 67.6 | 70.6 (+5 %) |
| `picture_panel_redraw` | 79 | 138 (+75 %) | 40 | 43 (+8 %) | 29.1 | 33.1 (+14 %) |
| `weather_panel_redraw` | 114 | 334 (+193 %) | 48 | 84 (+75 %) | 60.2 | 59.8 (-1 %) |
| `main_redraw_90` | 92 | 234 (+154 %) | 52 | 76 (+45 %) | 73.5 | 68.2 (-7 %) |
| `settings_scroll` | 45 | 100 (+121 %) | 39 | 46 (+20 %) | 29.1 | 29.7 (+2 %) |
| `pending_runner` | 6.4 | 12.7 | 18.4 | 6.2 (-66 %) | 0.49 | 0.55 |
| `alert_strip` | 11.8 | 22.7 | 20.2 | 5.7 (-72 %) | 1.38 | 1.27 |
| `clocks_tick` | 4.7 | 53.8 | 18.7 | 24.1 | 0.73 | 4.06 |
| internal RAM free | 128 KB | 128 KB | 148 KB | 162 KB | 300 KB | 149 KB |
| LVGL task stack left | 3.4 KB | 0.7 KB | 3.2 KB | 32 bytes | 4.4 KB | 0.6 KB |
| firmware | 2.30 MB | 2.46 MB | 2.40 MB | 2.57 MB | 3.04 MB | 3.23 MB |

* **ESP32 (WT32-SC01): 2 to 3 times slower** on every scene, far beyond the build spread of
  section 6. LVGL from flash only explains part of it.
* **ESP32-S3 (JC3248W535): 25 to 75 % slower on whole frames.** Small areas are faster (the frame
  display sends only down to the lowest changed row, and LVGL 9 changed less there).
* **ESP32-P4 (JC4880P443C): 5 to 15 % slower** on most redraws, slightly faster with the screen
  turned (esp_lvgl_port rotates the bands on LVGL 9), 150 KB less internal RAM.

What was published before, for comparison: [lvgl#5459](https://github.com/lvgl/lvgl/issues/5459)
found about 37 % slower on an ESP32-S3 from 8.3.9 to 9.0.0; Espressif's
[esp_lvgl_port performance notes](https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/docs/performance.md)
give LVGL 8 12 / 16 FPS and LVGL 9.2 9 / 11 FPS on an ESP32-S3 RGB panel; the
[LVGL 9.6 release notes](https://lvgl.io/blog/release-v9-6) speed up blending and rotated images
without a comparison with LVGL 8.
