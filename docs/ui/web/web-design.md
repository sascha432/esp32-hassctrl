# Web design

The conventions below are the ones of `web/`. The design guide
of the web UI: principles, look, page frame and how a page is built. Read it before adding or
changing a page. The pages themselves are in [web-app.md](web-app.md), the dashboard editor in
[web-dashboard-editor.md](web-dashboard-editor.md), the recovery page in
[web-recovery.md](web-recovery.md). Terms: [README](../README.md).

## 1. Principles

* **Phone first.** The first contact is a phone in the captive portal. One column, at most
  800 px wide (the log's lines need the room), centered on large screens.
  Only the dashboard editor uses the whole width (`app-wide`).
* **Works without internet.** Everything is served by the device: no CDN, no web fonts. In the
  captive portal there is no internet.
* **Bootstrap look with light and dark mode**, Bootstrap's CSS purged to the rules in use,
  section 2. No JavaScript framework; the files in `web/` are the readable sources, the browser
  gets what `tools/build_web.py` makes of them (pages of options the firmware lacks left out,
  minified, gzipped by `tools/build_fs.py`). The device keeps only the gzipped files: a client
  without `Accept-Encoding: gzip` gets 406 (`curl` needs `--compressed`). Every file carries the
  image's `ETag` (`/www/etag`, written by `build_fs.py`; `Cache-Control: no-cache`), so a browser
  asks each time and gets 304 while the image is the same.
* **Secrets are write-only.** The web UI can set Wi-Fi passwords, the Home Assistant token and the
  camera stream's password, but never reads them back. It shows whether a secret is stored, and an
  empty field keeps it.
* **The device is the truth.** The page sends what the user entered; the device checks it and
  answers with an `error` in words, which the page shows. Rules are not written twice in
  JavaScript (the dashboard editor's preview is the documented exception).
* **Always recoverable.** Login and firmware update work even when the file system is broken
 , see [web-recovery.md](web-recovery.md).
* **Same terms** as the screen. English only.

## 2. Look

* **Bootstrap 5.3**, `web/vendor/bootstrap.min.css`, the full file in the sources; the image
  holds only the rules whose classes the pages name (about 140 of 2000 classes, 9 KB gzipped
  instead of 30), purged by `tools/build_web.py`. A class the page assembles from a variable
  (`alert-${kind}`, `text-bg-${kind}`) is in the tool's `SAFELIST`; a new one of that kind goes
  there too, or the page writes the class out in full. Only the CSS: Bootstrap's script bundle is
  not shipped; tabs, the menu and the editor's dialogs are a few lines of the site's own
  JavaScript with Bootstrap's markup.
* **Pages of a board option** (Camera, SD card) sit between `// #if CONFIG_*` and `// #endif`
  lines in `app.js` (the view, its menu entry, its helpers) and are left out of the image of a
  firmware without the option; the hash of a missing page lands on Status.
* **Light and dark mode**: it follows the browser, with Automatic, Light and Dark in the
  menu, stored in the browser (`localStorage` `theme`) and set before the first paint
  (`index.html`).
* **Colors only through Bootstrap**: its classes (`text-body-secondary`, `text-bg-*`,
  `btn-*`) and its CSS variables (`var(--bs-success)`, `var(--bs-border-color)`) in `app.css`. No
  hex color in the site's own CSS or JS, so both modes and a custom theme would work. The screen's
  colors are used only where the web shows the screen: the dashboard editor's preview.
* **Custom themes** (not built): one small CSS file that overrides Bootstrap's CSS
  variables, uploaded like the other web files.
* Fonts: Bootstrap's system font stack, no web fonts.
* Icons: small inline SVGs in `app.js` for the header (Wi-Fi, menu, lock); the MDI icons of the
  dashboard from `web/icons.svg` (written by `tools/build_icons.py` with the screen's icon fonts),
  so an icon means the same on both.

## 3. Page frame

A header with the device name, the state icons and the menu button, then the page title and cards.

```
+--------------------------------------------+
| hassctrl-1a2b            [Wi-Fi] [HA]  [=] |
+--------------------------------------------+
| Page title                                 |
|                                            |
| +--------------------------------------+   |
| | card                                 |   |
| +--------------------------------------+   |
| +--------------------------------------+   |
| | card                                 |   |
| +--------------------------------------+   |
+--------------------------------------------+
```

The menu opens over the page on phones. On screens of 800 px and wider it is a sidebar instead
(`app.css`). It also holds the light, dark or automatic mode and the log out buttons.

```
+--------------------------------------------+
| hassctrl-1a2b            [Wi-Fi] [HA]  [x] |
+--------------------------------------------+
|  Status                                    |
|  Home Assistant                            |
|  Wi-Fi                                     |
|  Hotspot                                   |
|  Camera                                    |
|  Dashboard                                 |
|  Logs                                      |
|  System                                    |
+--------------------------------------------+
|  Light / dark / auto                       |
|  Log out                                   |
|  Log out everywhere                        |
+--------------------------------------------+
```

* The state icons: Wi-Fi and `HA`, green when connected, yellow while connecting or waiting, red
  for an error, the problem in the title (tooltip). Refreshed with every page.
* During the setup the header shows the step (`1 / 3`) instead of the icons and the menu.
* A page that a board does not have (Camera) is hidden in the menu, decided by `/api/status`.

## 4. How a page is built

`web/app.js` is one page; the views follow the hash (`#/wifi`). A new page is:

1. an entry in `menuPages` (route name, menu text);
2. a function in `views` that calls `render()` with the title and a placeholder, loads its data
   with `api()` and fills it in.

Helpers of `app.js`:

| Helper | What it does |
| --- | --- |
| `api(path, form)` | GET, or POST with an object (form encoded) or a `FormData` (files); JSON answers; a 401 leads to the login; an error answer throws with the device's `error` text |
| `render(html)` | puts the page into `#page` and scrolls to the top |
| `card(title, body)` | a Bootstrap card with a small title (`h2.h6`) |
| `facts(rows)` | a two-column list of names and values (`dl.facts`) |
| `busy(button, action)` | disables the button while `action` runs; an error becomes a red toast |
| `toast(text, kind)` | a short message at the bottom center for 4 s (`success`, `danger`, `warning`, `secondary`) |
| `refresh(action, ms)` | repeats `action` while the page is shown (2 to 5 s for live states); stopped on every route change |
| `esc(value)` | escapes a text for HTML |
| `go(name)` | goes to a page |
| `loadScript(src)` | loads a script once, for a large page that only some users open (`dashboard.js`) |

Patterns:

* **Every text from outside is escaped** with `esc()` before it goes into HTML: network names,
  entity names, device names, error texts.
* **Cards** group a page: one card per topic, the title names it ("Saved networks (2 of 5)").
  Values as `facts()`, lists as `list-group`.
* **Buttons**: the main action of a form `btn btn-primary` at the right; other actions
  `btn-outline-secondary`; destructive ones `btn-outline-danger`. Small (`btn-sm`) inside lists
  and card headers. Every action button runs through `busy()`.
* **Forms**: Bootstrap's `form-label` above `form-control`/`form-select`, `form-text` for a hint,
  `form-check form-switch` for on/off. The Wi-Fi password has a Show button instead of a second
  field. Enter in the last field submits.
* **Live settings** (a select, a switch) are saved on change with a `saved` toast; forms with
  several fields have a Save button.
* **Destructive actions** ask with `confirm()` and say what is lost ("Forget Home-Net? The device
  no longer connects to it.").
* **States** as badges: `text-bg-secondary` for neutral (`saved`, `stored`), `text-bg-warning`
  for something to do (`Password needed`), `text-bg-danger` for errors.
* **Loading** shows `Loading ...` in the placeholder; a device that does not answer shows an
  `alert-warning`.
* **Leaving with unsaved changes** asks first (the dashboard editor).
* Large pages keep their own script and CSS: `dashboard.js` is loaded by the Dashboard page only,
  so the setup on a phone stays small; its styles are in the script (`STYLE`).

## 5. Texts

* Sentence case for titles, cards and buttons: "Saved networks", "Save and connect", "Log out
  everywhere".
* Toasts and states in lower case where they continue a sentence: "saved", "reconnecting ...",
  "restarting ...". A running action ends in ` ...` like on the screen.
* Errors from the device are shown as they come; they are written for people ("the stream needs a
  username and a password").
* Units and times as on the screen ([lvgl-design.md](../lvgl/lvgl-design.md) section 9).

## 6. Checklist for a new or changed page

1. Wireframe or description in [web-app.md](web-app.md) (README "How to read the wireframes"),
   with the API it uses.
2. Built with the helpers of section 4 and Bootstrap classes; no colors of its own. A class built
   from a variable is in the `SAFELIST` of `tools/build_web.py`; a page of a board option between
   `// #if CONFIG_*` markers (section 2).
3. Checked at 360 px and above 800 px, in light and dark mode (headless Edge for screenshots).
4. Every outside text escaped; secrets write-only.
5. `python tools/test_web.py` against a board; the API's tests extended for a new endpoint.
6. The web image built and flashed (`tools/build_fs.py`) on every board, with the firmware when
   the API changed.
7. [web-app.md](web-app.md) and [TODO](../../TODO.md) updated in the same commit.

## 7. Open points

* None.
