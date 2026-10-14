#pragma once

// Swipe navigation of the dashboard (docs/ui/lvgl/lvgl-dashboard.md section 9): an area with
// `swipe` has no back tile on its page, a swipe right goes back to the parent page, and a band of
// dots at the bottom shows how deep the page is in nested swipe areas.

#include <cstddef>

#include "lvgl.h"

namespace hc::ui::swipe {

// the band of dots at the bottom of a swipe page; the grid ends above it. 24 px with 9 px dots;
// with Options::large_fonts (the 800x480 JC4880P443C) 32 px with 12 px dots (your request
// 2026-10-14: 50 % and 100 % larger than the first 16 px band with 6 px dots)
lv_coord_t dots_height();
// a finger that moved this far decides the direction of its drag: sideways is a swipe, up or
// down a tile's drag or the pull-down
constexpr lv_coord_t kSlop = 10;
// a sideways drag this long swipes; the rest of the press goes nowhere (no tap on the tile)
constexpr lv_coord_t kSwipeStart = 40;

// Watches `obj` (the page, every card, the step bars, the band) for a swipe; a swipe calls
// dashboard::swiped(). Not used while editing (the edit layer has its own).
void watch(lv_obj_t *obj);

// true while a press has decided for sideways: a dimmer's drag stays out
bool active();

// the band of `count` dots at the bottom of `parent` (the page object, `width` wide); the dot of
// `current` is lit (the shown page, the last one). A tap on its left half goes back.
lv_obj_t *dots(lv_obj_t *parent, lv_coord_t width, size_t count, size_t current);

} // namespace hc::ui::swipe
