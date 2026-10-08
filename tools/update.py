#!/usr/bin/env python3
"""Updates a device over the network (docs/ui/web.md section 8).

    python tools/update.py <device> firmware <image.bin> [--user admin] [--password ...]
    python tools/update.py <device> filesystem <image.bin> ...

<device> is an address or host name (192.168.0.206, hassctrl-1a2b.local). The password is asked
for when it is not given and the device has a login (also from HC_PASSWORD). On a 4 MB board the
firmware goes through the recovery app: the device restarts into it, receives the image and
starts the new firmware.

Only the Python standard library.
"""

import argparse
import getpass
import http.cookiejar
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


class Device:
    def __init__(self, host: str):
        self.base = host if host.startswith("http") else f"http://{host}"
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def request(self, path: str, data: bytes | None = None, form: dict | None = None, timeout: float = 10,
                content_type: str | None = None) -> tuple[int, dict]:
        if form is not None:
            data = urllib.parse.urlencode(form).encode()
            content_type = "application/x-www-form-urlencoded"
        req = urllib.request.Request(self.base + path, data=data, method="POST" if data is not None else "GET")
        if content_type:
            req.add_header("Content-Type", content_type)
        try:
            with self.opener.open(req, timeout=timeout) as response:
                return response.status, json.loads(response.read() or b"{}")
        except urllib.error.HTTPError as error:
            body = error.read()
            try:
                return error.code, json.loads(body or b"{}")
            except ValueError:
                return error.code, {"error": body.decode(errors="replace")}

    def update_info(self) -> dict | None:
        try:
            status, info = self.request("/api/update", timeout=5)
        except OSError:
            return None
        return info if status == 200 else None

    def wait_for(self, app: str, seconds: int = 120) -> dict:
        """Waits until `app` runs and has confirmed itself (a reset before that rolls it back)."""
        end = time.time() + seconds
        time.sleep(3)
        while time.time() < end:
            info = self.update_info()
            if info is not None and info.get("app") == app and info.get("confirmed", True):
                return info
            time.sleep(2)
        sys.exit(f"the device did not come back as the {app} app, or it did not confirm itself")


    def request_retried(self, path: str, **kwargs) -> tuple[int, dict]:
        """A short request, tried 3 times: right after a restart the device may not answer yet."""
        for attempt in range(3):
            try:
                return self.request(path, **kwargs)
            except OSError:
                if attempt == 2:
                    raise
                time.sleep(2)
        raise AssertionError


def login(device: Device, user: str, password: str | None) -> None:
    _, session = device.request_retried("/api/session")
    if session.get("setup") or session.get("logged_in"):
        return
    if password is None:
        password = os.environ.get("HC_PASSWORD") or getpass.getpass(f"password for {user}: ")
    status, answer = device.request_retried("/api/login", form={"username": user, "password": password, "remember": "0"})
    if status != 200:
        sys.exit(f"login failed: {answer.get('error', status)}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("device")
    parser.add_argument("target", choices=["firmware", "filesystem"])
    parser.add_argument("image")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--password")
    args = parser.parse_args()

    image = open(args.image, "rb").read()
    device = Device(args.device)
    login(device, args.user, args.password)

    info = None
    for _ in range(3):
        info = device.update_info()
        if info is not None:
            break
        time.sleep(1)
    if info is None:
        sys.exit("no answer from /api/update")
    if args.target == "firmware" and not info["firmware_direct"]:
        print("restarting into the recovery app ...")
        status, answer = device.request("/api/restart", form={"to": "recovery"})
        if status != 200:
            sys.exit(f"cannot restart into the recovery app: {answer.get('error', status)}")
        device.wait_for("recovery")
        login(device, args.user, args.password)

    print(f"uploading {len(image) // 1024} KB ...")
    started = time.time()
    status, answer = device.request(f"/api/update/{args.target}", data=image, timeout=300,
                                    content_type="application/octet-stream")
    if status != 200:
        sys.exit(f"update failed: {answer.get('error', status)}")
    print(f"written in {time.time() - started:.1f} s")
    if answer.get("restart"):
        print("restarting into the new firmware ...")
        info = device.wait_for("main")
        print(f"running firmware {info['firmware_version']}")


if __name__ == "__main__":
    main()
