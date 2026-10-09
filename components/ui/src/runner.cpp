#include "runner.hpp"

#include <algorithm>
#include <cmath>

namespace hc::ui::runner {

namespace {

constexpr uint32_t kColor = 0x00ffff;
// the dot runs this many pixels a second, the same speed on every size of card
constexpr int32_t kSpeed = 220;
// the tail: this many dots behind the head, this far apart, fading out
constexpr int kTailDots = 7;
constexpr float kTailGap = 3.5f;
// the head: a light core in two rings of glow
constexpr lv_coord_t kCore = 2;
constexpr lv_coord_t kGlow = 6;
constexpr float kPi = 3.14159265f;

// The path of the dot: the middle of the border, a rounded rectangle; clockwise from the top
// left, where the top edge starts.
struct Path {
    float x1, y1, x2, y2, r;

    float straight_w() const { return x2 - x1 - 2 * r; }
    float straight_h() const { return y2 - y1 - 2 * r; }
    float arc() const { return kPi / 2 * r; }
    float length() const { return 2 * (straight_w() + straight_h()) + 4 * arc(); }

    Point at(float d) const
    {
        const float total = length();
        d = std::fmod(d, total);
        if (d < 0) {
            d += total;
        }
        // the sides in order, each a straight edge then the corner after it
        const float w = straight_w();
        const float h = straight_h();
        const float a = arc();
        const auto corner = [&](float cx, float cy, float start, float along) {
            const float angle = start + along / r;
            return Point{cx + r * std::cos(angle), cy + r * std::sin(angle)};
        };
        if (d < w) {
            return {x1 + r + d, y1};
        }
        d -= w;
        if (d < a) {
            return corner(x2 - r, y1 + r, -kPi / 2, d);
        }
        d -= a;
        if (d < h) {
            return {x2, y1 + r + d};
        }
        d -= h;
        if (d < a) {
            return corner(x2 - r, y2 - r, 0, d);
        }
        d -= a;
        if (d < w) {
            return {x2 - r - d, y2};
        }
        d -= w;
        if (d < a) {
            return corner(x1 + r, y2 - r, kPi / 2, d);
        }
        d -= a;
        if (d < h) {
            return {x1, y2 - r - d};
        }
        d -= h;
        return corner(x1 + r, y1 + r, kPi, std::min(d, a));
    }
};

Path path_of(lv_obj_t *obj)
{
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    // the middle of the border
    const float inset = std::max<lv_coord_t>(1, lv_obj_get_style_border_width(obj, LV_PART_MAIN)) / 2.0f;
    Path p{c.x1 + inset, c.y1 + inset, c.x2 + 1 - inset, c.y2 + 1 - inset, 0};
    const float r = static_cast<float>(lv_obj_get_style_radius(obj, LV_PART_MAIN)) - inset;
    p.r = std::clamp(r, 0.0f, std::min(p.x2 - p.x1, p.y2 - p.y1) / 2);
    return p;
}

// Where the dot runs: the border of the object, or an outline in the object's coordinates.
struct Track {
    Path border{};
    const Outline *outline = nullptr;
    // the object's top left on the screen
    float left = 0;
    float top = 0;

    float length() const { return outline != nullptr ? outline->count * outline->step : border.length(); }

    Point at(float d) const
    {
        if (outline == nullptr) {
            return border.at(d);
        }
        const float total = length();
        d = std::fmod(d, total);
        if (d < 0) {
            d += total;
        }
        // the points are equally far apart: the segment follows from the distance
        const float position = d / outline->step;
        const auto index = std::min<uint16_t>(static_cast<uint16_t>(position), outline->count - 1);
        const float t = position - static_cast<float>(index);
        const Point &a = outline->points[index];
        const Point &b = outline->points[(index + 1) % outline->count];
        return {left + a.x + t * (b.x - a.x), top + a.y + t * (b.y - a.y)};
    }
};

void on_anim(void *var, int32_t);

// One running dot, the user data of its animation (freed with it, also when the object goes)
struct Run {
    // nullptr: round the border
    const Outline *outline;
    // where the head was at the last step, below 0 before the first
    float last;
};

Run *run_of(lv_obj_t *obj)
{
    lv_anim_t *anim = lv_anim_get(obj, on_anim);
    return anim != nullptr ? static_cast<Run *>(lv_anim_get_user_data(anim)) : nullptr;
}

// the outline the running animation of `obj` follows, nullptr for its border
const Outline *outline_of(lv_obj_t *obj)
{
    const Run *run = run_of(obj);
    return run != nullptr ? run->outline : nullptr;
}

Track track_of(lv_obj_t *obj, const Outline *outline)
{
    Track track;
    track.outline = outline;
    if (outline != nullptr) {
        lv_area_t c;
        lv_obj_get_coords(obj, &c);
        track.left = static_cast<float>(c.x1);
        track.top = static_cast<float>(c.y1);
    } else {
        track.border = path_of(obj);
    }
    return track;
}

lv_area_t around(Point p, lv_coord_t radius)
{
    const auto x = static_cast<lv_coord_t>(std::lround(p.x));
    const auto y = static_cast<lv_coord_t>(std::lround(p.y));
    return {static_cast<lv_coord_t>(x - radius), static_cast<lv_coord_t>(y - radius), static_cast<lv_coord_t>(x + radius),
            static_cast<lv_coord_t>(y + radius)};
}

// where the head is: the value of the running animation
bool head_of(lv_obj_t *obj, float &d)
{
    lv_anim_t *anim = lv_anim_get(obj, on_anim);
    if (anim == nullptr) {
        return false;
    }
    d = static_cast<float>(anim->current_value);
    return true;
}

void on_anim(void *var, int32_t value)
{
    auto *obj = static_cast<lv_obj_t *>(var);
    Run *run = run_of(obj);
    if (run == nullptr) {
        return;
    }
    const Track path = track_of(obj, run->outline);
    const float length = path.length();
    // The stretch from the end of the tail at the last step to the head: what is invalidated is
    // drawn again before the next frame, so nothing of the dot stays behind, however far it
    // moved in between (the first frames after the start take long).
    const float head = static_cast<float>(value);
    float from = run->last < 0 ? head : run->last;
    run->last = head;
    if (from > head) {
        from -= length; // the lap began again
    }
    if (head - from >= length) {
        lv_obj_invalidate(obj);
        return;
    }
    lv_area_t box = around(path.at(head), kGlow);
    for (float d = from - kTailDots * kTailGap - 2; d < head; d += kGlow) {
        const lv_area_t a = around(path.at(d), kGlow);
        box.x1 = std::min(box.x1, a.x1);
        box.y1 = std::min(box.y1, a.y1);
        box.x2 = std::max(box.x2, a.x2);
        box.y2 = std::max(box.y2, a.y2);
    }
    lv_obj_invalidate_area(obj, &box);
}

void dot(lv_draw_ctx_t *ctx, Point p, lv_coord_t radius, lv_color_t color, lv_opa_t opa)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_color = color;
    dsc.bg_opa = opa;
    const lv_area_t area = around(p, radius);
    lv_draw_rect(ctx, &dsc, &area);
}

void on_draw(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target(event);
    float head = 0;
    if (!head_of(obj, head)) {
        return;
    }
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    const Track path = track_of(obj, outline_of(obj));
    const lv_color_t color = lv_color_hex(kColor);
    // the tail from its end, so the dots nearer the head lie over the others
    for (int i = kTailDots; i >= 1; i--) {
        const auto opa = static_cast<lv_opa_t>(200 * (kTailDots + 1 - i) / (kTailDots + 1));
        const lv_coord_t radius = i <= kTailDots / 2 ? 2 : 1;
        dot(ctx, path.at(head - i * kTailGap), radius, color, opa);
    }
    const Point p = path.at(head);
    dot(ctx, p, kGlow, color, LV_OPA_20);
    dot(ctx, p, kGlow - 2, color, LV_OPA_50);
    dot(ctx, p, kCore, lv_color_mix(lv_color_white(), color, LV_OPA_60), LV_OPA_COVER);
}

void on_ext_size(lv_event_t *event)
{
    auto *size = static_cast<lv_coord_t *>(lv_event_get_param(event));
    *size = std::max<lv_coord_t>(*size, kGlow + 1);
}

} // namespace

void set(lv_obj_t *obj, bool on, const Outline *outline)
{
    if (obj == nullptr) {
        return;
    }
    const bool running = lv_anim_get(obj, on_anim) != nullptr;
    if (on == running) {
        return;
    }
    if (!on) {
        lv_anim_del(obj, on_anim);
        lv_obj_remove_event_cb(obj, on_draw);
        lv_obj_remove_event_cb(obj, on_ext_size);
        // the dot and its glow over the edge go
        lv_obj_invalidate(obj);
        lv_obj_refresh_ext_draw_size(obj);
        return;
    }
    lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, on_ext_size, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
    lv_obj_refresh_ext_draw_size(obj);
    // the size is known once LVGL laid the card out: a card made in this frame has none yet
    lv_obj_update_layout(obj);
    const auto length = static_cast<int32_t>(track_of(obj, outline).length());
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_user_data(&anim, new Run{outline, -1.0f});
    lv_anim_set_deleted_cb(&anim, [](lv_anim_t *a) { delete static_cast<Run *>(lv_anim_get_user_data(a)); });
    lv_anim_set_values(&anim, 0, std::max<int32_t>(length, 1));
    lv_anim_set_time(&anim, static_cast<uint32_t>(std::max<int32_t>(length, 1) * 1000 / kSpeed));
    lv_anim_set_exec_cb(&anim, on_anim);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

} // namespace hc::ui::runner
