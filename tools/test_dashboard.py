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
    check(status == 200 and answer.get("checked") is True and answer.get("tiles") == 18 and answer.get("pages") == 3
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
    check(status == 200 and answer.get("checked") is False and answer.get("tiles") == 18 and answer.get("revision", 0) >= 1,
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

    # the YAML export imports again to the same dashboard
    status, text, headers = c.request("GET", "/api/dashboard.yaml")
    check(status == 200 and "attachment" in headers.get("Content-Disposition", "") and isinstance(text, str)
          and text.startswith("defaults:"), "export as YAML", f"{status}")
    status, answer, _ = upload(c, {"hass.yaml": text.encode()}, check_only=True)
    check(status == 200 and answer.get("tiles") == 18 and answer.get("pages") == 3, "the exported YAML imports again", f"{status} {answer}")

    if args.set_up:
        files = folder(Path(args.set_up))
        status, answer, _ = upload(c, files)
        check(status == 200, f"import {args.set_up}", f"{status} {answer}")

    failed = [name for ok, name in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
