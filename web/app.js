// The web UI of hassctrl (docs/ui/web.md): one page, the views follow the hash (#/wifi). Plain
// JavaScript, no framework and no build step; Bootstrap 5.3 only as CSS. Everything comes from the
// device API (components/webserver). Texts from outside (network names) are escaped.
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
let refreshTimer = null;

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

function refresh(action, ms) {
  clearInterval(refreshTimer);
  refreshTimer = setInterval(() => action().catch(() => {}), ms);
}

function render(html) {
  $("#page").innerHTML = html;
  window.scrollTo(0, 0);
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

const menuPages = [
  ["status", "Status"], ["ha", "Home Assistant"], ["wifi", "Wi-Fi"], ["hotspot", "Hotspot"],
  ["dashboard", "Dashboard"], ["logs", "Logs"], ["system", "System"],
];

function frame(loggedIn, step) {
  $("#menu-button").hidden = !loggedIn;
  $("#icon-wifi").hidden = !loggedIn;
  $("#icon-ha").hidden = !loggedIn;
  $("#step").hidden = !step;
  $("#step").textContent = step || "";
  if (!loggedIn) $("#menu").hidden = true;
  $("#menu").classList.toggle("d-none", !loggedIn);
}

function closeMenuOnPhone() {
  if (!matchMedia("(min-width: 800px)").matches) $("#menu").hidden = true;
}

async function updateHeader() {
  const s = await api("/api/status");
  $("#device-name").textContent = s.device.name;
  document.title = s.device.name;
  const icon = $("#icon-wifi");
  icon.innerHTML = wifiIcon;
  icon.className = "state-icon " + (s.wifi.state === "connected" ? "ok" : s.wifi.error ? "error" : "warn");
  icon.title = s.wifi.state === "connected" ? `Wi-Fi: ${s.wifi.ssid}, ${s.wifi.rssi} dBm` : `Wi-Fi: ${s.wifi.error || s.wifi.state}`;
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
  clearInterval(refreshTimer);
  // the dashboard editor uses the whole width, every other page one column
  $("#page").classList.toggle("app-wide", name === "dashboard");
  try {
    session = await api("/api/session");
  } catch (e) {
    render(`<div class="alert alert-warning">The device does not answer. ${esc(e.message)}</div>`);
    return;
  }
  if (session.setup) return setupRoute(name);
  if (!session.logged_in) return loginPage();
  if (name === "setup-done") return setupDone();
  frame(true);
  for (const link of document.querySelectorAll("#menu-pages a")) {
    link.classList.toggle("active", link.dataset.page === name);
  }
  updateHeader().catch(() => {});
  await (views[name] || views.status)();
}

// -------------------------------------------------------------------------------------- setup

function setupRoute(name) {
  frame(false, name === "setup-login" ? "2 / 3" : "1 / 3");
  return name === "setup-login" ? setupLogin() : setupWifi();
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

async function setupWifi() {
  render(`<h1>Choose your Wi-Fi</h1><div class="card"><div class="card-body" id="setup-wifi">
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

function setupLogin() {
  render(`<h1>Web UI login</h1><div class="card"><div class="card-body">
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

async function setupDone() {
  frame(false, "3 / 3");
  render(`<h1>Done</h1><div class="card"><div class="card-body" id="done">Checking the Wi-Fi ...</div></div>`);
  const draw = async () => {
    const s = await api("/api/status");
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
  await draw();
  refresh(draw, 3000);
}

// -------------------------------------------------------------------------------------- login

function loginPage() {
  frame(false);
  render(`<h1>Log in</h1><div class="card"><div class="card-body"><form id="login-form">
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
const later = (title, text) => () => render(`<h1>${esc(title)}</h1>${card(title, `<p class="text-body-secondary mb-0">${esc(text)}</p>`)}`);

// The editor (web/dashboard.js, docs/ui/web-dashboard.md) is loaded on the first visit of the page.
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
      rows = facts([["Revision", d.revision], ["Tiles", (d.tiles || []).length]]) +
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
        `${r.pages} pages, ${r.tiles} tiles` + (r.ignored ? ` (ignored: ${r.ignored})` : "");
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
  return facts(rows);
}

const views = {
  async status() {
    render(`<h1>Status</h1><div id="status-cards">Loading ...</div>`);
    const draw = async () => {
      const s = await updateHeader();
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
        ]));
    };
    await draw();
    refresh(draw, 3000);
  },

  async wifi() {
    render(`<h1>Wi-Fi</h1><div id="wifi-saved">Loading ...</div>
      <div class="card"><div class="card-body"><h2 class="h6 card-title d-flex">Add a network
        <button id="rescan" class="btn btn-sm btn-outline-secondary ms-auto" type="button">Scan</button></h2>
        <div id="wifi-add"></div></div></div>`);
    let w = await api("/api/wifi");
    const drawSaved = () => {
      const rows = w.saved.map((n, i) => `
        <li class="list-group-item d-flex align-items-center gap-2">
          <span class="text-body-secondary">${i + 1}</span>
          <span class="flex-grow-1 text-truncate">${esc(n.ssid)}</span>
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
        const b = event.target.closest("button[data-index],button[data-forget]");
        if (!b) return;
        busy(b, async () => {
          if (b.dataset.forget !== undefined) {
            const ssid = w.saved[Number(b.dataset.forget)].ssid;
            if (!confirm(`Forget ${ssid}? The device no longer connects to it.`)) return;
            await api("/api/wifi/remove", { index: b.dataset.forget });
          } else {
            await api("/api/wifi/move", { index: b.dataset.index, delta: b.dataset.move });
          }
          await sleep(300);
          w = await api("/api/wifi");
          drawSaved();
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
        await sleep(300);
        w = await api("/api/wifi");
        drawSaved();
        drawAdd();
      });
    };
    drawSaved();
    drawAdd();
    $("#rescan").onclick = (e) => busy(e.target, async () => { w = await scanNetworks(); drawAdd(); });
  },

  async hotspot() {
    render(`<h1>Hotspot</h1><div id="hotspot">Loading ...</div>`);
    const draw = async () => {
      const h = await api("/api/hotspot");
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
      $("#hotspot-on").onchange = (event) => api("/api/hotspot", { on: event.target.checked ? "1" : "0" })
        .then(() => sleep(500)).then(draw, (e) => toast(e.message, "danger"));
      $("#new-password").onclick = (e) => busy(e.target, async () => {
        if (!confirm("New hotspot password? Devices on the hotspot have to join again.")) return;
        await api("/api/hotspot/password", {});
        await sleep(500);
        await draw();
      });
    };
    await draw();
    refresh(draw, 5000);
  },

  async ha() {
    render(`<h1>Home Assistant</h1><div id="ha-status">Loading ...</div><div id="ha-form"></div>`);
    let h;
    try {
      h = await api("/api/hass");
    } catch (e) {
      if (e.status !== 404) throw e;
      render(`<h1>Home Assistant</h1>${card("Home Assistant", '<p class="text-body-secondary mb-0">The recovery app has no Home Assistant connection. Start the firmware (System).</p>')}`);
      return;
    }
    const drawStatus = () => {
      const box = $("#ha-status");
      if (!box) return;
      const retry = h.state === "token_rejected" || h.state === "disconnected";
      box.innerHTML = card("Connection", hassFacts(h) + (retry
        ? '<div class="d-flex justify-content-end"><button id="ha-reconnect" class="btn btn-sm btn-outline-secondary" type="button">Try now</button></div>' : ""));
      const button = $("#ha-reconnect");
      if (button) button.onclick = (e) => busy(e.target, async () => { h = await api("/api/hass/reconnect", {}); drawStatus(); });
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
      views.ha();
    });
    drawStatus();
    refresh(async () => { h = await api("/api/hass"); drawStatus(); }, 2000);
  },
  async dashboard() {
    if ((await api("/api/update")).app === "recovery") {
      render(`<h1>Dashboard</h1>${card("Dashboard", '<p class="text-body-secondary mb-0">The recovery app has no dashboard. Start the firmware (System).</p>')}`);
      return;
    }
    let tab = "editor";
    try { tab = sessionStorage.getItem("dashboard-tab") || tab; } catch (e) {}
    render(`<div class="d-flex flex-wrap align-items-center gap-3 mb-3"><h1 class="mb-0">Dashboard</h1>
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
  logs: later("Logs", "The log view comes later."),

  async system() {
    const [s, u] = await Promise.all([api("/api/status"), api("/api/update")]);
    render(`<h1>System</h1>` +
      card("Device", facts([
        ["Firmware", esc(s.device.firmware)], ["Uptime", duration(s.device.uptime_s)],
        ["Free memory", `${s.device.heap_internal_kb} KB internal, ${s.device.heap_psram_kb} KB PSRAM`],
        ["Firmware update", u.firmware_direct ? "direct" : "through the recovery app"],
      ])) +
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
          <button id="reset" class="btn btn-outline-danger ms-auto" type="button">Factory reset</button>
        </div>`));
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
    $("#reset").onclick = (e) => busy(e.target, async () => {
      if (!confirm("Factory reset? All settings, Wi-Fi networks and the web UI login are deleted.")) return;
      if (!confirm("Really? The device starts like new, with its hotspot.")) return;
      await api("/api/factory-reset", { confirm: "yes" });
      toast("erased, restarting ...", "warning");
    });
  },
};

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
  clearInterval(refreshTimer);
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

$("#menu-pages").innerHTML = menuPages.map(([page, title]) =>
  `<a class="list-group-item list-group-item-action" href="#/${page}" data-page="${page}">${esc(title)}</a>`).join("");
$("#menu-pages").addEventListener("click", closeMenuOnPhone);
$("#menu-button").onclick = () => ($("#menu").hidden = !$("#menu").hidden);

const theme = $("#theme");
theme.value = localStorage.getItem("theme") || "auto";
function applyTheme() {
  const dark = theme.value === "dark" || (theme.value === "auto" && matchMedia("(prefers-color-scheme: dark)").matches);
  document.documentElement.setAttribute("data-bs-theme", dark ? "dark" : "light");
}
theme.onchange = () => {
  localStorage.setItem("theme", theme.value);
  applyTheme();
};
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", applyTheme);

$("#logout").onclick = async () => {
  await api("/api/logout", {}).catch(() => {});
  go("login");
};
$("#logout-all").onclick = async () => {
  if (!confirm("Log out on every browser and device?")) return;
  await api("/api/logout-all", {}).catch(() => {});
  go("login");
};

window.addEventListener("hashchange", route);
route();
