#!/usr/bin/env python3
"""Tests the taps on the dashboard's tiles in the Windows version of the UI
(docs/ui/lvgl/lvgl-touch.md section 2): a scripted finger with exact times, and what the UI then
asks of Home Assistant: which panel it opens, which action it sends.

    python tools/test_gestures.py              build the simulator and run every case
    python tools/test_gestures.py lock         only the cases whose name contains "lock"
    python tools/test_gestures.py --exe PATH   another build of the simulator (an older tree)
    python tools/test_gestures.py --list       the cases and their steps

Every case runs hc_sim.exe without a window on host/sim/gestures/hass.yaml, as the JC4880P443C
(panels are dialogs), the WT32-SC01 and the JC3248W535 (full-screen panels that slide in). The
simulator's clock only moves with the steps and its pointer is read every 30 ms, so a case gives
the same result every time. A second tap "inside" the tiles' double tap window of 300 ms comes
down about 150 ms after the first, one "after" it about 450 ms.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from msvc import ROOT

SIM = ROOT / "build" / "sim" / "hc_sim.exe"
DASHBOARD = ROOT / "host" / "sim" / "gestures" / "hass.yaml"
SIZES = ("p4", "wt32", "jc3248")
# the tiles of the dashboard, in its order: a panel is named by its tile
TILES = ("Gate", "Ceiling", "Lamp", "Hearth", "Timer", "Alarm", "Weather", "Moonbase")
ENTITIES = {"lock.stardust_gate": "Gate", "light.aurora_ceiling": "Ceiling", "switch.crystal_lamp": "Lamp",
            "climate.hearth_thermostat": "Hearth", "timer.bathroom_fan_30min": "Timer",
            "automation.gate_alarm": "Alarm"}


def intent_kinds():
    """The names of core::IntentKind in their order: the simulator prints the number."""
    header = (ROOT / "components/core/include/hc/core/entity.hpp").read_text(encoding="utf-8")
    body = re.search(r"enum class IntentKind[^{]*\{(.*?)\};", header, re.S).group(1)
    return re.findall(r"^\s*(\w+),", body, re.M)


KINDS = intent_kinds()


def tap(tile, hold=60):
    """A tap: the finger is down for `hold` + 30 ms."""
    return [f"down {tile}", f"wait {hold}", "up"]


def wait(ms):
    return [f"wait {ms}"]


INSIDE = wait(120)   # the next press is read about 150 ms after the release
AFTER = wait(420)    # ... about 450 ms after it: the window's action has run
SETTLE = wait(900)   # every timer and slide has ended

# name, steps, what the UI must have asked for, in order. "open X": the panel of tile X; "page":
# the page without a panel again (a panel closed, the page changed); "X Kind value": an action
# ("*" for any value). "..." at the end: whatever a later tap did on the panel that lay under it
# (its back button, a stepper), but no panel opened again.
CASES = [
    # ------------------------------------------------- a lock: the panel waits for the window
    ("lock: a tap opens the panel", tap("Gate") + SETTLE, ["open Gate"]),
    ("lock: a double tap unlocks, no panel", tap("Gate") + wait(60) + tap("Gate") + SETTLE, ["Gate SetLock 1"]),
    ("lock: a second tap lifted after the window opens the panel once, the lock stays",
     tap("Gate") + INSIDE + tap("Gate", 400) + SETTLE, ["open Gate"]),
    ("lock: a second tap held through the panel's slide", tap("Gate") + INSIDE + tap("Gate", 900) + SETTLE,
     ["open Gate"]),
    ("lock: a second tap after the window meets the panel", tap("Gate") + AFTER + tap("Gate") + SETTLE,
     ["open Gate"]),
    ("lock: three taps a window apart open it once",
     tap("Gate") + AFTER + tap("Gate") + AFTER + tap("Gate") + SETTLE, ["open Gate", "..."]),
    ("lock: one long tap that the controller drops for a moment",
     ["down Gate", "wait 150", "up", "down Gate", "wait 500", "up"] + SETTLE, ["open Gate"]),
    ("lock, then weather inside the window: the weather panel stays",
     tap("Gate") + wait(60) + tap("Weather", 30) + SETTLE, ["open Weather"]),
    ("timer: a tap opens the panel", tap("Timer") + SETTLE, ["open Timer"]),
    ("timer: a second tap lifted after the window opens the panel once",
     tap("Timer") + INSIDE + tap("Timer", 400) + SETTLE, ["open Timer"]),
    ("automation: a second tap lifted after the window opens the panel once",
     tap("Alarm") + INSIDE + tap("Alarm", 400) + SETTLE, ["open Alarm"]),
    # ------------------------------------------------- a switch: the toggle waits for the window
    ("switch: a tap toggles", tap("Lamp") + SETTLE, ["Lamp Toggle 0"]),
    ("switch: a double tap opens the panel, no toggle", tap("Lamp") + wait(60) + tap("Lamp") + SETTLE, ["open Lamp"]),
    ("switch: a second tap lifted after the window: one toggle, no panel",
     tap("Lamp") + INSIDE + tap("Lamp", 400) + SETTLE, ["Lamp Toggle 0"]),
    ("switch: two taps a window apart toggle twice", tap("Lamp") + AFTER + tap("Lamp") + SETTLE,
     ["Lamp Toggle 0", "Lamp Toggle 0"]),
    ("switch, then climate inside the window: the panel opens and the switch still toggles",
     tap("Lamp") + wait(60) + tap("Hearth", 30) + SETTLE, ["open Hearth", "Lamp Toggle 0"]),
    # ------------------------------------------------- a dimmer: tap, double tap, drag
    ("dimmer: a tap opens the panel", tap("Ceiling") + SETTLE, ["open Ceiling"]),
    ("dimmer: a double tap toggles, no panel", tap("Ceiling") + wait(60) + tap("Ceiling") + SETTLE,
     ["Ceiling Toggle 0"]),
    ("dimmer: a second tap lifted after the window opens the panel once",
     tap("Ceiling") + INSIDE + tap("Ceiling", 400) + SETTLE, ["open Ceiling"]),
    ("dimmer: a drag sets the level", ["down Ceiling", "move Ceiling,0,-60,300", "up"] + SETTLE, ["Ceiling SetLevel *"]),
    ("dimmer: a tap, then a drag inside the window: the level, no panel",
     tap("Ceiling") + INSIDE + ["down Ceiling", "move Ceiling,0,-60,300", "wait 300", "up"] + SETTLE,
     ["Ceiling SetLevel *"]),
    ("dimmer: a drag that another tile's panel cuts off sends nothing and shows the light's level again",
     tap("Gate") + wait(30) + ["down Ceiling", "move Ceiling,0,-40,150", "wait 300", "move Ceiling,0,-60,90", "up"] +
     wait(600) + ["back"] + SETTLE + ["see 44 %"], ["open Gate", "page"]),
    # ------------------------------------------------- a tile whose panel opens at once
    ("climate: a tap opens the panel", tap("Hearth") + SETTLE, ["open Hearth"]),
    ("climate: a quick second tap on the same spot", tap("Hearth") + wait(30) + tap("Hearth") + SETTLE,
     ["open Hearth"]),
    ("climate: a second tap held through the end of the slide", tap("Hearth") + wait(60) + tap("Hearth", 400) + SETTLE,
     ["open Hearth"]),
    ("climate, then a tile beside the sliding panel", tap("Hearth") + wait(30) + tap("Gate") + SETTLE,
     ["open Hearth"]),
    ("weather: a quick second tap on the same spot", tap("Weather") + wait(30) + tap("Weather") + SETTLE,
     ["open Weather"]),
]


def positions(exe, size, folder):
    """Where the tiles' names are on this screen: {name: (x, y)}."""
    args = [str(exe), "--size", size, "--dashboard", str(DASHBOARD), "--wait", "30"]
    for tile in TILES:
        args += ["--do", f"where {tile}"]
    args += ["--screenshot", str(Path(folder) / "where.png")]
    out = subprocess.run(args, capture_output=True, text=True, cwd=ROOT)
    found = {name: (int(x), int(y)) for name, x, y in re.findall(r"\[sim\] where (\w+): (-?\d+),(-?\d+)", out.stdout)}
    if len(found) != len(TILES):
        sys.exit(f"{size}: the simulator did not find every tile\n{out.stdout}{out.stderr}")
    return found


def resolve(step, at):
    """A step with the names as the simulator's coordinates; "back" taps the panel's back button."""
    verb, _, argument = step.partition(" ")
    if verb == "back":
        return "back"
    if verb not in ("down", "move"):
        return step
    parts = argument.split(",")
    x, y = at[parts[0]]
    if len(parts) >= 3:  # move NAME,dx,dy[,ms]: that far from the name
        x, y = x + int(parts[1]), y + int(parts[2])
    return f"{verb} {x},{y}" + (f",{parts[3]}" if len(parts) > 3 else "")


def asked(output):
    """What the UI asked of the backend after the start, in the words of CASES."""
    events = []
    started = False
    for line in output.splitlines():
        match = re.match(r"\[sim\] subscribe page (-?\d+), panel (-?\d+)", line)
        if match:
            panel = int(match.group(2))
            if not started:
                started = True  # the page after the boot
            elif panel >= 0:
                events.append(f"open {TILES[panel]}")
            else:
                events.append("page")
            continue
        match = re.match(r"\[sim\] call_service kind (\d+) for (\S+), value (\S+)", line)
        if match:
            events.append(f"{ENTITIES.get(match.group(2), match.group(2))} {KINDS[int(match.group(1))]} {match.group(3)}")
    return events


def matches(expected, got):
    if expected and expected[-1] == "...":
        expected = expected[:-1]
        if any(event.startswith("open ") for event in got[len(expected):]):
            return False
        got = got[:len(expected)]
    if len(expected) != len(got):
        return False
    for want, have in zip(expected, got):
        if want.endswith(" *"):
            if not have.startswith(want[:-1]):
                return False
        elif want != have:
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("filter", nargs="?", help="run only the cases whose name contains this")
    parser.add_argument("--exe", help="the simulator to test (default: build/sim/hc_sim.exe, built first)")
    parser.add_argument("--list", action="store_true", help="print the cases and their steps")
    args = parser.parse_args()

    cases = [case for case in CASES if not args.filter or args.filter in case[0]]
    if args.list:
        for name, steps, expected in cases:
            print(f"{name}\n    {' | '.join(steps)}\n    -> {expected}")
        return
    exe = Path(args.exe) if args.exe else SIM
    if not args.exe:
        build = subprocess.run([sys.executable, str(ROOT / "tools" / "sim.py"), "--build-only"], cwd=ROOT)
        if build.returncode != 0:
            sys.exit(build.returncode)

    failed = 0
    with tempfile.TemporaryDirectory(prefix="hc-gestures-") as folder:
        for size in SIZES:
            at = positions(exe, size, folder)
            for name, steps, expected in cases:
                command = [str(exe), "--size", size, "--dashboard", str(DASHBOARD), "--wait", "30"]
                for step in steps:
                    command += ["--do", resolve(step, at)]
                command += ["--screenshot", str(Path(folder) / "end.png")]
                out = subprocess.run(command, capture_output=True, text=True, cwd=ROOT)
                got = asked(out.stdout)
                problem = ""
                if out.returncode != 0:
                    problem = (out.stderr.strip().splitlines() or [f"exit code {out.returncode}"])[-1]
                elif not matches(expected, got):
                    problem = f"asked for {got}, expected {expected}"
                failed += 1 if problem else 0
                print(f"{'FAIL' if problem else 'ok  '}  {size:6}  {name}{'  ' + problem if problem else ''}")
    total = len(cases) * len(SIZES)
    print(f"{total - failed} of {total} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
