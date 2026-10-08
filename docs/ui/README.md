# UI documentation

Status: **proposal**. Layouts are agreed here first, as wireframes. Visual previews in HTML at
real pixel size follow once a layout is agreed (see "Previews" below).

| Document | Contents |
| --- | --- |
| [design-system.md](design-system.md) | terms, screen colors, fonts, sizes, touch targets, icons, resolutions, the web UI look |
| [screen.md](screen.md) | the touch screen: frame and status bar, navigation, first setup, Settings |
| [web.md](web.md) | the web UI: principles, site map, page frame, setup pages, main pages, login |
| [dashboard.md](dashboard.md) | the Home Assistant dashboard: grid, tiles, states, icons (carried over from `behavior.md` of the old firmware) |

## How to read the wireframes

* Wireframes are ASCII at a fixed scale: one column is 8 px, one line is about 16 px. The screen
  reference is 480 x 320 (60 columns), the web reference is a phone at 360 px (44 columns).
* `[text]` is a button or a field, `[  o]` / `[o  ]` a switch (off / on), `>` opens a sub page,
  `<` in the bar goes back, `||||` is the signal strength, `[wifi]` an icon.
* Every value in a wireframe is an example.
* Each document ends with its open points. Decided items are marked as decided; everything else
  is a proposal.

## Previews

The old firmware used HTML previews of whole screens (`screen1.html`) and generated SVG mockups.
They were good for deciding colors and proportions. Here they come after the wireframes: one
`previews/screen.html` that draws the agreed screens at 480 x 320 with the colors of
[design-system.md](design-system.md), and the web UI itself serves as its own preview.

The screens can also be seen and clicked through in the Windows version of the UI
(`python tools/sim.py`, see the README), with the real fonts, in every window size and rotation, and
rendered into PNG files by script.

## Keeping this current

A change of a screen or page is not done until its wireframe is updated. New terms go into the
glossary of [design-system.md](design-system.md) first.
