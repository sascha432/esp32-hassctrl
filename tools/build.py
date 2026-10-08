#!/usr/bin/env python3
"""Build, flash and monitor one board and one application with idf.py.

    python tools/build.py <board> [--app main|recovery] [idf.py actions and options ...]

Examples:
    python tools/build.py wt32_sc01                          build the main firmware
    python tools/build.py wt32_sc01 --app recovery -p COM9 flash
                                                             bootloader, partition table and the
                                                             recovery app (factory partition)
    python tools/build.py wt32_sc01 -p COM9 flash monitor    main firmware to ota_0, then monitor
    python tools/build.py wt32_sc01 menuconfig

Run it in a terminal prepared with the ESP-IDF activation script (EIM:
C:\\Espressif\\tools\\Microsoft.v6.1.PowerShell_profile.ps1).

Each board and application gets its own build directory, build/<board>/<app>, with its own
sdkconfig. The ESP-IDF defaults are layered (docs/architecture.md section 2):
sdkconfig.defaults, boards/<board>/sdkconfig.defaults, sdkconfig.<app>, each followed by its
<layer>.<target> variant when it exists (sdkconfig.main.esp32). When a layer is newer than
the generated sdkconfig, the sdkconfig is regenerated, so menuconfig changes are temporary.

Flashing the main firmware is different from a plain `idf.py flash`: idf.py writes the app to the
factory partition, which holds the recovery app. This script writes the main firmware to ota_0
with parttool.py and makes it the boot partition with otatool.py instead. The recovery app (and with it the
bootloader and the partition table) has to be flashed once before.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APPS = ("main", "recovery")
PROJECT_NAMES = {"main": "hassctrl", "recovery": "hassctrl-recovery"}
FLASH_ACTIONS = ("flash", "app-flash")


def fail(message: str) -> None:
    print(f"build.py: {message}", file=sys.stderr)
    sys.exit(2)


def board_target(board_defaults: Path) -> str:
    match = re.search(r'^CONFIG_IDF_TARGET="([^"]+)"', board_defaults.read_text(encoding="utf-8"), re.MULTILINE)
    if not match:
        fail(f"{board_defaults} does not set CONFIG_IDF_TARGET")
    return match.group(1)


def idf_python() -> str:
    """The Python of the ESP-IDF environment. EIM's activation script defines idf.py only as a
    PowerShell alias, so the script calls tools/idf.py with this interpreter instead."""
    env = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env:
        exe = Path(env) / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        if exe.is_file():
            return str(exe)
    return sys.executable


def port_of(args: list[str]) -> str | None:
    for i, arg in enumerate(args):
        if arg in ("-p", "--port") and i + 1 < len(args):
            return args[i + 1]
        if arg.startswith("--port="):
            return arg.split("=", 1)[1]
    return os.environ.get("ESPPORT")


def run(command: list[str]) -> None:
    print("+ " + " ".join(command), flush=True)
    result = subprocess.run(command, cwd=ROOT)
    if result.returncode != 0:
        sys.exit(result.returncode)


def main() -> None:
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0 if args else 2)

    board = args.pop(0)
    app = "main"
    if len(args) >= 2 and args[0] == "--app":
        app = args[1]
        args = args[2:]
    if app not in APPS:
        fail(f"--app must be one of {', '.join(APPS)}")

    if "IDF_PATH" not in os.environ:
        fail("IDF_PATH is not set, run the ESP-IDF activation script first")

    board_defaults = ROOT / "boards" / board / "sdkconfig.defaults"
    if not board_defaults.is_file():
        boards = sorted(p.name for p in (ROOT / "boards").iterdir() if p.is_dir())
        fail(f"unknown board '{board}', known boards: {', '.join(boards)}")

    layers = [ROOT / "sdkconfig.defaults", board_defaults]
    app_layer = ROOT / f"sdkconfig.{app}"
    if app_layer.is_file():
        layers.append(app_layer)

    build_dir = ROOT / "build" / board / app

    # ESP-IDF applies the defaults only when it creates the sdkconfig, so a changed layer would be
    # ignored by an existing build directory. The sdkconfig is regenerated instead; menuconfig
    # changes are temporary, permanent settings belong in the layer files.
    sdkconfig = build_dir / "sdkconfig"
    if sdkconfig.is_file():
        # ESP-IDF also reads <layer>.<target> next to every layer (sdkconfig.main.esp32)
        target = board_target(board_defaults)
        files = layers + [p.with_name(f"{p.name}.{target}") for p in layers]
        newest = max(p.stat().st_mtime for p in files if p.is_file())
        if newest > sdkconfig.stat().st_mtime:
            print(f"build.py: a config layer changed, regenerating {sdkconfig}", flush=True)
            sdkconfig.unlink()

    python = idf_python()
    idf = [
        python,
        str(Path(os.environ["IDF_PATH"]) / "tools" / "idf.py"),
        "-B", str(build_dir),
        "-D", f"SDKCONFIG={build_dir / 'sdkconfig'}",
        "-D", "SDKCONFIG_DEFAULTS=" + ";".join(str(p) for p in layers),
        "-D", f"IDF_TARGET={board_target(board_defaults)}",
        "-D", f"HC_APP={app}",
    ]

    actions = args or ["build"]
    flash_main = app == "main" and any(a in FLASH_ACTIONS for a in actions)
    # the web UI image belongs to the main firmware (tools/build_fs.py)
    fs_image = [sys.executable, str(ROOT / "tools" / "build_fs.py"), board]
    if not flash_main:
        run(idf + actions)
        if app == "main" and "build" in actions:
            run(fs_image)
        return

    # main firmware: build, write ota_0, select it, then the remaining actions (monitor)
    port = port_of(actions)
    if not port:
        fail("flashing the main firmware needs a port: -p COMx (or ESPPORT)")
    before = [a for a in actions if a not in FLASH_ACTIONS and a != "monitor"]
    run(idf + before + ([] if "build" in before else ["build"]))
    run(fs_image)

    # ota_0 is written with parttool.py: in ESP-IDF 6.1 `otatool.py write_ota_partition` fails with
    # "unexpected keyword argument 'input'" (its command table passes `input`, the function takes
    # `input_file`). otatool.py is still used to make ota_0 the boot partition.
    components = Path(os.environ["IDF_PATH"]) / "components"
    parttool = components / "partition_table" / "parttool.py"
    otatool = components / "app_update" / "otatool.py"
    image = build_dir / f"{PROJECT_NAMES[app]}.bin"
    run([python, str(parttool), "--port", port, "write_partition", "--partition-name", "ota_0", "--input", str(image)])
    run([python, str(otatool), "--port", port, "switch_ota_partition", "--slot", "0"])

    if "monitor" in actions:
        run(idf + ["-p", port, "monitor"])


if __name__ == "__main__":
    main()
