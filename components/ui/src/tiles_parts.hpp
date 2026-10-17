#pragma once

// What the files of the tiles share: the parts a view is built from (tiles.cpp), the event
// handlers of the gestures (tiles_gestures.cpp) and the clock (tiles_clock.cpp). The texts are
// public (tiles.hpp, tiles_text.cpp).

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "tiles.hpp"

namespace hc::ui::tiles::detail {

// a clock looks at the time this often: once a second, four times with seconds (so a second is
// never more than a quarter late); it redraws only when the shown time changed. A running timer
// counts down with the first.
constexpr uint32_t kClockTickMs = 1000;
constexpr uint32_t kClockSecondsTickMs = 250;

// ------------------------------------------------------------------- the parts (tiles.cpp)

const lv_font_t &value_font(const View &view);
const lv_font_t &name_font(const View &view);
std::string_view domain_of(std::string_view entity_id);
std::string number(float value, int decimals);
// sets the text only when it changed
void set_text(lv_obj_t *label, const std::string &text);
lv_coord_t text_width(const char *text, const lv_font_t &font);
// the first of `fonts` (largest first) that draws `text` within `width` and `height`, else the last
const lv_font_t &largest_font(std::initializer_list<const lv_font_t *> fonts, const char *text, lv_coord_t width,
                              lv_coord_t height = LV_COORD_MAX);
// the value in the largest font of the tile that fits its width
void set_value(const View &view, const std::string &text);
void set_color(lv_obj_t *label, uint32_t color);
void set_hidden(lv_obj_t *obj, bool hidden);

lv_obj_t *card(lv_obj_t *parent, lv_area_t area);
// the centered column of icon, value and name
lv_obj_t *column(lv_obj_t *parent);
// a row of labels as wide as its content, `gap` apart, centered in the column
lv_obj_t *flex_row(lv_obj_t *parent, lv_coord_t gap);
lv_obj_t *text(lv_obj_t *parent, const lv_font_t &font, uint32_t color);
lv_obj_t *name_label(View &view, lv_obj_t *parent);
// a step bar of a climate card
lv_obj_t *bar(lv_obj_t *parent, const char *glyph, lv_coord_t w, lv_coord_t h, lv_align_t align);
// the wide shape: the icon left, the column of texts returned right of it
lv_obj_t *wide_row(View &view, const lv_font_t &icon_font);

// ------------------------------------------------------- the gestures (tiles_gestures.cpp)

// the handlers of a card's events; the user data is its view
void on_click(lv_event_t *event);
void on_dimmer(lv_event_t *event);
void on_step(lv_event_t *event);
// sends the collected steps of a climate tile (the handler of its timer; also when the card goes)
void on_step_send(lv_timer_t *timer);

// --------------------------------------------------------------- the clock (tiles_clock.cpp)

// builds the clock of `tile` in the view's card and starts its timer
void create_clock(View &view, const core::Tile &tile, lv_coord_t width, lv_coord_t height);
// draws the time; `force` redraws although it did not change
void tick_clock(View &view, bool force);

} // namespace hc::ui::tiles::detail
