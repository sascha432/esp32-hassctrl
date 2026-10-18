#!/usr/bin/env python3
"""The rendering benchmark of a device over its serial console (docs/benchmark.md).

    python tools/bench.py <board>[.<variant>] -p COMx [--runs 3] [--label lvgl8.4]
    python tools/bench.py --compare A.json B.json

Each run restarts the device into benchmark mode (`bench start`: the benchmark dashboard on sample
data, no network), runs the scenes (`bench run`) and reads their lines. After the last run the
device restarts into its normal mode; nothing on it was changed. The medians of the runs go to
docs/benchmarks/<board>[.<variant>]-<label>.json. --compare prints two results side by side;
results of another benchmark version are refused.

Needs the development console (CONFIG_HC_DEV_CONSOLE) and pyserial (part of the ESP-IDF Python
environment). The port is opened with DTR and RTS released, so opening it does not reset the board.
"""

import argparse
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "docs" / "benchmarks"
# what a scene reports, as paths into its line
METRICS = [("fps",), ("total", "med"), ("total", "p95"), ("total", "max"), ("render", "med"), ("render", "p95"),
           ("flush", "med"), ("pixels",), ("frames",)]
BOOT_S = 25     # restart, the start screen, the sample values of the first page
RUN_S = 330     # the scenes take about two minutes; the device gives up after five


class Console:
    """The development console on a serial port that may vanish while the board restarts (USB-Serial/JTAG)."""

    def __init__(self, port: str):
        self.port = port
        self.s = None
        # what came after the last line read_until() returned, and that line
        self.buffer = b""
        self.last_line = ""

    def open(self, timeout: float = 30.0) -> None:
        end = time.time() + timeout
        while True:
            try:
                s = serial.Serial()
                s.port = self.port
                s.baudrate = 115200
                s.timeout = 0.2
                s.dtr = False
                s.rts = False
                s.open()
                self.s = s
                return
            except serial.SerialException:
                if time.time() > end:
                    raise
                time.sleep(0.5)

    def close(self) -> None:
        if self.s is not None:
            try:
                self.s.close()
            except serial.SerialException:
                pass
            self.s = None

    def send(self, line: str) -> None:
        self.s.reset_input_buffer()
        self.buffer = b""
        self.s.write(line.encode() + b"\r\n")

    def read_until(self, markers: tuple[str, ...], timeout: float) -> tuple[str, list[str]]:
        """Lines until one starts with a marker (the marker, the lines before); a port that vanishes is
        opened again."""
        lines: list[str] = []
        end = time.time() + timeout
        while time.time() < end:
            while b"\n" in self.buffer:
                raw, self.buffer = self.buffer.split(b"\n", 1)
                text = raw.decode("utf-8", errors="replace").strip()
                for marker in markers:
                    if marker in text:
                        self.last_line = text
                        return marker, lines
                lines.append(text)
            try:
                self.buffer += self.s.read(4096)
            except serial.SerialException:
                self.close()
                time.sleep(1.0)
                self.open(max(1.0, end - time.time()))
        raise TimeoutError(f"none of {markers} within {timeout:.0f} s")


def one_run(con: Console) -> list[dict]:
    # a board that is still booting (just flashed, just restarted) misses the first command
    for attempt in range(6):
        con.send("bench start")
        try:
            con.read_until(("restarting into benchmark mode",), 10)
            break
        except TimeoutError:
            if attempt == 5:
                raise
    # the boot: the console greets once the components run, then the sample values arrive
    con.read_until(("Type 'help'",), 60)
    time.sleep(BOOT_S - 15)
    con.send("bench run")
    marker, _ = con.read_until(("BENCH-BEGIN", "BENCH-ERROR", "not in benchmark mode", "did not start"), RUN_S)
    if marker != "BENCH-BEGIN":
        raise RuntimeError(f"the benchmark did not run: {marker}")
    _, lines = con.read_until(("BENCH-END",), 60)
    records = []
    for line in lines:
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    # the device counts its lines: a line lost or broken on the way fails the run
    if con.last_line.split()[-1:] != [str(len(records))]:
        raise RuntimeError(f"{len(records)} lines read, the device sent '{con.last_line}'")
    return records


def median_of(runs: list[list[dict]]) -> dict:
    """header, memory and the scenes with the median of every metric over the runs"""
    header = next(r for r in runs[0] if "bench" in r)
    memory = {}
    for when in ("before", "after"):
        records = [next(r for r in run if r.get("memory") == when) for run in runs if any(r.get("memory") == when for r in run)]
        if records:
            memory[when] = {k: statistics.median(rec[k] for rec in records) for k in records[0] if k != "memory"}
    scenes = {}
    names = [r["scene"] for r in runs[0] if "scene" in r]
    for name in names:
        records = [next(r for r in run if r.get("scene") == name) for run in runs]
        scene = {}
        for path in METRICS:
            values = []
            for rec in records:
                v = rec
                for key in path:
                    v = v[key]
                values.append(v)
            scene[".".join(path)] = round(statistics.median(values), 2)
        scene["spread.total.med"] = round(max(v["total"]["med"] for v in records) - min(v["total"]["med"] for v in records), 2)
        scenes[name] = scene
    return {"header": header, "memory": memory, "scenes": scenes}


def git_state() -> str:
    try:
        commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT, capture_output=True,
                               text=True).stdout.strip()
        return commit + ("+changes" if dirty else "")
    except OSError:
        return "?"


def run(args) -> None:
    con = Console(args.port)
    con.open()
    runs = []
    try:
        failures = 0
        while len(runs) < args.runs:
            print(f"run {len(runs) + 1} of {args.runs} ...", flush=True)
            try:
                runs.append(one_run(con))
            except (RuntimeError, TimeoutError) as e:
                failures += 1
                print(f"  failed: {e}", flush=True)
                if failures > args.runs:
                    raise
                continue
            print(f"  {sum(1 for r in runs[-1] if 'scene' in r)} scenes", flush=True)
    finally:
        # the normal boot: the device's own dashboard and Home Assistant again
        try:
            con.send("restart")
        except serial.SerialException:
            pass
        con.close()
    result = median_of(runs)
    binary = ROOT / "build" / args.board / "main" / "hassctrl.bin"
    result["meta"] = {"board": args.board, "label": args.label, "runs": args.runs, "git": git_state(),
                      "date": time.strftime("%Y-%m-%d %H:%M"),
                      "firmware_bytes": binary.stat().st_size if binary.exists() else None}
    result["raw"] = runs
    RESULTS.mkdir(parents=True, exist_ok=True)
    out = RESULTS / f"{args.board}-{args.label}.json"
    out.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"{out.relative_to(ROOT)}: {len(result['scenes'])} scenes, {args.runs} runs")
    print_table(result)


def print_table(result: dict) -> None:
    print(f"{'scene':26} {'fps':>6} {'total med':>9} {'p95':>7} {'render':>7} {'flush':>7} {'pixels':>8} {'spread':>6}")
    for name, s in result["scenes"].items():
        print(f"{name:26} {s['fps']:6.1f} {s['total.med']:9.2f} {s['total.p95']:7.2f} {s['render.med']:7.2f} "
              f"{s['flush.med']:7.2f} {s['pixels']:8.0f} {s['spread.total.med']:6.2f}")


def compare(a_path: str, b_path: str) -> None:
    a = json.loads(Path(a_path).read_text(encoding="utf-8"))
    b = json.loads(Path(b_path).read_text(encoding="utf-8"))
    for key in ("bench", "data"):
        if a["header"].get(key) != b["header"].get(key):
            sys.exit(f"different benchmark ({key} {a['header'].get(key)} and {b['header'].get(key)}): not comparable")
    print(f"A: {a_path} (LVGL {a['header']['lvgl']}), B: {b_path} (LVGL {b['header']['lvgl']}); ms per frame, "
          "change of B against A (negative: B is faster)")
    print(f"{'scene':26} {'fps A':>6} {'fps B':>6} {'total A':>8} {'total B':>8} {'change':>7} {'render A':>9} {'render B':>9}")
    for name, sa in a["scenes"].items():
        sb = b["scenes"].get(name)
        if sb is None:
            print(f"{name:26} (not in B)")
            continue
        change = (sb["total.med"] - sa["total.med"]) / sa["total.med"] * 100 if sa["total.med"] else 0.0
        print(f"{name:26} {sa['fps']:6.1f} {sb['fps']:6.1f} {sa['total.med']:8.2f} {sb['total.med']:8.2f} {change:+6.0f}% "
              f"{sa['render.med']:9.2f} {sb['render.med']:9.2f}")
    for when in ("before", "after"):
        ma, mb = a["memory"].get(when, {}), b["memory"].get(when, {})
        for key in ma:
            if key in mb:
                print(f"memory {when:6} {key:18} {ma[key]:>9.0f} {mb[key]:>9.0f}")
    fa, fb = a["meta"].get("firmware_bytes"), b["meta"].get("firmware_bytes")
    if fa and fb:
        print(f"firmware bytes        {fa:>9} {fb:>9}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("board", nargs="?", help="board[.variant], names the result and the firmware's build directory")
    parser.add_argument("-p", "--port")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--label", default="lvgl8.4")
    parser.add_argument("--compare", nargs=2, metavar=("A", "B"))
    args = parser.parse_args()
    if args.compare:
        compare(*args.compare)
    elif args.board and args.port:
        run(args)
    else:
        parser.error("a board and -p PORT, or --compare A B")


if __name__ == "__main__":
    main()
