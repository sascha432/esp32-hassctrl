#pragma once

// The look of an action on its way (docs/ui/lvgl/lvgl-dashboard.md section 4): a glowing dot with a short
// fading tail running round the border of a card, in place of a cyan border. It is drawn over the
// card after its children; every frame redraws only the stretch of the border the dot left and
// reached, not the card. The start screen lets it run round the Home Assistant logo (an outline).

#include <cstdint>

#include "lvgl.h"

namespace hc::ui::runner {

struct Point {
    float x;
    float y;
};

// A closed track: `count` points `step` pixels apart, clockwise, in pixels from the top left of
// the object. The points have to stay valid while the dot runs. The object has to leave room for
// the glow, about 8 px, round the track.
struct Outline {
    const Point *points;
    uint16_t count;
    float step;
};

// starts (`on`) or stops the dot on `obj`; nothing happens when it already is so. Without an
// outline it runs round the object's border.
void set(lv_obj_t *obj, bool on, const Outline *outline = nullptr);

} // namespace hc::ui::runner
