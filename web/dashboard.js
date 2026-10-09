// The dashboard editor of the web UI (docs/ui/web-dashboard.md section 2). Loaded by the Dashboard
// page only, so the setup on a phone stays small. It edits the JSON of GET /api/dashboard as it is
// and sends it back whole; the device checks it (POST ...&check=1 a second after each change) and
// stores it (POST with the revision it was read at, 409 when the screen or an import was faster).
//
// It copies rules of the firmware: the placement of core::finish() (components/core/src/layout.cpp),
// the sizes of a kind, the kinds of the domains. A change of the dashboard in core or config is a
// change here too, and the web image goes onto the devices with the firmware (docs/architecture.md
// section 5). The device's check is the truth. Icons are rough stand-ins until web/icons.svg exists.
// The preview shows no live values (decided 2026-10-09).
"use strict";

window.hcEditor = (() => {
  const esc = (v) => String(v ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
  const clone = (v) => JSON.parse(JSON.stringify(v));
  // the document as the device takes it: no local keys, no cells of an answer
  const plain = (v) => JSON.parse(JSON.stringify(v, (k, x) => (k === "_k" || k === "cells" ? undefined : x)));

  // the device's screen in landscape (GET /api/dashboard/editor); margin 8 (docs/ui/dashboard.md)
  let screen = { name: "", w: 480, h: 320, gap: 4 };

  // ------------------------------------------------------------------------- icon stand-ins
  const SHAPES = {
    bulb: '<path d="M9 18h6M10 21h4"/><path d="M12 3a6 6 0 0 0-3.5 10.9c.6.5 1 1.2 1 2.1h5c0-.9.4-1.6 1-2.1A6 6 0 0 0 12 3z"/>',
    power: '<path d="M12 3v9"/><path d="M6.3 6.3a8 8 0 1 0 11.4 0"/>',
    plug: '<path d="M9 3v5M15 3v5M6 8h12v3a6 6 0 0 1-12 0zM12 17v4"/>',
    therm: '<path d="M10 14V5a2 2 0 0 1 4 0v9a4 4 0 1 1-4 0z"/><path d="M12 9v6"/>',
    thermostat: '<circle cx="12" cy="12" r="8"/><path d="M12 12V7"/><circle cx="12" cy="12" r="1.5"/>',
    camera: '<path d="M4 8h3l2-3h6l2 3h3v11H4z"/><circle cx="12" cy="13" r="3.5"/>',
    folder: '<path d="M3 6h6l2 2h10v11H3z"/>',
    fan: '<circle cx="12" cy="12" r="1.5"/><path d="M12 10.5C11 6 13 3 16 4s1 5-4 6.5M13.5 12c4.5-1 7.5 1 6.5 4s-5 1-6.5-4M10.5 12.5C9.5 17 6.5 19 4.5 17s1-5 6-4.5"/>',
    bolt: '<path d="M13 2 5 14h6l-1 8 8-12h-6z"/>',
    lock: '<rect x="5" y="11" width="14" height="10" rx="1"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
    drop: '<path d="M12 3s6 7 6 11a6 6 0 0 1-12 0c0-4 6-11 6-11z"/>',
    eye: '<path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>',
    motion: '<circle cx="13" cy="4" r="2"/><path d="M8 21l3-7 3 3v4M10 9l3-2 3 4 3 1M11 14l-1-5"/>',
    home: '<path d="M3 11 12 3l9 8M5 9v12h14V9"/>',
    toggle: '<rect x="2" y="7" width="20" height="10" rx="5"/><circle cx="16" cy="12" r="3"/>',
    door: '<rect x="6" y="3" width="12" height="18"/><circle cx="15" cy="12" r="1"/>',
    window: '<rect x="4" y="4" width="16" height="16"/><path d="M12 4v16M4 12h16"/>',
    gauge: '<path d="M4 18a8 8 0 1 1 16 0"/><path d="M12 18l4-6"/>',
    palette: '<path d="M12 3a9 9 0 1 0 0 18c1 0 1.5-.8 1-1.7-.6-1 0-2.3 1.2-2.3H17a4 4 0 0 0 4-4c0-5.5-4-10-9-10z"/>',
    script: '<path d="M6 3h10l2 2v16H6z"/><path d="M9 8h6M9 12h6M9 16h4"/>',
    robot: '<rect x="5" y="8" width="14" height="11" rx="2"/><path d="M12 4v4"/><circle cx="9" cy="13" r="1"/><circle cx="15" cy="13" r="1"/>',
    remote: '<rect x="8" y="3" width="8" height="18" rx="2"/><circle cx="12" cy="8" r="1.5"/>',
    tap: '<circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="3"/>',
    check: '<circle cx="12" cy="12" r="9"/><path d="m8 12 3 3 5-6"/>',
    circle: '<circle cx="12" cy="12" r="8"/>',
    wave: '<path d="M2 12c2.5-6 5-6 7.5 0s5 6 7.5 0 3.5-4 5-2"/>',
    sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M5 5l1.5 1.5M17.5 17.5 19 19M5 19l1.5-1.5M17.5 6.5 19 5"/>',
    molecule: '<circle cx="7" cy="12" r="3"/><circle cx="17" cy="7" r="2.5"/><circle cx="17" cy="17" r="2.5"/><path d="M9.7 10.7 14.8 8M9.7 13.3l5.1 2.7"/>',
    radiator: '<path d="M5 6v12M9 6v12M13 6v12M17 6v12M3 18h18"/>',
    arrow: '<path d="M20 12H5M11 6l-6 6 6 6"/>',
    none: '<path d="M5 5l14 14" stroke-dasharray="2 3"/>',
  };
  const ICON_SHAPE = {
    "brightness-6": "sun", camera: "camera", "checkbox-marked-circle": "check", "current-ac": "wave", "door-closed": "door",
    "door-open": "door", eye: "eye", fan: "fan", "fan-off": "fan", flash: "bolt", "flash-off": "bolt", folder: "folder",
    gauge: "gauge", "gesture-tap-button": "tap", home: "home", "home-assistant": "home", lightbulb: "bulb",
    "lightbulb-off": "bulb", "lightbulb-on": "bulb", "lightning-bolt": "bolt", lock: "lock", "lock-open": "lock",
    "molecule-co2": "molecule", "motion-sensor": "motion", "motion-sensor-off": "motion", palette: "palette", power: "power",
    "power-plug": "plug", "power-plug-off": "plug", radiator: "radiator", "radiobox-blank": "circle", remote: "remote",
    robot: "robot", "robot-off": "robot", "script-text": "script", "sine-wave": "wave", thermometer: "therm",
    thermostat: "thermostat", "toggle-switch": "toggle", "toggle-switch-off": "toggle", "water-percent": "drop",
    "window-closed": "window", "window-open": "window", "arrow-left": "arrow",
  };
  const svg = (name, px) =>
    `<svg width="${px}" height="${px}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${SHAPES[ICON_SHAPE[name] || name] || SHAPES.circle}</svg>`;

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
  .tile.pic { background: linear-gradient(160deg, #3b5a6e 0%, #1f3240 45%, #4a3b2a 100%); justify-content: flex-end; }
  .tile.pic .nm { width: 100%; background: rgba(0,0,0,.55); color: #fff; padding: 3px 0; }
  .tile.spacer { background: transparent; border: 1px dashed #2b3844; color: #4d6274; }
  .tile.err { box-shadow: inset 0 0 0 3px #ff0000; }
  .tile.fallback { box-shadow: inset 0 0 0 3px #ffb000; opacity: .75; }
  .tile.sel { box-shadow: 0 0 0 3px #00ffff; z-index: 2; }
  .tile.drop-before::before, .tile.drop-after::before { content: ""; position: absolute; top: 0; bottom: 0; width: 5px; background: #00ffff; }
  .tile.drop-before::before { left: 0; }
  .tile.drop-after::before { right: 0; }
  .tile.pinned::after { content: ""; position: absolute; right: 5px; top: 5px; width: 7px; height: 7px; border-radius: 50%; background: #00ffff; }
  .tile .kind { font-size: 11px; color: #4d6274; }
  .cell-empty { position: absolute; border: 1px dashed #2b3844; border-radius: 8px; display: flex; align-items: center; justify-content: center;
                color: #4d6274; font-size: 22px; cursor: pointer; }
  .cell-empty:hover { color: #00ffff; border-color: #00ffff; }
  .hl { position: absolute; border-radius: 8px; pointer-events: none; z-index: 5; }
  .hl.ok { border: 3px solid #2ecc71; background: rgba(46,204,113,.15); }
  .hl.no { border: 3px solid #ff0000; background: rgba(255,0,0,.12); }
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
  let icons = Object.keys(ICON_SHAPE).filter((n) => n !== "arrow-left");
  let check = { state: "ok", text: "", k: null }, checkTimer = null, checkSeq = 0;
  let addCell = null, drag = null;

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
    if (["switch", "input_boolean", "fan", "automation"].includes(d)) return "switch";
    if (["button", "input_button", "scene", "script"].includes(d)) return "button";
    if (d === "climate") return "climate";
    if (d === "camera") return "picture";
    return "sensor";
  }
  function defaultSpan(t) {
    const k = kindOf(t);
    if (k === "light") return t.dimmer ? [1, 2] : [1, 1];
    if (k === "climate") return [1, 2];
    if (k === "picture") return [2, 2];
    return [1, 1];
  }
  // the sizes core::finish() allows for the kind (layout.cpp check_tile)
  function sizesFor(t) {
    const k = kindOf(t);
    if ((k === "light" && t.dimmer) || k === "climate") return [[1, 1], [1, 2]];
    if (k === "picture") return [[1, 1], [1, 2], [2, 1], [2, 2]];
    return [];
  }
  const spanOf = (t, o) => { const s = t.size || defaultSpan(t); return o === "p" ? [s[1], s[0]] : s; };
  const okey = (o) => (o === "p" ? "portrait" : "landscape");
  const oname = (o) => (o === "p" ? "portrait" : "landscape");
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
    if (t.type === "area") return "(no name)";
    return (t.entity || "?").split(".")[1] || t.entity;
  }
  function isInside(key, areaK) {
    for (let p = pageByKey(key); p; p = p.parent === null ? null : pageByKey(p.parent)) {
      if (p.key === areaK) return true;
      if (p.parent === null) return false;
    }
    return false;
  }

  // core::finish()'s placement (layout.cpp place()): per orientation the tiles with a cell first,
  // then first-fit
  function place(p, o) {
    const g = gridOf(p);
    const C = o === "p" ? g[1] : g[0], R = o === "p" ? g[0] : g[1];
    const used = Array.from({ length: R }, () => Array(C).fill(null));
    const res = { C, R, cells: {}, errors: {}, fallback: {}, arranged: false, used };
    const fits = (c, r, w, h) => {
      if (c < 0 || r < 0 || c + w > C || r + h > R) return false;
      for (let y = r; y < r + h; y++) for (let x = c; x < c + w; x++) if (used[y][x] !== null) return false;
      return true;
    };
    const take = (c, r, w, h, k) => { for (let y = r; y < r + h; y++) for (let x = c; x < c + w; x++) used[y][x] = k; };
    if (p.key !== 0) take(0, 0, 1, 1, "back");
    const key = okey(o);
    for (const t of p.tiles) {
      if (!t[key]) continue;
      res.arranged = true;
      const [w, h] = spanOf(t, o), [c, r] = t[key];
      if (c + w > C || r + h > R) res.errors[t._k] = `the ${oname(o)} cell ${c},${r} lies outside the ${C}x${R} grid`;
      else if (p.key !== 0 && c === 0 && r === 0) res.errors[t._k] = `the ${oname(o)} cell 0,0 is the back tile's`;
      else if (!fits(c, r, w, h)) res.errors[t._k] = `the ${oname(o)} cell ${c},${r} overlaps another tile`;
      else { take(c, r, w, h, t._k); res.cells[t._k] = { c, r, w, h }; }
    }
    for (const t of p.tiles) {
      if (t[key]) continue;
      const [w, h] = spanOf(t, o);
      let found = null;
      if (w <= C && h <= R) {
        outer: for (let r = 0; r + h <= R; r++) for (let c = 0; c + w <= C; c++) if (fits(c, r, w, h)) { found = { c, r, w, h }; break outer; }
      }
      if (found) { take(found.c, found.r, w, h, t._k); res.cells[t._k] = found; }
      else if (o === "p") { res.cells[t._k] = { c: 0, r: 0, w, h }; res.fallback[t._k] = true; }
      else res.errors[t._k] = w > C || h > R ? `a ${w}x${h} tile does not fit into the ${C}x${R} grid` : `no free ${w}x${h} block left`;
    }
    return res;
  }

  // core::move_tile: the page gets its current cells in `o`, the tiles in the way are pushed aside
  function arrangeMove(p, o, k, c, r) {
    const pl = place(p, o), key = okey(o);
    const t = p.tiles.find((x) => x._k === k), [w, h] = spanOf(t, o);
    if (c < 0 || r < 0 || c + w > pl.C || r + h > pl.R) return false;
    if (p.key !== 0 && c === 0 && r === 0) return false;
    const inBlock = (q) => q.c < c + w && q.c + q.w > c && q.r < r + h && q.r + q.h > r;
    const placed = p.tiles.filter((x) => pl.cells[x._k] && !pl.fallback[x._k] && x._k !== k);
    const pushed = placed.filter((x) => inBlock(pl.cells[x._k]));
    const used = Array.from({ length: pl.R }, () => Array(pl.C).fill(false));
    const take = (cc, rr, ww, hh) => { for (let y = rr; y < rr + hh; y++) for (let x = cc; x < cc + ww; x++) used[y][x] = true; };
    const free = (cc, rr, ww, hh) => {
      if (cc + ww > pl.C || rr + hh > pl.R) return false;
      for (let y = rr; y < rr + hh; y++) for (let x = cc; x < cc + ww; x++) if (used[y][x]) return false;
      return true;
    };
    if (p.key !== 0) take(0, 0, 1, 1);
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
    for (const x of placed) x[key] = moved.get(x._k) || [pl.cells[x._k].c, pl.cells[x._k].r];
    t[key] = [c, r];
    return true;
  }
  const tryMove = (p, o, k, c, r) => arrangeMove({ ...p, tiles: clone(p.tiles) }, o, k, c, r);
  function resetArrangement(p, o) { for (const t of p.tiles) delete t[okey(o)]; }

  function stripForKind(t) {
    const k = kindOf(t);
    if (k !== "light") delete t.dimmer;
    if (k !== "light" && k !== "climate") delete t.step;
    if (k !== "sensor") { delete t.unit; delete t.decimals; }
    if (k !== "picture") { delete t.refresh; delete t.stream; }
    if (t.size && !sizesFor(t).some(([c, r]) => c === t.size[0] && r === t.size[1])) delete t.size;
  }
  function insertTile(p, t, cell) {
    if (cell) {
      const pl = place(p, orient);
      if (pl.arranged) {
        p.tiles.push(t);
        const [w, h] = spanOf(t, orient);
        let ok = true;
        for (let y = cell.r; y < cell.r + h; y++) for (let x = cell.c; x < cell.c + w; x++) if (y >= pl.R || x >= pl.C || pl.used[y][x] !== null) ok = false;
        if (ok) t[okey(orient)] = [cell.c, cell.r];
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
      doc = { format: "hassctrl-dashboard", version: 1, revision: 0, next_id: 1, defaults: { grid: [4, 2] }, tiles: [] };
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
    if (k === "light") return "lightbulb";
    if (k === "climate") return "thermostat";
    if (k === "picture") return "camera";
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
    const cw = (W - 16 - gap * (pl.C - 1)) / pl.C, ch = (H - 16 - gap * (pl.R - 1)) / pl.R;
    const box = (c, r, w, h) => ({ x: 8 + c * (cw + gap), y: 8 + r * (ch + gap), w: w * cw + (w - 1) * gap, h: h * ch + (h - 1) * gap });
    const cellAt = (x, y) => ({ c: Math.floor((x - 8 + gap / 2) / (cw + gap)), r: Math.floor((y - 8 + gap / 2) / (ch + gap)) });
    return { W, H, pl, cw, ch, box, cellAt, large: cw >= 90 && ch >= 90 };
  }

  function tileHTML(t, b, large, flags) {
    const k = kindOf(t), nm = esc(displayName(t));
    const attrs = `data-k="${t._k}" draggable="true" style="left:${b.x}px;top:${b.y}px;width:${b.w}px;height:${b.h}px"`;
    const name = `<div class="nm" style="font-size:${large ? 16 : 12}px">${nm}</div>`;
    if (k === "spacer") return `<div class="tile spacer ${flags}" ${attrs}><div class="nm" style="font-size:12px">spacer</div></div>`;
    if (k === "picture") return `<div class="tile pic ${flags}" ${attrs}>${name}</div>`;
    const icon = iconOf(t) === "none" ? "" : svg(iconOf(t), large ? 44 : 24);
    const hint = k === "light" && t.dimmer ? "dimmer" : k === "area" ? "area" : "";
    return `<div class="tile ${flags}" ${attrs}>${icon}${name}${hint ? `<div class="kind">${hint}</div>` : ""}</div>`;
  }

  function boardHTML(p, o) {
    const g = geometry(p, o), pl = g.pl;
    let html = "";
    if (p.key !== 0) html += `<div class="tile" data-back="1" style="left:8px;top:8px;width:${g.cw}px;height:${g.ch}px">${svg("arrow-left", g.large ? 44 : 24)}</div>`;
    for (let r = 0; r < pl.R; r++) for (let c = 0; c < pl.C; c++) if (pl.used[r][c] === null) {
      const b = g.box(c, r, 1, 1);
      html += `<div class="cell-empty" data-act="add" data-c="${c}" data-r="${r}" title="Add a tile here" style="left:${b.x}px;top:${b.y}px;width:${b.w}px;height:${b.h}px">+</div>`;
    }
    for (const t of p.tiles) {
      const cell = pl.cells[t._k];
      if (!cell) continue;
      let flags = "";
      if (sel === t._k) flags += " sel";
      if (pl.fallback[t._k]) flags += " fallback";
      if (t[okey(o)]) flags += " pinned";
      if (check.state === "error" && check.k === t._k) flags += " err";
      html += tileHTML(t, g.box(cell.c, cell.r, cell.w, cell.h), g.large, flags);
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

  const gridSelect = (ctl, value, disabled) => `<select class="form-select form-select-sm" data-ctl="${ctl}" ${disabled ? "disabled" : ""}>
    ${[1, 2, 3, 4, 5, 6, 7, 8].map((n) => `<option ${n === value ? "selected" : ""}>${n}</option>`).join("")}</select>`;

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
    const missing = p.tiles.filter((t) => !pl.cells[t._k]);
    const fallback = Object.keys(pl.fallback).length;
    $("#ed-body").innerHTML = `
      <div class="ed">
        <div><div class="card mb-0"><div class="card-body p-2">
          <div class="d-flex justify-content-between align-items-center px-1 mb-2">
            <span class="small fw-semibold">Pages</span>
            <button class="btn btn-sm btn-outline-secondary py-0" data-act="add-area" ${p.depth >= 5 ? "disabled" : ""}>+ Area</button>
          </div>
          <div class="list-group list-group-flush tree ed-tree-list">${tree}</div>
          <select class="form-select form-select-sm ed-tree-select" data-ctl="page">
            ${all.map((x) => `<option value="${x.key}" ${x.key === p.key ? "selected" : ""}>${"  ".repeat(x.depth)}${esc(x.name)}</option>`).join("")}
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
            <span class="small text-body-secondary">${esc(screen.name)} ${orient === "p" ? `${screen.h}x${screen.w}` : `${screen.w}x${screen.h}`}</span>
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
          ${missing.length ? `<div class="alert alert-danger py-2 small mt-2 mb-0">Not on the page: ${missing.map((t) => `<a href="#" data-act="select" data-k="${t._k}">${esc(displayName(t))}</a>`).join(", ")}</div>` : ""}
          ${fallback ? `<div class="alert alert-warning py-2 small mt-2 mb-0">In portrait ${fallback} tile(s) find no free block and fall back to the first cell (outlined yellow).</div>` : ""}
          <div id="ed-problem" class="mt-2"></div>
          <div class="d-flex flex-wrap gap-2 mt-2">
            <button class="btn btn-sm btn-outline-primary" data-act="add">+ Tile</button>
            <button class="btn btn-sm btn-outline-secondary" data-act="add-area" ${p.depth >= 5 ? "disabled" : ""}>+ Area</button>
            <button class="btn btn-sm btn-outline-secondary" data-act="add-spacer">+ Spacer</button>
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

  function drawInspector() {
    const el = $("#ed-insp");
    const loc = sel ? locate(sel) : null;
    if (!loc) {
      const p = pageByKey(pageKey);
      el.innerHTML = `<h2 class="h6">${esc(p.name)}</h2>
        <p class="small text-body-secondary">${p.tiles.length} tiles on this page. Select a tile in the preview to edit it,
        click an empty cell to add one there. A second click on a selected area opens its page.</p>
        ${p.area ? `<button class="btn btn-sm btn-outline-secondary" data-act="select" data-k="${p.area._k}">Edit the area tile</button>` : ""}`;
      return;
    }
    const t = loc.tile, k = kindOf(t), def = defaultSpan(t), sizes = sizesFor(t);
    const field = (label, html, hint = "") => `<div class="row-field"><label class="form-label">${label}</label>${html}${hint ? `<div class="form-text mt-0">${hint}</div>` : ""}</div>`;
    const cellText = (o) => {
      const pl = place(loc.page, o), c = pl.cells[t._k], v = t[okey(o)];
      return v ? `<b>${v[0]},${v[1]}</b> (arranged)` : c ? `auto, ${c.c},${c.r}${pl.fallback[t._k] ? " (fallback)" : ""}` : '<span class="text-danger">no place</span>';
    };
    const movable = pages().filter((x) => x.key !== loc.page.key && !(t.type === "area" && isInside(x.key, t._k)));
    const known = [...entitiesInUse()].sort();
    let body = "";
    if (k !== "area" && k !== "spacer") {
      body += field("Entity", `<input class="form-control form-control-sm" list="ed-ents" data-field="entity" value="${esc(t.entity)}" spellcheck="false">
        <datalist id="ed-ents">${known.map((x) => `<option value="${x}">`).join("")}</datalist>`, `${k} tile`);
    }
    if (k !== "spacer") {
      body += field("Name", `<input class="form-control form-control-sm" data-field="name" maxlength="32" value="${esc(t.name || "")}" placeholder="${k === "area" ? "required" : "the friendly name of Home Assistant"}">`);
      body += field("Icon", `<button class="btn btn-sm btn-outline-secondary icon-chip" data-act="icon">
        ${t.icon === "none" ? "none" : svg(iconOf(t), 20) + " " + esc(t.icon || "automatic")}</button>`);
    }
    if (k === "area") body += field("Page", `<button class="btn btn-sm btn-outline-secondary" data-act="page" data-page="${t._k}">Open its page (${t.tiles.length} tiles)</button>`);
    if (k === "light") {
      body += `<div class="row-field form-check"><input class="form-check-input" type="checkbox" id="ed-dim" data-field="dimmer" ${t.dimmer ? "checked" : ""}>
        <label class="form-check-label small" for="ed-dim">Dimmer (level fill, drag sets the level)</label></div>`;
    }
    if (sizes.length) {
      body += field("Size", `<select class="form-select form-select-sm" data-field="size">
          <option value="">automatic (${def[0]}x${def[1]})</option>
          ${sizes.map(([c, r]) => `<option value="${c}x${r}" ${t.size && t.size[0] === c && t.size[1] === r ? "selected" : ""}>${c}x${r}</option>`).join("")}
        </select>`, "columns x rows, in landscape");
    }
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
        <button class="btn btn-sm btn-outline-secondary" data-act="down" ${loc.index === loc.page.tiles.length - 1 ? "disabled" : ""} title="Later in the order">Down</button>
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
  function wireDrag(el, p) {
    const s = () => +$("#ed-wrap").dataset.s;
    const g = () => geometry(p, orient);
    const pointerCell = (e) => { const r = el.getBoundingClientRect(); return g().cellAt((e.clientX - r.left) / s(), (e.clientY - r.top) / s()); };
    const forward = (k, target) => p.tiles.findIndex((x) => x._k === k) < p.tiles.findIndex((x) => x._k === target);
    const clearMarks = () => {
      el.querySelectorAll(".drop-before, .drop-after").forEach((x) => x.classList.remove("drop-before", "drop-after"));
      root.querySelector("#ed-hl")?.remove();
    };
    el.addEventListener("dragstart", (e) => {
      const t = e.target.closest("[data-k]");
      if (!t) return;
      const k = +t.dataset.k, cell = place(p, orient).cells[k], pc = pointerCell(e);
      drag = { k, dc: pc.c - cell.c, dr: pc.r - cell.r };
      e.dataTransfer.effectAllowed = "move";
      e.dataTransfer.setData("text/plain", String(k));
    });
    el.addEventListener("dragend", () => { drag = null; clearMarks(); });
    el.addEventListener("dragover", (e) => {
      if (!drag) return;
      e.preventDefault();
      clearMarks();
      const pl = place(p, orient), pc = pointerCell(e), under = pl.used[pc.r]?.[pc.c];
      if (!pl.arranged && typeof under === "number" && under !== drag.k) {
        el.querySelector(`.tile[data-k="${under}"]`)?.classList.add(forward(drag.k, under) ? "drop-after" : "drop-before");
        return;
      }
      const t = locate(drag.k).tile, [w, h] = spanOf(t, orient), c = pc.c - drag.dc, r = pc.r - drag.dr;
      const b = g().box(c, r, w, h);
      el.insertAdjacentHTML("beforeend", `<div id="ed-hl" class="hl ${tryMove(p, orient, drag.k, c, r) ? "ok" : "no"}" style="left:${b.x}px;top:${b.y}px;width:${b.w}px;height:${b.h}px"></div>`);
    });
    el.addEventListener("drop", (e) => {
      if (!drag) return;
      e.preventDefault();
      const d = drag;
      drag = null;
      const pl = place(p, orient), pc = pointerCell(e), under = pl.used[pc.r]?.[pc.c];
      if (!pl.arranged && typeof under === "number" && under !== d.k) {
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
  }

  function moveToPage(k, target) {
    const loc = locate(k);
    if (loc.tile.type === "area" && isInside(target, k)) { ctx.toast("An area cannot go into itself.", "warning"); return; }
    change(() => {
      const [t] = loc.page.tiles.splice(loc.index, 1);
      delete t.landscape;
      delete t.portrait;
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
  function closeModal() { root.querySelector("#ed-modal")?.remove(); }

  function openAdd(cell) {
    addCell = cell;
    modal("Add a tile", `
      <label class="form-label small" for="ed-ent">Entity id</label>
      <div class="input-group input-group-sm mb-2">
        <input class="form-control" id="ed-ent" placeholder="sensor.attic_temperature" spellcheck="false" autocomplete="off">
        <button class="btn btn-primary" data-act="add-typed">Add</button>
      </div>
      <p class="small text-body-secondary">The kind of the tile follows from the domain (light, switch, climate, camera, ...).
        Picking entities from Home Assistant comes with the next step; below are the ones already on the dashboard.</p>
      <div class="list-group ent-list mb-3" id="ed-ent-list"></div>
      <div class="d-flex gap-2"><button class="btn btn-sm btn-outline-secondary" data-act="add-area">+ Area</button>
        <button class="btn btn-sm btn-outline-secondary" data-act="add-spacer">+ Spacer</button></div>`, "modal-lg");
    const drawList = () => {
      const q = root.querySelector("#ed-ent").value.trim().toLowerCase();
      const rows = [...entitiesInUse()].sort().filter((id) => !q || id.includes(q)).slice(0, 200);
      root.querySelector("#ed-ent-list").innerHTML = rows.map((id) => `
        <div class="list-group-item list-group-item-action d-flex align-items-center gap-2" data-act="add-ent" data-id="${esc(id)}">
          <span class="icon-chip">${svg(autoIcon({ entity: id }), 20)}</span><span class="flex-grow-1 text-truncate">${esc(id)}</span>
          <span class="badge text-bg-secondary">${kindOf({ entity: id })}</span></div>`).join("") ||
        '<div class="list-group-item text-body-secondary small">No match.</div>';
    };
    drawList();
    const input = root.querySelector("#ed-ent");
    input.addEventListener("input", drawList);
    input.addEventListener("keydown", (e) => { if (e.key === "Enter") actions["add-typed"](); });
    input.focus();
  }
  function addTile(t) {
    const p = pageByKey(pageKey);
    change(() => { insertTile(p, t, addCell); sel = t._k; });
    closeModal();
  }

  function openIcons() {
    const t = locate(sel).tile;
    modal("Icon", `<input class="form-control form-control-sm mb-2" id="ed-icon-q" placeholder="Search">
      <div class="icon-grid" id="ed-icon-grid"></div>
      <p class="small text-body-secondary mt-2 mb-0">The names the firmware holds. The pictures here are rough stand-ins.</p>`, "modal-lg");
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
    add: (el) => openAdd(el.dataset.c !== undefined ? { c: +el.dataset.c, r: +el.dataset.r } : null),
    "add-ent": (el) => addTile({ _k: K++, entity: el.dataset.id }),
    "add-typed": () => {
      const id = root.querySelector("#ed-ent").value.trim();
      if (!validEntity(id)) { ctx.toast("Not an entity id: lowercase letters, digits and _ on both sides of one dot.", "warning"); return; }
      addTile({ _k: K++, entity: id });
    },
    "add-area": () => {
      if (!root.querySelector("#ed-modal")) addCell = null;
      addTile({ _k: K++, type: "area", name: "New area", tiles: [] });
      ctx.toast("An area needs at least one tile before it can be saved.", "info");
    },
    "add-spacer": () => { if (!root.querySelector("#ed-modal")) addCell = null; addTile({ _k: K++, type: "spacer" }); },
    "reset-arr": () => change(() => resetArrangement(pageByKey(pageKey), orient)),
    icon: () => openIcons(),
    "set-icon": (el) => { const v = el.dataset.icon; closeModal(); change(() => { const t = locate(sel).tile; if (v) t.icon = v; else delete t.icon; }); },
    up: () => change(() => { const l = locate(sel); l.page.tiles.splice(l.index - 1, 0, l.page.tiles.splice(l.index, 1)[0]); }),
    down: () => change(() => { const l = locate(sel); l.page.tiles.splice(l.index + 1, 0, l.page.tiles.splice(l.index, 1)[0]); }),
    dup: () => change(() => {
      const l = locate(sel), c = clone(l.tile);
      (function strip(t) { delete t.id; delete t.landscape; delete t.portrait; t._k = K++; if (t.tiles) t.tiles.forEach(strip); })(c);
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
      const t = e.target.closest("#ed-board [data-k]");
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
      const g = [...gridOf(p)];
      g[ctl === "gcols" ? 0 : 1] = +el.value;
      change(() => {
        if (p.key === 0) doc.defaults.grid = g; else p.area.grid = g;
        // cells outside the new grid: offer to reset the arrangement of the page
        for (const o of ["l", "p"]) {
          const pl = place(p, o);
          if (Object.values(pl.errors).some((m) => m.includes("outside")) &&
              confirm(`Some ${oname(o)} cells are outside the new grid. Reset the arrangement of ${p.name} in ${oname(o)}?`)) resetArrangement(p, o);
        }
      });
    }
  }

  function setField(f, el) {
    change(() => {
      const t = locate(sel).tile, v = el.type === "checkbox" ? el.checked : el.value.trim();
      if (f === "entity") { t.entity = v; stripForKind(t); return; }
      if (f === "dimmer") { if (v) t.dimmer = true; else delete t.dimmer; stripForKind(t); return; }
      if (f === "size") { if (v) t.size = v.split("x").map(Number); else delete t.size; return; }
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
    const [, info] = await Promise.all([load(), request("GET", "/api/dashboard/editor").catch(() => null)]);
    if (info && info.status === 200) {
      const s = info.answer.screen || {};
      if (s.width > 0 && s.height > 0) screen = { name: s.name || "", w: s.width, h: s.height, gap: s.gap ?? 4 };
      if (Array.isArray(info.answer.icons)) icons = info.answer.icons;
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
