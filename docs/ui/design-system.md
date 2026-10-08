# Design system

Status: **proposal**, except where marked as decided. Shared by the touch screen and the web UI.

## 1. Terms

| Term | Meaning | Status |
| --- | --- | --- |
| **Wi-Fi** | the connection of the device to the home network (ESP-IDF: station, STA) | decided |
| **Hotspot** | the network the device opens itself, for setup and recovery (ESP-IDF: access point, AP) | decided |
| **Settings** | the settings dialog on the screen | decided |
| **Quick settings** | the first page of Settings | decided |
| **Web UI** | the web site of the device | proposal |
| **Dashboard** | the Home Assistant tiles, the main screen of the device | proposal |
| **Page**, **tile**, **panel** | a page of the dashboard, one cell of it, the full screen view a tile opens | from the old firmware |
| **Status bar** | the bar at the top of Settings and the setup screens. The dashboard has none | decided |
| **Pull-down** | the swipe down from the top center that opens Settings | decided |
| **Alert strip** | the message panel at the top center of the dashboard, for errors, warnings and hints (also called error popup) | decided |

Code and developer docs use the ESP-IDF terms (AP, STA). Users never see them.

The UI is English only (decided). It is mostly settings and error texts.

## 2. Screen colors

The LVGL look of the old firmware is kept (decided): the theme of `lvgl_ui.h`, dark only. The web
UI has its own look, see section 7.

| Name | Value | Use |
| --- | --- | --- |
| background | `#0b0f14` | screen and page background |
| bar | `#12181f` | status bar, web header |
| card | `#182029` | cards, list rows, tiles |
| card alt | `#161d25` | second card shade, unavailable tiles |
| border | `#2b3844` | borders, separators, slider track |
| text | `#ffffff` | primary text |
| text value | `#c8d4dd` | values in lists |
| text label | `#7fa8c9` | labels, secondary text |
| text muted | `#4d6274` | disabled, hints |
| accent | `#00ffff` | pending state, focus |
| active | `#ff8000` | on state, filled buttons, slider fill |
| alert | `#ff8000` | notes and status lines |
| error | `#ff0000` | errors, destructive buttons |

QR codes are always drawn black on a white card with a quiet zone of 4 modules, also in the dark
theme. Phone cameras do not read light-on-dark codes reliably.

## 3. Typography

Montserrat in the sizes of the old firmware. LVGL cannot scale fonts, so each size is a font in
flash.

| Size | Name | Use |
| --- | --- | --- |
| 12 | small | hints, units, captions |
| 14 | normal | status bar, buttons |
| 16 | medium | list labels and values, form fields |
| 20 | large | dialog titles |
| 24 | title | page titles where a page has one |
| 28 / 32 | value / huge | large readouts on the dashboard and in panels |

## 4. Sizes and touch targets

Reference: 480 x 320 at about 165 DPI (3.5 inch).

| Item | Size |
| --- | --- |
| status bar | 36 px high |
| content area | 480 x 284 |
| page padding | 8 px |
| list row | 44 px high, full width |
| button | 44 px high, at least 96 px wide |
| switch | 52 x 28 px, the whole row is the touch target |
| card radius | 12 px |
| minimum touch target | 44 x 44 px, about 7 mm |

Other DPI values scale these sizes with `lv_dpx()`, so a target keeps its physical size.

## 5. Icons

Material Design Icons, generated as subset fonts like in the old firmware, 20 px inline and
48 px tile sized. The web UI uses the same glyphs as inline SVG, so an icon means the same thing
on both.

## 6. Resolutions

The layout adapts to the resolution and is not drawn per board. Sizes in use or planned:
480 x 320, 320 x 480, 640 x 480, 480 x 800, each in landscape and portrait. More can be
added later.

| Board | Resolution | Diagonal | DPI | Layout |
| --- | --- | --- | --- | --- |
| WT32-SC01 | 480 x 320 | 3.5" | 165 | reference, landscape |
| JC3248W535 | 320 x 480 | 3.5" | 165 | portrait: two-column pages stack their columns |
| JC4880P443C | 480 x 800 | 4.3" | 217 | portrait, sizes scaled by 1.3 |
| TTGO T4 (test device only) | 240 x 320 | 2.4" | 167 | everything shown, touch does nothing. Touch is a requirement for supported boards |

## 7. Web UI look

* **Bootstrap look** (decided). Proposal: Bootstrap 5.3, the current major version. It keeps the
  classic Bootstrap look and has built-in light and dark color modes.
* **Light and dark mode** (decided). The mode follows the browser setting, with a switch in the
  menu. It is stored in the browser.
* **Custom themes**: Bootstrap 5.3 is styled through CSS variables, so a custom theme is one small
  CSS file that overrides them. It is uploaded like the other web files (proposal).
* **Size**: the full minified Bootstrap CSS file is shipped (decided), about 30 KB gzipped. The
  site's own JavaScript is written without Bootstrap's script bundle unless a component really
  needs it (proposal).
* Fonts: Bootstrap's system font stack, no web fonts.
