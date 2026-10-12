#include "builtin_page.hpp"

namespace hc::webserver::detail {

// No external resources: in the captive portal there is no internet. Barely any design on
// purpose; the web UI on LittleFS is the real one (docs/ui/web/web-recovery.md).
const char kBuiltinPage[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>hassctrl</title>
<style>
:root { color-scheme: light dark; --bg: #f4f6f8; --fg: #1b2530; --card: #ffffff; --muted: #5d6b78; --accent: #ff8000; }
@media (prefers-color-scheme: dark) { :root { --bg: #0b0f14; --fg: #e8eef3; --card: #182029; --muted: #7fa8c9; } }
body { margin: 0; font-family: system-ui, sans-serif; background: var(--bg); color: var(--fg); }
main { max-width: 480px; margin: 0 auto; padding: 16px; }
h1 { font-size: 1.4rem; margin: 8px 0 16px; }
section { background: var(--card); border-radius: 12px; padding: 12px 16px; margin-bottom: 12px; }
h2 { font-size: 1rem; margin: 0 0 8px; }
dl { display: grid; grid-template-columns: max-content 1fr; gap: 4px 12px; margin: 0; }
dt { color: var(--muted); }
dd { margin: 0; overflow-wrap: anywhere; }
label { display: block; margin: 8px 0 4px; color: var(--muted); }
input, select, button { font: inherit; padding: 8px; border-radius: 8px; box-sizing: border-box; }
input[type=text], input[type=password], input[type=file], select { width: 100%; }
progress { width: 100%; margin-top: 12px; }
button { margin-top: 12px; border: 0; background: var(--accent); color: #000; padding: 8px 16px; }
button.plain { background: transparent; color: var(--fg); border: 1px solid var(--muted); }
.check { display: flex; gap: 8px; align-items: center; color: var(--fg); }
.message { color: var(--accent); min-height: 1.2em; }
[hidden] { display: none; }
</style>
</head>
<body>
<main>
<h1 id="name">hassctrl</h1>

<section id="setup-wifi" hidden>
  <h2>1. Wi-Fi</h2>
  <label for="ssid">Network</label>
  <select id="networks"></select>
  <input id="ssid" type="text" placeholder="or type the network name" autocomplete="off">
  <label for="wifi-password">Password</label>
  <input id="wifi-password" type="password" autocomplete="off">
  <button id="scan" class="plain" type="button">Scan</button>
  <button id="add-network" type="button">Save network</button>
  <p class="message" id="wifi-message"></p>
</section>

<section id="setup-login" hidden>
  <h2>2. Web UI login</h2>
  <label for="new-user">Username</label>
  <input id="new-user" type="text" value="admin" autocomplete="username">
  <label for="new-password">Password (8 to 64 characters)</label>
  <input id="new-password" type="password" autocomplete="new-password">
  <button id="create-login" type="button">Create login</button>
  <p class="message" id="setup-message"></p>
</section>

<section id="login" hidden>
  <h2>Log in</h2>
  <label for="user">Username</label>
  <input id="user" type="text" autocomplete="username">
  <label for="password">Password</label>
  <input id="password" type="password" autocomplete="current-password">
  <label class="check"><input id="remember" type="checkbox"> Remember me</label>
  <button id="do-login" type="button">Log in</button>
  <p class="message" id="login-message"></p>
</section>

<div id="status" hidden>
  <section><h2>Wi-Fi</h2><dl id="wifi"></dl></section>
  <section><h2>Hotspot</h2><dl id="hotspot"></dl></section>
  <section><h2>Device</h2><dl id="device"></dl></section>
  <section id="update">
    <h2>Update</h2>
    <dl id="update-info"></dl>
    <label for="fw-file">Firmware image</label>
    <input id="fw-file" type="file" accept=".bin">
    <input id="fw-url" type="text" placeholder="or a URL: http(s)://.../hassctrl.bin" autocomplete="off">
    <button id="fw-go" type="button">Update firmware</button>
    <label for="fs-file">File system image (web UI)</label>
    <input id="fs-file" type="file" accept=".bin">
    <input id="fs-url" type="text" placeholder="or a URL" autocomplete="off">
    <button id="fs-go" type="button">Update file system</button>
    <progress id="progress" max="100" value="0" hidden></progress>
    <p class="message" id="update-message"></p>
    <h2 style="margin-top:16px">File system</h2>
    <p>Save what can still be read before formatting: <a href="/api/fs/files.zip">download the files (.zip)</a></p>
    <button id="format" class="plain" type="button">Format the file system</button>
    <button id="restart" class="plain" type="button">Restart</button>
    <button id="to-firmware" class="plain" type="button" hidden>Start the firmware</button>
  </section>
  <section id="dashboard" hidden>
    <h2>Dashboard</h2>
    <dl id="dashboard-info"></dl>
    <label for="dash-folder">Dashboard folder (hass.yaml and its includes)</label>
    <input id="dash-folder" type="file" webkitdirectory multiple>
    <label for="dash-file">or one YAML file (an export)</label>
    <input id="dash-file" type="file" accept=".yaml,.yml">
    <button id="dash-check" class="plain" type="button">Check</button>
    <button id="dash-import" type="button">Import</button>
    <p class="message" id="dash-message"></p>
    <p><a href="/api/dashboard.yaml">Download the dashboard (.yaml)</a></p>
  </section>
  <section id="account" hidden>
    <h2>Account</h2>
    <button id="logout" class="plain" type="button">Log out</button>
    <button id="logout-all" class="plain" type="button">Log out everywhere</button>
  </section>
</div>
<p>Built-in page. The web UI is not installed yet.</p>
</main>
<script>
const $ = (id) => document.getElementById(id);
let session = {};

async function post(url, data) {
  const response = await fetch(url, { method: "POST", body: new URLSearchParams(data) });
  const body = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(body.error || response.statusText);
  return body;
}

function rows(id, list) {
  const dl = $(id);
  dl.replaceChildren();
  for (const [label, value] of list) {
    const dt = document.createElement("dt"); dt.textContent = label;
    const dd = document.createElement("dd"); dd.textContent = value;
    dl.append(dt, dd);
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
let update = {};
let updating = false;

function updateMessage(text) { $("update-message").textContent = text; }
function progress(done, total) {
  const bar = $("progress");
  bar.hidden = !total;
  if (total) bar.value = Math.round(100 * done / total);
}

async function loadUpdate() {
  try {
    const response = await fetch("/api/update", { cache: "no-store" });
    if (!response.ok) return null;
    update = await response.json();
  } catch (e) { return null; }
  const recovery = update.app === "recovery";
  document.title = recovery ? "hassctrl recovery" : "hassctrl";
  rows("update-info", [["Running", recovery ? "recovery app" : "firmware"],
                       ["Firmware in flash", !update.firmware_present ? "none" : update.firmware_version +
                        (update.firmware_failed ? " (did not start, rolled back)" : "")],
                       ["Firmware update", update.firmware_direct ? "direct" : "through the recovery app"]]);
  $("to-firmware").hidden = !recovery || !update.firmware_present;
  $("dashboard").hidden = recovery;  // the recovery app has no dashboard
  if (!recovery) loadDashboard();
  return update;
}

function dashMessage(text) { $("dash-message").textContent = text; }

async function loadDashboard() {
  try {
    const response = await fetch("/api/dashboard", { cache: "no-store" });
    if (response.status === 404) { rows("dashboard-info", [["Stored", "none"]]); return; }
    if (!response.ok) return;
    const d = await response.json();
    rows("dashboard-info", [["Revision", d.revision], ["Tiles", (d.tiles || []).length]]);
  } catch (e) { /* the device may be restarting */ }
}

// The files of the chosen folder by their path inside it (the folder's own name is cut off),
// or the one chosen file as hass.yaml.
function dashboardFiles() {
  const single = $("dash-file").files[0];
  if (single) return [[single, "hass.yaml"]];
  const list = [];
  for (const f of $("dash-folder").files) {
    const path = (f.webkitRelativePath || f.name).split("/").slice(1).join("/") || f.name;
    if (/\.ya?ml$/i.test(path)) list.push([f, path]);
  }
  return list;
}

async function importDashboard(checkOnly) {
  const files = dashboardFiles();
  if (!files.length) { dashMessage("choose a folder with hass.yaml or one YAML file"); return; }
  if (!checkOnly && !files.some(([, path]) => path === "hass.yaml")) {
    dashMessage("the folder has no hass.yaml at its top"); return;
  }
  const form = new FormData();
  for (const [file, path] of files) form.append("files", file, path);
  dashMessage(checkOnly ? "checking ..." : "importing ...");
  try {
    const response = await fetch("/api/dashboard/import" + (checkOnly ? "?check=1" : ""), { method: "POST", body: form });
    const body = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(body.error || response.statusText);
    dashMessage((checkOnly ? "valid: " : "imported, revision " + body.revision + ": ") +
                body.pages + " pages, " + body.tiles + " tiles" + (body.ignored ? " (ignored: " + body.ignored + ")" : ""));
    if (!checkOnly) loadDashboard();
  } catch (e) { dashMessage(e.message); }
}

// after a restart: waits until the device answers with the expected app
async function waitForApp(app) {
  for (let i = 0; i < 60; i++) {
    await sleep(2000);
    const u = await loadUpdate();
    if (u && u.app === app && u.confirmed !== false) return;
  }
  throw new Error("the device did not come back, reload the page");
}

// XMLHttpRequest, because fetch() reports no upload progress
function upload(url, file) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", url);
    xhr.upload.onprogress = (e) => progress(e.loaded, e.total);
    xhr.onload = () => {
      let body = {};
      try { body = JSON.parse(xhr.responseText); } catch (e) { /* no JSON */ }
      if (xhr.status === 200) resolve(body); else reject(new Error(body.error || xhr.statusText));
    };
    xhr.onerror = () => reject(new Error("the connection broke off"));
    xhr.send(file);
  });
}

// a download by the device: its progress comes from /api/update
async function download(target, url) {
  await post("/api/update/url", { target, url });
  for (;;) {
    await sleep(1000);
    const u = await loadUpdate();
    if (!u) continue;
    progress(u.done, u.total || 0);
    updateMessage("downloading " + Math.round(u.done / 1024) + " KB ...");
    if (u.state === "done") return;
    if (u.state === "failed") throw new Error(u.error);
  }
}

async function runUpdate(target) {
  const file = $(target === "firmware" ? "fw-file" : "fs-file").files[0];
  const url = $(target === "firmware" ? "fw-url" : "fs-url").value.trim();
  if (!file && !url) { updateMessage("choose an image or enter a URL"); return; }
  if (updating) return;
  updating = true;
  try {
    await loadUpdate();
    if (target === "firmware" && !update.firmware_direct) {
      updateMessage("restarting into the recovery app ...");
      await post("/api/restart", { to: "recovery" });
      await waitForApp("recovery");
    }
    if (file) {
      updateMessage("uploading " + Math.round(file.size / 1024) + " KB ...");
      await upload("/api/update/" + target, file);
    } else {
      await download(target, url);
    }
    progress(0, 0);
    if (target === "firmware") {
      updateMessage("written, restarting into the new firmware ...");
      await waitForApp("main");
      updateMessage("running firmware " + update.firmware_version);
    } else {
      updateMessage("file system updated");
    }
  } catch (e) {
    progress(0, 0);
    updateMessage(e.message);
  }
  updating = false;
}

async function loadSession() {
  session = await (await fetch("/api/session", { cache: "no-store" })).json();
  $("setup-wifi").hidden = !session.setup;
  $("setup-login").hidden = !session.setup;
  $("login").hidden = session.setup || session.logged_in;
  $("status").hidden = !(session.setup || session.logged_in);
  $("account").hidden = !session.logged_in;
  if (session.setup) loadNetworks();
  if (session.setup || session.logged_in) loadUpdate();
}

async function loadNetworks() {
  const w = await (await fetch("/api/wifi", { cache: "no-store" })).json();
  const select = $("networks");
  select.replaceChildren(new Option("choose a network", ""));
  for (const n of w.scan) {
    select.append(new Option(n.ssid + "  (" + n.rssi + " dBm" + (n.saved ? ", saved" : "") + ")", n.ssid));
  }
}

async function updateStatus() {
  if ($("status").hidden) return;
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (response.status === 401) { loadSession(); return; }
    const s = await response.json();
    $("name").textContent = s.device.name;
    const w = s.wifi;
    rows("wifi", [["State", w.state], ["Network", w.ssid || "-"], ["Address", w.ip || "-"],
                  ["Signal", w.state === "connected" ? w.rssi + " dBm" : "-"], ["Problem", w.error || "-"]]);
    const h = s.hotspot;
    rows("hotspot", [["State", h.on ? "on" : "off"], ["Name", h.ssid], ["Devices", h.clients],
                     ["Turns off in", h.off_in_s ? Math.floor(h.off_in_s / 60) + " min" : "-"]]);
    const d = s.device;
    rows("device", [["Firmware", d.firmware], ["Uptime", Math.floor(d.uptime_s / 60) + " min"],
                    ["Free memory", d.heap_internal_kb + " KB internal, " + d.heap_psram_kb + " KB PSRAM"],
                    ["File system", s.fs.state + (s.fs.state === "mounted" ? ", " + s.fs.used_kb + " of " + s.fs.total_kb + " KB used" : "")]]);
  } catch (e) { /* the device may be restarting */ }
}

$("networks").onchange = () => { $("ssid").value = $("networks").value; };
$("scan").onclick = async () => {
  $("wifi-message").textContent = "scanning ...";
  try { await post("/api/wifi/scan", {}); } catch (e) { $("wifi-message").textContent = e.message; return; }
  setTimeout(async () => { await loadNetworks(); $("wifi-message").textContent = ""; }, 4000);
};
$("add-network").onclick = async () => {
  try {
    await post("/api/wifi/add", { ssid: $("ssid").value, password: $("wifi-password").value });
    $("wifi-message").textContent = "saved, connecting ...";
  } catch (e) { $("wifi-message").textContent = e.message; }
};
$("create-login").onclick = async () => {
  try {
    await post("/api/setup/user", { username: $("new-user").value, password: $("new-password").value, remember: "1" });
    $("setup-message").textContent = "";
    loadSession();
  } catch (e) { $("setup-message").textContent = e.message; }
};
$("do-login").onclick = async () => {
  try {
    await post("/api/login", { username: $("user").value, password: $("password").value,
                               remember: $("remember").checked ? "1" : "0" });
    $("login-message").textContent = "";
    loadSession();
  } catch (e) { $("login-message").textContent = e.message; }
};
$("fw-go").onclick = () => runUpdate("firmware");
$("fs-go").onclick = () => runUpdate("filesystem");
$("dash-folder").onchange = () => { $("dash-file").value = ""; };
$("dash-file").onchange = () => { $("dash-folder").value = ""; };
$("dash-check").onclick = () => importDashboard(true);
$("dash-import").onclick = () => importDashboard(false);
$("format").onclick = async () => {
  if (!confirm("Format the file system? Every file on it is deleted, also the web UI.")) return;
  try { await post("/api/fs/format", { confirm: "yes" }); updateMessage("file system formatted"); }
  catch (e) { updateMessage(e.message); }
};
$("restart").onclick = async () => {
  try { await post("/api/restart", {}); updateMessage("restarting ..."); } catch (e) { updateMessage(e.message); }
};
$("to-firmware").onclick = async () => {
  try {
    await post("/api/restart", { to: "firmware" });
    updateMessage("starting the firmware ...");
    await waitForApp("main");
    updateMessage("");
  } catch (e) { updateMessage(e.message); }
};
$("logout").onclick = async () => { await post("/api/logout", {}); loadSession(); };
$("logout-all").onclick = async () => { await post("/api/logout-all", {}); loadSession(); };

loadSession().then(updateStatus);
setInterval(updateStatus, 2000);
</script>
</body>
</html>
)HTML";

} // namespace hc::webserver::detail
