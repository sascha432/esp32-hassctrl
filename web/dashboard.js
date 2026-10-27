// The dashboard editor of the web UI (docs/ui/web/web-dashboard-editor.md). Loaded by the Dashboard
// page only, so the setup on a phone stays small. It edits the JSON of GET /api/dashboard as it is
// and sends it back whole; the device checks it (POST ...&check=1 a second after each change) and
// stores it (POST with the revision it was read at, 409 when the screen or an import was faster).
//
// It copies rules of the firmware: the placement of core::finish() (components/core/src/layout.cpp:
// place(), has_back_tile(), swipe_chain()), the kinds of the domains. A change of the dashboard in
// core or config is a change here too, and the web image goes onto the devices with the firmware
// (docs/architecture.md section 5). The device's check is the truth. Icons come from web/icons.svg.
// The preview shows no live values (decided 2026-10-09).
"use strict";

window.hcEditor = (() => {
  const esc = (v) => String(v ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
  const clone = (v) => JSON.parse(JSON.stringify(v));
  // the document as the device takes it: no local keys, no cells of an answer
  const plain = (v) => JSON.parse(JSON.stringify(v, (k, x) => (k === "_k" || k === "cells" ? undefined : x)));

  // the device's screen in landscape (GET /api/dashboard/editor); margin 8, the dot band of a swipe
  // area 16 px (docs/ui/lvgl/lvgl-dashboard.md sections 2 and 9)
  let screen = { name: "", w: 480, h: 320, gap: 4 };
  // 24 px with 9 px dots, 32 px with 12 px dots on a screen of 800 px (swipe.cpp: large fonts)
  const dotsHeight = () => (Math.max(screen.w, screen.h) >= 800 ? 32 : 24);
  const dotSize = () => (Math.max(screen.w, screen.h) >= 800 ? 12 : 9);

  // ------------------------------------------------------------------------------- icons
  // web/icons.svg (tools/build_icons.py): one <symbol id="mdi-<name>"> per icon name, put into the
  // page once when the editor opens.
  const svg = (name, px) =>
    name === "none"
      ? `<svg width="${px}" height="${px}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M5 5l14 14" stroke-dasharray="2 3"/></svg>`
      : `<svg width="${px}" height="${px}" viewBox="0 0 24 24" fill="currentColor"><use href="#mdi-${name}"/></svg>`;
  async function loadSprite() {
    if (document.getElementById("hc-icons")) return;
    try {
      const response = await fetch("icons.svg");
      if (!response.ok) return;
      const holder = document.createElement("div");
      holder.id = "hc-icons";
      holder.hidden = true;
      holder.innerHTML = await response.text();
      document.body.appendChild(holder);
    } catch (e) { /* the icons stay empty; the editor works without them */ }
  }

  const STYLE = `
  .ed { display: grid; gap: 1rem; grid-template-columns: minmax(0, 1fr); align-items: start; }
  .ed-tree-list { display: none; }
  @media (min-width: 800px) {
    .ed { grid-template-columns: 200px minmax(0, 1fr); }
    .ed-insp { grid-column: 1 / -1; }
    .ed-tree-list { display: block; }
    .ed-tree-select { display: none; }
  }
  @media (min-width: 1280px) { .ed { grid-template-columns: 210px minmax(0, 1fr) 330px; } .ed-insp { grid-column: auto; } }
  .tree .list-group-item { cursor: pointer; display: flex; justify-content: space-between; align-items: center; gap: .5rem; }
  .tree .list-group-item.drop-target { outline: 2px dashed var(--bs-primary); outline-offset: -2px; }
  .ed-bar { display: flex; flex-wrap: wrap; gap: .5rem; align-items: center; }
  .ed-row { display: flex; flex-wrap: wrap; gap: .5rem 1rem; align-items: center; margin-bottom: .75rem; }
  .ed-row .form-select { width: auto; }
  .insp .form-label { margin-bottom: .2rem; font-size: .875rem; color: var(--bs-secondary-color); }
  .insp .row-field { margin-bottom: .6rem; }
  .icon-chip { display: inline-flex; align-items: center; gap: .4rem; }
  .problem { cursor: pointer; }
  .board-wrap { position: relative; width: 100%; max-width: 760px; }
  .board-scale { position: relative; overflow: hidden; }
  .board { position: absolute; left: 0; top: 0; transform-origin: 0 0; background: #0b0f14; color: #fff; user-select: none; overflow: hidden; }
  .tile { position: absolute; background: #182029; border: 1px solid #2b3844; border-radius: 8px; overflow: hidden; cursor: pointer;
          display: flex; flex-direction: column; align-items: center; justify-content: center; text-align: center; color: #7fa8c9; gap: 4px; }
  .tile .nm { padding: 0 6px; max-width: 100%; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .tile.wide { flex-direction: row; gap: 10px; }
  .tile.wide .nm { white-space: normal; text-align: left; }
  .tile.pic { background: linear-gradient(160deg, #3b5a6e 0%, #1f3240 45%, #4a3b2a 100%); justify-content: flex-end; }
  .tile.pic .nm { width: 100%; background: rgba(0,0,0,.55); color: #fff; padding: 3px 0; }
  .tile.spacer { background: transparent; border: 1px dashed #2b3844; color: #4d6274; }
  .tile.err { box-shadow: inset 0 0 0 3px #ff0000; }
  .tile.sel { box-shadow: 0 0 0 3px #00ffff; z-index: 2; }
  .tile.drop-before::before, .tile.drop-after::before { content: ""; position: absolute; top: 0; bottom: 0; width: 5px; background: #00ffff; }
  .tile.drop-before::before { left: 0; }
  .tile.drop-after::before { right: 0; }
  .tile.pinned::after { content: ""; position: absolute; right: 5px; top: 5px; width: 7px; height: 7px; border-radius: 50%; background: #00ffff; }
  .tile .kind { font-size: 11px; color: #4d6274; }
  .tile .clock { font-weight: 500; color: #fff; line-height: 1.1; }
  .cell-empty { position: absolute; border: 1px dashed #2b3844; border-radius: 8px; display: flex; align-items: center; justify-content: center;
                color: #4d6274; font-size: 22px; cursor: pointer; }
  .cell-empty:hover { color: #00ffff; border-color: #00ffff; }
  .dots { position: absolute; left: 0; right: 0; bottom: 0; display: flex; justify-content: center; align-items: center; }
  .dots span { border-radius: 50%; background: #7fa8c9; opacity: .5; }
  .dots span.on { background: #fff; opacity: 1; }
  .hl { position: absolute; border-radius: 8px; pointer-events: none; z-index: 5; }
  .hl.ok { border: 3px solid #2ecc71; background: rgba(46,204,113,.15); }
  .hl.no { border: 3px solid #ff0000; background: rgba(255,0,0,.12); }
  .tray { display: flex; flex-wrap: wrap; gap: .4rem; align-items: center; min-height: 2.4rem; padding: .35rem .5rem;
          border: 1px dashed var(--bs-border-color); border-radius: .375rem; }
  .tray.drop-target { outline: 2px dashed var(--bs-primary); outline-offset: -2px; }
  .chip { display: inline-flex; align-items: center; gap: .35rem; padding: .15rem .55rem; border-radius: 1rem; cursor: grab;
          background: #182029; color: #7fa8c9; border: 1px solid #2b3844; font-size: .8rem; user-select: none; }
  .chip.sel { box-shadow: 0 0 0 2px #00ffff; }
  .chip .badge { font-size: .65rem; }
  .ed-modal { display: block; }
  .ent-list { max-height: 40vh; overflow-y: auto; }
  .ent-list .list-group-item { cursor: pointer; }
  .icon-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(96px, 1fr)); gap: .4rem; max-height: 50vh; overflow-y: auto; }
  .icon-grid button { display: flex; flex-direction: column; align-items: center; gap: .25rem; font-size: .7rem; padding: .5rem .25rem; }`;

  // ---------------------------------------------------------------------------------- state
  let root = null;   // the element the editor draws into
  let ctx = null;    // {toast, login} of app.js
  let K = 1;         // local keys: a new tile has no id until the device gives it one
  let doc = null, saved = null, undoStack = [], dirty = false;
  let pageKey = 0, sel = null, orient = "l";
  let icons = [];    // the device sends the names (/api/dashboard/editor)
  let zones = [];    // the time zone names of a clock tile, from the device too
  // the folder picker of a media tile: where it is ({src: "hass"|"sd", id, title} per level)
  let mediaPath = [];
  let check = { state: "ok", text: "", k: null }, checkTimer = null, checkSeq = 0;
  let addCell = null, drag = null;
  // the entity picker: the tile whose entity it replaces (null: a new tile), the newest search,
  // the friendly names Home Assistant answered (the Name field's placeholder)
  let pickFor = null, pickSeq = 0, pickTimer = null;
  const haNames = new Map();

  const $ = (s) => root.querySelector(s);

  function keyAll(tiles) {
    for (const t of tiles) {
      if (!t._k) t._k = K++;
      delete t.cells;
      if (t.tiles) keyAll(t.tiles);
    }
  }

  async function request(method, path, body) {
    const response = await fetch(path, {
      method, cache: "no-store",
      headers: body !== undefined ? { "Content-Type": "application/json" } : {},
      body: body !== undefined ? JSON.stringify(body) : undefined,
    });
    const answer = await response.json().catch(() => ({}));
    if (response.status === 401) {
      ctx.login();
      throw new Error("login required");
    }
    return { status: response.status, answer };
  }

  // --------------------------------------------------------------------------------- model
  function kindOf(t) {
    if (t.type) return t.type;
    const d = (t.entity || "").split(".")[0];
    if (d === "light") return "light";
    if (["switch", "input_boolean", "fan"].includes(d)) return "switch";
    if (["automation", "script"].includes(d)) return "automation";
    if (["button", "input_button", "scene"].includes(d)) return "button";
    if (d === "climate") return "climate";
    if (d === "camera") return "picture";
    if (d === "lock") return "lock";
    if (d === "weather") return "weather";
    if (d === "timer") return "timer";
    return "sensor";
  }
  const okey = (o) => (o === "p" ? "portrait" : "landscape");
  const oname = (o) => (o === "p" ? "portrait" : "landscape");
  const other = (o) => (o === "p" ? "l" : "p");
  // the placement of a tile in an orientation: {size, cell, hidden}, every key optional
  const side = (t, o) => t[okey(o)] || {};
  const sideW = (t, o) => (t[okey(o)] ||= {});
  // an empty map is not written
  const tidy = (t) => { for (const o of ["l", "p"]) if (t[okey(o)] && !Object.keys(t[okey(o)]).length) delete t[okey(o)]; };
  // core::finish(): 1x1 in landscape, the landscape size turned in portrait, unless set
  const landscapeSize = (t) => side(t, "l").size || [1, 1];
  const sizeOf = (t, o) => side(t, o).size || (o === "p" ? [landscapeSize(t)[1], landscapeSize(t)[0]] : landscapeSize(t));
  const hiddenIn = (t, o) => !!side(t, o).hidden;
  const validEntity = (id) => /^[a-z0-9_]+\.[a-z0-9_]+$/.test(id) && id.length <= 64;

  function pages() {
    const out = [{ key: 0, name: "Main page", tiles: doc.tiles, area: null, depth: 0, parent: null }];
    (function rec(tiles, parent, depth) {
      for (const t of tiles) if (t.type === "area") {
        out.push({ key: t._k, name: t.name || "(no name)", tiles: t.tiles, area: t, depth, parent });
        rec(t.tiles, t._k, depth + 1);
      }
    })(doc.tiles, 0, 1);
    return out;
  }
  const pageByKey = (k) => pages().find((p) => p.key === k) || pages()[0];
  // core::swipe_page() and has_back_tile(): an area with `swipe` has no back tile on its page
  const swipePage = (p) => !!(p.area && p.area.swipe);
  const hasBackTile = (p) => p.depth >= 1 && !swipePage(p);
  // core::swipe_chain(): the pages the dots of `p` stand for (one entry: no dots)
  function swipeChain(p) {
    const chain = [p];
    for (let q = p; swipePage(q) && q.parent !== null;) { q = pageByKey(q.parent); chain.unshift(q); }
    return chain;
  }
  function locate(k) {
    for (const p of pages()) {
      const i = p.tiles.findIndex((t) => t._k === k);
      if (i >= 0) return { page: p, index: i, tile: p.tiles[i] };
    }
    return null;
  }
  // the tile at a path of the device's answer (config::tile_path)
  function atPath(path) {
    let tiles = doc.tiles, tile = null;
    for (const i of path || []) {
      tile = tiles && tiles[i];
      if (!tile) return null;
      tiles = tile.tiles;
    }
    return tile;
  }
  const gridOf = (p) => (p.area && p.area.grid) || doc.defaults.grid;
  function countTiles(tiles) { let n = 0; for (const t of tiles) { n++; if (t.tiles) n += countTiles(t.tiles); } return n; }
  function displayName(t) {
    if (t.name) return t.name;
    if (t.type === "spacer") return "Spacer";
    if (t.type === "clock") return "Clock";
    if (t.type === "media") return "Media";
    if (t.type === "area") return "(no name)";
    return haNames.get(t.entity) || (t.entity || "?").split(".")[1] || t.entity;
  }
  function isInside(key, areaK) {
    for (let p = pageByKey(key); p; p = p.parent === null ? null : pageByKey(p.parent)) {
      if (p.key === areaK) return true;
      if (p.parent === null) return false;
    }
    return false;
  }

  // core::finish()'s placement (layout.cpp place()): per orientation the tiles with a cell first,
  // then first-fit; a tile that cannot take its cell or finds no block is not on the screen
  // (res.out[k]: "room"; hidden tiles "hidden")
  function place(p, o) {
    const g = gridOf(p);
    const C = o === "p" ? g[1] : g[0], R = o === "p" ? g[0] : g[1];
    const used = Array.from({ length: R }, () => Array(C).fill(null));
    const res = { C, R, cells: {}, out: {}, arranged: false, used };
    const fits = (c, r, w, h) => {
      if (c < 0 || r < 0 || c + w > C || r + h > R) return false;
      for (let y = r; y < r + h; y++) for (let x = c; x < c + w; x++) if (used[y][x] !== null) return false;
      return true;
    };
    const take = (c, r, w, h, k) => { for (let y = r; y < r + h; y++) for (let x = c; x < c + w; x++) used[y][x] = k; };
    if (hasBackTile(p)) take(0, 0, 1, 1, "back");
    for (const t of p.tiles) {
      const cell = side(t, o).cell;
      if (!cell) continue;
      res.arranged = true;
      if (hiddenIn(t, o)) { res.out[t._k] = "hidden"; continue; }
      const [w, h] = sizeOf(t, o), [c, r] = cell;
      if (fits(c, r, w, h)) { take(c, r, w, h, t._k); res.cells[t._k] = { c, r, w, h }; } else res.out[t._k] = "room";
    }
    for (const t of p.tiles) {
      if (side(t, o).cell) continue;
      if (hiddenIn(t, o)) { res.out[t._k] = "hidden"; continue; }
      const [w, h] = sizeOf(t, o);
      let found = null;
      outer: for (let r = 0; r + h <= R; r++) for (let c = 0; c + w <= C; c++) if (fits(c, r, w, h)) { found = { c, r, w, h }; break outer; }
      if (found) { take(found.c, found.r, w, h, t._k); res.cells[t._k] = found; } else res.out[t._k] = "room";
    }
    return res;
  }

  // core::move_tile: the page gets its current cells in `o`, the tiles in the way are pushed aside.
  // A tile that is not on the screen (hidden, no room) can be dropped onto a cell as well: it is
  // shown in `o` from then on (the tray of the editor).
  function arrangeMove(p, o, k, c, r) {
    const t = p.tiles.find((x) => x._k === k);
    delete side(t, o).hidden;
    const pl = place(p, o), [w, h] = sizeOf(t, o);
    if (c < 0 || r < 0 || c + w > pl.C || r + h > pl.R) return false;
    if (hasBackTile(p) && c === 0 && r === 0) return false;
    const inBlock = (q) => q.c < c + w && q.c + q.w > c && q.r < r + h && q.r + q.h > r;
    const placed = p.tiles.filter((x) => pl.cells[x._k] && x._k !== k);
    const pushed = placed.filter((x) => inBlock(pl.cells[x._k]));
    const used = Array.from({ length: pl.R }, () => Array(pl.C).fill(false));
    const take = (cc, rr, ww, hh) => { for (let y = rr; y < rr + hh; y++) for (let x = cc; x < cc + ww; x++) used[y][x] = true; };
    const free = (cc, rr, ww, hh) => {
      if (cc + ww > pl.C || rr + hh > pl.R) return false;
      for (let y = rr; y < rr + hh; y++) for (let x = cc; x < cc + ww; x++) if (used[y][x]) return false;
      return true;
    };
    if (hasBackTile(p)) take(0, 0, 1, 1);
    for (const x of placed) if (!pushed.includes(x)) { const q = pl.cells[x._k]; take(q.c, q.r, q.w, q.h); }
    take(c, r, w, h);
    const moved = new Map(), start = r * pl.C + c, n = pl.C * pl.R;
    for (const x of pushed) {
      const q = pl.cells[x._k];
      let found = null;
      for (let i = 0; i < n && !found; i++) {
        const at = (start + i) % n, cc = at % pl.C, rr = Math.floor(at / pl.C);
        if (free(cc, rr, q.w, q.h)) found = [cc, rr];
      }
      if (!found) return false;
      take(found[0], found[1], q.w, q.h);
      moved.set(x._k, found);
    }
    for (const x of placed) sideW(x, o).cell = moved.get(x._k) || [pl.cells[x._k].c, pl.cells[x._k].r];
    sideW(t, o).cell = [c, r];
    tidy(t);
    return true;
  }
  const tryMove = (p, o, k, c, r) => arrangeMove({ ...p, tiles: clone(p.tiles) }, o, k, c, r);
  function resetArrangement(p, o) { for (const t of p.tiles) { delete side(t, o).cell; tidy(t); } }

  function stripForKind(t) {
    const k = kindOf(t);
    if (k !== "light") delete t.dimmer;
    if (k !== "area") delete t.swipe;
    if (k !== "light" && k !== "climate") delete t.step;
    if (k !== "sensor") { delete t.unit; delete t.decimals; }
    if (k !== "picture") { delete t.refresh; delete t.stream; }
    if (k !== "clock") for (const key of ["zone", "hours", "seconds", "date", "style", "border", "ticks"]) delete t[key];
    if (k === "clock" && t.style !== "analog") { delete t.border; delete t.ticks; }
    if (k !== "media") delete t.folder;
  }
  function insertTile(p, t, cell) {
    if (cell) {
      const pl = place(p, orient);
      if (pl.arranged) {
        p.tiles.push(t);
        const [w, h] = sizeOf(t, orient);
        let ok = true;
        for (let y = cell.r; y < cell.r + h; y++) for (let x = cell.c; x < cell.c + w; x++) if (y >= pl.R || x >= pl.C || pl.used[y][x] !== null) ok = false;
        if (ok) sideW(t, orient).cell = [cell.c, cell.r];
        return;
      }
      const at = cell.r * pl.C + cell.c;
      const i = p.tiles.findIndex((x) => pl.cells[x._k] && pl.cells[x._k].r * pl.C + pl.cells[x._k].c > at);
      if (i >= 0) { p.tiles.splice(i, 0, t); return; }
    }
    p.tiles.push(t);
  }

  // ------------------------------------------------------------------------- change, check
  function change(fn) {
    undoStack.push(JSON.stringify(doc));
    if (undoStack.length > 50) undoStack.shift();
    fn();
    dirty = true;
    scheduleCheck();
    draw();
  }

  function scheduleCheck() {
    check = { state: "checking", text: "", k: null };
    clearTimeout(checkTimer);
    checkTimer = setTimeout(runCheck, 1000);
  }
  async function runCheck() {
    const seq = ++checkSeq;
    try {
      const { status, answer } = await request("POST", `/api/dashboard?revision=${doc.revision}&check=1`, plain(doc));
      if (seq !== checkSeq) return; // a newer check is on its way
      if (status === 200) check = { state: "ok", text: "", k: null };
      else check = { state: "error", text: answer.error || `error ${status}`, k: atPath(answer.path)?._k ?? null };
    } catch (e) {
      if (seq === checkSeq) check = { state: "error", text: e.message, k: null };
    }
    drawToolbar();
  }

  async function load() {
    const { status, answer } = await request("GET", "/api/dashboard");
    if (status === 404) {
      doc = { format: "hassctrl-dashboard", version: 2, revision: 0, next_id: 1, defaults: { grid: [4, 2] }, tiles: [] };
    } else if (status !== 200) {
      throw new Error(answer.error || `error ${status}`);
    } else {
      doc = answer;
    }
    keyAll(doc.tiles);
    saved = clone(doc);
    undoStack = [];
    dirty = false;
    check = { state: "ok", text: "", k: null };
    if (!pages().some((p) => p.key === pageKey)) pageKey = 0;
    if (sel && !locate(sel)) sel = null;
  }

  async function save(overwrite = false) {
    let revision = doc.revision;
    if (overwrite) {
      const current = await request("GET", "/api/dashboard");
      revision = current.status === 200 ? current.answer.revision : 0;
    }
    const { status, answer } = await request("POST", `/api/dashboard?revision=${revision}`, plain(doc));
    if (status === 409) {
      $("#ed-conflict").innerHTML = `<div class="alert alert-warning d-flex flex-wrap align-items-center gap-2 py-2">
        <span class="flex-grow-1">The dashboard was changed on the device (the screen or an import) after this page loaded it.</span>
        <button class="btn btn-sm btn-outline-secondary" data-act="reload">Reload (drop my changes)</button>
        <button class="btn btn-sm btn-warning" data-act="overwrite">Overwrite</button></div>`;
      return;
    }
    if (status !== 200) {
      check = { state: "error", text: answer.error || `error ${status}`, k: atPath(answer.path)?._k ?? null };
      drawToolbar();
      ctx.toast(check.text, "danger");
      return;
    }
    // the stored dashboard: new tiles have their ids now; the local keys follow by position
    const keys = [];
    (function collect(tiles) { for (const t of tiles) { keys.push(t._k); if (t.tiles) collect(t.tiles); } })(doc.tiles);
    doc = answer.dashboard;
    let i = 0;
    (function give(tiles) { for (const t of tiles) { t._k = keys[i++] || K++; delete t.cells; if (t.tiles) give(t.tiles); } })(doc.tiles);
    saved = clone(doc);
    dirty = false;
    check = { state: "ok", text: "", k: null };
    ctx.toast(`Saved as revision ${doc.revision}. The screen shows it now.`, "success");
    draw();
  }

  // ------------------------------------------------------------------------------- the look
  function autoIcon(t) {
    const k = kindOf(t), e = t.entity || "";
    if (k === "area") return "folder";
    if (k === "clock") return "none"; // the time is the picture
    if (k === "media") return "music";
    if (k === "light") return "lightbulb";
    if (k === "climate") return "thermostat";
    if (k === "picture") return "camera";
    if (k === "weather") return "weather-partly-cloudy";
    if (k === "timer") return "timer-outline";
    const d = e.split(".")[0];
    if (d === "fan") return "fan";
    if (d === "scene") return "palette";
    if (d === "script") return "script-text";
    if (d === "automation") return "robot";
    if (d === "lock") return "lock";
    if (d === "button" || d === "input_button") return "gesture-tap-button";
    if (/temperature/.test(e)) return "thermometer";
    if (/humidity/.test(e)) return "water-percent";
    if (/power|energy/.test(e)) return "flash";
    if (/co2/.test(e)) return "molecule-co2";
    if (/motion|presence/.test(e)) return "motion-sensor";
    if (/door/.test(e)) return "door-closed";
    if (/window/.test(e)) return "window-closed";
    if (k === "switch") return "toggle-switch";
    return "eye";
  }
  const iconOf = (t) => t.icon || autoIcon(t);

  function geometry(p, o) {
    const W = o === "p" ? screen.h : screen.w, H = o === "p" ? screen.w : screen.h, gap = screen.gap;
    const pl = place(p, o);
    // the dot band of a swipe page takes the bottom, like the device
    const bottom = swipeChain(p).length >= 2 ? dotsHeight() : 0;
    const cw = (W - 16 - gap * (pl.C - 1)) / pl.C, ch = (H - bottom - 16 - gap * (pl.R - 1)) / pl.R;
    const box = (c, r, w, h) => ({ x: 8 + c * (cw + gap), y: 8 + r * (ch + gap), w: w * cw + (w - 1) * gap, h: h * ch + (h - 1) * gap });
    const cellAt = (x, y) => ({ c: Math.floor((x - 8 + gap / 2) / (cw + gap)), r: Math.floor((y - 8 + gap / 2) / (ch + gap)) });
    // the fonts follow the size of the card (tiles.cpp): a tall card needs twice the height
    const large = (w, h, tall) => w >= 90 && (tall ? h / 2 : h) >= 90;
    return { W, H, pl, cw, ch, box, cellAt, large: large(cw, ch, false), largeFor: large };
  }

  function tileHTML(t, b, large, flags) {
    const k = kindOf(t), nm = esc(displayName(t));
    const attrs = `data-k="${t._k}" draggable="true" style="left:${b.x}px;top:${b.y}px;width:${b.w}px;height:${b.h}px"`;
    const name = `<div class="nm" style="font-size:${large ? 16 : 12}px">${nm}</div>`;
    if (k === "spacer") return `<div class="tile spacer ${flags}" ${attrs}><div class="nm" style="font-size:12px">spacer</div></div>`;
    if (k === "picture") return `<div class="tile pic ${flags}" ${attrs}>${name}</div>`;
    if (k === "clock") {
      // the preview shows no live values (decided): a fixed time, or the clock glyph for a face
      const icon = t.icon && t.icon !== "none" ? svg(t.icon, 24) : "";
      const face = t.style === "analog" ? svg("clock-outline", large ? 44 : 24)
        : `<div class="clock" style="font-size:${large ? 28 : 20}px">${t.hours === 12 ? "9:41" : "09:41"}${t.seconds ? ":00" : ""}${t.hours === 12 ? ' <span style="font-size:60%;color:#7fa8c9">AM</span>' : ""}</div>`;
      const date = t.date ? `<div class="kind">Tue 13 Oct</div>` : "";
      return `<div class="tile ${flags}" ${attrs}>${icon}${face}${date}${t.name ? name : ""}<div class="kind">${t.zone ? esc(t.zone) : "clock"}</div></div>`;
    }
    const icon = iconOf(t) === "none" ? "" : svg(iconOf(t), large ? 44 : 24);
    const hint = k === "light" && t.dimmer ? "dimmer" : k === "area" ? "area" : k === "media" ? "media" : "";
    return `<div class="tile ${flags}" ${attrs}>${icon}${name}${hint ? `<div class="kind">${hint}</div>` : ""}</div>`;
  }

  function boardHTML(p, o) {
    const g = geometry(p, o), pl = g.pl;
    let html = "";
    if (hasBackTile(p)) html += `<div class="tile" data-back="1" style="left:8px;top:8px;width:${g.cw}px;height:${g.ch}px">${svg("arrow-left", g.large ? 44 : 24)}</div>`;
    for (let r = 0; r < pl.R; r++) for (let c = 0; c < pl.C; c++) if (pl.used[r][c] === null) {
      const b = g.box(c, r, 1, 1);
      html += `<div class="cell-empty" data-act="add" data-c="${c}" data-r="${r}" title="Add a tile here" style="left:${b.x}px;top:${b.y}px;width:${b.w}px;height:${b.h}px">+</div>`;
    }
    for (const t of p.tiles) {
      const cell = pl.cells[t._k];
      if (!cell) continue;
      let flags = "";
      if (sel === t._k) flags += " sel";
      if (side(t, o).cell) flags += " pinned";
      // a tile wider than tall puts the icon beside the text (tiles.cpp, the wide shape)
      if (cell.w > cell.h && kindOf(t) !== "picture") flags += " wide";
      if (check.state === "error" && check.k === t._k) flags += " err";
      const b = g.box(cell.c, cell.r, cell.w, cell.h);
      html += tileHTML(t, b, g.largeFor(b.w, b.h, cell.h > cell.w && b.h >= 180), flags);
    }
    const chain = swipeChain(p);
    if (chain.length >= 2) {
      const d = dotSize();
      html += `<div class="dots" style="height:${dotsHeight()}px;gap:${d * 2 / 3}px">${chain.map((x) => `<span class="${x.key === p.key ? "on" : ""}" style="width:${d}px;height:${d}px"></span>`).join("")}</div>`;
    }
    return { html, g };
  }

  // ---------------------------------------------------------------------------------- draw
  function draw() {
    if (!root || !doc) return;
    root.innerHTML = `
      <div class="ed-bar mb-2" id="ed-toolbar"></div>
      <div id="ed-conflict"></div>
      <div id="ed-body"></div>`;
    drawToolbar();
    drawEditor();
  }

  function drawToolbar() {
    const el = root && root.querySelector("#ed-toolbar");
    if (!el) return;
    const badge = check.state === "checking" ? '<span class="badge text-bg-secondary">checking ...</span>'
      : check.state === "error" ? `<span class="badge text-bg-danger problem" data-act="goto-problem" title="${esc(check.text)}">problem</span>`
      : '<span class="badge text-bg-success">valid</span>';
    el.innerHTML = `
      <span class="text-body-secondary small">revision ${doc.revision}${dirty ? ' · <span class="text-warning-emphasis fw-semibold">unsaved</span>' : ""}</span>
      ${badge}
      <span class="flex-grow-1"></span>
      <button class="btn btn-sm btn-outline-secondary" data-act="undo" ${undoStack.length ? "" : "disabled"}>Undo</button>
      <button class="btn btn-sm btn-outline-secondary" data-act="discard" ${dirty ? "" : "disabled"}>Discard</button>
      <button class="btn btn-sm btn-primary" data-act="save" ${dirty ? "" : "disabled"}>Save</button>`;
    const pb = root.querySelector("#ed-problem");
    if (pb) pb.innerHTML = check.state === "error" ? `<div class="alert alert-danger py-2 mb-0 small problem" data-act="goto-problem">${esc(check.text)}</div>` : "";
    root.querySelectorAll(".tile.err").forEach((x) => x.classList.remove("err"));
    if (check.state === "error" && check.k) root.querySelectorAll(`#ed-board .tile[data-k="${check.k}"]`).forEach((x) => x.classList.add("err"));
  }

  // a grid of 1x1 to 32x32 (core::kMaxGrid), a tile of 1x1 to 8x8 (kMaxTileSize)
  const MAX_GRID = 32, MAX_TILE = 8;
  const gridSelect = (ctl, value, disabled) => `<select class="form-select form-select-sm" data-ctl="${ctl}" ${disabled ? "disabled" : ""}>
    ${Array.from({ length: MAX_GRID }, (_, i) => i + 1).map((n) => `<option ${n === value ? "selected" : ""}>${n}</option>`).join("")}</select>`;

  // the tiles of `p` that are not on the screen of `o`: hidden there, or without room
  function trayHTML(p, o) {
    const pl = place(p, o);
    const out = p.tiles.filter((t) => pl.out[t._k] && pl.out[t._k] !== "swipe");
    const chips = out.map((t) => `<span class="chip ${sel === t._k ? "sel" : ""}" draggable="true" data-k="${t._k}" data-tray="1">
      ${iconOf(t) === "none" || kindOf(t) === "spacer" ? "" : svg(iconOf(t), 16)}${esc(displayName(t))}
      <span class="badge ${pl.out[t._k] === "hidden" ? "text-bg-secondary" : "text-bg-warning"}">${pl.out[t._k] === "hidden" ? "hidden" : "no room"}</span></span>`).join("");
    return `<div class="small text-body-secondary mt-2 mb-1">Not on the screen in ${oname(o)}
        <span class="text-body-tertiary">· drag a tile here to hide it, onto a cell to show it</span></div>
      <div class="tray" id="ed-tray">${chips || '<span class="small text-body-tertiary">every tile is on the screen</span>'}</div>`;
  }

  function drawEditor() {
    const p = pageByKey(pageKey);
    pageKey = p.key;
    const all = pages(), g = gridOf(p), pl = place(p, orient);
    const tree = all.map((x) => `
      <div class="list-group-item list-group-item-action ${x.key === p.key ? "active" : ""}" data-act="page" data-page="${x.key}" style="padding-left:${0.75 + x.depth}rem">
        <span class="text-truncate">${esc(x.name)}</span>
        <span class="small ${x.key === p.key ? "" : "text-body-secondary"}">${x.tiles.length}</span>
      </div>`).join("");
    const gridCtl = p.key === 0
      ? `<span class="d-inline-flex align-items-center gap-1 small">Default grid ${gridSelect("gcols", g[0])}x${gridSelect("grows", g[1])}</span>`
      : `<span class="d-inline-flex align-items-center gap-1 small"><span class="form-check form-check-inline mb-0 me-1">
           <input class="form-check-input" type="checkbox" id="ed-owngrid" data-ctl="owngrid" ${p.area.grid ? "checked" : ""}>
           <label class="form-check-label" for="ed-owngrid">own grid</label></span>
           ${gridSelect("gcols", g[0], !p.area.grid)}x${gridSelect("grows", g[1], !p.area.grid)}</span>`;
    $("#ed-body").innerHTML = `
      <div class="ed">
        <div><div class="card mb-0"><div class="card-body p-2">
          <div class="d-flex justify-content-between align-items-center px-1 mb-2">
            <span class="small fw-semibold">Pages</span>
            <button class="btn btn-sm btn-outline-secondary py-0" data-act="add-area" ${p.depth >= 5 ? "disabled" : ""}>+ Area</button>
          </div>
          <div class="list-group list-group-flush tree ed-tree-list">${tree}</div>
          <select class="form-select form-select-sm ed-tree-select" data-ctl="page">
            ${all.map((x) => `<option value="${x.key}" ${x.key === p.key ? "selected" : ""}>${"  ".repeat(x.depth)}${esc(x.name)}</option>`).join("")}
          </select>
          <p class="small text-body-secondary px-1 mt-2 mb-0">Drop a tile on a page to move it there.</p>
        </div></div></div>
        <div>
          <div class="ed-row">
            <span class="fw-semibold">${esc(p.name)}</span>
            ${gridCtl}
            <div class="btn-group btn-group-sm" role="group">
              <button class="btn btn-outline-secondary ${orient === "l" ? "active" : ""}" data-act="orient" data-o="l">Landscape</button>
              <button class="btn btn-outline-secondary ${orient === "p" ? "active" : ""}" data-act="orient" data-o="p">Portrait</button>
            </div>
          </div>
          <div class="ed-row small">
            ${pl.arranged
              ? `<span class="badge text-bg-info">arranged in ${oname(orient)}</span>
                 <button class="btn btn-sm btn-outline-secondary py-0" data-act="reset-arr">Reset to automatic order</button>
                 <span class="text-body-secondary">Drag a tile to a cell; the tiles in the way are pushed aside.</span>`
              : `<span class="badge text-bg-secondary">automatic order</span>
                 <span class="text-body-secondary">Drag onto a tile to take its place in the order; onto an empty cell to arrange the page.</span>`}
          </div>
          <div class="board-wrap" id="ed-wrap"><div class="board-scale"><div class="board" id="ed-board"></div></div></div>
          ${trayHTML(p, orient)}
          <div id="ed-problem" class="mt-2"></div>
          <div class="d-flex flex-wrap gap-2 mt-2">
            <button class="btn btn-sm btn-outline-primary" data-act="add">+ Tile</button>
            <button class="btn btn-sm btn-outline-secondary" data-act="add-area" ${p.depth >= 5 ? "disabled" : ""}>+ Area</button>
            <button class="btn btn-sm btn-outline-secondary" data-act="add-spacer">+ Spacer</button>
            <button class="btn btn-sm btn-outline-secondary" data-act="add-clock">+ Clock</button>
            <button class="btn btn-sm btn-outline-secondary" data-act="add-media">+ Media</button>
          </div>
        </div>
        <div class="ed-insp"><div class="card mb-0"><div class="card-body insp" id="ed-insp"></div></div></div>
      </div>`;
    const b = boardHTML(p, orient);
    const el = $("#ed-board");
    el.innerHTML = b.html;
    el.style.width = b.g.W + "px";
    el.style.height = b.g.H + "px";
    fitBoard();
    wireDrag(el, p);
    drawInspector();
    drawToolbar();
  }

  function fitBoard() {
    const wrap = root && root.querySelector("#ed-wrap");
    if (!wrap) return;
    const W = orient === "p" ? screen.h : screen.w, H = orient === "p" ? screen.w : screen.h;
    const s = Math.min(wrap.clientWidth / W, (innerHeight * 0.62) / H, 1.8);
    const scale = wrap.querySelector(".board-scale");
    scale.style.width = W * s + "px";
    scale.style.height = H * s + "px";
    scale.querySelector(".board").style.transform = `scale(${s})`;
    wrap.dataset.s = s;
  }

  // the size select of one orientation: 1x1 up to the grid of the page in that orientation
  function sizeSelect(p, t, o) {
    const g = gridOf(p), C = o === "p" ? g[1] : g[0], R = o === "p" ? g[0] : g[1];
    const own = side(t, o).size, cur = sizeOf(t, o);
    const options = [];
    for (let r = 1; r <= Math.min(R, MAX_TILE); r++) for (let c = 1; c <= Math.min(C, MAX_TILE); c++) options.push([c, r]);
    if (own && !options.some(([c, r]) => c === own[0] && r === own[1])) options.push(own); // larger than the grid: kept, not shown
    const auto = o === "p" ? `landscape turned (${cur[0]}x${cur[1]})` : "1x1";
    return `<select class="form-select form-select-sm" data-field="size-${o}">
        <option value="">${own ? auto : `${auto} ✓`}</option>
        ${options.map(([c, r]) => `<option value="${c}x${r}" ${own && own[0] === c && own[1] === r ? "selected" : ""}>${c}x${r}</option>`).join("")}
      </select>`;
  }

  function drawInspector() {
    const el = $("#ed-insp");
    const loc = sel ? locate(sel) : null;
    if (!loc) {
      const p = pageByKey(pageKey);
      el.innerHTML = `<h2 class="h6">${esc(p.name)}</h2>
        <p class="small text-body-secondary">${p.tiles.length} tiles on this page. Select a tile in the preview or in the tray to edit it,
        click an empty cell to add one there. A second click on a selected area opens its page.</p>
        ${p.area ? `<button class="btn btn-sm btn-outline-secondary" data-act="select" data-k="${p.area._k}">Edit the area tile</button>` : ""}`;
      return;
    }
    const t = loc.tile, k = kindOf(t), p = loc.page;
    const field = (label, html, hint = "") => `<div class="row-field"><label class="form-label">${label}</label>${html}${hint ? `<div class="form-text mt-0">${hint}</div>` : ""}</div>`;
    const cellText = (o) => {
      const pl = place(p, o), c = pl.cells[t._k], v = side(t, o).cell;
      if (c && v) return `<b>${v[0]},${v[1]}</b> (arranged)`;
      if (c) return `auto, ${c.c},${c.r}`;
      const why = pl.out[t._k] === "hidden" ? "hidden" : "no room";
      const text = `<span class="text-warning-emphasis">${why}${v ? `, cell ${v[0]},${v[1]}` : ""}</span>`;
      return o === orient ? text : `<a href="#" data-act="orient" data-o="${o}">${text}, show ${oname(o)}</a>`;
    };
    const movable = pages().filter((x) => x.key !== p.key && !(t.type === "area" && isInside(x.key, t._k)));
    const known = [...entitiesInUse()].sort();
    let body = "";
    if (k !== "area" && k !== "spacer" && k !== "clock" && k !== "media") {
      body += field("Entity", `<div class="input-group input-group-sm">
          <input class="form-control" list="ed-ents" data-field="entity" value="${esc(t.entity)}" spellcheck="false">
          <button class="btn btn-outline-secondary" data-act="pick-entity">Pick</button></div>
        <datalist id="ed-ents">${known.map((x) => `<option value="${x}">`).join("")}</datalist>`, `${k} tile`);
    }
    if (k !== "spacer") {
      body += field("Name", `<input class="form-control form-control-sm" data-field="name" maxlength="32" value="${esc(t.name || "")}" placeholder="${k === "area" ? "required" : k === "clock" ? "none (a title under the time)" : esc(haNames.get(t.entity) || "the friendly name of Home Assistant")}">`);
      body += field("Icon", `<button class="btn btn-sm btn-outline-secondary icon-chip" data-act="icon">
        ${t.icon === "none" ? "none" : svg(iconOf(t), 20) + " " + esc(t.icon || "automatic")}</button>`);
    }
    if (k === "area") {
      body += field("Page", `<button class="btn btn-sm btn-outline-secondary" data-act="page" data-page="${t._k}">Open its page (${t.tiles.length} tiles)</button>`);
      body += `<div class="row-field form-check"><input class="form-check-input" type="checkbox" id="ed-swipe" data-field="swipe" ${t.swipe ? "checked" : ""}>
        <label class="form-check-label small" for="ed-swipe">Swipe navigation: no back tile on its page, a swipe to the right goes back, dots show the depth</label></div>`;
    }
    if (k === "light") {
      body += `<div class="row-field form-check"><input class="form-check-input" type="checkbox" id="ed-dim" data-field="dimmer" ${t.dimmer ? "checked" : ""}>
        <label class="form-check-label small" for="ed-dim">Dimmer (level fill, drag sets the level)</label></div>`;
    }
    if (k === "clock") {
      const check = (id, label, on) => `<div class="form-check"><input class="form-check-input" type="checkbox" id="${id}" data-field="${id.slice(3)}" ${on ? "checked" : ""}>
        <label class="form-check-label small" for="${id}">${label}</label></div>`;
      body += `<div class="d-flex gap-2">${field("Style", `<select class="form-select form-select-sm" data-field="style">
          <option value="" ${t.style !== "analog" ? "selected" : ""}>digital</option><option value="analog" ${t.style === "analog" ? "selected" : ""}>analog</option></select>`)}
        ${field("Hours", `<select class="form-select form-select-sm" data-field="hours">
          <option value="" ${t.hours !== 12 ? "selected" : ""}>24 h</option><option value="12" ${t.hours === 12 ? "selected" : ""}>12 h, AM/PM</option></select>`)}</div>`;
      body += `<div class="row-field d-flex flex-wrap gap-3">${check("ed-seconds", t.style === "analog" ? "Second hand" : "Seconds", t.seconds)}${check("ed-date", "Date under the time", t.date)}
        ${t.style === "analog" ? check("ed-border", "Rim", t.border) : ""}</div>`;
      if (t.style === "analog") {
        body += field("Ticks", `<select class="form-select form-select-sm" data-field="ticks">
          ${["none", "quarter", "hour", "minute"].map((x) => `<option value="${x === "hour" ? "" : x}" ${(t.ticks || "hour") === x ? "selected" : ""}>${x}</option>`).join("")}</select>`);
      }
      body += field("Time zone", `<input class="form-control form-control-sm" list="ed-zones" data-field="zone" value="${esc(t.zone || "")}" placeholder="the device's (from Home Assistant)" spellcheck="false">
        <datalist id="ed-zones">${zones.map((x) => `<option value="${x}">`).join("")}</datalist>`, "an IANA name, Europe/London");
    }
    if (k === "media") {
      body += field("Folder", `<div class="input-group input-group-sm">
          <input class="form-control" data-field="folder" value="${esc(t.folder || "")}" placeholder="none: Home Assistant and SD card" spellcheck="false">
          <button class="btn btn-outline-secondary" data-act="pick-folder">Pick</button></div>`,
        "where the browser starts: a folder of Home Assistant's media library (media-source://...) or of the SD card (music/albums)");
    }
    body += field(`Size in ${oname(orient)}`, sizeSelect(p, t, orient),
      orient === "p" ? "columns x rows of the turned screen; without one the landscape size turned" : "columns x rows; portrait turns it unless set there");
    body += `<div class="row-field d-flex gap-3">
      <div class="form-check"><input class="form-check-input" type="checkbox" id="ed-hide-l" data-field="hidden-l" ${hiddenIn(t, "l") ? "checked" : ""}>
        <label class="form-check-label small" for="ed-hide-l">Hidden in landscape</label></div>
      <div class="form-check"><input class="form-check-input" type="checkbox" id="ed-hide-p" data-field="hidden-p" ${hiddenIn(t, "p") ? "checked" : ""}>
        <label class="form-check-label small" for="ed-hide-p">Hidden in portrait</label></div></div>`;
    if (k === "light") body += field("Step of + and − (%)", `<input class="form-control form-control-sm" type="number" min="1" max="50" data-field="step" value="${t.step ?? ""}" placeholder="default">`);
    if (k === "climate") body += field("Step (°)", `<input class="form-control form-control-sm" type="number" min="0.1" max="5" step="0.1" data-field="step" value="${t.step ?? ""}" placeholder="from Home Assistant">`);
    if (k === "sensor") {
      body += `<div class="d-flex gap-2">${field("Unit", `<input class="form-control form-control-sm" maxlength="12" data-field="unit" value="${esc(t.unit || "")}" placeholder="from HA">`)}
        ${field("Decimals", `<select class="form-select form-select-sm" data-field="decimals"><option value="">auto</option>
          ${[0, 1, 2, 3, 4, 5, 6].map((n) => `<option ${t.decimals === n ? "selected" : ""}>${n}</option>`).join("")}</select>`)}</div>`;
    }
    if (k === "picture") {
      body += field("Refresh (s)", `<input class="form-control form-control-sm" type="number" min="1" max="3600" data-field="refresh" value="${t.refresh ?? ""}" placeholder="60">`);
      body += field("Stream", `<input class="form-control form-control-sm" data-field="stream" value="${esc(t.stream || "")}" placeholder="none · hass · http://..." spellcheck="false">`,
        "<code>hass</code>: Home Assistant's MJPEG stream; or an MJPEG address (go2rtc).");
    }
    body += field("Position", `<div class="small">Landscape: ${cellText("l")}<br>Portrait: ${cellText("p")}</div>`);
    el.innerHTML = `
      <div class="d-flex justify-content-between align-items-start mb-2">
        <h2 class="h6 mb-0 text-truncate">${esc(displayName(t))}</h2>
        <span class="badge text-bg-secondary">${k}${t.id ? " · id " + t.id : " · new"}</span>
      </div>
      ${body}
      <div class="d-flex flex-wrap gap-2 mt-3">
        <button class="btn btn-sm btn-outline-secondary" data-act="up" ${loc.index === 0 ? "disabled" : ""} title="Earlier in the order">Up</button>
        <button class="btn btn-sm btn-outline-secondary" data-act="down" ${loc.index === p.tiles.length - 1 ? "disabled" : ""} title="Later in the order">Down</button>
        <button class="btn btn-sm btn-outline-secondary" data-act="dup">Duplicate</button>
        <select class="form-select form-select-sm w-auto" data-ctl="moveto"><option value="">Move to ...</option>
          ${movable.map((x) => `<option value="${x.key}">${esc(x.name)}</option>`).join("")}</select>
        <button class="btn btn-sm btn-outline-danger ms-auto" data-act="del">Remove</button>
      </div>`;
  }

  function entitiesInUse() {
    const used = new Set();
    (function rec(ts) { for (const t of ts) { if (t.entity) used.add(t.entity); if (t.tiles) rec(t.tiles); } })(doc.tiles);
    return used;
  }

  // -------------------------------------------------------------------------------- dragging
  // Tiles move on the board (a reorder in automatic order, a cell in an arranged page), from the
  // tray onto a cell (shown again) and from the board into the tray (hidden in this orientation).
  function wireDrag(el, p) {
    const s = () => +$("#ed-wrap").dataset.s;
    const g = () => geometry(p, orient);
    const pointerCell = (e) => { const r = el.getBoundingClientRect(); return g().cellAt((e.clientX - r.left) / s(), (e.clientY - r.top) / s()); };
    const forward = (k, target) => p.tiles.findIndex((x) => x._k === k) < p.tiles.findIndex((x) => x._k === target);
    const clearMarks = () => {
      el.querySelectorAll(".drop-before, .drop-after").forEach((x) => x.classList.remove("drop-before", "drop-after"));
      root.querySelector("#ed-hl")?.remove();
      root.querySelector("#ed-tray")?.classList.remove("drop-target");
    };
    el.addEventListener("dragstart", (e) => {
      const t = e.target.closest("[data-k]");
      if (!t) return;
      const k = +t.dataset.k, cell = place(p, orient).cells[k], pc = pointerCell(e);
      drag = { k, dc: pc.c - cell.c, dr: pc.r - cell.r, fromTray: false };
      e.dataTransfer.effectAllowed = "move";
      e.dataTransfer.setData("text/plain", String(k));
    });
    el.addEventListener("dragend", () => { drag = null; clearMarks(); });
    el.addEventListener("dragover", (e) => {
      if (!drag) return;
      e.preventDefault();
      clearMarks();
      const pl = place(p, orient), pc = pointerCell(e), under = pl.used[pc.r]?.[pc.c];
      if (!pl.arranged && !drag.fromTray && typeof under === "number" && under !== drag.k) {
        el.querySelector(`.tile[data-k="${under}"]`)?.classList.add(forward(drag.k, under) ? "drop-after" : "drop-before");
        return;
      }
      const t = locate(drag.k).tile, [w, h] = sizeOf(t, orient), c = pc.c - drag.dc, r = pc.r - drag.dr;
      const b = g().box(c, r, w, h);
      el.insertAdjacentHTML("beforeend", `<div id="ed-hl" class="hl ${tryMove(p, orient, drag.k, c, r) ? "ok" : "no"}" style="left:${b.x}px;top:${b.y}px;width:${b.w}px;height:${b.h}px"></div>`);
    });
    el.addEventListener("drop", (e) => {
      if (!drag) return;
      e.preventDefault();
      const d = drag;
      drag = null;
      const pl = place(p, orient), pc = pointerCell(e), under = pl.used[pc.r]?.[pc.c];
      if (!pl.arranged && !d.fromTray && typeof under === "number" && under !== d.k) {
        change(() => {
          const to = p.tiles.findIndex((x) => x._k === under);
          const [t] = p.tiles.splice(p.tiles.findIndex((x) => x._k === d.k), 1);
          p.tiles.splice(to, 0, t);
        });
        return;
      }
      if (!tryMove(p, orient, d.k, pc.c - d.dc, pc.r - d.dr)) { clearMarks(); ctx.toast("The tile does not fit there.", "warning"); return; }
      change(() => { arrangeMove(pageByKey(p.key), orient, d.k, pc.c - d.dc, pc.r - d.dr); sel = d.k; });
      if (!pl.arranged) ctx.toast(`The page is arranged in ${oname(orient)} now.`, "info");
    });
    const tray = root.querySelector("#ed-tray");
    if (!tray) return;
    tray.addEventListener("dragstart", (e) => {
      const chip = e.target.closest("[data-k]");
      if (!chip) return;
      drag = { k: +chip.dataset.k, dc: 0, dr: 0, fromTray: true };
      e.dataTransfer.effectAllowed = "move";
      e.dataTransfer.setData("text/plain", chip.dataset.k);
    });
    tray.addEventListener("dragend", () => { drag = null; clearMarks(); });
    tray.addEventListener("dragover", (e) => {
      if (!drag || drag.fromTray) return;
      e.preventDefault();
      clearMarks();
      tray.classList.add("drop-target");
    });
    tray.addEventListener("drop", (e) => {
      if (!drag || drag.fromTray) return;
      e.preventDefault();
      const k = drag.k;
      drag = null;
      clearMarks();
      change(() => { sideW(locate(k).tile, orient).hidden = true; sel = k; });
      ctx.toast(`Hidden in ${oname(orient)}.`, "info");
    });
  }

  function moveToPage(k, target) {
    const loc = locate(k);
    if (loc.tile.type === "area" && isInside(target, k)) { ctx.toast("An area cannot go into itself.", "warning"); return; }
    change(() => {
      const [t] = loc.page.tiles.splice(loc.index, 1);
      for (const o of ["l", "p"]) delete side(t, o).cell;
      tidy(t);
      pageByKey(target).tiles.push(t);
    });
    ctx.toast(`Moved to ${pageByKey(target).name}.`, "success");
  }

  // ---------------------------------------------------------------------------------- modals
  function modal(title, body, size = "") {
    closeModal();
    const box = document.createElement("div");
    box.id = "ed-modal";
    box.innerHTML = `
      <div class="modal ed-modal" tabindex="-1" data-act="modal-bg"><div class="modal-dialog modal-dialog-scrollable ${size}"><div class="modal-content">
        <div class="modal-header"><h5 class="modal-title">${title}</h5><button type="button" class="btn-close" data-act="close"></button></div>
        <div class="modal-body">${body}</div>
      </div></div></div><div class="modal-backdrop show"></div>`;
    root.appendChild(box);
  }
  function closeModal() { clearTimeout(pickTimer); root.querySelector("#ed-modal")?.remove(); }

  // ------------------------------------------------------------- entity picker (section 2.3)
  // The device searches Home Assistant (GET /api/hass/entities); without a connection the list
  // falls back to the entities on the dashboard, and a typed entity id works always.
  const PICK_DOMAINS = ["light", "switch", "sensor", "binary_sensor", "climate", "camera", "fan", "input_boolean",
    "button", "input_button", "scene", "script", "automation", "lock", "cover", "media_player", "weather", "timer"];

  function openPicker(cell, replaceK = null) {
    addCell = cell;
    pickFor = replaceK;
    const current = replaceK ? locate(replaceK).tile.entity || "" : "";
    modal(replaceK ? "Entity of the tile" : "Add a tile", `
      <div class="d-flex gap-2 mb-2">
        <select class="form-select form-select-sm w-auto" id="ed-dom"><option value="">all domains</option>
          ${PICK_DOMAINS.map((d) => `<option ${current.startsWith(d + ".") ? "selected" : ""}>${d}</option>`).join("")}</select>
        <input class="form-control form-control-sm" id="ed-ent" placeholder="Search a name or an entity id" spellcheck="false" autocomplete="off">
      </div>
      <div class="small text-body-secondary mb-2" id="ed-ent-note">Searching Home Assistant ...</div>
      <div class="list-group ent-list mb-3" id="ed-ent-list"></div>
      ${replaceK ? "" : `<div class="d-flex gap-2"><button class="btn btn-sm btn-outline-secondary" data-act="add-area">+ Area</button>
        <button class="btn btn-sm btn-outline-secondary" data-act="add-spacer">+ Spacer</button>
        <button class="btn btn-sm btn-outline-secondary" data-act="add-clock">+ Clock</button>
        <button class="btn btn-sm btn-outline-secondary" data-act="add-media">+ Media</button></div>`}`, "modal-lg");
    const input = root.querySelector("#ed-ent");
    input.addEventListener("input", () => { clearTimeout(pickTimer); pickTimer = setTimeout(search, 300); });
    input.addEventListener("keydown", (e) => {
      if (e.key !== "Enter") return;
      clearTimeout(pickTimer);
      // Enter takes the first row once the list shows this search
      search().then(() => { const first = root.querySelector("#ed-ent-list [data-act=pick]"); if (first) actions.pick(first); });
    });
    root.querySelector("#ed-dom").addEventListener("change", search);
    input.focus();
    search();
  }

  async function search() {
    const seq = ++pickSeq;
    const input = root.querySelector("#ed-ent"), dom = root.querySelector("#ed-dom")?.value || "";
    if (!input) return;
    const q = input.value.trim();
    let rows = null, more = false, problem = "";
    try {
      const { status, answer } = await request("GET", `/api/hass/entities?domain=${encodeURIComponent(dom)}&q=${encodeURIComponent(q)}`);
      if (status === 200 && Array.isArray(answer.rows)) { rows = answer.rows; more = !!answer.more; } else problem = answer.error || `error ${status}`;
    } catch (e) {
      problem = e.message;
    }
    if (seq !== pickSeq || !root.querySelector("#ed-ent-list")) return; // a newer search, or closed
    const used = entitiesInUse();
    if (rows) {
      for (const [id, name] of rows) if (name) haNames.set(id, name);
    } else {
      // without Home Assistant: the entities of the dashboard
      const words = q.toLowerCase().split(/\s+/).filter(Boolean);
      rows = [...used].sort().filter((id) => (!dom || id.startsWith(dom + ".")) && words.every((w) => id.includes(w))).map((id) => [id, haNames.get(id) || ""]);
    }
    // a typed entity id that is not in the list can be taken as it is
    const typed = q.toLowerCase();
    const extra = validEntity(typed) && !rows.some((r) => r[0] === typed) ? [[typed, "", null, null, null, true]] : [];
    root.querySelector("#ed-ent-note").innerHTML = problem
      ? `<span class="text-warning-emphasis">Home Assistant: ${esc(problem)}.</span> The list shows the entities of the dashboard; a typed entity id works too.`
      : more ? `The first ${rows.length} matches. Type more to narrow the search.` : `${rows.length} ${rows.length === 1 ? "entity" : "entities"} in Home Assistant.`;
    root.querySelector("#ed-ent-list").innerHTML = [...extra, ...rows].map(([id, name, , dc, unit, isTyped]) => `
      <div class="list-group-item list-group-item-action d-flex align-items-center gap-2" data-act="pick" data-id="${esc(id)}">
        <span class="icon-chip">${svg(autoIcon({ entity: id }), 20)}</span>
        <span class="flex-grow-1 text-truncate">${isTyped ? `<span class="text-body-secondary">Use</span> ${esc(id)}`
          : `${esc(name || id)}<br><span class="small text-body-secondary">${esc(id)}${dc ? " · " + esc(dc) : ""}${unit ? " · " + esc(unit) : ""}</span>`}</span>
        ${used.has(id) ? '<span class="badge text-bg-info">on the dashboard</span>' : ""}
        <span class="badge text-bg-secondary">${kindOf({ entity: id })}</span></div>`).join("") ||
      '<div class="list-group-item text-body-secondary small">No match.</div>';
  }

  // what a tile takes over from its entity: a light dims unless it only knows on and off (keys
  // equal to the automatic value are left out, the tile follows Home Assistant); every new tile
  // is 1x1 (decided 2026-10-14)
  async function pick(id) {
    let info = null;
    try {
      const { status, answer } = await request("GET", `/api/hass/entity?id=${encodeURIComponent(id)}`);
      if (status === 200) info = answer;
    } catch (e) { /* taken without the details */ }
    if (info && info.missing) ctx.toast(`Home Assistant has no ${id}; the tile shows it as unavailable until it does.`, "warning");
    if (info && info.name) haNames.set(id, info.name);
    const known = info && !info.missing;
    const dimmable = known && Array.isArray(info.color_modes) && info.color_modes.some((m) => m !== "onoff");
    const apply = (t) => {
      t.entity = id;
      if (kindOf(t) === "light" && known) { if (dimmable) t.dimmer = true; else delete t.dimmer; }
      stripForKind(t);
    };
    if (pickFor) {
      const k = pickFor;
      closeModal();
      change(() => apply(locate(k).tile));
      return;
    }
    const t = { _k: K++ };
    apply(t);
    addTile(t);
  }

  function addTile(t) {
    const p = pageByKey(pageKey);
    change(() => { insertTile(p, t, addCell); sel = t._k; });
    closeModal();
  }

  // The folder picker of a media tile: the two tops, then folder by folder through Home Assistant's
  // media library (GET /api/hass/media) or the SD card (GET /api/sd/list); "Use this folder" takes
  // the one shown, "Both tops" clears the folder.
  const natural = new Intl.Collator(undefined, { numeric: true, sensitivity: "base" }).compare;
  function openFolderPicker() {
    mediaPath = [];
    modal("Folder of the media tile", `<div class="small text-body-secondary mb-2" id="ed-mp-where"></div>
      <div class="list-group ent-list mb-3" id="ed-mp-list"></div>
      <div class="d-flex gap-2"><button class="btn btn-sm btn-primary" data-act="mp-use" id="ed-mp-use">Use this folder</button>
        <button class="btn btn-sm btn-outline-secondary" data-act="mp-both">None: both tops</button></div>`, "modal-lg");
    drawFolderPicker();
  }
  async function drawFolderPicker() {
    const list = root.querySelector("#ed-mp-list");
    if (!list) return;
    const here = mediaPath[mediaPath.length - 1];
    root.querySelector("#ed-mp-where").textContent = here ? mediaPath.map((x) => x.title).join(" / ") : "Home Assistant's media library or the SD card";
    root.querySelector("#ed-mp-use").disabled = !here;
    const row = (act, data, text) => `<a href="#" class="list-group-item list-group-item-action" data-act="${act}" ${data}>${text}</a>`;
    const up = here ? row("mp-up", "", "..") : "";
    if (!here) {
      list.innerHTML = row("mp-open", `data-src="hass" data-id="" data-title="Home Assistant"`, "Home Assistant") +
        row("mp-open", `data-src="sd" data-id="" data-title="SD card"`, "SD card");
      return;
    }
    list.innerHTML = up + `<div class="list-group-item text-body-secondary">Loading ...</div>`;
    let folders = [];
    try {
      if (here.src === "hass") {
        const r = await request("GET", "/api/hass/media?id=" + encodeURIComponent(here.id));
        if (r.status !== 200) throw new Error((r.answer && r.answer.error) || "Home Assistant did not answer");
        folders = r.answer.folders.map(([id, title]) => ({ id, title }));
      } else {
        const r = await request("GET", "/api/sd/list?dir=" + encodeURIComponent(here.id));
        if (r.status !== 200) throw new Error((r.answer && r.answer.error) || "no SD card");
        folders = r.answer.entries.filter((e) => e.dir).map((e) => ({ id: (here.id ? here.id + "/" : "") + e.name, title: e.name }))
          .sort((a, b) => natural(a.title, b.title));
      }
    } catch (e) {
      if (mediaPath[mediaPath.length - 1] === here) list.innerHTML = up + `<div class="list-group-item text-warning-emphasis">${esc(e.message)}</div>`;
      return;
    }
    // a newer step replaced this one while it waited
    if (mediaPath[mediaPath.length - 1] !== here) return;
    list.innerHTML = up + (folders.length ? folders.map((f) => row("mp-open", `data-src="${here.src}" data-id="${esc(f.id)}" data-title="${esc(f.title)}"`, esc(f.title))).join("")
      : `<div class="list-group-item text-body-secondary">No folders in it</div>`);
  }

  function openIcons() {
    const t = locate(sel).tile;
    modal("Icon", `<input class="form-control form-control-sm mb-2" id="ed-icon-q" placeholder="Search">
      <div class="icon-grid" id="ed-icon-grid"></div>
      <p class="small text-body-secondary mt-2 mb-0">The names the firmware holds.</p>`, "modal-lg");
    const drawGrid = () => {
      const q = root.querySelector("#ed-icon-q").value.toLowerCase();
      root.querySelector("#ed-icon-grid").innerHTML =
        `<button class="btn btn-sm ${!t.icon ? "btn-primary" : "btn-outline-secondary"}" data-act="set-icon" data-icon="">${svg(autoIcon(t), 26)}automatic</button>
         <button class="btn btn-sm ${t.icon === "none" ? "btn-primary" : "btn-outline-secondary"}" data-act="set-icon" data-icon="none">${svg("none", 26)}none</button>` +
        icons.filter((n) => n.includes(q)).map((n) => `<button class="btn btn-sm ${t.icon === n ? "btn-primary" : "btn-outline-secondary"}" data-act="set-icon" data-icon="${esc(n)}">${svg(n, 26)}${esc(n)}</button>`).join("");
    };
    drawGrid();
    root.querySelector("#ed-icon-q").addEventListener("input", drawGrid);
  }

  // --------------------------------------------------------------------------------- actions
  const actions = {
    page: (el) => { pageKey = +el.dataset.page; sel = null; draw(); },
    select: (el) => { const loc = locate(+el.dataset.k); if (!loc) return; pageKey = loc.page.key; sel = +el.dataset.k; draw(); },
    orient: (el) => { orient = el.dataset.o; draw(); },
    undo: () => {
      if (!undoStack.length) return;
      doc = JSON.parse(undoStack.pop());
      dirty = JSON.stringify(doc) !== JSON.stringify(saved);
      if (sel && !locate(sel)) sel = null;
      scheduleCheck();
      draw();
    },
    discard: () => { doc = clone(saved); undoStack = []; dirty = false; sel = null; check = { state: "ok", text: "", k: null }; draw(); },
    save: (el) => { el.disabled = true; save().catch((e) => ctx.toast(e.message, "danger")).finally(() => { el.disabled = false; }); },
    reload: async () => { await load(); ctx.toast("Loaded the dashboard of the device.", "info"); draw(); },
    overwrite: () => save(true).catch((e) => ctx.toast(e.message, "danger")),
    "goto-problem": () => { if (check.k) actions.select({ dataset: { k: check.k } }); },
    add: (el) => openPicker(el.dataset.c !== undefined ? { c: +el.dataset.c, r: +el.dataset.r } : null),
    "pick-entity": () => openPicker(null, sel),
    "pick-folder": () => openFolderPicker(),
    "mp-open": (el) => { mediaPath.push({ src: el.dataset.src, id: el.dataset.id, title: el.dataset.title }); drawFolderPicker(); },
    "mp-up": () => { mediaPath.pop(); drawFolderPicker(); },
    "mp-use": () => {
      const here = mediaPath[mediaPath.length - 1];
      // the top of the library has no id of its own: its first level is media-source://
      const folder = here.src === "hass" ? here.id || "media-source://" : here.id || "/";
      closeModal();
      change(() => { const t = locate(sel).tile; if (folder) t.folder = folder; else delete t.folder; });
    },
    "mp-both": () => { closeModal(); change(() => { delete locate(sel).tile.folder; }); },
    pick: (el) => {
      if (el.classList.contains("disabled")) return;
      root.querySelectorAll("#ed-ent-list [data-act=pick]").forEach((x) => x.classList.add("disabled"));
      pick(el.dataset.id).catch((e) => ctx.toast(e.message, "danger"));
    },
    "add-area": () => {
      if (!root.querySelector("#ed-modal")) addCell = null;
      // its page opens: an area needs a tile, and the next "+ Tile" belongs there
      const t = { _k: K++, type: "area", name: "New area", tiles: [] };
      addTile(t);
      pageKey = t._k;
      sel = null;
      draw();
      ctx.toast("The new area's page is open: add its tiles here (an area needs at least one before it can be saved).", "info");
    },
    "add-spacer": () => { if (!root.querySelector("#ed-modal")) addCell = null; addTile({ _k: K++, type: "spacer" }); },
    "add-clock": () => { if (!root.querySelector("#ed-modal")) addCell = null; addTile({ _k: K++, type: "clock" }); },
    "add-media": () => { if (!root.querySelector("#ed-modal")) addCell = null; addTile({ _k: K++, type: "media" }); },
    "reset-arr": () => change(() => resetArrangement(pageByKey(pageKey), orient)),
    icon: () => openIcons(),
    "set-icon": (el) => { const v = el.dataset.icon; closeModal(); change(() => { const t = locate(sel).tile; if (v) t.icon = v; else delete t.icon; }); },
    up: () => change(() => { const l = locate(sel); l.page.tiles.splice(l.index - 1, 0, l.page.tiles.splice(l.index, 1)[0]); }),
    down: () => change(() => { const l = locate(sel); l.page.tiles.splice(l.index + 1, 0, l.page.tiles.splice(l.index, 1)[0]); }),
    dup: () => change(() => {
      const l = locate(sel), c = clone(l.tile);
      // the copy keeps size and hidden, not the cell: it would overlap the original
      (function strip(t) { delete t.id; for (const o of ["l", "p"]) delete side(t, o).cell; tidy(t); t._k = K++; if (t.tiles) t.tiles.forEach(strip); })(c);
      l.page.tiles.splice(l.index + 1, 0, c);
      sel = c._k;
    }),
    del: () => {
      const l = locate(sel), n = l.tile.tiles ? countTiles(l.tile.tiles) : 0;
      if (n && !confirm(`Remove ${displayName(l.tile)} and its ${n} tiles?`)) return;
      change(() => { l.page.tiles.splice(l.index, 1); sel = null; if (!pages().some((x) => x.key === pageKey)) pageKey = l.page.key; });
    },
    close: () => closeModal(),
    "modal-bg": (el, e) => { if (e.target === el) closeModal(); },
  };

  function onClick(e) {
    const el = e.target.closest("[data-act]");
    if (!el) {
      const t = e.target.closest("#ed-board [data-k], #ed-tray [data-k]");
      if (t) {
        const k = +t.dataset.k;
        if (sel === k && locate(k)?.tile.type === "area") { pageKey = k; sel = null; } else sel = k;
        draw();
      } else if (e.target.closest("#ed-board [data-back]")) {
        pageKey = pageByKey(pageKey).parent ?? 0;
        sel = null;
        draw();
      }
      return;
    }
    if (el.tagName === "A") e.preventDefault();
    actions[el.dataset.act]?.(el, e);
  }

  function onChange(e) {
    const el = e.target;
    if (el.dataset.field) return setField(el.dataset.field, el);
    const ctl = el.dataset.ctl;
    if (!ctl) return;
    const p = pageByKey(pageKey);
    if (ctl === "page") { pageKey = +el.value; sel = null; draw(); }
    if (ctl === "moveto" && el.value) moveToPage(sel, +el.value);
    if (ctl === "owngrid") change(() => { if (el.checked) p.area.grid = [...doc.defaults.grid]; else delete p.area.grid; });
    if (ctl === "gcols" || ctl === "grows") {
      // a cell outside the new grid puts its tile into the tray, nothing else changes
      const g = [...gridOf(p)];
      g[ctl === "gcols" ? 0 : 1] = +el.value;
      change(() => { if (p.key === 0) doc.defaults.grid = g; else p.area.grid = g; });
    }
  }

  function setField(f, el) {
    change(() => {
      const t = locate(sel).tile, v = el.type === "checkbox" ? el.checked : el.value.trim();
      if (f === "entity") { t.entity = v; stripForKind(t); return; }
      if (f === "dimmer") { if (v) t.dimmer = true; else delete t.dimmer; stripForKind(t); return; }
      if (f === "swipe") { if (v) t.swipe = true; else delete t.swipe; return; }
      if (["seconds", "date", "border"].includes(f)) { if (v) t[f] = true; else delete t[f]; return; }
      if (f === "hours") { if (v) t.hours = 12; else delete t.hours; return; }
      if (f === "style" || f === "ticks") { if (v) t[f] = v; else delete t[f]; stripForKind(t); return; }
      if (f === "folder") {
        // "/" is the card's root, a media-source:// id stays as it is
        const x = v.startsWith("media-source://") || /^\/+$/.test(v) ? (v.startsWith("/") ? "/" : v) : v.replace(/^\/+|\/+$/g, "");
        if (x) t.folder = x; else delete t.folder;
        return;
      }
      if (f === "size-l" || f === "size-p") {
        const o = f.slice(-1);
        if (v) sideW(t, o).size = v.split("x").map(Number); else delete side(t, o).size;
        tidy(t);
        return;
      }
      if (f === "hidden-l" || f === "hidden-p") {
        const o = f.slice(-1);
        if (v) sideW(t, o).hidden = true; else delete side(t, o).hidden;
        tidy(t);
        return;
      }
      if (["step", "decimals", "refresh"].includes(f)) { if (v === "") delete t[f]; else t[f] = Number(v); return; }
      if (v === "") delete t[f]; else t[f] = v;
    });
  }

  // the page tree takes tiles too: a drop moves the tile to that page
  function onTreeDragOver(e) {
    const item = e.target.closest?.(".tree [data-page]");
    root.querySelectorAll(".tree .drop-target").forEach((x) => x.classList.remove("drop-target"));
    if (!item || !drag || +item.dataset.page === pageKey) return;
    e.preventDefault();
    item.classList.add("drop-target");
  }
  function onTreeDrop(e) {
    const item = e.target.closest?.(".tree [data-page]");
    if (!item || !drag) return;
    e.preventDefault();
    const k = drag.k;
    drag = null;
    moveToPage(k, +item.dataset.page);
  }

  let lastWidth = innerWidth;
  function onResize() { if (innerWidth !== lastWidth) { lastWidth = innerWidth; fitBoard(); } }
  function onBeforeUnload(e) { if (dirty) { e.preventDefault(); e.returnValue = ""; } }

  // ------------------------------------------------------------------------------ the page
  async function mount(element, context) {
    unmount();
    root = element;
    ctx = context;
    if (!document.getElementById("ed-style")) {
      const style = document.createElement("style");
      style.id = "ed-style";
      style.textContent = STYLE;
      document.head.appendChild(style);
    }
    root.addEventListener("click", onClick);
    root.addEventListener("change", onChange);
    root.addEventListener("dragover", onTreeDragOver);
    root.addEventListener("drop", onTreeDrop);
    addEventListener("resize", onResize);
    addEventListener("beforeunload", onBeforeUnload);
    root.innerHTML = '<p class="text-body-secondary">Loading ...</p>';
    const [, , info] = await Promise.all([loadSprite(), load(), request("GET", "/api/dashboard/editor").catch(() => null)]);
    if (info && info.status === 200) {
      const s = info.answer.screen || {};
      if (s.width > 0 && s.height > 0) screen = { name: s.name || "", w: s.width, h: s.height, gap: s.gap ?? 4 };
      if (Array.isArray(info.answer.icons)) icons = info.answer.icons;
      if (Array.isArray(info.answer.zones)) zones = info.answer.zones;
    }
    draw();
  }

  function unmount() {
    if (!root) return;
    root.removeEventListener("click", onClick);
    root.removeEventListener("change", onChange);
    root.removeEventListener("dragover", onTreeDragOver);
    root.removeEventListener("drop", onTreeDrop);
    removeEventListener("resize", onResize);
    removeEventListener("beforeunload", onBeforeUnload);
    clearTimeout(checkTimer);
    root = null;
  }

  return { mount, unmount, dirty: () => dirty };
})();
