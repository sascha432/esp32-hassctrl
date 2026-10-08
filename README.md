# Home Assistant Control

A Home Assistant touch panel for ESP32, ESP32-S3 and ESP32-P4 boards, built on ESP-IDF 6.1 with
LVGL. Design: [docs/architecture.md](docs/architecture.md), UI: [docs/ui/](docs/ui/README.md),
new boards: [docs/adding-a-board.md](docs/adding-a-board.md), open items:
[docs/TODO.md](docs/TODO.md).

## Build

Open a PowerShell prepared with the ESP-IDF activation script of EIM, then use the build script.
It keeps one build directory per board and application (`build/<board>/<app>`).

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1

python tools/build.py wt32_sc01                      # build the main firmware
python tools/build.py wt32_sc01 --app recovery       # build the recovery app
python tools/build.py wt32_sc01 menuconfig           # configure one board and app
```

## Tests on the PC

The pure components (`memory`, `formats`, `core`, `config`, `hass_protocol`, the URL code of
`hass`) are compiled for the PC with MSVC of Visual Studio 2022 and tested there. No ESP-IDF
activation is needed; the script finds Visual Studio and the cmake and ninja that ESP-IDF
installs. cJSON comes from `managed_components/`, so build the firmware once first.

```powershell
python tools/host_test.py              # build and run every test
python tools/host_test.py config_      # only the tests whose name contains "config_"
```

## The UI on the PC

The Windows version of the UI (`host/sim/`) runs the screens of `components/ui` in a window, for
the design review: LVGL 8.4 with the firmware's settings, the same fonts and text widths, the mouse
as the finger. It shows only the GUI. The data comes from a stub of `hc::ui::Backend` with sample
states ("scenarios": connected, no network, hotspot on, Home Assistant token rejected, ...), and
a tap changes them (a network is forgotten, a connect succeeds or fails). The dashboard shows
the dashboard file with sample values made up from the entities' domains and names; a tap on a
tile acts on them. Same compiler and tools as the tests; build the firmware once first so
`managed_components/lvgl__lvgl` exists. The icon fonts are generated files:
`python tools/build_icons.py` (needs Node.js) after the icon list of `core` changed.

```powershell
python tools/sim.py                          # build and open the window (480x320)
python tools/sim.py --size 320x480 --zoom 2  # a board's size (wt32, jc3248, t4, vga, p4), 2x pixels
python tools/sim.py --scenario hotspot --page hotspot
python tools/sim.py --dashboard my/hass.yaml   # the dashboard file (default docs/dashboard-sample/hass.yaml)
python tools/sim.py --dashboard none           # no dashboard: the test screen
python tools/sim.py --list                   # scenarios, pages, keys
```

In the window: `R` rotates (the window turns like the panel), `S` / `Shift+S` or `1`-`9` change the
scenario, `F12` saves a screenshot, `Q` quits. Without a person at the window, a script of taps
renders into a PNG and exits (time only moves when a step waits, so `wait 11000` is free):

```powershell
python tools/sim.py --page display --do "tap Rotation 0" --do "tap Rotate" --screenshot out.png
python tools/sim.py --scenario token-rejected --wait 11500 --screenshot strip.png
```

Steps of `--do`: `page NAME`, `scenario NAME`, `rotate`, `tap TEXT` (a label on the screen) or
`tap X,Y`, `swipe X1,Y1,X2,Y2`, `wait MS`, `shot FILE`. `python tools/sim.py --help` lists all
options. The simulator checks `host/sim/lv_conf.h` against the firmware's sdkconfig on every run
and says when a value differs.

## Flash

The recovery app goes to the factory partition and brings the bootloader and the partition table
with it, so it is flashed first. The main firmware goes to the OTA partition; the build script
does that instead of a plain `idf.py flash`, which would overwrite the recovery app.

```powershell
python tools/build.py wt32_sc01 --app recovery -p COM9 flash
python tools/build.py wt32_sc01 -p COM9 flash monitor
```

Flashing the recovery app also resets the boot selection: the device starts the recovery app
until a main firmware is flashed or uploaded.

## Web UI

The web pages are in `web/` (plain HTML, CSS and JavaScript, Bootstrap 5.3 CSS in
`web/vendor/`, no build step). Every main firmware build also builds the file system image
`build/<board>/fs/storage.bin` from them (`tools/build_fs.py`: gzipped into `/www`, sized for
the board's "storage" partition). Upload it on the System page, with `tools/update.py`, or over
serial with `python tools/build_fs.py <board> -p COM9`. Without the image the device serves its
built-in page; `/recovery` always serves that page.

## Update over the network

Once a device runs, firmware and file system images go over the network: the System part of the
web page (file or URL), or the script, which logs in, goes through the recovery app on 4 MB
boards and waits for the new firmware:

```powershell
python tools/update.py 192.168.0.206 firmware build/ttgo_t4/main/hassctrl.bin
python tools/update.py 192.168.0.206 filesystem build/ttgo_t4/fs/storage.bin
```

The password is asked for, or taken from `HC_PASSWORD`.

`python tools/test_web.py 192.168.0.206` checks the web server of a running device (login,
refusals, load, a stalled client); it changes nothing on the device.
