#!/usr/bin/env python3
"""Builds and starts the Windows version of the UI (docs/architecture.md section 1): the screens of
components/ui on LVGL 8.4 with a Windows window and sample data. Only the look, no network.

    python tools/sim.py                          build and open the window (480x320)
    python tools/sim.py --size 320x480           the window size of a board; R in the window rotates
    python tools/sim.py --scenario hotspot       start in a sample state (S in the window changes it)
    python tools/sim.py --page wifi --screenshot out.png
                                                 draw a page into a PNG and exit, no window
    python tools/sim.py --list                   scenarios, pages, keys

Options that sim.py itself takes are --clean and --build-only; everything else goes to the
simulator (hc_sim.exe --help lists it). The build directory is build/sim, MSVC and the cmake and
ninja of ESP-IDF are found by tools/msvc.py.
"""
import re
import subprocess
import sys

from msvc import ROOT, cmake, msvc_environment, ninja

BUILD = ROOT / "build" / "sim"
CONF = ROOT / "host" / "sim" / "lv_conf.h"
# what the simulator deliberately does differently from the firmware (see lv_conf.h)
KNOWN_DIFFERENCES = {"LV_COLOR_16_SWAP", "LV_ATTRIBUTE_FAST_MEM_USE_IRAM", "LV_MEM_CUSTOM_INCLUDE",
                     "LV_ASSERT_HANDLER_INCLUDE", "LV_CONF_SKIP", "LV_LOG_LEVEL",
                     # the larger fonts of the JC4880P443C, not in the WT32's build
                     "LV_FONT_MONTSERRAT_18", "LV_FONT_MONTSERRAT_26", "LV_FONT_MONTSERRAT_36"}


def sdkconfig_values(path):
    """CONFIG_LV_* of an sdkconfig as {name without CONFIG_: value}; 'n' for 'is not set'."""
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"CONFIG_(LV_\w+)=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
        match = re.match(r"# CONFIG_(LV_\w+) is not set", line)
        if match:
            values[match.group(1)] = "n"
    return values


def normalized(value):
    """A value of sdkconfig or lv_conf.h in one form: 1/0 for y/n, quotes off, arithmetic done."""
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] == '"':
        return value[1:-1]
    if value in ("y", "n"):
        return "1" if value == "y" else "0"
    if re.fullmatch(r"[\d\s*+()]+", value):
        return str(eval(value))  # digits and arithmetic only, checked above
    return value


def conf_values():
    values = {}
    for line in CONF.read_text(encoding="utf-8").splitlines():
        match = re.match(r"#define (LV_\w+)\s+(.*?)\s*(/\*.*)?$", line)
        if match:
            values[match.group(1)] = normalized(match.group(2))
    return values


def check_config():
    """Warns when lv_conf.h and the firmware's LVGL settings disagree (the layout would differ).

    Compared are the symbols both files name, and the set of Montserrat sizes. A symbol the
    firmware leaves at LVGL's default and lv_conf.h does not mention agrees by construction. The
    log level and the like are Kconfig choices with other names than in lv_conf.h and are not
    compared."""
    sdkconfig = ROOT / "build" / "wt32_sc01" / "main" / "sdkconfig"
    if not sdkconfig.exists():
        print("sim: no build/wt32_sc01 yet, lv_conf.h is not compared with the firmware's sdkconfig")
        return
    firmware = sdkconfig_values(sdkconfig)
    sim = conf_values()
    for name, value in sorted(firmware.items()):
        if name in KNOWN_DIFFERENCES or name not in sim:
            continue
        if normalized(value) != sim[name]:
            print(f"sim: lv_conf.h differs from the firmware's sdkconfig: {name} is {value} there, "
                  f"{sim[name]} here")
    fonts = lambda values, on: sorted(n for n, v in values.items()
                                      if re.fullmatch(r"LV_FONT_MONTSERRAT_\d+", n) and v == on and n not in KNOWN_DIFFERENCES)
    if fonts(firmware, "y") != fonts(sim, "1"):
        print(f"sim: lv_conf.h differs from the firmware's sdkconfig: Montserrat sizes {fonts(firmware, 'y')} "
              f"there, {fonts(sim, '1')} here")


def main():
    args = sys.argv[1:]
    clean = "--clean" in args
    build_only = "--build-only" in args
    args = [a for a in args if a not in ("--clean", "--build-only")]
    if "-h" in args and len(args) == 1:
        print(__doc__)
        return

    check_config()

    configure = [str(cmake()), "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja()}", "-DCMAKE_BUILD_TYPE=Release",
                 "-S", str(ROOT / "host" / "sim"), "-B", str(BUILD)]
    if clean:
        configure.append("--fresh")
    build = [str(cmake()), "--build", str(BUILD)]
    run = [str(BUILD / "hc_sim.exe")] + args

    env = msvc_environment()
    steps = (configure, build) if build_only else (configure, build, run)
    for step in steps:
        # the configure and build output only matters when it fails
        quiet = step is not run
        result = subprocess.run(step, cwd=ROOT, env=env, capture_output=quiet, text=quiet)
        if result.returncode != 0:
            if quiet:
                sys.stdout.write(result.stdout or "")
                sys.stderr.write(result.stderr or "")
            sys.exit(result.returncode)


if __name__ == "__main__":
    main()
