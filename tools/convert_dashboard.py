#!/usr/bin/env python3
"""Converts a dashboard of format version 1 to version 2 (docs/architecture.md section 5).

    python tools/convert_dashboard.py INPUT [OUTPUT] [--in-place]

INPUT is one of
  * a JSON document: the stored /fs/dashboard.json (files.zip of the System page) or the answer
    of GET /api/dashboard of a version 1 firmware;
  * a YAML file: the dashboard export (hassctrl-dashboard.yaml) or the backup file of the System
    page (its settings sections stay as they are);
  * a folder of YAML files with includes (the import set of the Dashboard page).
OUTPUT is the converted file or folder; without it INPUT gets ".v2" before its extension (a
folder: "-v2" after its name). --in-place overwrites INPUT. Only the Python standard library.

THIS IS THE ONE-TIME CONVERSION OF A DEVELOPMENT FORMAT, not part of the firmware: the firmware
reads version 2 only and refuses older files with a message that names this tool. Later versions
of the format add their conversion here, the firmware never converts.

Version 1 (2026-10-08) -> version 2 (2026-10-14):
  * `size: 1x2` moves into `landscape: {size: 1x2}`. A tile without `size` had a default by its
    kind (a dimmer light and a climate 1x2, a camera 2x2); version 2 defaults to 1x1 for every
    kind, so the old default is written out, and the tile looks as before.
  * `landscape: 2,1` / `portrait: 0,3` (the cell) become `landscape: {cell: 2,1}` /
    `portrait: {cell: 0,3}`.
  * The portrait size is the landscape size turned, as before, so no `portrait: {size}` is
    written. `hidden` and `defaults.navigation` are new and not written.
  * JSON: "version" 2; the "cells" of an API answer are dropped.
"""

import argparse
import json
import re
import shutil
import sys
from pathlib import Path

# the default size of version 1 by the kind of the tile (core::default_span of that version)
V1_DEFAULTS = {"climate": "1x2", "camera": "2x2"}
V1_DIMMER_DEFAULT = "1x2"

KEY = re.compile(r"^(\s*)(- )?([A-Za-z_]+):[ \t]*(.*?)[ \t]*(#.*)?$")


def v1_default_size(entity: str | None, dimmer: bool) -> str | None:
    """The size a version 1 tile had without `size`, or None for 1x1."""
    if not entity:
        return None
    domain = entity.split(".", 1)[0]
    if domain == "light":
        return V1_DIMMER_DEFAULT if dimmer else None
    return V1_DEFAULTS.get(domain)


# ------------------------------------------------------------------------------------- JSON


def convert_json_tile(tile: dict) -> dict:
    out = {}
    landscape: dict = {}
    portrait: dict = {}
    for key, value in tile.items():
        if key == "size":
            landscape["size"] = value
        elif key == "landscape":
            landscape["cell"] = value
        elif key == "portrait":
            portrait["cell"] = value
        elif key == "cells":
            continue
        elif key == "tiles":
            out["tiles"] = [convert_json_tile(t) for t in value]
        else:
            out[key] = value
    if "size" not in landscape and tile.get("type") is None:
        default = v1_default_size(tile.get("entity"), bool(tile.get("dimmer")))
        if default:
            cols, rows = default.split("x")
            landscape["size"] = [int(cols), int(rows)]
    if landscape:
        out["landscape"] = landscape
    if portrait:
        out["portrait"] = portrait
    return out


def convert_json(text: str) -> str:
    doc = json.loads(text)
    if doc.get("format") != "hassctrl-dashboard":
        raise SystemExit("not a dashboard: the JSON has no \"format\": \"hassctrl-dashboard\"")
    version = doc.get("version")
    if version == 2:
        raise SystemExit("the file has version 2 already")
    if version != 1:
        raise SystemExit(f"version {version} is not known to this tool")
    doc["version"] = 2
    doc["tiles"] = [convert_json_tile(t) for t in doc.get("tiles", [])]
    return json.dumps(doc, separators=(",", ":"), ensure_ascii=False) + "\n"


# ------------------------------------------------------------------------------------- YAML


def indent_of(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def is_blank(line: str) -> bool:
    stripped = line.strip()
    return stripped == "" or stripped.startswith("#")


def convert_yaml(text: str) -> tuple[str, int]:
    """The text with every tile of version 1 rewritten; also how many tiles changed."""
    lines = text.split("\n")
    changed = 0
    i = 0
    while i < len(lines):
        match = KEY.match(lines[i])
        if match is None or match.group(2) is None:
            i += 1
            continue
        # a list item: its own keys stand at the indent of the text after "- "
        dash_indent = len(match.group(1))
        base = dash_indent + 2
        end = i + 1
        while end < len(lines) and (is_blank(lines[end]) or indent_of(lines[end]) > dash_indent):
            end += 1
        own = []  # (index, key, value) of the keys of this item
        for k in range(i, end):
            m = KEY.match(lines[k])
            if m is None or is_blank(lines[k]):
                continue
            if k == i or (len(m.group(1)) == base and m.group(2) is None):
                own.append((k, m.group(3), m.group(4)))
        keys = {key: (index, value) for index, key, value in own}
        # the items of a nested list (the tiles of an area) are visited by the same scan: on
        # from the next line, whatever happens to this item
        i += 1
        if "include" in keys or ("entity" not in keys and keys.get("type", (0, ""))[1] not in ("area", "spacer")):
            continue
        if "position" in keys:
            raise SystemExit(f"line {keys['position'][0] + 1}: position: is older than version 1; set landscape: {{cell: c,r}} by hand")
        size = keys.get("size", (None, None))[1]
        landscape_cell = keys.get("landscape", (None, None))[1]
        portrait_cell = keys.get("portrait", (None, None))[1]
        for key in ("landscape", "portrait"):
            value = keys.get(key, (None, ""))[1]
            if key in keys and value == "":
                raise SystemExit(f"line {keys[key][0] + 1}: {key}: is a map already; is the file version 2?")
        if size is None and "type" not in keys:
            size = v1_default_size(keys["entity"][1], keys.get("dimmer", (None, "false"))[1] == "true")
        if size is None and landscape_cell is None and portrait_cell is None:
            continue
        removed = sorted(keys[k][0] for k in ("size", "landscape", "portrait") if k in keys)
        pad = " " * base
        block = []
        if size is not None or landscape_cell is not None:
            block.append(f"{pad}landscape:")
            if size is not None:
                block.append(f"{pad}  size: {size}")
            if landscape_cell is not None:
                block.append(f"{pad}  cell: {landscape_cell}")
        if portrait_cell is not None:
            block.append(f"{pad}portrait:")
            block.append(f"{pad}  cell: {portrait_cell}")
        if removed:
            at = removed[0]
            if at == i - 1:
                raise SystemExit(f"line {i}: size, landscape or portrait on the line of the dash is not supported")
        else:
            # only a default size to write out: after the last own key, before any nested list
            at = max(index for index, _, _ in own) + 1
        for index in reversed(removed):
            del lines[index]
            if index < at:
                at -= 1
        lines[at:at] = block
        changed += 1
    return "\n".join(lines), changed


# ------------------------------------------------------------------------------------- files


def convert_file(source: Path, target: Path) -> None:
    text = source.read_text(encoding="utf-8")
    if source.suffix.lower() == ".json":
        out = convert_json(text)
        print(f"{source} -> {target}: JSON converted")
    else:
        out, changed = convert_yaml(text)
        print(f"{source} -> {target}: {changed} tile(s) changed")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(out, encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--in-place", action="store_true", help="overwrite the input")
    args = parser.parse_args()
    source: Path = args.input
    if not source.exists():
        sys.exit(f"{source} does not exist")
    if args.in_place:
        target = source
    elif args.output is not None:
        target = args.output
    elif source.is_dir():
        target = source.with_name(source.name + "-v2")
    else:
        target = source.with_name(source.stem + ".v2" + source.suffix)
    if source.is_dir():
        for path in sorted(source.rglob("*")):
            if not path.is_file():
                continue
            out = target / path.relative_to(source)
            if path.suffix.lower() in (".yaml", ".yml"):
                convert_file(path, out)
            elif out != path:
                out.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, out)
    else:
        convert_file(source, target)
    print("check the result with the Dashboard page of the web UI (Import & export, Check) before importing it")


if __name__ == "__main__":
    main()
