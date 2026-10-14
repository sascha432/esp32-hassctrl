#include "swipe.hpp"

#include "dashboard.hpp"
#include "sound.hpp"
#include "theme.hpp"

namespace hc::ui::swipe {

namespace {

namespace t = theme;

// the band, the dots and their gap: the base sizes times 1.5, times 2 with large fonts
constexpr lv_coord_t kBandBase = 16;
constexpr lv_coord_t kDotBase = 6;
constexpr lv_coord_t kGapBase = 4;

lv_coord_t scaled(lv_coord_t base)
{
    return static_cast<lv_coord_t>(dashboard::options().large_fonts ? base * 2 : base * 3 / 2);
}

enum class Lock : uint8_t { None, Horizontal, Vertical };

struct Watch {
    bool pressed = false;
    lv_point_t at{};
    Lock lock = Lock::None;
};

Watch g;

bool touch_point(lv_point_t &out)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == nullptr) {
        return false;
    }
    lv_indev_get_point(indev, &out);
    return true;
}

void on_watch(lv_event_t *event)
{
    lv_point_t point;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        g = {};
        if (touch_point(point) && dashboard::panel_tile() == core::kNone) {
            g.pressed = true;
            g.at = point;
        }
        break;
    case LV_EVENT_PRESSING: {
        if (!g.pressed || !touch_point(point)) {
            break;
        }
        const lv_coord_t dx = static_cast<lv_coord_t>(point.x - g.at.x);
        const lv_coord_t dy = static_cast<lv_coord_t>(point.y - g.at.y);
        if (g.lock == Lock::None && (LV_ABS(dx) > kSlop || LV_ABS(dy) > kSlop)) {
            g.lock = LV_ABS(dx) > LV_ABS(dy) ? Lock::Horizontal : Lock::Vertical;
        }
        if (g.lock == Lock::Horizontal && LV_ABS(dx) >= kSwipeStart) {
            g = {};
            // the rest of this press goes nowhere: the tile under the finger is not tapped
            lv_indev_wait_release(lv_indev_get_act());
            dashboard::swiped(dx < 0 ? 1 : -1);
        }
        break;
    }
    default: // released, press lost
        g = {};
        break;
    }
}

void on_band(lv_event_t *event)
{
    lv_point_t point;
    lv_obj_t *band = lv_event_get_current_target(event);
    if (!touch_point(point)) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(band, &area);
    dashboard::swiped(point.x < (area.x1 + area.x2) / 2 ? -1 : 1);
}

} // namespace

void watch(lv_obj_t *obj)
{
    for (const lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(obj, on_watch, code, nullptr);
    }
}

bool active()
{
    return g.pressed && g.lock == Lock::Horizontal;
}

lv_coord_t dots_height()
{
    return scaled(kBandBase);
}

lv_obj_t *dots(lv_obj_t *parent, lv_coord_t width, size_t count, size_t current)
{
    lv_obj_t *band = t::plain(parent);
    lv_obj_set_size(band, width, dots_height());
    lv_obj_align(band, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(band, scaled(kGapBase), 0);
    lv_obj_add_flag(band, LV_OBJ_FLAG_CLICKABLE);
    sound::tappable(band);
    lv_obj_add_event_cb(band, on_band, LV_EVENT_CLICKED, nullptr);
    watch(band);
    for (size_t i = 0; i < count; i++) {
        lv_obj_t *dot = t::plain(band);
        lv_obj_set_size(dot, scaled(kDotBase), scaled(kDotBase));
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, i == current ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_style_bg_color(dot, t::color(i == current ? t::kText : t::kTextLabel), 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
    return band;
}

} // namespace hc::ui::swipe
