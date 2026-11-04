#!/usr/bin/env python3
"""Tests the web app in a browser against a running device: what tools/test_web.py cannot see from
outside (docs/ui/web/web-app.md). Headless Microsoft Edge over the DevTools protocol.

    python tools/test_web_ui.py <device> [--user admin] [--password ...]

The password is asked for, or taken from HC_PASSWORD. Needs Microsoft Edge and websocket-client
(pip install websocket-client). Nothing is changed on the device; the browser's profile is a
temporary folder.
"""

import argparse
import getpass
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

import websocket

EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
PORT = 9333

results: list[tuple[bool, str]] = []


def check(ok: bool, name: str, detail: str = "") -> None:
    results.append((ok, name))
    print(f"{'ok  ' if ok else 'FAIL'}  {name}{'  ' + detail if detail else ''}")


class Browser:
    """One tab of a headless Edge: commands, and the events that came meanwhile."""

    def __init__(self):
        self.profile = tempfile.mkdtemp(prefix="hc-edge-")
        self.process = subprocess.Popen([EDGE, "--headless=new", f"--remote-debugging-port={PORT}",
                                         f"--user-data-dir={self.profile}", "--no-first-run", "about:blank"])
        for _ in range(50):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json") as response:
                    pages = [t for t in json.load(response) if t["type"] == "page"]
                if pages:
                    break
            except OSError:
                pass
            time.sleep(0.2)
        self.ws = websocket.create_connection(pages[0]["webSocketDebuggerUrl"], suppress_origin=True)
        self.next_id = 0
        self.events: list[dict] = []

    def _receive(self, timeout: float) -> dict | None:
        self.ws.settimeout(timeout)
        try:
            message = json.loads(self.ws.recv())
        except websocket.WebSocketTimeoutException:
            return None
        if "method" in message:
            self.handle(message)
        return message

    def handle(self, event: dict) -> None:
        # a confirm() or alert() would hang the page
        if event["method"] == "Page.javascriptDialogOpening":
            self.send_only("Page.handleJavaScriptDialog", accept=True)
        self.events.append(event)

    def send_only(self, method: str, **params) -> int:
        self.next_id += 1
        self.ws.send(json.dumps({"id": self.next_id, "method": method, "params": params}))
        return self.next_id

    def call(self, method: str, **params) -> dict:
        sent = self.send_only(method, **params)
        while True:
            message = self._receive(30)
            if message is None:
                raise TimeoutError(method)
            if message.get("id") == sent:
                if "error" in message:
                    raise RuntimeError(f"{method}: {message['error']}")
                return message.get("result", {})

    def wait_event(self, method: str, timeout: float, match=lambda params: True) -> dict | None:
        deadline = time.time() + timeout
        while True:
            for i, event in enumerate(self.events):
                if event["method"] == method and match(event["params"]):
                    return self.events.pop(i)["params"]
            left = deadline - time.time()
            if left <= 0:
                return None
            self._receive(left)

    def eval(self, expression: str):
        result = self.call("Runtime.evaluate", expression=expression, awaitPromise=True, returnByValue=True)
        if "exceptionDetails" in result:
            raise RuntimeError(f"{expression}: {result['exceptionDetails'].get('text')}")
        return result["result"].get("value")

    def wait_for(self, expression: str, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.eval(expression):
                return True
            self._receive(0.2)
        return False

    def close(self) -> None:
        try:
            self.ws.close()
        finally:
            self.process.terminate()
            self.process.wait(10)
            shutil.rmtree(self.profile, ignore_errors=True)


def stale_page_test(b: Browser) -> None:
    """A page left while it still loads its data does not take the live socket from the next page:
    the Status page's GET is held until the Hotspot page follows its own path, then let go."""
    b.eval('location.hash = "#/hotspot"')
    state = ('JSON.stringify({hash: location.hash, path: liveState.path, socket: liveState.socket ? liveState.socket.readyState : null, '
             'h1: document.querySelector("h1") ? document.querySelector("h1").textContent : ""})')
    check(b.wait_for('liveState.socket && liveState.socket.readyState === 1 && document.querySelector("#hotspot-on") !== null', 15),
          "the Hotspot page shows and follows on the live socket", b.eval(state))
    b.call("Fetch.enable", patterns=[{"urlPattern": "*/api/status*", "requestStage": "Request"}])
    b.eval('location.hash = "#/status"')
    held = b.wait_event("Fetch.requestPaused", 10)
    check(held is not None, "the Status page's GET is held")
    b.eval('location.hash = "#/hotspot"')
    shown = b.wait_for('document.querySelector("#hotspot-on") !== null', 15)
    # the hotspot page's own GETs pass; another /api/status (none expected) is let through as well
    while (other := b.wait_event("Fetch.requestPaused", 0.5)) is not None:
        b.call("Fetch.continueRequest", requestId=other["requestId"])
    check(shown, "the Hotspot page shows again while the Status page still loads")
    if held is not None:
        b.call("Fetch.continueRequest", requestId=held["requestId"])
    b.call("Fetch.disable")
    b.wait_for("false", 2)
    follows = b.eval("JSON.stringify([...(liveState.follows ? liveState.follows.keys() : [liveState.path])])")
    on_page = b.eval('document.querySelector("h1") ? document.querySelector("h1").textContent : ""')
    check(on_page == "Hotspot" and follows == '["/api/hotspot"]',
          "after the held GET the live socket follows the Hotspot page only", f"page {on_page}, follows {follows}")


def hold(b: Browser, pattern: str) -> None:
    b.call("Fetch.enable", patterns=[{"urlPattern": pattern, "requestStage": "Request"}])


def release(b: Browser, request: dict | None) -> None:
    if request is not None:
        b.call("Fetch.continueRequest", requestId=request["requestId"])


def shown(b: Browser) -> str:
    """The page's title and the path the live socket follows: "Hotspot /api/hotspot"."""
    return b.eval('(document.querySelector("h1") ? document.querySelector("h1").textContent : "") + " " + liveState.path')


def overtaken_route_test(b: Browser) -> None:
    """A route overtaken while it asks for the session draws nothing: the session of the route to
    Status is held, the route to Time after it passes, then the first one is let go."""
    b.eval('location.hash = "#/hotspot"')
    b.wait_for('document.querySelector("#hotspot-on") !== null', 15)
    hold(b, "*/api/session*")
    b.eval('location.hash = "#/status"')
    first = b.wait_event("Fetch.requestPaused", 10)
    b.eval('location.hash = "#/time"')
    second = b.wait_event("Fetch.requestPaused", 10)
    check(first is not None and second is not None, "both routes' sessions are held")
    release(b, second)
    b.wait_for('liveState.path === "/api/time"', 15)
    release(b, first)
    b.call("Fetch.disable")
    b.wait_for("false", 2)
    check(shown(b) == "Time /api/time", "a route overtaken while it waits for the session draws nothing", shown(b))


def left_dashboard_test(b: Browser) -> None:
    """The dashboard left while it asks which app runs: no editor over the next page."""
    b.eval('location.hash = "#/hotspot"')
    b.wait_for('document.querySelector("#hotspot-on") !== null', 15)
    hold(b, "*/api/update*")
    b.eval('location.hash = "#/dashboard"')
    held = b.wait_event("Fetch.requestPaused", 10)
    check(held is not None, "the dashboard's GET /api/update is held")
    # the Hotspot page before is still shown while the dashboard waits: the route to Hotspot is done
    # when it follows its path (the route to the dashboard stopped it)
    b.eval('location.hash = "#/hotspot"')
    b.wait_for('liveState.path === "/api/hotspot"', 15)
    release(b, held)
    b.call("Fetch.disable")
    b.wait_for("false", 3)
    # window.hcEditor stays once dashboard.js was loaded: the editor's box tells
    editor = b.eval('document.querySelector("#dash-tab") !== null')
    check(shown(b) == "Hotspot /api/hotspot" and not editor, "a dashboard left while it loads draws no editor",
          f"{shown(b)}, editor {editor}")


def pages_test(b: Browser) -> None:
    """Every page of the menu draws its title, with no script error on the way."""
    b.call("Runtime.enable")
    b.events = [e for e in b.events if e["method"] != "Runtime.exceptionThrown"]
    names = b.eval('[...document.querySelectorAll("#menu-pages a")].map(a => a.dataset.page)')
    check(len(names) >= 5, "the menu lists the pages", ", ".join(names))
    for name in names:
        b.eval(f'location.hash = "#/{name}"')
        drawn = b.wait_for('document.querySelector("#page h1") !== null && !document.querySelector("#page").textContent.includes("Loading ...")', 15)
        b.wait_for("false", 1)
        errors = [e["params"]["exceptionDetails"] for e in b.events if e["method"] == "Runtime.exceptionThrown"]
        b.events = [e for e in b.events if e["method"] != "Runtime.exceptionThrown"]
        detail = "; ".join(str(d.get("exception", {}).get("description", d.get("text")))[:120] for d in errors)
        check(drawn and not errors, f"page {name} draws without a script error", detail or shown(b))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("device")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--password")
    args = parser.parse_args()
    password = args.password or os.environ.get("HC_PASSWORD") or getpass.getpass(f"password for {args.user}: ")

    b = Browser()
    try:
        b.call("Page.enable")
        b.call("Page.navigate", url=f"http://{args.device}/")
        b.wait_event("Page.loadEventFired", 20)
        form = json.dumps({"username": args.user, "password": password, "remember": "0"})
        status = b.eval(f'fetch("/api/login", {{method: "POST", body: new URLSearchParams({form})}}).then(r => r.status)')
        check(status == 200, "login in the browser", str(status))
        b.call("Page.reload")
        b.wait_event("Page.loadEventFired", 20)
        pages_test(b)
        stale_page_test(b)
        overtaken_route_test(b)
        left_dashboard_test(b)
        b.eval('fetch("/api/logout", {method: "POST"}).then(r => r.status)')
    finally:
        b.close()

    failed = [name for ok, name in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
