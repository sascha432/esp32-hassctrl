#pragma once

// The look of the screens (docs/ui/design-system.md): colors and sizes of the old firmware, and
// the small building blocks every page uses.

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace hc::ui::theme {

constexpr uint32_t kBackground = 0x0b0f14;
constexpr uint32_t kBar = 0x12181f;
constexpr uint32_t kCard = 0x182029;
constexpr uint32_t kCardAlt = 0x161d25;
constexpr uint32_t kBorder = 0x2b3844;
constexpr uint32_t kText = 0xffffff;
constexpr uint32_t kTextValue = 0xc8d4dd;
constexpr uint32_t kTextLabel = 0x7fa8c9;
constexpr uint32_t kTextMuted = 0x4d6274;
constexpr uint32_t kActive = 0xff8000;
constexpr uint32_t kAlert = 0xff8000;
constexpr uint32_t kError = 0xff0000;

constexpr lv_coord_t kStatusBarHeight = 36;
constexpr lv_coord_t kRowHeight = 44;
constexpr lv_coord_t kPad = 8;
constexpr lv_coord_t kRadius = 12;

// Montserrat 12, 14, 16, 20 and 28 (the values of the tiles and panels); with
// Options::large_fonts 16, 18, 20, 26 and 36, about 28 % larger. Chosen once by ui::start(),
// before anything is built.
void use_large_fonts(bool large);
const lv_font_t &font_small();
const lv_font_t &font_normal();
const lv_font_t &font_medium();
const lv_font_t &font_large();
const lv_font_t &font_huge();
// a size that holds text (a column of labels, a chip): `px` at the normal fonts, about 30 % more
// with the large ones
lv_coord_t scaled(lv_coord_t px);

lv_color_t color(uint32_t hex);

// an object without the default theme styles
lv_obj_t *plain(lv_obj_t *parent);

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t &font, uint32_t text_color);

// Sets the text of a one-line label whose width comes from the layout (flex grow): a text that is
// too wide ends in "...". LVGL's LV_LABEL_LONG_DOT wraps at spaces first, which leaves "1..." of
// "1  My network". The text is kept and fitted again when the label's size changes.
void line_text(lv_obj_t *label, const char *text);

// A list row of kRowHeight over the full width: a label on the left, an optional value, and a
// ">" when `opens_page`. Clicking it sends LV_EVENT_CLICKED to `callback`.
lv_obj_t *row(lv_obj_t *parent, const char *text, const char *value, bool opens_page, lv_event_cb_t callback,
              void *user_data = nullptr);

// the value label of a row made by row(); its text is set with line_text()
lv_obj_t *row_value(lv_obj_t *row);

// a flat button of kRowHeight; `danger` paints it in the error color
lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *user_data = nullptr,
                 bool danger = false);

// a switch styled like the theme
lv_obj_t *toggle(lv_obj_t *parent, bool on, lv_event_cb_t callback, void *user_data = nullptr);

// a slider styled like the theme
lv_obj_t *slider(lv_obj_t *parent, int32_t min, int32_t max, int32_t value);

// a column container that fills the parent and scrolls vertically
lv_obj_t *column(lv_obj_t *parent);

// A small caption with its value wrapped below it, for narrow columns where a name and a value
// side by side would cut the value (addresses, versions). Returns the value label.
lv_obj_t *caption_value(lv_obj_t *parent, const char *caption, const char *value,
                        const lv_font_t &font = font_normal());

// A white card with a QR code: dark on light with a quiet zone, also in the dark theme
// (docs/ui/design-system.md section 2).
lv_obj_t *qr_card(lv_obj_t *parent, const char *data, lv_coord_t size);

// the Wi-Fi QR code format: WIFI:T:WPA;S:<ssid>;P:<password>;; with \ ; , : " escaped
void wifi_qr_text(char *out, size_t size, const char *ssid, const char *password);

// A scrolling column whose first child is a text column; the caller adds a second child (a QR
// code). Landscape: side by side; portrait: below each other; switches when the display turns.
lv_obj_t *split(lv_obj_t *content);

} // namespace hc::ui::theme
