// The web UI of hassctrl (docs/ui/web/web-app.md): one page, the views follow the hash (#/wifi). Plain
// JavaScript, no framework; Bootstrap 5.3 only as CSS. tools/build_web.py drops the `// #if CONFIG_*`
// blocks of options the firmware lacks and minifies the file. Everything comes from the device API
// (components/webserver). Texts from outside (network names) are escaped.
"use strict";

const $ = (selector, root = document) => root.querySelector(selector);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const esc = (value) =>
  String(value ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);

class HttpError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

let session = {};

// GET, or POST with a form (an object, or FormData for files); JSON answers. A 401 leads to the login.
async function api(path, form) {
  const options = { cache: "no-store" };
  if (form !== undefined) {
    options.method = "POST";
    options.body = form instanceof FormData ? form : new URLSearchParams(form);
  }
  const response = await fetch(path, options);
  const body = await response.json().catch(() => ({}));
  if (response.status === 401 && path !== "/api/login") {
    session.logged_in = false;
    go("login");
    throw new HttpError(401, "login required");
  }
  if (!response.ok) throw new HttpError(response.status, body.error || response.statusText);
  return body;
}

function go(name) {
  if (location.hash !== "#/" + name) location.hash = "#/" + name;
  else route();
}

function toast(text, kind = "secondary") {
  const t = $("#toast");
  t.className = `app-toast alert alert-${kind} py-2 shadow-sm`;
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => (t.hidden = true), 4000);
}

// runs `action` for a button: disabled while it runs, errors as a toast
async function busy(button, action) {
  button.disabled = true;
  try {
    await action();
  } catch (e) {
    if (e.status !== 401) toast(e.message, "danger");
  } finally {
    button.disabled = false;
  }
}

// The live socket (docs/ui/web/web-app.md, "Live updates"): one WebSocket for the app instead of
// pages that ask every few seconds. A page's live(path, onData) (newPage) follows one GET: the device sends its answer at once and again whenever it changes, `onData`
// gets it. A route change stops it (liveStop), a hidden tab pauses it; a lost socket comes back by
// itself after 1, 2, 4 ... 10 s and asks again.
// The recovery app has no live socket (`"live": false` in /api/status): when one does not open
// and the device says so, live() asks with the GET every 3 s instead (a tick skipped while the
// last waits or the tab is hidden). A device that restarts says nothing: the socket keeps trying.
const liveState = { socket: null, path: "", onData: null, retry: 1000, timer: null, polling: false, poll: null, seen: 0 };

// The device sends at least `{}` every 10 s; a socket silent for 25 s died without a close (the
// device restarted, the laptop slept) and is opened again.
setInterval(() => {
  const socket = liveState.socket;
  if (!socket || socket.readyState !== WebSocket.OPEN || Date.now() - liveState.seen <= 25000) return;
  // its close handshake can wait long on a dead link: let it go and open a new one at once
  socket.onclose = socket.onmessage = null;
  socket.close();
  liveState.socket = null;
  liveConnect();
}, 5000);

function liveSend() {
  const socket = liveState.socket;
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(document.hidden ? "" : liveState.path);
}

function livePoll() {
  clearInterval(liveState.poll);
  let waiting = false;
  liveState.poll = setInterval(() => {
    const { path, onData } = liveState;
    if (!path || waiting || document.hidden) return;
    waiting = true;
    api(path).then((state) => liveState.path === path && onData(state)).catch(() => {}).finally(() => (waiting = false));
  }, 3000);
}

function liveConnect() {
  clearTimeout(liveState.timer);
  if (liveState.socket || !liveState.path || liveState.polling) return;
  const socket = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/api/live`);
  liveState.socket = socket;
  let opened = false;
  socket.onopen = () => {
    opened = true;
    liveState.seen = Date.now();
    liveState.retry = 1000;
    liveSend();
  };
  socket.onmessage = (event) => {
    liveState.seen = Date.now();
    let message;
    try { message = JSON.parse(event.data); } catch (e) { return; }
    // a frame of the page before is still on its way after a route change
    if (message.path !== liveState.path || !liveState.onData) return;
    Promise.resolve().then(() => liveState.onData(message.state)).catch(() => {});
  };
  socket.onclose = () => {
    liveState.socket = null;
    if (!liveState.path) return;
    if (!opened) {
      fetch("/api/status", { cache: "no-store" }).then((r) => r.json()).then((s) => {
        if (s.live !== false || liveState.polling) return;
        liveState.polling = true;
        clearTimeout(liveState.timer);
        livePoll();
      }).catch(() => {});
    }
    liveState.timer = setTimeout(liveConnect, liveState.retry);
    liveState.retry = Math.min(liveState.retry * 2, 10000);
  };
}

function live(path, onData) {
  liveState.path = path;
  liveState.onData = onData;
  if (liveState.socket) liveSend();
  else liveConnect();
}

function liveStop() {
  liveState.path = "";
  liveState.onData = null;
  liveSend();
}

document.addEventListener("visibilitychange", () => {
  if (liveState.socket) liveSend();
  else if (!document.hidden) liveConnect();
});

function render(html) {
  $("#page").innerHTML = html;
  window.scrollTo(0, 0);
}

// Every route() is a page of its own; the view draws and follows through its page's handle:
// `page.render(html)`, `page.live(path, onData)`. Both do nothing once the next route started: a
// route or view left while it still waited (the session, its data) would otherwise draw over the
// next page or take the live socket from it (tools/test_web_ui.py).
let pageEpoch = 0;

function newPage() {
  const epoch = ++pageEpoch;
  const current = () => epoch === pageEpoch;
  return {
    current,
    render: (html) => { if (current()) render(html); },
    live: (path, onData) => { if (current()) live(path, onData); },
  };
}

const duration = (s) =>
  s >= 86400 ? `${Math.floor(s / 86400)} d ${Math.floor((s % 86400) / 3600)} h`
    : s >= 3600 ? `${Math.floor(s / 3600)} h ${Math.floor((s % 3600) / 60)} min`
    : s >= 60 ? `${Math.floor(s / 60)} min`
    : `${s} s`;

function bars(rssi) {
  const n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  return `<span class="bars" title="${rssi} dBm">${[6, 9, 12, 14].map((h, i) =>
    `<i class="${i < n ? "on" : ""}" style="height:${h}px"></i>`).join("")}</span>`;
}

const lock = '<svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor" aria-label="secured"><path d="M8 1a3 3 0 0 0-3 3v3H4a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V8a1 1 0 0 0-1-1h-1V4a3 3 0 0 0-3-3m-2 6V4a2 2 0 1 1 4 0v3z"/></svg>';
const wifiIcon = '<svg width="18" height="18" viewBox="0 0 16 16" fill="currentColor"><path d="M8 12.5a1.25 1.25 0 1 1 0 2.5 1.25 1.25 0 0 1 0-2.5M3.3 9.6a6.6 6.6 0 0 1 9.4 0l-1 1a5.2 5.2 0 0 0-7.4 0zM.7 7a10.3 10.3 0 0 1 14.6 0l-1 1A8.9 8.9 0 0 0 1.7 8z"/></svg>';

// -------------------------------------------------------------------------------------- frame

// Icons of the menu, 24 x 24 strokes in the text's color.
const navIcons = {
  dashboard: '<rect x="3" y="3" width="8" height="8" rx="1"/><rect x="13" y="3" width="8" height="8" rx="1"/><rect x="3" y="13" width="8" height="8" rx="1"/><rect x="13" y="13" width="8" height="8" rx="1"/>',
  ha: '<path d="M3 11l9-8 9 8M5 9.5V21h14V9.5"/><circle cx="12" cy="14" r="2"/><path d="M12 16v5"/>',
  player: '<circle cx="12" cy="12" r="9"/><path d="M10 8.5v7l6-3.5z"/>',
  camera: '<path d="M3 8h3l2-3h8l2 3h3v11H3z"/><circle cx="12" cy="13" r="3.5"/>',
  wifi: '<path d="M2 9a16 16 0 0 1 20 0M5.5 12.5a11 11 0 0 1 13 0M9 16a5 5 0 0 1 6 0"/><circle cx="12" cy="19.5" r="1"/>',
  hotspot: '<circle cx="12" cy="12" r="2"/><path d="M8.5 8.5a5 5 0 0 0 0 7M15.5 8.5a5 5 0 0 1 0 7M5.6 5.6a9 9 0 0 0 0 12.8M18.4 5.6a9 9 0 0 1 0 12.8"/>',
  status: '<path d="M3 12h4l3-7 4 14 3-7h4"/>',
  time: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  logs: '<path d="M6 3h12v18H6zM9 8h6M9 12h6M9 16h4"/>',
  sd: '<path d="M7 3h8l4 4v14H7zM10 3v4M13 3v4"/>',
  system: '<rect x="4" y="4" width="16" height="16" rx="2"/><path d="M9 9h6v6H9z"/>',
  more: '<circle cx="5" cy="12" r="1.5"/><circle cx="12" cy="12" r="1.5"/><circle cx="19" cy="12" r="1.5"/>',
  auto: '<circle cx="12" cy="12" r="9"/><path d="M12 3a9 9 0 0 1 0 18z" fill="currentColor"/>',
  light: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M5 5l1.5 1.5M17.5 17.5L19 19M5 19l1.5-1.5M17.5 6.5L19 5"/>',
  dark: '<path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5z"/>',
  logout: '<path d="M15 4h4v16h-4M10 8l-4 4 4 4M6 12h10"/>',
};
const navIcon = (name) => `<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"
  stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${navIcons[name]}</svg>`;

// The menu in groups (web-design.md section 3): [route, menu text, rail text, the feature it needs].
// A feature is a key of /api/status: "hass" (the main firmware's pages, not in the recovery app), "camera", "player", "sd".
// From 800 px a rail of icons with the rail text, the groups apart by a line; on a phone the tab
// bar below (tabPages and More) and More's sheet with every page. An entry whose feature the device
// lacks is hidden, a group without a shown entry too. The pages of a board option sit between
// `// #if CONFIG_*` markers (tools/build_web.py).
const menuGroups = [
  ["Home", [
    ["dashboard", "Dashboard", "Dashboard", "hass"], ["ha", "Home Assistant", "HA", "hass"],
    // #if CONFIG_HC_UI_PLAYER
    ["player", "Player", "Player", "player"],
    // #endif
    // #if CONFIG_HC_BOARD_HAS_CAMERA
    ["camera", "Camera", "Camera", "camera"],
    // #endif
  ]],
  ["Network", [["wifi", "Wi-Fi", "Wi-Fi"], ["hotspot", "Hotspot", "Hotspot"]]],
  ["Device", [
    ["status", "Status", "Status"], ["time", "Time", "Time", "hass"], ["logs", "Logs", "Logs"],
    // #if CONFIG_HC_SDCARD
    ["sd", "SD card", "SD card", "sd"],
    // #endif
    ["system", "System", "System"],
  ]],
];
const tabPages = ["status", "dashboard", "ha", "wifi"];

// a menu entry: the icon, the rail's short text, the sheet's full one; a dot for a state (Wi-Fi)
const navItem = ([page, title, short, feature]) => `<a class="app-nav-item" href="#/${page}" data-page="${page}"
  ${feature ? `data-feature="${feature}" hidden` : ""} title="${esc(title)}">${navIcon(page)}<span class="app-nav-dot" hidden></span>
  <span class="app-nav-short">${esc(short)}</span><span class="app-nav-long">${esc(title)}</span></a>`;

// shows the entries of the features the device has (a key of /api/status that is there and not false)
function showFeatures(s) {
  for (const link of document.querySelectorAll("[data-feature]")) link.hidden = !s[link.dataset.feature];
  for (const group of document.querySelectorAll("#menu-pages [data-group]")) {
    group.hidden = ![...group.querySelectorAll("a")].some((link) => !link.hidden);
  }
}

// the sheet of every page on a phone (More); from 800 px the rail is always there. More is the
// active tab while the sheet is open and on a page that has no tab of its own.
function openSheet(open) {
  $("#menu").hidden = !open;
  const page = location.hash.replace(/^#\/?/, "") || "status";
  $("#tab-more").classList.toggle("active", open || !tabPages.includes(page));
}

function frame(loggedIn, step) {
  $("#icon-wifi").hidden = !loggedIn;
  $("#icon-ha").hidden = !loggedIn;
  $("#tabbar").hidden = !loggedIn;
  $("#step").hidden = !step;
  $("#step").textContent = step || "";
  if (!loggedIn) openSheet(false);
  $("#menu").classList.toggle("d-none", !loggedIn);
  document.body.classList.toggle("app-logged-in", loggedIn);
}

async function updateHeader() {
  return showHeader(await api("/api/status"));
}

// the device name, the menu entries of the board's options and the state icons, from /api/status
function showHeader(s) {
  $("#device-name").textContent = s.device.name;
  showFeatures(s);
  document.title = s.device.name;
  const icon = $("#icon-wifi");
  icon.innerHTML = wifiIcon;
  icon.className = "state-icon " + (s.wifi.state === "connected" ? "ok" : s.wifi.error ? "error" : "warn");
  icon.title = s.wifi.state === "connected" ? `Wi-Fi: ${s.wifi.ssid}, ${s.wifi.rssi} dBm` : `Wi-Fi: ${s.wifi.error || s.wifi.state}`;
  // the same state as a dot on the Wi-Fi entries of the rail, the sheet and the tab bar
  for (const dot of document.querySelectorAll('[data-page="wifi"] .app-nav-dot')) {
    dot.hidden = false;
    dot.className = `app-nav-dot ${icon.className.split(" ")[1]}`;
  }
  const ha = $("#icon-ha");
  if (s.hass && s.hass.state !== "not_set_up") {
    const [text] = hassStates[s.hass.state] || [s.hass.state];
    ha.className = "state-icon small fw-bold " + (s.hass.state === "connected" ? "ok" : s.hass.state === "token_rejected" ? "error" : "warn");
    ha.title = `Home Assistant: ${s.hass.error || text}`;
  } else {
    ha.className = "state-icon small fw-bold";
    ha.title = "Home Assistant: not set up";
  }
  return s;
}

// ------------------------------------------------------------------------------------- router

// the hash goes back to the dashboard after a refused leave: no new route for that
let stayOnDashboard = false;

async function route() {
  const name = location.hash.replace(/^#\/?/, "") || "status";
  if (stayOnDashboard) {
    stayOnDashboard = false;
    return;
  }
  if (window.hcEditor && name !== "dashboard") {
    if (window.hcEditor.dirty() && !confirm("Leave the dashboard editor? Your changes are not saved.")) {
      stayOnDashboard = true;
      location.hash = "#/dashboard";
      return;
    }
    window.hcEditor.unmount();
  }
  const page = newPage();
  liveStop();
  // the dashboard editor uses the whole width, every other page one column
  $("#page").classList.toggle("app-wide", name === "dashboard");
  $("#page").style.maxWidth = "";
  try {
    session = await api("/api/session");
  } catch (e) {
    page.render(`<div class="alert alert-warning">The device does not answer. ${esc(e.message)}</div>`);
    return;
  }
  if (!page.current()) return;
  if (session.setup) return setupRoute(name, page);
  if (!session.logged_in) return loginPage(page);
  if (name === "setup-done") return setupDone(page);
  frame(true);
  for (const link of document.querySelectorAll("#menu-pages a, #tabbar a")) {
    link.classList.toggle("active", link.dataset.page === name);
  }
  openSheet(false);
  updateHeader().catch(() => {});
  await (views[name] || views.status)(page);
}

// -------------------------------------------------------------------------------------- setup

function setupRoute(name, page) {
  frame(false, name === "setup-login" ? "2 / 3" : "1 / 3");
  return name === "setup-login" ? setupLogin(page) : setupWifi(page);
}

function networkPicker(scan, saved) {
  const items = scan.map((n, i) => `
    <button type="button" class="list-group-item list-group-item-action d-flex align-items-center gap-2" data-index="${i}">
      <span class="flex-grow-1 text-truncate">${esc(n.ssid)}</span>
      ${n.saved ? '<span class="badge text-bg-secondary">saved</span>' : ""}
      ${n.secure ? lock : ""} ${bars(n.rssi)}
    </button>`).join("");
  return `
    <div class="list-group mb-3" id="scan-list">${items || '<div class="list-group-item text-body-secondary">Searching ...</div>'}
      <button type="button" class="list-group-item list-group-item-action" data-other="1">Other network ...</button>
    </div>
    <div id="ssid-row" class="mb-3" hidden>
      <label class="form-label" for="ssid">Network name</label>
      <input id="ssid" class="form-control" autocomplete="off" maxlength="32">
    </div>
    <label class="form-label" for="wifi-password">Password</label>
    <div class="input-group mb-3">
      <input id="wifi-password" type="password" class="form-control" autocomplete="off" maxlength="64">
      <button id="show-password" class="btn btn-outline-secondary" type="button">Show</button>
    </div>`;
}

// wires a picker made by networkPicker(); returns a function that gives the chosen name
function wirePicker(scan) {
  let chosen = "";
  $("#scan-list").addEventListener("click", (event) => {
    const item = event.target.closest("button");
    if (!item) return;
    for (const b of $("#scan-list").querySelectorAll("button")) b.classList.remove("active");
    item.classList.add("active");
    const other = item.dataset.other === "1";
    $("#ssid-row").hidden = !other;
    chosen = other ? "" : scan[Number(item.dataset.index)].ssid;
    $("#wifi-password").placeholder = !other && scan[Number(item.dataset.index)].saved
      ? "empty keeps the saved password" : "";
    (other ? $("#ssid") : $("#wifi-password")).focus();
  });
  $("#show-password").onclick = () => {
    const field = $("#wifi-password");
    field.type = field.type === "password" ? "text" : "password";
    $("#show-password").textContent = field.type === "password" ? "Show" : "Hide";
  };
  return () => ($("#ssid-row").hidden ? chosen : $("#ssid").value.trim());
}

async function scanNetworks() {
  await api("/api/wifi/scan", {}).catch(() => {});
  await sleep(4000);
  return api("/api/wifi");
}

async function setupWifi(page) {
  page.render(`<h1>Choose your Wi-Fi</h1><div class="card"><div class="card-body" id="setup-wifi">
    <p class="text-body-secondary">Searching for networks ...</p></div></div>`);
  let w = await api("/api/wifi");
  if (w.scan.length === 0) w = await scanNetworks();
  const draw = () => {
    $("#setup-wifi").innerHTML = `
      ${w.saved.length ? `<p>Saved: ${w.saved.map((n) => esc(n.ssid)).join(", ")}. You can add another one or go on.</p>` : ""}
      <div class="d-flex justify-content-end mb-2"><button id="rescan" class="btn btn-sm btn-outline-secondary" type="button">Rescan</button></div>
      ${networkPicker(w.scan)}
      <div class="d-flex gap-2 justify-content-end">
        ${w.saved.length ? '<button id="next" class="btn btn-outline-secondary" type="button">Next</button>' : ""}
        <button id="connect" class="btn btn-primary" type="button">Connect</button>
      </div>`;
    const chosen = wirePicker(w.scan);
    $("#rescan").onclick = (e) => busy(e.target, async () => { w = await scanNetworks(); draw(); });
    if ($("#next")) $("#next").onclick = () => go("setup-login");
    $("#connect").onclick = (e) => busy(e.target, async () => {
      const ssid = chosen();
      if (!ssid) throw new Error("choose a network");
      await api("/api/wifi/add", { ssid, password: $("#wifi-password").value });
      toast(`Saved ${ssid}, connecting ...`, "success");
      go("setup-login");
    });
  };
  draw();
}

function setupLogin(page) {
  page.render(`<h1>Web UI login</h1><div class="card"><div class="card-body">
    <p class="text-body-secondary">The login protects these pages. It is kept on the device.</p>
    <label class="form-label" for="new-user">Username</label>
    <input id="new-user" class="form-control mb-3" value="admin" autocomplete="username" maxlength="32">
    <label class="form-label" for="new-password">Password (8 to 64 characters)</label>
    <input id="new-password" type="password" class="form-control mb-3" autocomplete="new-password" maxlength="64">
    <label class="form-label" for="new-password2">Password again</label>
    <input id="new-password2" type="password" class="form-control mb-3" autocomplete="new-password" maxlength="64">
    <div class="d-flex gap-2 justify-content-end">
      <button class="btn btn-outline-secondary" type="button" onclick="go('setup')">Back</button>
      <button id="create" class="btn btn-primary" type="button">Create login</button>
    </div></div></div>`);
  $("#create").onclick = (e) => busy(e.target, async () => {
    if ($("#new-password").value !== $("#new-password2").value) throw new Error("the passwords differ");
    await api("/api/setup/user", { username: $("#new-user").value, password: $("#new-password").value, remember: "1" });
    go("setup-done");
  });
}

async function setupDone(page) {
  frame(false, "3 / 3");
  page.render(`<h1>Done</h1><div class="card"><div class="card-body" id="done">Checking the Wi-Fi ...</div></div>`);
  const draw = (s) => {
    const w = s.wifi;
    const box = $("#done");
    if (!box) return;
    const keep = $("#keep-hotspot")?.checked ?? s.hotspot.on;
    box.innerHTML = (w.state === "connected"
      ? `<p>Connected to <b>${esc(w.ssid)}</b>.</p>
         <p>Join ${esc(w.ssid)} with this phone and open<br>
         <a href="http://${esc(s.device.name)}.local/">http://${esc(s.device.name)}.local</a><br>
         <a href="http://${esc(w.ip)}/">http://${esc(w.ip)}</a></p>`
      : `<p>${w.error ? `<span class="text-danger">${esc(w.error)}</span>` : `Wi-Fi: ${esc(w.state)} ${esc(w.ssid)} ...`}</p>
         <p><a href="#/setup">Change the Wi-Fi</a></p>`) +
      `<div class="form-check form-switch mb-3">
         <input class="form-check-input" type="checkbox" role="switch" id="keep-hotspot" ${keep ? "checked" : ""}>
         <label class="form-check-label" for="keep-hotspot">Keep the hotspot on</label>
       </div>
       <div class="d-flex justify-content-end"><button id="finish" class="btn btn-primary" type="button">Done</button></div>`;
    $("#finish").onclick = (e) => busy(e.target, async () => {
      if ($("#keep-hotspot").checked) await api("/api/hotspot", { on: "1" });
      go("status");
    });
  };
  draw(await api("/api/status"));
  page.live("/api/status", draw);
}

// -------------------------------------------------------------------------------------- login

function loginPage(page) {
  frame(false);
  page.render(`<h1>Log in</h1><div class="card"><div class="card-body"><form id="login-form">
    <label class="form-label" for="user">Username</label>
    <input id="user" class="form-control mb-3" autocomplete="username" required>
    <label class="form-label" for="password">Password</label>
    <input id="password" type="password" class="form-control mb-3" autocomplete="current-password" required>
    <div class="form-check mb-3"><input class="form-check-input" type="checkbox" id="remember">
      <label class="form-check-label" for="remember">Remember me</label></div>
    <div class="d-flex justify-content-end"><button class="btn btn-primary" type="submit">Log in</button></div>
    </form></div></div>
    <p class="small text-body-secondary">Problems with these pages? <a href="/recovery">Recovery page</a></p>`);
  $("#login-form").onsubmit = (event) => {
    event.preventDefault();
    busy($("#login-form button"), async () => {
      await api("/api/login", { username: $("#user").value, password: $("#password").value,
                                remember: $("#remember").checked ? "1" : "0" });
      go(location.hash.replace(/^#\/?/, "") === "login" ? "status" : location.hash.replace(/^#\/?/, ""));
    });
  };
  $("#user").focus();
}

// -------------------------------------------------------------------------------------- views

const facts = (rows) => `<dl class="facts">${rows.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${v}</dd>`).join("")}</dl>`;
const card = (title, body) => `<div class="card"><div class="card-body"><h2 class="h6 card-title">${esc(title)}</h2>${body}</div></div>`;

// The editor (web/dashboard.js, docs/ui/web/web-dashboard-editor.md) is loaded on the first visit of the page.
function loadScript(src) {
  return new Promise((resolve, reject) => {
    const script = document.createElement("script");
    script.src = src;
    script.onload = resolve;
    script.onerror = () => reject(new Error(`${src} could not be loaded`));
    document.head.appendChild(script);
  });
}

async function dashboardEditor(box) {
  if (!window.hcEditor) await loadScript("dashboard.js");
  await window.hcEditor.mount(box, { toast, login: () => go("login") });
}

// The YAML import (a folder with includes, or one file) and the export.
async function dashboardFiles(box) {
  box.innerHTML = `<div class="app-column"><div id="dash-stored">Loading ...</div>` +
    card("Import", `
      <label class="form-label" for="dash-folder">Dashboard folder (<code>hass.yaml</code> and its includes)</label>
      <input id="dash-folder" type="file" class="form-control mb-2" webkitdirectory multiple>
      <label class="form-label" for="dash-file">or one YAML file (an export)</label>
      <input id="dash-file" type="file" class="form-control mb-3" accept=".yaml,.yml">
      <div class="d-flex gap-2 justify-content-end">
        <button id="dash-check" class="btn btn-outline-secondary" type="button">Check</button>
        <button id="dash-import" class="btn btn-primary" type="button">Import</button>
      </div>
      <p class="mt-2 mb-0" id="dash-message"></p>`) +
    `<p class="small text-body-secondary">The backup of the settings and the dashboard in one file is on the System page.</p></div>`;
  const drawStored = async () => {
    let rows;
    try {
      const d = await api("/api/dashboard");
      // every level, as the device counts an import: the tiles with areas and spacers, the main
      // page and one page per area
      let pages = 1, tiles = 0;
      (function count(list) { for (const t of list || []) { tiles++; if (t.type === "area") { pages++; count(t.tiles); } } })(d.tiles);
      rows = facts([["Revision", d.revision], ["Pages", pages], ["Tiles", tiles]]) +
        '<div class="d-flex justify-content-end"><a class="btn btn-outline-secondary" href="/api/dashboard.yaml">Download (.yaml)</a></div>';
    } catch (e) {
      if (e.status !== 404) throw e;
      rows = '<p class="text-body-secondary mb-0">No dashboard stored. Import one below.</p>';
    }
    $("#dash-stored").innerHTML = card("Stored", rows);
  };
  // the files of the folder by their path inside it (the folder's own name cut off), or the
  // one file as hass.yaml
  const chosen = () => {
    const single = $("#dash-file").files[0];
    if (single) return [[single, "hass.yaml"]];
    return [...$("#dash-folder").files]
      .map((f) => [f, (f.webkitRelativePath || f.name).split("/").slice(1).join("/") || f.name])
      .filter(([, path]) => /\.ya?ml$/i.test(path));
  };
  const run = (button, checkOnly) => busy(button, async () => {
    const files = chosen();
    if (!files.length) throw new Error("choose a folder with hass.yaml or one YAML file");
    if (!files.some(([, path]) => path === "hass.yaml")) throw new Error("the folder has no hass.yaml at its top");
    const form = new FormData();
    for (const [file, path] of files) form.append("files", file, path);
    $("#dash-message").textContent = checkOnly ? "checking ..." : "importing ...";
    try {
      const r = await api("/api/dashboard/import" + (checkOnly ? "?check=1" : ""), form);
      $("#dash-message").textContent = (checkOnly ? "valid: " : `imported as revision ${r.revision}: `) +
        `${r.pages} pages, ${r.tiles} tiles` + (r.settings ? `, settings: ${r.settings}` : "") +
        (r.notes ? `. ${r.notes}` : "") + (r.ignored ? ` (ignored: ${r.ignored})` : "");
    } catch (e) {
      $("#dash-message").textContent = "";
      throw e;
    }
    if (!checkOnly) {
      toast("dashboard imported", "success");
      await drawStored();
    }
  });
  $("#dash-folder").onchange = () => { $("#dash-file").value = ""; };
  $("#dash-file").onchange = () => { $("#dash-folder").value = ""; };
  $("#dash-check").onclick = (e) => run(e.target, true);
  $("#dash-import").onclick = (e) => run(e.target, false);
  await drawStored();
}

// the states of /api/hass in words, with a Bootstrap color
const hassStates = {
  not_set_up: ["not set up", "secondary"],
  waiting_for_network: ["waiting for Wi-Fi", "secondary"],
  connecting: ["connecting", "info"],
  connected: ["connected", "success"],
  token_rejected: ["token rejected", "danger"],
  disconnected: ["disconnected", "warning"],
};
function hassFacts(h) {
  const [text, kind] = hassStates[h.state] || [h.state, "secondary"];
  const retry = h.state === "disconnected" && h.retry_in_s ? ` <span class="text-body-secondary small">next try in ${h.retry_in_s} s</span>` : "";
  const rows = [["State", `<span class="badge text-bg-${kind}">${esc(text)}</span>${retry}`]];
  if (h.error) rows.push(["Problem", `<span class="text-danger">${esc(h.error)}</span>`]);
  rows.push(["Address", esc(h.url || "-")], ["Version", esc(h.version || "-")]);
  if (h.state === "connected" && h.connected_s !== undefined) rows.push(["Connected for", duration(h.connected_s)]);
  if (h.certificate) rows.push(["Certificate", "self-signed, trusted on this device"]);
  // a board with a motion sensor: its toggle helper (docs/home-assistant/motion-sensor.md)
  if (h.motion_helper) {
    const entity = `<code>${esc(h.motion_entity)}</code>`;
    rows.push(["Motion helper", h.motion_helper === "found" ? entity
      : h.motion_helper === "missing" ? `${entity} <span class="text-danger">missing: create this toggle helper in Home Assistant</span>`
      : `${entity} <span class="text-body-secondary small">not checked yet</span>`]);
  }
  return facts(rows);
}
// a SHA-256 the way browsers show it: AB:CD:...
function fingerprint(hex) {
  return hex.toUpperCase().match(/../g).join(":");
}

const views = {
  async status(page) {
    page.render(`<h1>Status</h1><div id="status-cards">Loading ...</div>`);
    const draw = (s) => {
      showHeader(s);
      const w = s.wifi, h = s.hotspot, d = s.device;
      const box = $("#status-cards");
      if (!box) return;
      box.innerHTML =
        card("Wi-Fi", facts([
          ["State", esc(w.state) + (w.error ? ` <span class="text-danger">${esc(w.error)}</span>` : "")],
          ["Network", esc(w.ssid || "-")], ["Address", esc(w.ip || "-")],
          ["Signal", w.state === "connected" ? `${bars(w.rssi)} ${w.rssi} dBm` : "-"],
        ])) +
        card("Hotspot", facts([
          ["State", h.on ? "on" : "off"], ["Name", esc(h.ssid)], ["Devices", h.clients],
          ["Turns off in", h.off_in_s ? duration(h.off_in_s) : "-"],
        ])) +
        card("Home Assistant", s.hass ? hassFacts(s.hass) : '<p class="text-body-secondary mb-0">Not in the recovery app.</p>') +
        card("Device", facts([
          ["Firmware", esc(d.firmware)], ["Uptime", duration(d.uptime_s)],
          ["Free memory", `${d.heap_internal_kb} KB internal, ${d.heap_psram_kb} KB PSRAM`],
          ["File system", esc(s.fs.state) + (s.fs.state === "mounted" ? `, ${s.fs.used_kb} of ${s.fs.total_kb} KB used` : "")],
          // #if CONFIG_HC_SDCARD
          ...(s.sd ? [["SD card", esc(sdSpace(s.sd))]] : []),
          // #endif
        ]));
    };
    draw(await api("/api/status"));
    page.live("/api/status", draw);
  },

  async wifi(page) {
    page.render(`<h1>Wi-Fi</h1><div id="wifi-saved">Loading ...</div>
      <div class="card"><div class="card-body"><h2 class="h6 card-title d-flex">Add a network
        <button id="rescan" class="btn btn-sm btn-outline-secondary ms-auto" type="button">Scan</button></h2>
        <div id="wifi-add"></div></div></div>`);
    let w = await api("/api/wifi");
    // a saved network's password is being typed in its row
    let editing = false;
    const drawSaved = () => {
      const rows = w.saved.map((n, i) => `
        <li class="list-group-item d-flex align-items-center gap-2">
          <span class="text-body-secondary">${i + 1}</span>
          <span class="flex-grow-1 text-truncate">${esc(n.ssid)}</span>
          ${n.password_needed ? `<span class="badge text-bg-warning" title="From a backup without its password; not tried until it has one">Password needed</span>
            <button class="btn btn-sm btn-outline-primary" data-password="${i}">Enter</button>` : ""}
          <button class="btn btn-sm btn-outline-secondary" data-move="-1" data-index="${i}" ${i === 0 ? "disabled" : ""} aria-label="up">&uarr;</button>
          <button class="btn btn-sm btn-outline-secondary" data-move="1" data-index="${i}" ${i === w.saved.length - 1 ? "disabled" : ""} aria-label="down">&darr;</button>
          <button class="btn btn-sm btn-outline-danger" data-forget="${i}">Forget</button>
        </li>`).join("");
      $("#wifi-saved").innerHTML = card(`Saved networks (${w.saved.length} of 5)`, `
        <ul class="list-group mb-3">${rows || '<li class="list-group-item text-body-secondary">none</li>'}</ul>
        <div class="d-flex gap-2 align-items-center">
          <label class="text-body-secondary small" for="policy">Use</label>
          <select id="policy" class="form-select form-select-sm w-auto">
            <option value="order" ${w.policy === "order" ? "selected" : ""}>the first one in range</option>
            <option value="strongest" ${w.policy === "strongest" ? "selected" : ""}>the strongest one</option>
          </select>
          <button id="reconnect" class="btn btn-sm btn-outline-secondary ms-auto" type="button">Reconnect</button>
        </div>`);
      $("#wifi-saved").onclick = (event) => {
        const b = event.target.closest("button[data-index],button[data-forget],button[data-password]");
        if (!b) return;
        if (b.dataset.password !== undefined) {
          // the password in place of the badge, saved like a new network (it connects at once)
          const ssid = w.saved[Number(b.dataset.password)].ssid;
          const item = b.closest("li");
          item.innerHTML = `<span class="text-truncate">${esc(ssid)}</span>
            <input type="password" class="form-control form-control-sm flex-grow-1" placeholder="password" autocomplete="off">
            <button class="btn btn-sm btn-primary" type="button">Save</button>
            <button class="btn btn-sm btn-outline-secondary" type="button">Cancel</button>`;
          const [input, save, cancel] = item.querySelectorAll("input,button");
          editing = true;
          input.focus();
          cancel.onclick = (e) => { e.stopPropagation(); editing = false; drawSaved(); };
          save.onclick = (e) => {
            e.stopPropagation();
            busy(save, async () => {
              await api("/api/wifi/add", { ssid, password: input.value });
              toast(`Saved ${ssid}`, "success");
              editing = false;
              drawSaved(); // a push that came meanwhile was not drawn
            });
          };
          input.onkeydown = (e) => { if (e.key === "Enter") save.click(); };
          return;
        }
        busy(b, async () => {
          if (b.dataset.forget !== undefined) {
            const ssid = w.saved[Number(b.dataset.forget)].ssid;
            if (!confirm(`Forget ${ssid}? The device no longer connects to it.`)) return;
            await api("/api/wifi/remove", { index: b.dataset.forget });
          } else {
            await api("/api/wifi/move", { index: b.dataset.index, delta: b.dataset.move });
          }
          // the list follows with the next push
        });
      };
      $("#policy").onchange = (event) => api("/api/wifi/policy", { policy: event.target.value })
        .then(() => toast("saved", "success"), (e) => toast(e.message, "danger"));
      $("#reconnect").onclick = (e) => busy(e.target, async () => {
        await api("/api/wifi/reconnect", {});
        toast("reconnecting ...");
      });
    };
    const drawAdd = () => {
      $("#wifi-add").innerHTML = networkPicker(w.scan) +
        '<div class="d-flex justify-content-end"><button id="save-network" class="btn btn-primary" type="button">Save network</button></div>';
      const chosen = wirePicker(w.scan);
      $("#save-network").onclick = (e) => busy(e.target, async () => {
        const ssid = chosen();
        if (!ssid) throw new Error("choose a network");
        await api("/api/wifi/add", { ssid, password: $("#wifi-password").value });
        toast(`Saved ${ssid}`, "success");
        // done with the picker: the next push may draw it with the network's "saved" tag
        $("#wifi-password").value = "";
        document.activeElement.blur();
      });
    };
    drawSaved();
    drawAdd();
    $("#rescan").onclick = (e) => busy(e.target, async () => { w = await scanNetworks(); drawAdd(); });
    // A push draws what changed. Not the list while a password is typed into it, not the picker
    // while it has the focus or a password: a choice half made stays.
    let shown = JSON.stringify(w);
    page.live("/api/wifi", (data) => {
      const before = JSON.parse(shown);
      shown = JSON.stringify(data);
      w = data;
      const savedChanged = JSON.stringify([before.saved, before.policy]) !== JSON.stringify([w.saved, w.policy]);
      if (savedChanged && !editing) drawSaved();
      const add = $("#wifi-add");
      const busyPicking = add.contains(document.activeElement) || ($("#wifi-password")?.value ?? "") !== "";
      if ((savedChanged || JSON.stringify(before.scan) !== JSON.stringify(w.scan)) && !busyPicking) drawAdd();
      else if (busyPicking) pickerBehind = true;
    });
    // what was not drawn while picking, once the picker is left without a password typed
    let pickerBehind = false;
    $("#wifi-add").addEventListener("focusout", (event) => {
      if (!pickerBehind || $("#wifi-add").contains(event.relatedTarget) || $("#wifi-password")?.value) return;
      pickerBehind = false;
      drawAdd();
    });
  },

  async hotspot(page) {
    page.render(`<h1>Hotspot</h1><div id="hotspot">Loading ...</div>`);
    const draw = (h) => {
      const box = $("#hotspot");
      if (!box || box.contains(document.activeElement)) return;
      box.innerHTML = card("Hotspot", `
        <div class="form-check form-switch mb-3">
          <input class="form-check-input" type="checkbox" role="switch" id="hotspot-on" ${h.kept_on ? "checked" : ""}>
          <label class="form-check-label" for="hotspot-on">Keep the hotspot on</label>
        </div>
        ${facts([
          ["State", h.on ? "on" + (h.off_in_s ? `, turns off in ${duration(h.off_in_s)}` : "") : "off"],
          ["Name", esc(h.ssid)], ["Password", `<code>${esc(h.password)}</code>`], ["Devices", h.clients],
        ])}
        <div class="d-flex justify-content-end mt-3"><button id="new-password" class="btn btn-outline-secondary" type="button">New password</button></div>`) +
        '<p class="small text-body-secondary">The hotspot is on by itself while no Wi-Fi is set up and for 15 minutes after the setup.</p>';
      // the new state follows with the next push (a focused switch is not drawn over: blur it)
      $("#hotspot-on").onchange = (event) => api("/api/hotspot", { on: event.target.checked ? "1" : "0" })
        .then(() => event.target.blur(), (e) => toast(e.message, "danger"));
      $("#new-password").onclick = (e) => busy(e.target, async () => {
        if (!confirm("New hotspot password? Devices on the hotspot have to join again.")) return;
        await api("/api/hotspot/password", {});
        e.target.blur();
      });
    };
    // a push that came while a control had the focus is drawn when the focus leaves
    let latest = await api("/api/hotspot");
    draw(latest);
    page.live("/api/hotspot", (h) => draw((latest = h)));
    $("#hotspot").addEventListener("focusout", (event) => {
      if (!$("#hotspot").contains(event.relatedTarget)) setTimeout(() => draw(latest));
    });
  },

  // #if CONFIG_HC_BOARD_HAS_CAMERA
  // The camera's web stream (docs/ui/web/web-app.md section 4). The picture itself (exposure, mirror, flip,
  // mains frequency) is set on the device, next to its preview.
  async camera(page) {
    page.render(`<h1>Camera</h1><div id="camera">Loading ...</div>`);
    let c;
    try {
      c = await api("/api/camera");
    } catch (e) {
      if (e.status !== 404) throw e;
      page.render(`<h1>Camera</h1>${card("Camera", '<p class="text-body-secondary mb-0">This device has no camera.</p>')}`);
      return;
    }
    const base = `${location.origin}/api/camera`;
    const draw = () => {
      $("#camera").innerHTML = card("Stream", `
        <form id="camera-form" autocomplete="off">
          <div class="form-check form-switch mb-3">
            <input class="form-check-input" type="checkbox" role="switch" id="camera-on" ${c.enabled ? "checked" : ""}>
            <label class="form-check-label" for="camera-on">Stream on</label>
          </div>
          <div class="row g-3 mb-3">
            <div class="col-sm-6">
              <label class="form-label" for="camera-size">Size</label>
              <select id="camera-size" class="form-select">
                <option value="full" ${c.size === "full" ? "selected" : ""}>${c.width} x ${c.height}</option>
                <option value="half" ${c.size === "half" ? "selected" : ""}>${c.width / 2} x ${c.height / 2}</option>
              </select>
            </div>
            <div class="col-sm-6">
              <label class="form-label" for="camera-fps">Frames per second (at most)</label>
              <select id="camera-fps" class="form-select">
                ${[5, 10, 15, 30].map((f) => `<option ${c.fps === f ? "selected" : ""}>${f}</option>`).join("")}
              </select>
            </div>
          </div>
          <label class="form-label" for="camera-quality">Quality <span id="camera-quality-value" class="text-body-secondary">${c.quality}</span></label>
          <input id="camera-quality" type="range" class="form-range mb-3" min="30" max="95" value="${c.quality}">
          <div class="row g-3">
            <div class="col-sm-6">
              <label class="form-label" for="camera-user">Username</label>
              <input id="camera-user" class="form-control" value="${esc(c.username)}" maxlength="32" spellcheck="false" autocapitalize="off">
            </div>
            <div class="col-sm-6">
              <label class="form-label" for="camera-password">Password</label>
              <input id="camera-password" class="form-control" type="password" maxlength="32"
                placeholder="${c.password_set ? "stored - leave empty to keep it" : "not set"}">
            </div>
          </div>
          <div class="form-text mb-3">The stream's own login, not the one of this web UI. The stream is only on with both;
            another username needs its password again, an empty one removes the login. The device never shows the password.</div>
          <div class="d-flex justify-content-end"><button class="btn btn-primary" type="submit">Save</button></div>
        </form>`) +
        card("Addresses", `
          <p class="mb-1"><code>${esc(base)}.mjpg</code> plays the stream (MJPEG),</p>
          <p class="mb-3"><code>${esc(base)}.jpg</code> is one picture.</p>
          <p class="small text-body-secondary mb-0">Both ask for the login (HTTP Basic auth): a browser asks for it, a player
            or Home Assistant's MJPEG IP camera takes it with the address. Plain HTTP: the login crosses the network
            readable. A smaller size, fewer frames or a lower quality need less of the Wi-Fi.</p>`) +
        '<p class="small text-body-secondary">The picture (exposure, mirror, flip, mains frequency) is set on the device, next to its preview: Settings, Camera.</p>';
      $("#camera-quality").oninput = (e) => ($("#camera-quality-value").textContent = e.target.value);
      $("#camera-form").onsubmit = (e) => {
        e.preventDefault();
        busy(e.submitter || $("#camera-form button"), async () => {
          c = await api("/api/camera", {
            enabled: $("#camera-on").checked ? "1" : "0",
            size: $("#camera-size").value,
            fps: $("#camera-fps").value,
            quality: $("#camera-quality").value,
            username: $("#camera-user").value.trim(),
            password: $("#camera-password").value,
          });
          toast("saved", "success");
          draw();
        });
      };
    };
    draw();
  },
  // #endif

  // #if CONFIG_HC_UI_PLAYER
  // The media player of the device, as its Player page on the screen shows it (docs/ui/web/web-app.md
  // section 4): what plays, the buttons, the volume and what comes next. The music starts on the device.
  async player(page) {
    page.render(`<h1>Player</h1><div id="player">Loading ...</div>`);
    let p;
    try {
      p = await api("/api/player");
    } catch (e) {
      if (e.status !== 404) throw e;
      page.render(`<h1>Player</h1>${card("Player", '<p class="text-body-secondary mb-0">Not in the recovery app. Start the firmware (System).</p>')}`);
      return;
    }
    const svg = (path) => `<svg width="20" height="20" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><path d="${path}"/></svg>`;
    const icons = {
      play: svg("M4 2.5v11l9.5-5.5z"),
      pause: svg("M3.5 2h3v12h-3zm6 0h3v12h-3z"),
      previous: svg("M2.5 2h2v12h-2zm11 0v12L5 8z"),
      next: svg("M11.5 2h2v12h-2zM2.5 2v12L11 8z"),
      stop: svg("M3 3h10v10H3z"),
    };
    const clock = (ms) => `${Math.floor(ms / 60000)}:${String(Math.floor(ms / 1000) % 60).padStart(2, "0")}`;
    const states = { paused: "Paused", buffering: "Loading ...", interrupted: "Interrupted by a sound", failed: "Could not play" };
    $("#player").innerHTML = card("Now playing", `
        <div class="player-head mb-3">
          <div class="player-cover"><img id="player-cover" alt="" hidden><span id="player-no-cover">&#9835;</span></div>
          <div class="min-w-0">
            <div id="player-title" class="fs-5 fw-semibold text-break"></div>
            <div id="player-tags" class="text-body-secondary text-break"></div>
            <div id="player-state" class="small text-body-secondary"></div>
          </div>
        </div>
        <div class="d-flex justify-content-between small text-body-secondary"><span id="player-at"></span><span id="player-length"></span></div>
        <input id="player-seek" type="range" class="form-range mb-2" min="0" max="1000" value="0" aria-label="Position">
        <div class="d-flex justify-content-center gap-2 mb-3">
          <button id="player-previous" class="btn btn-outline-secondary" type="button" title="Previous">${icons.previous}</button>
          <button id="player-toggle" class="btn btn-primary px-4" type="button" title="Play / pause">${icons.play}</button>
          <button id="player-next" class="btn btn-outline-secondary" type="button" title="Next">${icons.next}</button>
          <button id="player-stop" class="btn btn-outline-secondary" type="button" title="Stop">${icons.stop}</button>
        </div>
        <div class="d-flex justify-content-center gap-2 mb-3">
          <button id="player-shuffle" class="btn btn-sm btn-outline-secondary" type="button">Shuffle</button>
          <button id="player-repeat" class="btn btn-sm btn-outline-secondary" type="button">Repeat</button>
        </div>
        <label class="form-label" for="player-volume">Volume <span id="player-volume-value" class="text-body-secondary"></span></label>
        <input id="player-volume" type="range" class="form-range" min="0" max="100" aria-label="Volume">`) +
      `<div id="player-next-card"></div>` +
      `<p class="small text-body-secondary">The music starts on the device: a media tile of the dashboard, or the SD card's file manager.</p>`;
    // a slider that is being moved is not set by the next answer
    const held = new Set();
    for (const id of ["player-seek", "player-volume"]) {
      const slider = $("#" + id);
      slider.onpointerdown = () => held.add(id);
      slider.oninput = () => {
        held.add(id);
        if (id === "player-volume") $("#player-volume-value").textContent = slider.value;
        else $("#player-at").textContent = clock(slider.value * p.duration_ms / 1000);
      };
    }
    const send = (form) => api("/api/player", form).then((answer) => { p = answer; draw(); });
    const button = (id, form) => ($("#" + id).onclick = (e) => busy(e.currentTarget, () => send(form())));
    button("player-toggle", () => ({ action: "toggle" }));
    button("player-previous", () => ({ action: "previous" }));
    button("player-next", () => ({ action: "next" }));
    button("player-stop", () => ({ action: "stop" }));
    button("player-shuffle", () => ({ shuffle: p.shuffle ? "0" : "1" }));
    button("player-repeat", () => ({ repeat: { off: "all", all: "one", one: "off" }[p.repeat] }));
    $("#player-seek").onchange = (e) => {
      held.delete("player-seek");
      if (p.duration_ms) send({ seek: Math.round(e.target.value * p.duration_ms / 1000) }).catch((err) => toast(err.message, "danger"));
    };
    $("#player-volume").onchange = (e) => {
      held.delete("player-volume");
      send({ volume: e.target.value }).catch((err) => toast(err.message, "danger"));
    };
    let errorSerial = p.error_serial;
    let coverSerial = -1;
    let upcoming = null;
    const draw = () => {
      const loaded = p.count > 0 || p.state === "buffering";
      $("#player-title").textContent = loaded && p.title ? p.title : "Nothing queued";
      $("#player-tags").textContent = [p.artist, p.album].filter(Boolean).join(" \u00b7 ");
      $("#player-state").textContent = [loaded ? states[p.state] : "", p.count > 1 ? `${p.index + 1} of ${p.count}` : ""]
        .filter(Boolean).join(" \u00b7 ");
      const playing = ["playing", "buffering", "interrupted"].includes(p.state);
      $("#player-toggle").innerHTML = playing ? icons.pause : icons.play;
      $("#player-toggle").title = playing ? "Pause" : "Play";
      for (const id of ["player-toggle", "player-previous", "player-next", "player-stop"]) $("#" + id).disabled = p.count === 0;
      $("#player-shuffle").classList.toggle("btn-primary", p.shuffle);
      $("#player-shuffle").classList.toggle("btn-outline-secondary", !p.shuffle);
      $("#player-repeat").textContent = { off: "Repeat", all: "Repeat all", one: "Repeat one" }[p.repeat];
      $("#player-repeat").classList.toggle("btn-primary", p.repeat !== "off");
      $("#player-repeat").classList.toggle("btn-outline-secondary", p.repeat === "off");
      const seek = $("#player-seek");
      seek.disabled = !p.duration_ms;
      if (!held.has("player-seek")) {
        seek.value = p.duration_ms ? Math.round(p.position_ms * 1000 / p.duration_ms) : 0;
        $("#player-at").textContent = loaded ? clock(p.position_ms) : "";
      }
      $("#player-length").textContent = p.duration_ms ? clock(p.duration_ms) : "";
      if (!held.has("player-volume")) {
        $("#player-volume").value = p.volume;
        $("#player-volume-value").textContent = p.volume;
      }
      if (p.cover_serial !== coverSerial) {
        coverSerial = p.cover_serial;
        const img = $("#player-cover");
        img.hidden = true;
        $("#player-no-cover").hidden = false;
        if (coverSerial) {
          img.onload = () => { img.hidden = false; $("#player-no-cover").hidden = true; };
          img.src = `/api/player/cover?s=${coverSerial}`;
        }
      }
      // the list again only when it changed: a push for the position keeps it as it is
      const next = JSON.stringify([p.index, p.next]);
      if (next !== upcoming) {
        upcoming = next;
        const left = p.repeat === "off" ? p.count - p.index - 1 : p.count - 1;
        $("#player-next-card").innerHTML = p.next.length ? card("Up next", `<ol class="player-next mb-0">${p.next.map((title, i) =>
          `<li value="${(p.index + 1 + i) % p.count + 1}">${esc(title)}</li>`).join("")}</ol>` +
          (left > p.next.length ? `<div class="small text-body-secondary mt-2">and ${left - p.next.length} more</div>` : "")) : "";
      }
      if (p.error_serial !== errorSerial) {
        errorSerial = p.error_serial;
        if (p.error) toast(p.error, "warning");
      }
    };
    draw();
    page.live("/api/player", (state) => { p = state; draw(); });
  },
  // #endif

  // the clock: time zone and SNTP servers (docs/ui/web/web-app.md section 4)
  async time(page) {
    page.render(`<h1>Time</h1><div id="time-status">Loading ...</div><div id="time-form"></div>`);
    let t, zones;
    try {
      [t, zones] = await Promise.all([api("/api/time"), api("/api/time/zones")]);
    } catch (e) {
      if (e.status !== 404) throw e;
      page.render(`<h1>Time</h1>${card("Time", '<p class="text-body-secondary mb-0">Not in the recovery app. Start the firmware (System).</p>')}`);
      return;
    }
    const drawStatus = () => {
      const ago = t.last_sync ? duration(Math.max(0, t.now - t.last_sync)) + " ago" : "not yet";
      $("#time-status").innerHTML = card("Clock", facts([
        ["Time", t.synced ? esc(t.local) : '<span class="badge text-bg-warning">not set</span>'],
        ["Time zone", esc(t.zone || "UTC") + (t.fixed_zone ? " (fixed)" : t.zone ? " (Home Assistant's)" : "")],
        ...(t.rule !== t.zone ? [["Rule", `<code>${esc(t.rule)}</code>`]] : []),
        ["Last answer", t.synced ? ago : "-"],
      ]));
    };
    drawStatus();
    // the zones grouped by their region ("Europe/Berlin": Europe), names without one last
    const groups = {};
    for (const z of zones) (groups[z.includes("/") ? z.split("/")[0] : "Other"] ||= []).push(z);
    const option = (z) => `<option value="${esc(z)}" ${z === t.fixed_zone ? "selected" : ""}>${esc(z.replace(/_/g, " "))}</option>`;
    const browserZone = Intl.DateTimeFormat().resolvedOptions().timeZone;
    // a fixed zone that is not a name of the list is a POSIX rule
    const custom = t.fixed_zone !== "" && !zones.includes(t.fixed_zone);
    $("#time-form").innerHTML = card("Settings", `
      <form id="time" autocomplete="off">
        <label class="form-label" for="time-zone">Time zone</label>
        <div class="d-flex gap-2 mb-1">
          <select id="time-zone" class="form-select">
            <option value="" ${t.fixed_zone ? "" : "selected"}>From Home Assistant (${esc(t.hass_zone || "none yet, UTC")})</option>
            ${Object.keys(groups).sort((a, b) => (a === "Other") - (b === "Other") || a.localeCompare(b))
              .map((g) => `<optgroup label="${esc(g)}">${groups[g].map(option).join("")}</optgroup>`).join("")}
            <option value="custom" ${custom ? "selected" : ""}>Custom rule (POSIX TZ)</option>
          </select>
          ${zones.includes(browserZone) ? `<button id="time-browser" class="btn btn-outline-secondary text-nowrap" type="button"
            title="${esc(browserZone)}">This browser's</button>` : ""}
        </div>
        <div id="time-custom" ${custom ? "" : "hidden"}>
          <input id="time-rule" class="form-control font-monospace mt-2" value="${esc(custom ? t.fixed_zone : t.rule)}"
            maxlength="63" spellcheck="false" autocapitalize="off" placeholder="CET-1CEST,M3.5.0,M10.5.0/3">
          <div class="form-text">For a zone or a change the firmware's list does not have yet: the rule as in TZ,
            such as <code>CET-1CEST,M3.5.0,M10.5.0/3</code> (offsets west of UTC, so CET is -1).</div>
        </div>
        <div class="form-text mb-3">A zone chosen here stays, whatever Home Assistant's is.</div>
        <div class="row g-3">
          ${t.servers.map((s, i) => `<div class="col-sm-6"><label class="form-label" for="time-server${i + 1}">Time server ${i + 1}</label>
            <input id="time-server${i + 1}" class="form-control" value="${esc(s)}" placeholder="${esc(t.defaults[i] || "")}"
              maxlength="63" spellcheck="false" autocapitalize="off"></div>`).join("")}
        </div>
        <div class="form-text">Host names or addresses of NTP servers, such as the router. Both empty: ${t.defaults.map(esc).join(" and ")}.</div>
        <div class="d-flex justify-content-end mt-3"><button class="btn btn-primary" type="submit">Save</button></div>
      </form>`);
    const browser = $("#time-browser");
    const showCustom = () => ($("#time-custom").hidden = $("#time-zone").value !== "custom");
    $("#time-zone").onchange = showCustom;
    if (browser) browser.onclick = () => { $("#time-zone").value = browserZone; showCustom(); };
    $("#time").onsubmit = (e) => {
      e.preventDefault();
      busy(e.submitter || $("#time button[type=submit]"), async () => {
        const zone = $("#time-zone").value;
        const rule = $("#time-rule").value.trim();
        if (zone === "custom" && !rule) throw new Error("enter a rule, or pick a zone");
        const form = { fixed_zone: zone === "custom" ? rule : zone };
        t.servers.forEach((_, i) => (form[`server${i + 1}`] = $(`#time-server${i + 1}`).value.trim()));
        t = await api("/api/time", form);
        drawStatus();
        toast("saved", "success");
      });
    };
    page.live("/api/time", (state) => { t = state; drawStatus(); });
  },

  async ha(page) {
    page.render(`<h1>Home Assistant</h1><div id="ha-status">Loading ...</div><div id="ha-form"></div>`);
    let h;
    try {
      h = await api("/api/hass");
    } catch (e) {
      if (e.status !== 404) throw e;
      page.render(`<h1>Home Assistant</h1>${card("Home Assistant", '<p class="text-body-secondary mb-0">The recovery app has no Home Assistant connection. Start the firmware (System).</p>')}`);
      return;
    }
    // https with a self-signed certificate: the user compares its fingerprint and trusts it
    const certificateCard = () => {
      if (h.refused_certificate) return card("Certificate not trusted", `
        <p>No certificate authority vouches for this server's certificate. It may be the self-signed
          certificate of your Home Assistant. Open the address in your browser and compare the
          certificate's SHA-256 fingerprint (the lock or warning icon in the address bar, certificate details).
          Trust it only if both match.</p>
        <p class="font-monospace small text-break mb-3">${esc(fingerprint(h.refused_certificate))}</p>
        ${h.certificate ? '<p class="text-danger small">It replaces the certificate trusted so far: that one is no longer used by the server.</p>' : ""}
        <div class="d-flex justify-content-end"><button id="ha-trust" class="btn btn-sm btn-warning" type="button"
          data-fingerprint="${esc(h.refused_certificate)}">Trust this certificate</button></div>`);
      if (h.certificate) return card("Certificate", `
        <p class="small mb-2">This device trusts the self-signed certificate with the SHA-256 fingerprint</p>
        <p class="font-monospace small text-break mb-3">${esc(fingerprint(h.certificate))}</p>
        <div class="d-flex justify-content-end"><button id="ha-forget-cert" class="btn btn-sm btn-outline-danger" type="button">Forget</button></div>`);
      return "";
    };
    const drawStatus = () => {
      const box = $("#ha-status");
      if (!box) return;
      const retry = h.state === "token_rejected" || h.state === "disconnected";
      box.innerHTML = card("Connection", hassFacts(h) + (retry
        ? '<div class="d-flex justify-content-end"><button id="ha-reconnect" class="btn btn-sm btn-outline-secondary" type="button">Try now</button></div>' : "")) +
        certificateCard();
      const button = $("#ha-reconnect");
      if (button) button.onclick = (e) => busy(e.target, async () => { h = await api("/api/hass/reconnect", {}); drawStatus(); });
      const trust = $("#ha-trust");
      if (trust) trust.onclick = (e) => busy(e.target, async () => {
        h = await api("/api/hass/certificate", { fingerprint: trust.dataset.fingerprint });
        toast("certificate trusted, connecting ...", "success");
        drawStatus();
      });
      const forget = $("#ha-forget-cert");
      if (forget) forget.onclick = (e) => busy(e.target, async () => {
        if (!confirm("Forget the trusted certificate? The device no longer connects to this Home Assistant until you trust it again.")) return;
        h = await api("/api/hass/certificate", { fingerprint: "" });
        drawStatus();
      });
    };
    $("#ha-form").innerHTML = card("Address and token", `
      <form id="ha-settings" autocomplete="off">
        <label class="form-label" for="ha-url">Address</label>
        <div class="input-group">
          <input id="ha-url" class="form-control" value="${esc(h.url)}" placeholder="http://homeassistant.local:8123" inputmode="url" spellcheck="false">
          <button id="ha-find" class="btn btn-outline-secondary" type="button">Search</button>
        </div>
        <div id="ha-found" class="list-group mt-2"></div>
        <div class="form-text mb-3">The address you open Home Assistant with in the browser, or one found on the network.</div>
        <label class="form-label" for="ha-token">Long-lived access token</label>
        <input id="ha-token" class="form-control font-monospace" type="password" spellcheck="false"
          placeholder="${h.token_set ? "stored - leave empty to keep it" : "paste the token here"}">
        <div class="form-text mb-3">In Home Assistant: your profile (bottom left), tab Security, Long-lived access tokens,
          Create token. Copy it right away, Home Assistant shows it only once. The device never shows it again.</div>
        <div class="d-flex gap-2">
          ${h.url ? '<button id="ha-remove" class="btn btn-outline-danger" type="button">Remove</button>' : ""}
          <button class="btn btn-primary ms-auto" type="submit">Save and connect</button>
        </div>
      </form>`);
    $("#ha-settings").onsubmit = (event) => {
      event.preventDefault();
      busy(event.submitter, async () => {
        h = await api("/api/hass", { url: $("#ha-url").value, token: $("#ha-token").value });
        $("#ha-url").value = h.url;
        $("#ha-token").value = "";
        $("#ha-token").placeholder = "stored - leave empty to keep it";
        toast("saved, connecting ...", "success");
        drawStatus();
      });
    };
    // Home Assistant announces itself over mDNS; the search takes about 2 s
    const find = (button) => busy(button, async () => {
      const box = $("#ha-found");
      box.innerHTML = '<div class="small text-body-secondary">Searching the network ...</div>';
      const r = await api("/api/hass/discover");
      box.innerHTML = r.found.length
        ? r.found.map((f) => `<button type="button" class="list-group-item list-group-item-action" data-url="${esc(f.url)}">
            <div class="fw-semibold">${esc(f.name || "Home Assistant")}</div>
            <div class="small text-body-secondary">${esc(f.url)}${f.version ? " &middot; " + esc(f.version) : ""}</div></button>`).join("")
        : '<div class="small text-body-secondary">No Home Assistant found on this network. Type its address.</div>';
    });
    $("#ha-find").onclick = (e) => find(e.target);
    $("#ha-found").onclick = (event) => {
      const item = event.target.closest("[data-url]");
      if (!item) return;
      $("#ha-url").value = item.dataset.url;
      $("#ha-found").innerHTML = "";
      $("#ha-token").focus();
    };
    if (!h.url) find($("#ha-find"));
    const remove = $("#ha-remove");
    if (remove) remove.onclick = (e) => busy(e.target, async () => {
      if (!confirm("Remove the Home Assistant connection? Address and token are deleted.")) return;
      h = await api("/api/hass", { url: "", token: "" });
      views.ha(page);
    });
    drawStatus();
    page.live("/api/hass", (state) => { h = state; drawStatus(); });
  },
  async dashboard(page) {
    const app = (await api("/api/update")).app;
    // left meanwhile: no editor over the next page
    if (!page.current()) return;
    if (app === "recovery") {
      page.render(`<h1>Dashboard</h1>${card("Dashboard", '<p class="text-body-secondary mb-0">The recovery app has no dashboard. Start the firmware (System).</p>')}`);
      return;
    }
    let tab = "editor";
    try { tab = sessionStorage.getItem("dashboard-tab") || tab; } catch (e) {}
    page.render(`<div class="d-flex flex-wrap align-items-center gap-3 mb-3"><h1 class="mb-0">Dashboard</h1>
      <ul class="nav nav-pills">
        <li class="nav-item"><a class="nav-link" href="#/dashboard" data-tab="editor">Editor</a></li>
        <li class="nav-item"><a class="nav-link" href="#/dashboard" data-tab="io">Import &amp; export</a></li>
      </ul></div><div id="dash-tab"></div>`);
    const show = async (name) => {
      if (name !== tab && window.hcEditor && window.hcEditor.dirty() &&
          !confirm("Leave the editor? Your changes are not saved.")) return;
      tab = name;
      try { sessionStorage.setItem("dashboard-tab", tab); } catch (e) {}
      for (const link of document.querySelectorAll("[data-tab]")) link.classList.toggle("active", link.dataset.tab === tab);
      if (window.hcEditor) window.hcEditor.unmount();
      if (tab === "editor") await dashboardEditor($("#dash-tab"));
      else await dashboardFiles($("#dash-tab"));
    };
    for (const link of document.querySelectorAll("[data-tab]")) {
      link.onclick = (e) => { e.preventDefault(); show(link.dataset.tab); };
    }
    await show(tab);
  },
  async logs(page) {
    page.render(`<h1>Logs</h1><div class="card"><div class="card-body">
      <div class="d-flex justify-content-between align-items-center gap-2 mb-2">
        <span id="log-count" class="small text-body-secondary"></span>
        <select id="log-filter" class="form-select form-select-sm w-auto" aria-label="Show">
          <option value="E">Errors</option><option value="W">Warnings and errors</option>
          <option value="I" selected>Everything</option>
        </select>
      </div>
      <div class="d-flex mb-3">
        <pre id="log-lines" class="app-log flex-grow-1 mb-0">Loading ...</pre>
        <div id="log-grip" class="app-grip" title="Drag to resize, double-click for the default"></div>
      </div>
      <div class="d-flex flex-wrap gap-2 align-items-center">
        <button id="log-refresh" class="btn btn-outline-secondary" type="button">Refresh</button>
        <button id="log-download" class="btn btn-outline-secondary" type="button">Download</button>
        <div class="form-check form-switch ms-auto mb-0">
          <input id="log-debug" class="form-check-input" type="checkbox" role="switch">
          <label class="form-check-label" for="log-debug">Debug</label>
        </div>
      </div>
      <p class="small text-body-secondary mt-2 mb-0">Errors and warnings are kept in the device's memory, the
        oldest go first. Debug keeps the info messages as well. The log starts again with each restart.</p>
    </div></div>`);
    let entries = [];
    const rank = { E: 0, W: 1, I: 2, D: 3, V: 4 };
    const pad = (n) => String(n).padStart(2, "0");
    // the device's clock once it is set, else the time since the start
    const when = (e) => {
      if (e.unix) {
        const d = new Date(e.unix * 1000);
        const time = `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
        return d.toDateString() === new Date().toDateString() ? time
          : `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${time}`;
      }
      const s = Math.floor(e.ms / 1000);
      return `+${Math.floor(s / 3600)}:${pad(Math.floor(s / 60) % 60)}:${pad(s % 60)}`;
    };
    const draw = () => {
      const max = rank[$("#log-filter").value];
      const shown = entries.filter((e) => rank[e.level] <= max);
      $("#log-count").textContent = `${shown.length} of ${entries.length} messages`;
      $("#log-lines").innerHTML = shown.length ? shown.map((e) =>
        `<span class="log-${e.level}">${esc(when(e))} ${e.level} ${esc(e.tag)}  ${esc(e.text)}</span>`).join("\n")
        : '<span class="text-body-secondary">No messages.</span>';
      const box = $("#log-lines");
      box.scrollTop = box.scrollHeight;
    };
    const load = async () => {
      const [settings, response] = await Promise.all([api("/api/log"), fetch("/api/log.txt", { cache: "no-store" })]);
      if (response.status === 401) return go("login");
      if (!response.ok) throw new Error(`the log could not be loaded (${response.status})`);
      $("#log-debug").checked = settings.debug;
      // `<unix time or 0> <level> (<ms since the start>) <tag>: <message>`
      entries = (await response.text()).split("\n").map((line) => {
        const m = /^(\d+) ([EWIDV]) \((\d+)\) ([^:]*): ?(.*)$/.exec(line);
        return m && { unix: Number(m[1]), level: m[2], ms: Number(m[3]), tag: m[4], text: m[5] };
      }).filter(Boolean);
      draw();
    };
    // the border right of the box drags the page's width, kept in this browser; a double-click goes
    // back to the 800 px column. The page stays centered: it grows by twice the drag.
    const column = $("#page"), grip = $("#log-grip");
    const size = (w) => { column.style.maxWidth = w ? `${Math.max(480, w)}px` : ""; };
    size(Number(localStorage.getItem("logWidth")) || 0);
    grip.onpointerdown = (e) => {
      const x0 = e.clientX, w0 = column.offsetWidth;
      grip.setPointerCapture(e.pointerId);
      grip.onpointermove = (m) => size(w0 + 2 * (m.clientX - x0));
      grip.onpointerup = grip.onpointercancel = () => {
        grip.onpointermove = null;
        localStorage.setItem("logWidth", column.offsetWidth);
      };
    };
    grip.ondblclick = () => { size(0); localStorage.removeItem("logWidth"); };
    $("#log-filter").onchange = draw;
    $("#log-refresh").onclick = (e) => busy(e.target, load);
    $("#log-download").onclick = () => {
      const text = entries.map((e) => `${when(e)} ${e.level} (${e.ms}) ${e.tag}: ${e.text}`).join("\n") + "\n";
      const link = document.createElement("a");
      link.href = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
      const d = new Date();
      link.download = `${document.title || "hassctrl"}-log-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}.txt`;
      link.click();
      URL.revokeObjectURL(link.href);
    };
    $("#log-debug").onchange = (e) => busy(e.target, async () => {
      try {
        await api("/api/log", { debug: e.target.checked ? 1 : 0 });
      } catch (err) {
        e.target.checked = !e.target.checked;
        throw err;
      }
      toast(e.target.checked ? "debug on: info messages are kept from now on" : "debug off", "success");
    });
    await load();
  },

  // #if CONFIG_HC_SDCARD
  // The SD card (docs/ui/web/web-app.md section 4, SD card): its files, upload, a download from a
  // URL, mount and format. Only with a card slot (/api/status has "sd").
  async sd(page) {
    let dir = sessionStorage.getItem("sd-dir") || "";
    const join = (name) => (dir ? dir + "/" : "") + name;
    const fileUrl = (path) => "/api/sd/file?path=" + encodeURIComponent(path);
    page.render(`<h1>SD card</h1>` +
      card("Card", `<p class="mb-2" id="sd-state">...</p>
        <p class="small text-body-secondary mb-0">The slot has no card detection: put a card in or take it
          out with the device off, it is mounted when the device starts.</p>`) +
      card("Files", `<nav class="mb-2" id="sd-path"></nav>
        <div class="list-group mb-3" id="sd-list"></div>
        <label class="form-label" for="sd-file">Upload into this folder</label>
        <input id="sd-file" type="file" class="form-control mb-2" multiple>
        <div class="d-flex gap-2 flex-wrap mb-2">
          <button id="sd-upload" class="btn btn-primary" type="button">Upload</button>
          <button id="sd-mkdir" class="btn btn-outline-secondary ms-auto" type="button">New folder</button>
        </div>
        <div class="progress" id="progress" hidden><div class="progress-bar" style="width:0%"></div></div>
        <p class="small text-body-secondary mt-2 mb-0">Alarm sounds: MP3 files in <code>sounds/alarms</code>.
          Saved recordings: <code>recordings</code>.</p>`) +
      card("Download from a URL", `
        <input id="sd-url" class="form-control mb-2" placeholder="http(s)://.../file" autocomplete="off">
        <input id="sd-name" class="form-control mb-2" placeholder="file name (in this folder)" autocomplete="off">
        <div class="d-flex justify-content-end"><button id="sd-fetch" class="btn btn-primary" type="button">Download</button></div>
        <p class="mt-2 mb-0" id="sd-fetch-message"></p>`) +
      card("Format", `
        <p class="mb-2">Erases the card and writes an empty FAT. Enter your password to confirm.</p>
        <input id="sd-password" type="password" class="form-control mb-2" autocomplete="current-password"
          placeholder="password">
        <div class="d-flex justify-content-end"><button id="sd-format" class="btn btn-outline-danger" type="button">Format the card</button></div>`));

    // a download from a URL waits for the push that says it ended (fetchDone)
    let fetchDone = null;
    const showState = (c) => {
      if (fetchDone) fetchDone(c);
      $("#sd-state").textContent = sdSpace(c) + (c.state === "mounted" ? `, ${c.name}, ${c.clock_khz / 1000} MHz` : "");
      const f = c.fetch;
      $("#sd-fetch-message").textContent = f.state === "running" ? `downloading ${f.path}: ${kib(f.done)}${f.total ? " of " + kib(f.total) : ""}`
        : f.state === "failed" ? `download failed: ${f.error}` : f.state === "done" ? `downloaded ${f.path}` : "";
      return c;
    };
    const list = async () => {
      sessionStorage.setItem("sd-dir", dir);
      const parts = dir ? dir.split("/") : [];
      $("#sd-path").innerHTML = [`<a href="#" data-dir="">card</a>`, ...parts.map((p, i) =>
        `<a href="#" data-dir="${esc(parts.slice(0, i + 1).join("/"))}">${esc(p)}</a>`)].join(" / ");
      let entries = [];
      try {
        entries = (await api("/api/sd/list?dir=" + encodeURIComponent(dir))).entries;
      } catch (e) {
        if (e.status === 404 && dir) {
          dir = "";
          return list();
        }
        $("#sd-list").innerHTML = `<div class="list-group-item text-body-secondary">${esc(e.message)}</div>`;
        return;
      }
      entries.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
      $("#sd-list").innerHTML = entries.map((e) => `
        <div class="list-group-item d-flex align-items-center gap-2">
          ${e.dir ? `<a href="#" class="flex-grow-1 text-truncate" data-dir="${esc(join(e.name))}">${esc(e.name)}/</a>`
                  : `<a class="flex-grow-1 text-truncate" href="${fileUrl(join(e.name))}" download="${esc(e.name)}">${esc(e.name)}</a>
                     <span class="small text-body-secondary">${kib(e.size)}</span>`}
          <button class="btn btn-sm btn-outline-secondary" type="button" data-rename="${esc(join(e.name))}">Rename</button>
          <button class="btn btn-sm btn-outline-danger" type="button" data-delete="${esc(join(e.name))}">Delete</button>
        </div>`).join("") || '<div class="list-group-item text-body-secondary">empty</div>';
    };
    const load = async () => showState(await api("/api/sd"));

    for (const id of ["#sd-path", "#sd-list"]) {
      $(id).onclick = async (e) => {
        const link = e.target.closest("[data-dir]");
        const del = e.target.closest("[data-delete]");
        const ren = e.target.closest("[data-rename]");
        if (link) {
          e.preventDefault();
          dir = link.dataset.dir;
          await list();
        } else if (del) {
          if (!confirm(`Delete ${del.dataset.delete}?`)) return;
          await busy(del, async () => {
            await api("/api/sd/delete", { path: del.dataset.delete });
            await list();
            await load();
          });
        } else if (ren) {
          // the whole path: another folder in it moves the file
          const to = prompt("New name (a folder path in front moves it)", ren.dataset.rename);
          if (!to || to === ren.dataset.rename) return;
          await busy(ren, async () => {
            await api("/api/sd/rename", { from: ren.dataset.rename, to: to.replace(/^\/+/, "") });
            await list();
          });
        }
      };
    }
    $("#sd-upload").onclick = (e) => busy(e.target, async () => {
      const files = [...$("#sd-file").files];
      if (!files.length) throw new Error("choose a file");
      for (const file of files) {
        await upload("/api/sd/file?path=" + encodeURIComponent(join(file.name)), file);
      }
      progress(0, 0);
      $("#sd-file").value = "";
      toast(files.length === 1 ? `${files[0].name} uploaded` : `${files.length} files uploaded`, "success");
      await list();
      await load();
    });
    $("#sd-mkdir").onclick = (e) => busy(e.target, async () => {
      const name = prompt("Name of the new folder");
      if (!name) return;
      await api("/api/sd/mkdir", { path: join(name) });
      await list();
    });
    $("#sd-fetch").onclick = (e) => busy(e.target, async () => {
      const url = $("#sd-url").value.trim();
      const name = $("#sd-name").value.trim() || decodeURIComponent(url.split("?")[0].split("/").pop() || "");
      if (!url || !name) throw new Error("enter a URL and a file name");
      // running from the POST's answer on; a short file may have ended before the GET
      await api("/api/sd/fetch", { url, path: join(name) });
      let c = showState(await api("/api/sd"));
      if (c.fetch.state === "running") {
        c = await new Promise((resolve) => {
          fetchDone = (next) => {
            if (next.fetch.state === "running") return;
            fetchDone = null;
            resolve(next);
          };
        });
      }
      if (c.fetch.state === "failed") throw new Error(c.fetch.error);
      toast(`${name} downloaded`, "success");
      await list();
    });
    $("#sd-format").onclick = (e) => busy(e.target, async () => {
      if (!confirm("Format the SD card? Every file on it is deleted.")) return;
      const password = $("#sd-password").value;
      $("#sd-password").value = "";
      showState(await api("/api/sd/format", { confirm: "yes", password }));
      dir = "";
      toast("SD card formatted", "warning");
      await list();
    });

    const c = await load();
    if (c.state === "mounted") await list();
    else $("#sd-list").innerHTML = '<div class="list-group-item text-body-secondary">no card mounted</div>';
    page.live("/api/sd", showState);
  },
  // #endif

  async system(page) {
    const [s, u] = await Promise.all([api("/api/status"), api("/api/update")]);
    // the recovery app has no backup (its /api/backup answers 404)
    const recovery = u.app === "recovery";
    page.render(`<h1>System</h1>` +
      card("Device", facts([
        ["Firmware", esc(s.device.firmware)], ["Built", esc(s.device.built)], ["Uptime", duration(s.device.uptime_s)],
        ["Free memory", `${s.device.heap_internal_kb} KB internal, ${s.device.heap_psram_kb} KB PSRAM`],
        ["Firmware update", u.firmware_direct ? "direct" : "through the recovery app"],
      ])) +
      (recovery ? "" : card("Backup", `
        <p class="small text-body-secondary">The settings and the dashboard in one YAML file. Wi-Fi passwords and the
          Home Assistant token are not in it; they may be added to the file by hand before an import.</p>
        <div class="d-flex gap-2 flex-wrap mb-3">
          <a class="btn btn-outline-secondary" href="/api/backup.yaml">Export all</a>
          <a class="btn btn-outline-secondary" href="/api/backup.yaml?parts=settings">Settings only</a>
          <a class="btn btn-outline-secondary" href="/api/backup.yaml?parts=dashboard">Dashboard only</a>
        </div>
        <label class="form-label" for="backup-file">Import a backup</label>
        <input id="backup-file" type="file" class="form-control mb-2" accept=".yaml,.yml">
        <div class="d-flex gap-2 justify-content-end">
          <button id="backup-check" class="btn btn-outline-secondary" type="button">Check</button>
          <button id="backup-import" class="btn btn-primary" type="button">Import</button>
        </div>
        <div id="backup-report" class="mt-3"></div>`)) +
      (session.logged_in ? card("Password", `
        <p class="small text-body-secondary">The password of the web UI login <code>${esc(session.user)}</code>. Every other
          browser is logged out; this one stays logged in until it is closed.</p>
        <input id="pw-current" type="password" class="form-control mb-2" placeholder="Current password" autocomplete="current-password">
        <input id="pw-new" type="password" class="form-control mb-2" placeholder="New password, 8 to 64 characters" autocomplete="new-password">
        <input id="pw-repeat" type="password" class="form-control mb-2" placeholder="New password again" autocomplete="new-password">
        <div class="d-flex justify-content-end"><button id="pw-go" class="btn btn-primary" type="button">Change password</button></div>
        <div class="d-flex justify-content-between align-items-center gap-2 border-top pt-3 mt-3">
          <span class="small text-body-secondary">Ends the session of every browser, this one too.</span>
          <button id="logout-all" class="btn btn-outline-secondary text-nowrap" type="button">Log out everywhere</button>
        </div>`) : "") +
      card("Update", `
        <label class="form-label" for="fw-file">Firmware image</label>
        <input id="fw-file" type="file" class="form-control mb-2" accept=".bin">
        <input id="fw-url" class="form-control mb-2" placeholder="or a URL: http(s)://.../hassctrl.bin" autocomplete="off">
        <div class="d-flex justify-content-end mb-3"><button id="fw-go" class="btn btn-primary" type="button">Update firmware</button></div>
        <label class="form-label" for="fs-file">File system image (these web pages)</label>
        <input id="fs-file" type="file" class="form-control mb-2" accept=".bin">
        <input id="fs-url" class="form-control mb-2" placeholder="or a URL" autocomplete="off">
        <div class="d-flex justify-content-end"><button id="fs-go" class="btn btn-primary" type="button">Update file system</button></div>
        <div class="progress mt-3" id="progress" hidden><div class="progress-bar" style="width:0%"></div></div>
        <p class="mt-2 mb-0" id="update-message"></p>`) +
      card("File system", `
        <p class="mb-2">${esc(s.fs.state)}${s.fs.state === "mounted" ? `, ${s.fs.used_kb} of ${s.fs.total_kb} KB used` : ""}</p>
        <div class="d-flex gap-2 flex-wrap">
          <a class="btn btn-outline-secondary" href="/api/fs/files.zip">Download the files (.zip)</a>
          <button id="format" class="btn btn-outline-danger ms-auto" type="button">Format</button>
        </div>`) +
      card("Restart and reset", `
        <div class="d-flex gap-2 flex-wrap">
          <button id="restart" class="btn btn-outline-secondary" type="button">Restart</button>
          ${recovery && u.firmware_present ? '<button id="to-firmware" class="btn btn-outline-secondary" type="button">Start the firmware</button>' : ""}
          <button id="reset" class="btn btn-outline-danger ms-auto" type="button">Factory reset</button>
        </div>`));
    if (!recovery) {
      $("#backup-check").onclick = (e) => busy(e.target, () => importBackup(true));
      $("#backup-import").onclick = (e) => busy(e.target, () => importBackup(false));
    }
    if ($("#pw-go")) {
      $("#logout-all").onclick = (e) => busy(e.target, async () => {
        if (!confirm("Log out on every browser and device?")) return;
        await api("/api/logout-all", {}).catch(() => {});
        go("login");
      });
      $("#pw-go").onclick = (e) => busy(e.target, async () => {
        const current = $("#pw-current").value, password = $("#pw-new").value;
        if (password.length < 8 || password.length > 64) throw new Error("the new password needs 8 to 64 characters");
        if (password !== $("#pw-repeat").value) throw new Error("the two new passwords differ");
        await api("/api/password", { current, password });
        for (const id of ["#pw-current", "#pw-new", "#pw-repeat"]) $(id).value = "";
        toast("password changed, other browsers are logged out");
      });
    }
    $("#fw-go").onclick = (e) => busy(e.target, () => runUpdate("firmware"));
    $("#fs-go").onclick = (e) => busy(e.target, () => runUpdate("filesystem"));
    $("#format").onclick = (e) => busy(e.target, async () => {
      if (!confirm("Format the file system? Every file on it is deleted, also these web pages: the device then shows its built-in page.")) return;
      await api("/api/fs/format", { confirm: "yes" });
      toast("formatted", "warning");
      setTimeout(() => location.reload(), 1500);
    });
    $("#restart").onclick = (e) => busy(e.target, async () => {
      if (!confirm("Restart the device?")) return;
      await api("/api/restart", {});
      toast("restarting ...");
    });
    if ($("#to-firmware")) {
      // back from the recovery app; the page then shows the firmware's System page
      $("#to-firmware").onclick = (e) => busy(e.target, async () => {
        await api("/api/restart", { to: "firmware" });
        toast("starting the firmware ...");
        await waitForApp("main");
        location.reload();
      });
    }
    $("#reset").onclick = (e) => busy(e.target, async () => {
      if (!confirm("Factory reset? All settings, Wi-Fi networks and the web UI login are deleted.")) return;
      if (!confirm("Really? The device starts like new, with its hotspot.")) return;
      await api("/api/factory-reset", { confirm: "yes" });
      toast("erased, restarting ...", "warning");
    });
  },
};

// --------------------------------------------------------------------------------- backup

// The file goes up as it is; the answer lists the sections, the problems (nothing imported then),
// what the user should know and what was ignored.
async function importBackup(checkOnly) {
  const file = $("#backup-file").files[0];
  if (!file) throw new Error("choose a backup file");
  if (!checkOnly && !confirm("Import the backup? What is in the file replaces the settings and the dashboard of this device.")) return;
  const response = await fetch("/api/backup/import" + (checkOnly ? "?check=1" : ""),
    { method: "POST", body: await file.text(), cache: "no-store" });
  const r = await response.json().catch(() => ({}));
  if (response.status === 401) {
    session.logged_in = false;
    go("login");
    return;
  }
  if (!Array.isArray(r.problems)) throw new Error(r.error || response.statusText);
  const list = (title, items, kind) => items.length
    ? `<div class="mb-2"><strong class="${kind}">${title}</strong><ul class="mb-0 small">${items.map((i) => `<li>${esc(i)}</li>`).join("")}</ul></div>`
    : "";
  const sections = esc(r.sections.join(", ") || "nothing");
  const head = r.problems.length ? '<p class="text-danger mb-2">Not imported, nothing was changed: fix the file first.</p>'
    : r.imported ? `<p class="text-success mb-2">Imported: ${sections}.</p>`
    : `<p class="text-success mb-2">The file is fine (${sections}). Import applies it.</p>`;
  $("#backup-report").innerHTML = head + list("Problems", r.problems, "text-danger") +
    list("Please note", r.notes, "") + list("Ignored", r.ignored, "text-body-secondary");
  if (r.imported) toast("backup imported", "success");
}

// --------------------------------------------------------------------------------- SD card

// #if CONFIG_HC_SDCARD
const kib = (bytes) => bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.ceil(bytes / 1024)} KB`;

// "7.4 of 29.7 GB free", or the state ("no card", "not formatted") of /api/status or /api/sd
function sdSpace(c) {
  if (c.state !== "mounted") return c.state;
  // the device's units (1000s), as Settings, System shows them
  const [unit, name] = c.total_kb * 1024 >= 1e9 ? [1e9, "GB"] : [1e6, "MB"];
  const size = (kb) => ((kb * 1024) / unit).toFixed(1);
  return `${size(c.free_kb)} of ${size(c.total_kb)} ${name} free`;
}
// #endif

// --------------------------------------------------------------------------------- update

function progress(done, total) {
  const box = $("#progress");
  if (!box) return;
  box.hidden = !total;
  if (total) box.firstElementChild.style.width = Math.round((100 * done) / total) + "%";
}

function updateMessage(text) {
  if ($("#update-message")) $("#update-message").textContent = text;
}

async function waitForApp(app) {
  for (let i = 0; i < 60; i++) {
    await sleep(2000);
    try {
      const u = await (await fetch("/api/update", { cache: "no-store" })).json();
      // a new firmware is kept only after it confirmed itself
      if (u.app === app && u.confirmed !== false) return u;
    } catch (e) { /* restarting */ }
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

async function runUpdate(target) {
  const file = $(target === "firmware" ? "#fw-file" : "#fs-file").files[0];
  const url = $(target === "firmware" ? "#fw-url" : "#fs-url").value.trim();
  if (!file && !url) throw new Error("choose an image or enter a URL");
  // the device restarts: nothing to follow meanwhile
  liveStop();
  try {
    let u = await api("/api/update");
    if (target === "firmware" && !u.firmware_direct) {
      updateMessage("restarting into the recovery app ...");
      await api("/api/restart", { to: "recovery" });
      await waitForApp("recovery");
    }
    if (file) {
      updateMessage(`uploading ${Math.round(file.size / 1024)} KB ...`);
      await upload("/api/update/" + target, file);
    } else {
      await api("/api/update/url", { target, url });
      for (;;) {
        await sleep(1000);
        u = await api("/api/update").catch(() => null);
        if (!u) continue;
        progress(u.done, u.total || 0);
        updateMessage(`downloading ${Math.round(u.done / 1024)} KB ...`);
        if (u.state === "done") break;
        if (u.state === "failed") throw new Error(u.error);
      }
    }
    progress(0, 0);
    if (target === "firmware") {
      updateMessage("written, restarting into the new firmware ...");
      u = await waitForApp("main");
      updateMessage(`running firmware ${u.firmware_version}`);
    } else {
      updateMessage("file system updated, reloading the page ...");
      await sleep(1500);
      location.reload();
    }
  } catch (e) {
    progress(0, 0);
    updateMessage(e.message);
    throw e;
  }
}

// --------------------------------------------------------------------------------------- start

// the entries of a feature stay hidden until /api/status says the device has it (no flicker)
$("#menu-pages").innerHTML = menuGroups.map(([group, pages]) => `<div data-group="${esc(group)}" ${pages.some((p) => p[3]) ? "hidden" : ""}>
  <div class="app-menu-heading">${esc(group)}</div>${pages.map(navItem).join("")}</div>`).join("");
const allPages = menuGroups.flatMap(([, pages]) => pages);
$("#tabbar").innerHTML = tabPages.map((page) => navItem(allPages.find((p) => p[0] === page))).join("") +
  `<button id="tab-more" class="app-nav-item" type="button" aria-label="More pages">${navIcon("more")}<span class="app-nav-short">More</span></button>`;
$("#tab-more").onclick = () => openSheet($("#menu").hidden);

// the appearance: one button that goes Auto, Light, Dark
const themes = { auto: "Auto", light: "Light", dark: "Dark" };
let theme = localStorage.getItem("theme") || "auto";
function applyTheme() {
  const dark = theme === "dark" || (theme === "auto" && matchMedia("(prefers-color-scheme: dark)").matches);
  document.documentElement.setAttribute("data-bs-theme", dark ? "dark" : "light");
  $("#theme").innerHTML = `${navIcon(theme)}<span class="app-nav-short">${themes[theme]}</span><span class="app-nav-long">Appearance: ${themes[theme]}</span>`;
  $("#theme").title = `Appearance: ${themes[theme]} (click for the next)`;
}
$("#theme").onclick = () => {
  theme = { auto: "light", light: "dark", dark: "auto" }[theme] || "auto";
  localStorage.setItem("theme", theme);
  applyTheme();
};
applyTheme();
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", applyTheme);

$("#logout").innerHTML = `${navIcon("logout")}<span class="app-nav-short">Log out</span><span class="app-nav-long">Log out</span>`;
$("#logout").onclick = async () => {
  await api("/api/logout", {}).catch(() => {});
  go("login");
};

window.addEventListener("hashchange", route);
route();
