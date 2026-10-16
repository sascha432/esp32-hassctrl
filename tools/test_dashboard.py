#!/usr/bin/env python3
"""Tests the dashboard API of a running device (docs/architecture.md section 5).

    python tools/test_dashboard.py <device> [--user admin] [--password ...] [--set-up FOLDER]

The password is asked for, or taken from HC_PASSWORD. It REPLACES the dashboard stored on the
device with docs/dashboard-sample (the device is a test device), then edits it and checks the
answers. With --set-up FOLDER it ends by importing that folder (your own dashboard) instead.
Only the Python standard library.
"""

import argparse
import getpass
import json
import os
import sys
import uuid
from pathlib import Path

from test_web import Client, check, results

ROOT = Path(__file__).resolve().parent.parent
SAMPLE = ROOT / "docs" / "dashboard-sample"


def multipart(files: dict[str, bytes]) -> tuple[bytes, dict]:
    """The body and headers of an upload of a set of files (path -> content)."""
    boundary = "----hc" + uuid.uuid4().hex
    body = b""
    for path, content in files.items():
        body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"files\"; filename=\"{path}\"\r\n"
                 f"Content-Type: text/yaml\r\n\r\n").encode() + content + b"\r\n"
    body += f"--{boundary}--\r\n".encode()
    return body, {"Content-Type": f"multipart/form-data; boundary={boundary}"}


def folder(path: Path) -> dict[str, bytes]:
    return {p.relative_to(path).as_posix(): p.read_bytes() for p in sorted(path.rglob("*.yaml"))}


def upload(c: Client, files: dict[str, bytes], check_only: bool = False):
    body, headers = multipart(files)
    return c.request("POST", "/api/dashboard/import" + ("?check=1" if check_only else ""), body=body,
                     headers=headers, timeout=30)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("device")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--password")
    parser.add_argument("--set-up", metavar="FOLDER", help="import this folder at the end")
    args = parser.parse_args()
    password = args.password or os.environ.get("HC_PASSWORD") or getpass.getpass(f"password for {args.user}: ")
    c = Client(args.device)

    for method, path in [("GET", "/api/dashboard"), ("GET", "/api/dashboard.yaml"),
                         ("POST", "/api/dashboard?revision=0"), ("POST", "/api/dashboard/import")]:
        status, _, _ = c.request(method, path, body=b"" if method == "POST" else None)
        check(status in (401, 404), f"{method} {path} without login is refused", str(status))
    status, _, _ = c.request("POST", "/api/login", form={"username": args.user, "password": password})
    check(status == 200, "login", str(status))

    sample = folder(SAMPLE)
    status, answer, _ = upload(c, sample, check_only=True)
    check(status == 200 and answer.get("checked") is True and answer.get("tiles") == 21 and answer.get("pages") == 3
          and answer.get("revision") == 0 and answer.get("ignored") == "hass", "check only: the sample is valid", f"{status} {answer}")

    broken = dict(sample)
    broken["rooms/workshop.yaml"] = sample["rooms/workshop.yaml"].replace(b"entity: light.forge_lamp", b"entity: light.forge_lamp\n      nmae: x")
    status, answer, _ = upload(c, broken, check_only=True)
    check(status == 400 and answer.get("error", "").startswith("rooms/workshop.yaml:")
          and "unknown tile key 'nmae'" in answer.get("error", "") and "included from rooms.yaml:" in answer.get("error", ""),
          "a typo is reported with file, line and include chain", f"{status} {answer}")

    missing = {k: v for k, v in sample.items() if k != "rooms.yaml"}
    status, answer, _ = upload(c, missing, check_only=True)
    check(status == 400 and "rooms.yaml is not in the uploaded files" in answer.get("error", ""), "a missing include file is reported", f"{status} {answer}")
    status, answer, _ = c.request("POST", "/api/dashboard/import", body=b"not multipart", headers={"Content-Type": "text/plain"})
    check(status == 400, "a request that is not multipart is refused", f"{status} {answer}")
    status, answer, _ = c.request("POST", "/api/dashboard/import", body=b"x" * (600 * 1024),
                                  headers={"Content-Type": "multipart/form-data; boundary=x"}, timeout=30)
    check(status == 400, "a 600 KB upload is refused", f"{status}")

    # a rejected import changes nothing; a real one replaces the dashboard
    before_status, before, _ = c.request("GET", "/api/dashboard")
    status, answer, _ = upload(c, broken)
    check(status == 400, "an invalid import is refused", str(status))
    after_status, after, _ = c.request("GET", "/api/dashboard")
    check(before_status == after_status and before == after, "a refused import changed nothing")

    status, answer, _ = upload(c, sample)
    check(status == 200 and answer.get("checked") is False and answer.get("tiles") == 21 and answer.get("revision", 0) >= 1,
          "import the sample", f"{status} {answer}")
    revision = answer.get("revision", 0)

    status, stored, _ = c.request("GET", "/api/dashboard")
    check(status == 200 and isinstance(stored, dict) and stored.get("revision") == revision and stored.get("format") == "hassctrl-dashboard"
          and len(stored.get("tiles", [])) == 9, "GET /api/dashboard returns what was imported", f"{status}")

    # edit through the JSON API with the revision
    edited = json.loads(json.dumps(stored))
    edited["tiles"][2]["name"] = "Crystal Lamp"
    status, answer, _ = c.request("POST", f"/api/dashboard?revision={revision}", body=json.dumps(edited).encode(),
                                  headers={"Content-Type": "application/json"})
    check(status == 200 and answer.get("revision") == revision + 1, "save with the current revision", f"{status} {answer}")
    status, answer, _ = c.request("POST", f"/api/dashboard?revision={revision}", body=json.dumps(edited).encode(),
                                  headers={"Content-Type": "application/json"})
    check(status == 409, "save with an old revision is a conflict (409)", f"{status} {answer}")
    status, answer, _ = c.request("POST", "/api/dashboard", body=b"{}", headers={"Content-Type": "application/json"})
    check(status == 400, "save without a revision is refused", str(status))
    status, answer, _ = c.request("POST", f"/api/dashboard?revision={revision + 1}", body=b"{",
                                  headers={"Content-Type": "application/json"})
    check(status == 400 and "not valid JSON" in answer.get("error", ""), "broken JSON is refused", f"{status} {answer}")
    bad = json.loads(json.dumps(edited))
    bad["tiles"][0]["dimmer"] = True
    bad["tiles"][1]["dimmer"] = True  # a climate tile
    status, answer, _ = c.request("POST", f"/api/dashboard?revision={revision + 1}", body=json.dumps(bad).encode(),
                                  headers={"Content-Type": "application/json"})
    check(status == 400 and "dimmer is only for lights" in answer.get("error", ""), "a rule of the format is checked", f"{status} {answer}")
    status, stored, _ = c.request("GET", "/api/dashboard")
    check(stored.get("revision") == revision + 1 and stored["tiles"][2]["name"] == "Crystal Lamp", "the edit is stored")

    # the editor of the web UI (docs/ui/web/web-dashboard-editor.md): check=1, new tiles, cells, paths
    revision = stored["revision"]
    workshop = stored["tiles"][7]["tiles"][2]
    check(workshop.get("name") == "Workshop" and workshop["tiles"][0].get("landscape") == {"cell": [3, 0]},
          "the cells of an arranged page are stored", f"{workshop}")
    added = json.loads(json.dumps(stored))
    added["tiles"].append({"entity": "switch.new_tile"})
    added["tiles"].remove(added["tiles"][5])  # the spacer: room for the new tile
    status, answer, _ = c.request("POST", f"/api/dashboard?revision={revision}&check=1", body=json.dumps(added).encode(),
                                  headers={"Content-Type": "application/json"})
    dashboard = answer.get("dashboard", {}) if isinstance(answer, dict) else {}
    new = dashboard.get("tiles", [{}])[-1]
    check(status == 200 and answer.get("checked") is True and answer.get("revision") == revision
          and new.get("entity") == "switch.new_tile" and new.get("id", 0) > 0
          and isinstance(new.get("cells", {}).get("landscape"), list) and len(new["cells"]["landscape"]) == 2,
          "check=1: a new tile gets an id and its cells", f"{status} {answer if status != 200 else new}")
    # a tile without room and a hidden tile answer 200 with null cells (docs/ui/lvgl/lvgl-dashboard.md section 6)
    crowded = json.loads(json.dumps(stored))
    crowded["tiles"] = [t for t in crowded["tiles"] if t.get("type") != "area"]
    crowded["tiles"] += [{"entity": f"switch.extra_{i}", "landscape": {"size": [8, 8]}} for i in range(3)]
    crowded["tiles"][0]["portrait"] = {"hidden": True}
    status, answer, _ = c.request("POST", f"/api/dashboard?revision={revision}&check=1", body=json.dumps(crowded).encode(),
                                  headers={"Content-Type": "application/json"})
    tiles = answer.get("dashboard", {}).get("tiles", []) if status == 200 else []
    check(status == 200 and len(tiles) >= 4 and all(t["cells"] == {"landscape": None, "portrait": None} for t in tiles[-3:])
          and tiles[0]["cells"]["portrait"] is None and isinstance(tiles[0]["cells"]["landscape"], list),
          "tiles without room or hidden are checked with null cells", f"{status} {answer if status != 200 else tiles[-1]}")
    status, after, _ = c.request("GET", "/api/dashboard")
    check(after.get("revision") == revision and len(after.get("tiles", [])) == 9, "check=1 stores nothing")

    wrong = json.loads(json.dumps(stored))
    wrong["tiles"][7]["tiles"].append({"entity": "sensor.new_tile", "dimmer": True})
    status, answer, _ = c.request("POST", f"/api/dashboard?revision={revision}&check=1", body=json.dumps(wrong).encode(),
                                  headers={"Content-Type": "application/json"})
    check(status == 400 and "dimmer is only for lights" in answer.get("error", "")
          and answer.get("path") == [7, len(wrong["tiles"][7]["tiles"]) - 1],
          "a problem names the path of the tile, also of a new one", f"{status} {answer}")

    status, answer, _ = c.request("POST", f"/api/dashboard?revision={revision}&check=1", body=json.dumps(stored).encode(),
                                  headers={"Content-Type": "application/json"})
    with_cells = answer.get("dashboard") if isinstance(answer, dict) else None
    status, answer, _ = c.request("POST", f"/api/dashboard?revision={revision}", body=json.dumps(with_cells).encode(),
                                  headers={"Content-Type": "application/json"})
    check(status == 200 and answer.get("revision") == revision + 1, "an answer with cells can be saved as it is",
          f"{status} {answer if status != 200 else ''}")
    status, stored, _ = c.request("GET", "/api/dashboard")
    check("cells" not in json.dumps(stored), "the stored file has no cells")

    status, answer, _ = c.request("GET", "/api/dashboard/editor")
    icons = answer.get("icons", []) if isinstance(answer, dict) else []
    screen = answer.get("screen", {}) if isinstance(answer, dict) else {}
    check(status == 200 and "lightbulb" in icons and "none" not in icons and len(icons) >= 40, "the icon names", f"{status} {len(icons)}")
    check(screen.get("width", 0) >= screen.get("height", 1) > 0 and screen.get("gap", 0) > 0 and screen.get("name"),
          "the screen of the device, in landscape", f"{screen}")

    # the YAML export imports again to the same dashboard
    status, text, headers = c.request("GET", "/api/dashboard.yaml")
    check(status == 200 and "attachment" in headers.get("Content-Disposition", "") and isinstance(text, str)
          and text.startswith("defaults:"), "export as YAML", f"{status}")
    status, answer, _ = upload(c, {"hass.yaml": text.encode()}, check_only=True)
    check(status == 200 and answer.get("tiles") == 21 and answer.get("pages") == 3, "the exported YAML imports again", f"{status} {answer}")

    if args.set_up:
        files = folder(Path(args.set_up))
        status, answer, _ = upload(c, files)
        check(status == 200, f"import {args.set_up}", f"{status} {answer}")

    failed = [name for ok, name in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
