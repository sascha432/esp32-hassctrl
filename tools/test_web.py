#!/usr/bin/env python3
"""Tests the web server of a running device (docs/ui/web/web-app.md). Needs a device with a login.

    python tools/test_web.py <device> [--user admin] [--password ...]

The password is asked for, or taken from HC_PASSWORD. Nothing is changed on the device: no
factory reset, no new login, no "log out everywhere"; it logs in and out once. Only the Python
standard library.
"""

import argparse
import concurrent.futures
import getpass
import gzip
import io
import http.client
import json
import os
import socket
import sys
import time
import urllib.parse
import zipfile

results: list[tuple[bool, str]] = []


def check(ok: bool, name: str, detail: str = "") -> None:
    results.append((ok, name))
    print(f"{'ok  ' if ok else 'FAIL'}  {name}{'  ' + detail if detail else ''}")


class Client:
    def __init__(self, host: str):
        self.host = host
        self.cookie = ""

    def request(self, method: str, path: str, body: bytes | None = None, form: dict | None = None,
                headers: dict | None = None, timeout: float = 10) -> tuple[int, dict | str, dict]:
        conn = http.client.HTTPConnection(self.host, timeout=timeout)
        # as a browser does: the web image has gzip copies only (406 without)
        headers = {"Accept-Encoding": "gzip", **(headers or {})}
        if form is not None:
            body = urllib.parse.urlencode(form).encode()
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        if self.cookie:
            headers["Cookie"] = self.cookie
        conn.request(method, path, body=body, headers=headers)
        response = conn.getresponse()
        raw = response.read()
        if response.getheader("Content-Encoding") == "gzip":
            raw = gzip.decompress(raw)
        cookie = response.getheader("Set-Cookie")
        if cookie:
            self.cookie = cookie.split(";", 1)[0]
        conn.close()
        try:
            data: dict | str = json.loads(raw)
        except ValueError:
            data = raw.decode(errors="replace")
        return response.status, data, dict(response.getheaders())


def sd_rename_test(c: Client) -> None:
    """Rename and move on the card: a test folder with a file, removed at the end."""
    base = "hc-test-rename"
    names = lambda d: {e["name"] for e in c.request("GET", "/api/sd/list?dir=" + urllib.parse.quote(d))[1]["entries"]}
    c.request("POST", "/api/sd/mkdir", form={"path": base})
    c.request("POST", "/api/sd/mkdir", form={"path": base + "/sub"})
    status, _, _ = c.request("POST", "/api/sd/file?path=" + urllib.parse.quote(base + "/a.txt"), body=b"hello",
                             headers={"Content-Type": "application/octet-stream"})
    check(status == 200, "SD test file uploaded", str(status))
    status, _, _ = c.request("POST", "/api/sd/rename", form={"from": base + "/a.txt", "to": base + "/b.txt"})
    check(status == 200 and "b.txt" in names(base) and "a.txt" not in names(base), "SD rename", str(status))
    status, _, _ = c.request("POST", "/api/sd/rename", form={"from": base + "/b.txt", "to": base + "/sub/b.txt"})
    check(status == 200 and "b.txt" in names(base + "/sub"), "SD move into a folder", str(status))
    status, _, _ = c.request("POST", "/api/sd/rename", form={"from": base + "/sub", "to": base + "/sub/x"})
    check(status == 400, "SD move of a folder into itself is refused", str(status))
    c.request("POST", "/api/sd/mkdir", form={"path": base + "/other"})
    status, _, _ = c.request("POST", "/api/sd/rename", form={"from": base + "/sub", "to": base + "/other"})
    check(status == 409, "SD rename onto a name that exists is refused", str(status))
    status, _, _ = c.request("POST", "/api/sd/rename", form={"from": base + "/none", "to": base + "/y"})
    check(status == 404, "SD rename of a missing file is 404", str(status))
    for path in [base + "/sub/b.txt", base + "/sub", base + "/other", base]:
        c.request("POST", "/api/sd/delete", form={"path": path})
    check(base not in names(""), "SD test folder removed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("device")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--password")
    args = parser.parse_args()
    password = args.password or os.environ.get("HC_PASSWORD") or getpass.getpass(f"password for {args.user}: ")
    c = Client(args.device)

    # ------------------------------------------------------------------------ without a login
    status, page, _ = c.request("GET", "/")
    check(status == 200 and "<html" in str(page), "GET / answers a page")
    status, _, _ = c.request("GET", "/", headers={"Accept-Encoding": "identity"})
    check(status == 406, "GET / without gzip is 406 (the web image is gzip only)", str(status))
    status, _, headers = c.request("GET", "/")
    etag = headers.get("ETag", "")
    check(status == 200 and etag.startswith('"'), "GET / carries the image's ETag", etag)
    status, _, _ = c.request("GET", "/app.js", headers={"If-None-Match": etag})
    check(status == 304, "GET /app.js with the image's ETag is 304", str(status))
    status, page, _ = c.request("GET", "/recovery")
    check(status == 200 and "<html" in str(page), "GET /recovery answers the built-in page")
    status, session, _ = c.request("GET", "/api/session")
    check(status == 200 and session.get("setup") is False, "a login exists (not in setup mode)", str(session))
    for path in ["/api/status", "/api/wifi", "/api/update", "/api/hotspot", "/api/fs/files.zip"]:
        status, _, _ = c.request("GET", path)
        check(status == 401, f"GET {path} without login is 401", str(status))
    status, _, _ = c.request("POST", "/api/update/firmware", body=b"x" * 100,
                             headers={"Content-Type": "application/octet-stream"})
    check(status == 401, "an upload without login is 401", str(status))
    # the SD card's routes (CONFIG_HC_SDCARD; unknown paths without it)
    for path in ["/api/sd", "/api/sd/list", "/api/sd/file?path=x"]:
        status, _, _ = c.request("GET", path)
        check(status in (401, 404), f"GET {path} without login is refused", str(status))
    status, _, _ = c.request("POST", "/api/sd/format", form={"confirm": "yes"})
    check(status in (401, 404, 405), "POST /api/sd/format without login is refused", str(status))
    # the recovery app has no Home Assistant connection: there the path is unknown
    status, _, _ = c.request("GET", "/api/hass")
    check(status in (401, 404), "GET /api/hass without login is 401", str(status))
    status, _, _ = c.request("POST", "/api/hass", form={"url": "http://x", "token": "y"})
    check(status in (401, 404), "POST /api/hass without login is refused", str(status))
    # the player's routes (CONFIG_HC_UI_PLAYER; unknown paths without it)
    for path in ["/api/player", "/api/player/cover"]:
        status, _, _ = c.request("GET", path)
        check(status in (401, 404), f"GET {path} without login is refused", str(status))
    status, _, _ = c.request("POST", "/api/player", form={"action": "toggle"})
    check(status in (401, 404, 405), "POST /api/player without login is refused", str(status))
    status, answer, _ = c.request("POST", "/api/setup/user", form={"username": "intruder", "password": "12345678"})
    check(status == 403, "a second login cannot be created", f"{status} {answer}")

    status, _, _ = c.request("POST", "/api/login", form={"username": args.user, "password": "wrong-password"})
    check(status == 401, "a wrong password is 401", str(status))
    status, _, _ = c.request("POST", "/api/login", form={"username": "", "password": ""})
    check(status in (400, 401), "an empty login is refused", str(status))
    status, _, _ = c.request("POST", "/api/login", body=b"username=" + b"a" * 8000,
                             headers={"Content-Type": "application/x-www-form-urlencoded"})
    check(status in (400, 413), "an 8 KB login body is refused", str(status))
    status, _, _ = c.request("POST", "/api/login", form={"username": args.user, "password": "%ZZ%"})
    check(status in (400, 401), "a malformed form value is refused", str(status))

    for path in ["/../../settings", "/%2e%2e/%2e%2e/etc", "/www/../../x", "/nothing.html", "/api/nothing"]:
        status, _, _ = c.request("GET", path)
        check(status == 404, f"GET {path} is 404", str(status))

    # ---------------------------------------------------------------------------- with a login
    status, _, headers = c.request("POST", "/api/login", form={"username": args.user, "password": password, "remember": "0"})
    cookie_header = headers.get("Set-Cookie", "")
    check(status == 200 and bool(c.cookie), "login", str(status))
    check("HttpOnly" in cookie_header and "SameSite=Strict" in cookie_header, "the session cookie is HttpOnly and SameSite=Strict",
          cookie_header)
    status, s, _ = c.request("GET", "/api/status")
    check(status == 200 and "device" in s and "wifi" in s and s["device"].get("built"), "GET /api/status",
          f"heap {s.get('device', {}).get('heap_internal_kb')} KB, built {s.get('device', {}).get('built')}")
    heap_before = s["device"]["heap_internal_kb"] if status == 200 else 0
    status, w, _ = c.request("GET", "/api/wifi")
    check(status == 200 and "saved" in w, "GET /api/wifi", f"{len(w.get('saved', []))} saved")
    # `password_needed` is a flag, not a password
    check(all("password" not in n for n in w.get("saved", [])), "no password in the network list",
          str([sorted(n) for n in w.get("saved", [])]))
    status, u, _ = c.request("GET", "/api/update")
    check(status == 200 and u.get("app") in ("main", "recovery"), "GET /api/update", str(u.get("app")))
    status, answer, _ = c.request("POST", "/api/wifi/add", form={"password": "12345678"})
    check(status == 400, "a network without a name is refused", str(status))
    status, _, _ = c.request("POST", "/api/update/filesystem", body=b"",
                             headers={"Content-Type": "application/octet-stream"})
    check(status in (400, 411), "an empty image is refused", str(status))
    status, h, _ = c.request("GET", "/api/hotspot")
    check(status == 200 and {"on", "kept_on", "ssid", "password", "clients"} <= set(h), "GET /api/hotspot", str(status))
    for path, form in [("/api/wifi/remove", {"index": "7"}), ("/api/wifi/remove", {"index": "x"}),
                       ("/api/wifi/move", {"index": "0", "delta": "5"}), ("/api/wifi/policy", {"policy": "random"}),
                       ("/api/hotspot", {"on": "maybe"}), ("/api/factory-reset", {"confirm": "no"}),
                       ("/api/fs/format", {"confirm": "no"})]:
        status, _, _ = c.request("POST", path, form=form)
        check(status == 400, f"POST {path} {form} is refused", str(status))
    status, answer, _ = c.request("POST", "/api/update/url", form={"target": "firmware", "url": "ftp://x"})
    check(status in (400, 409), "a URL that is not http(s) is refused", f"{status} {answer}")

    # the SD card (a firmware with CONFIG_HC_SDCARD reports it in /api/status); only a test folder
    # is written, and removed again
    if "sd" in s:
        status, sd, _ = c.request("GET", "/api/sd")
        check(status == 200 and "state" in sd and "fetch" in sd, "GET /api/sd", str(sd.get("state")))
        for form, what in [({"confirm": "no", "password": password}, "without confirm=yes"),
                           ({"confirm": "yes", "password": "wrong-password"}, "with a wrong password"),
                           ({"confirm": "yes"}, "without a password")]:
            status, _, _ = c.request("POST", "/api/sd/format", form=form)
            check(status == 400, f"formatting the SD card {what} is refused", str(status))
        for path in ["../fs/dashboard.json", "a/../../x", "a\\b"]:
            status, _, _ = c.request("GET", "/api/sd/file?path=" + urllib.parse.quote(path))
            check(status in (400, 409), f"GET /api/sd/file {path!r} is refused", str(status))
        status, _, _ = c.request("POST", "/api/sd/fetch", form={"url": "ftp://x/y", "path": "y"})
        check(status in (400, 409), "an SD download from a URL that is not http(s) is refused", str(status))
        if sd.get("state") == "mounted":
            sd_rename_test(c)

    # a new password over 64 characters is named as too long (the current one is wrong: nothing changes)
    status, answer, _ = c.request("POST", "/api/password", form={"current": "wrong-password", "password": "x" * 65})
    check(status == 400 and "8 to 64" in str(answer), "a new password of 65 characters is too long", f"{status} {answer}")

    # Home Assistant: only what is refused before anything is stored, the connection stays as it is
    status, ha, _ = c.request("GET", "/api/hass")
    if u.get("app") == "recovery":
        check(status == 404, "the recovery app has no /api/hass", str(status))
    else:
        check(status == 200 and {"state", "url", "token_set", "version", "error"} <= set(ha), "GET /api/hass",
              f"{ha.get('state')} {ha.get('url')}")
        check("token" not in {k for k in ha if k != "token_set"}, "no token in the Home Assistant answer")
        for form, name in [({"token": "abc"}, "without url"), ({"url": "ftp://ha.local"}, "ftp:// url"),
                           ({"url": "http://:8123"}, "url without a host"), ({"url": "ha.local:99999"}, "port 99999"),
                           ({"url": "http://ha.local", "token": "two words"}, "token with a space"),
                           ({"url": "http://ha.local", "token": "x" * 600}, "token of 600 characters")]:
            status, answer, _ = c.request("POST", "/api/hass", form=form)
            check(status == 400, f"POST /api/hass {name} is refused", f"{status} {answer.get('error', '')}")
        status, again, _ = c.request("GET", "/api/hass")
        check(status == 200 and again.get("url") == ha.get("url"), "the refused requests changed nothing",
              str(again.get("url")))

    # the player: read and wrong values only, nothing starts or stops
    status, player, _ = c.request("GET", "/api/player")
    if status == 404:
        print("  (no player: /api/player skipped)")
    else:
        check(status == 200 and {"state", "title", "volume", "index", "count", "repeat", "next"} <= set(player),
              "GET /api/player", f"{player.get('state')} {player.get('count')} queued")
        check(isinstance(player.get("next"), list) and len(player["next"]) <= 20, "up next is a list of at most 20",
              str(player.get("next")))
        for form, name in [({"action": "dance"}, "an unknown action"), ({"volume": "101"}, "volume 101"),
                           ({"volume": "x"}, "a volume that is no number"), ({"repeat": "twice"}, "repeat twice"),
                           ({"shuffle": "yes"}, "shuffle yes"), ({"seek": "-5"}, "a negative seek")]:
            status, answer, _ = c.request("POST", "/api/player", form=form)
            check(status == 400, f"POST /api/player {name} is refused", f"{status} {answer.get('error', '')}")
        status, again, _ = c.request("GET", "/api/player")
        check(status == 200 and again.get("volume") == player.get("volume"), "the refused requests changed nothing",
              str(again.get("volume")))

    conn = http.client.HTTPConnection(args.device, timeout=60)
    conn.request("GET", "/api/fs/files.zip", headers={"Cookie": c.cookie})
    response = conn.getresponse()
    archive = response.read()
    conn.close()
    try:
        with zipfile.ZipFile(io.BytesIO(archive)) as z:
            bad = z.testzip()
            names = z.namelist()
        check(response.status == 200 and bad is None, "GET /api/fs/files.zip is a valid archive", f"{len(names)} files")
    except zipfile.BadZipFile:
        check(False, "GET /api/fs/files.zip is a valid archive", f"{response.status}, {len(archive)} bytes")

    # ------------------------------------------------------------------------------------ load
    def fetch(i: int) -> tuple[int, float]:
        started = time.time()
        client = Client(args.device)
        client.cookie = c.cookie
        try:
            status, _, _ = client.request("GET", "/api/status" if i % 2 else "/", timeout=20)
        except OSError:
            status = 0
        return status, time.time() - started

    started = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        answers = list(pool.map(fetch, range(60)))
    good = sum(1 for status, _ in answers if status == 200)
    slowest = max(t for _, t in answers)
    check(good == len(answers), "60 requests from 8 connections at once",
          f"{good} ok in {time.time() - started:.1f} s, slowest {slowest:.2f} s")

    # A client that announces a body and stalls must not block the server for long.
    stall = socket.create_connection((args.device, 80), timeout=5)
    stall.sendall(b"POST /api/login HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                  b"Content-Length: 100\r\n\r\nusername=a")
    time.sleep(1)
    started = time.time()
    try:
        status, _, _ = c.request("GET", "/api/session", timeout=40)
    except OSError:
        status = 0
    waited = time.time() - started
    stall.close()
    check(status == 200 and waited < 30, "a stalled upload does not block other requests for long",
          f"{status} after {waited:.1f} s")

    time.sleep(1)
    status, s, _ = c.request("GET", "/api/status")
    heap_after = s["device"]["heap_internal_kb"] if status == 200 else 0
    check(status == 200 and heap_after >= heap_before - 8, "free memory after the load",
          f"{heap_before} KB before, {heap_after} KB after")

    # ---------------------------------------------------------------------------------- logout
    cookie = c.cookie
    status, _, _ = c.request("POST", "/api/logout", form={})
    check(status == 200, "logout", str(status))
    c.cookie = cookie
    status, session, _ = c.request("GET", "/api/session")
    check(status == 200, "the old cookie after logout", f"logged_in {session.get('logged_in')} (the token stays valid until it expires)")

    failed = [name for ok, name in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
