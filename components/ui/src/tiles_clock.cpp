// The clock tile (docs/ui/lvgl/lvgl-dashboard.md section 3): digital or analog, ticked by its own
// timer, in the tile's time zone or the device's.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "dashboard.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "tiles_parts.hpp"

namespace hc::ui::tiles::detail {

namespace {

namespace t = theme;

constexpr int kHour = 0;
constexpr int kMinute = 1;

// the middle of the face inside its padding (the rim)
lv_point_t face_center(lv_obj_t *face)
{
    lv_area_t area;
    lv_obj_get_content_coords(face, &area);
    return {static_cast<lv_coord_t>(area.x1 + lv_area_get_width(&area) / 2),
            static_cast<lv_coord_t>(area.y1 + lv_area_get_height(&area) / 2)};
}

// the point `radius` px from `c` at `angle` (tenths of a degree clockwise from twelve)
lv_point_t at(lv_point_t c, int32_t angle, lv_coord_t radius)
{
    const float rad = static_cast<float>(angle) * 3.14159265f / 1800.0f;
    return {static_cast<lv_coord_t>(c.x + std::lround(static_cast<float>(radius) * std::sin(rad))),
            static_cast<lv_coord_t>(c.y - std::lround(static_cast<float>(radius) * std::cos(rad)))};
}

// a hand: its length and width as parts of the face's radius, its color
struct Hand {
    lv_coord_t length;
    lv_coord_t width;
    uint32_t color;
};

Hand hand(const ClockFace &f, int which)
{
    switch (which) {
    case kHour:
        return {static_cast<lv_coord_t>(f.r - f.r * 45 / 100), std::max<lv_coord_t>(3, f.r / 14), t::kText};
    case kMinute:
        return {static_cast<lv_coord_t>(f.r - f.r * 20 / 100), std::max<lv_coord_t>(2, f.r / 20), t::kText};
    default:
        return {static_cast<lv_coord_t>(f.r - f.r * 10 / 100), static_cast<lv_coord_t>(f.r >= 100 ? 2 : 1), t::kActive};
    }
}

// the strip a hand covers at `angle`, with its round end and the hub
lv_area_t hand_area(lv_obj_t *face, const ClockFace &f, int which, int16_t angle)
{
    const lv_point_t c = face_center(face);
    const Hand h = hand(f, which);
    const lv_point_t end = at(c, angle, h.length);
    const auto grow = static_cast<lv_coord_t>(std::max<lv_coord_t>(h.width, f.hub) / 2 + 2);
    return {static_cast<lv_coord_t>(std::min(c.x, end.x) - grow), static_cast<lv_coord_t>(std::min(c.y, end.y) - grow),
            static_cast<lv_coord_t>(std::max(c.x, end.x) + grow), static_cast<lv_coord_t>(std::max(c.y, end.y) + grow)};
}

// Turns the hands and redraws only the strips of the ones that moved (each holds the hub).
void set_hands(View &view, const int16_t (&angles)[3], bool clock_set)
{
    ClockFace &f = view.clock_face;
    if (clock_set != f.clock_set) {
        f.clock_set = clock_set;
        lv_obj_invalidate(view.face);
    }
    for (int i = 0; i < (f.seconds ? 3 : 2); i++) {
        if (angles[i] == f.angles[i]) {
            continue;
        }
        lv_area_t area = hand_area(view.face, f, i, f.angles[i]);
        lv_obj_invalidate_area(view.face, &area);
        f.angles[i] = angles[i];
        area = hand_area(view.face, f, i, f.angles[i]);
        lv_obj_invalidate_area(view.face, &area);
    }
}

void line(lv_draw_ctx_t *ctx, lv_point_t from, lv_point_t to, lv_coord_t width, lv_color_t color, bool rounded)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.width = width;
    dsc.color = color;
    dsc.round_start = rounded;
    dsc.round_end = rounded;
    lv_draw_line(ctx, &dsc, &from, &to);
}

// a filled circle `d` px across (odd: centered on the pixel) around `center`
void dot(lv_draw_ctx_t *ctx, lv_point_t center, lv_coord_t d, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    const lv_area_t area{static_cast<lv_coord_t>(center.x - d / 2), static_cast<lv_coord_t>(center.y - d / 2),
                         static_cast<lv_coord_t>(center.x - d / 2 + d - 1),
                         static_cast<lv_coord_t>(center.y - d / 2 + d - 1)};
    lv_draw_rect(ctx, &dsc, &area);
}

// The face: the ticks (lines from the radius inwards), the hands from the middle, the hour hand
// first so the later ones lie over it, and the hub over them all.
void on_draw_face(lv_event_t *event)
{
    const View &view = *static_cast<const View *>(lv_event_get_user_data(event));
    const ClockFace &f = view.clock_face;
    const lv_point_t c = face_center(view.face);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    const auto ticks = [&](int count, lv_coord_t width, lv_coord_t length, uint32_t color) {
        for (int i = 0; i < count; i++) {
            const int32_t angle = i * 3600 / count;
            line(ctx, at(c, angle, static_cast<lv_coord_t>(f.r - length)), at(c, angle, f.r), width,
                 t::color(color), false);
        }
    };
    switch (f.ticks) {
    case core::ClockTicks::Quarter:
        ticks(4, std::max<lv_coord_t>(2, f.r / 25), static_cast<lv_coord_t>(f.r / 5), t::kTextLabel);
        break;
    case core::ClockTicks::Minute:
        ticks(60, 1, static_cast<lv_coord_t>(f.r / 10), t::kTextMuted);
        [[fallthrough]];
    case core::ClockTicks::Hour:
        ticks(12, std::max<lv_coord_t>(2, f.r / 40), static_cast<lv_coord_t>(f.r / 6), t::kTextLabel);
        break;
    case core::ClockTicks::None:
        break;
    }
    for (int i = 0; i < (f.seconds ? 3 : 2); i++) {
        const Hand h = hand(f, i);
        line(ctx, c, at(c, f.angles[i], h.length), h.width, t::color(h.color), true);
    }
    dot(ctx, c, f.hub, t::color(f.clock_set ? t::kText : t::kTextMuted));
}

} // namespace

// Draws the time of the clock view: the device's clock, read through the backend, in the tile's
// zone (core::local_time) or the device's (the backend's local time, the same as the status bar).
// `force` redraws although the minute (the second with `seconds`) did not change.
void tick_clock(View &view, bool force)
{
    const Backend &backend = dashboard::backend();
    const int64_t now = backend.clock();
    const int64_t key = now == 0 ? 0 : view.seconds ? now : now / 60;
    if (!force && key == view.clock_shown) {
        return;
    }
    view.clock_shown = key;
    if (now == 0) {
        // the clock is not set yet: dashes, the hands at twelve with a muted hub
        set_text(view.value, view.seconds ? "--:--:--" : "--:--");
        set_hidden(view.suffix, true);
        set_hidden(view.date, true);
        if (view.face != nullptr) {
            set_hands(view, {0, 0, 0}, false);
        }
        return;
    }
    std::tm local{};
    if (view.has_zone) {
        core::local_time(now, view.zone_rule, local);
    } else {
        backend.local_time(now, local);
    }
    char text[24];
    int hour = local.tm_hour;
    if (view.hours12) {
        hour = hour % 12 == 0 ? 12 : hour % 12;
    }
    if (view.seconds) {
        std::snprintf(text, sizeof(text), view.hours12 ? "%d:%02d:%02d" : "%02d:%02d:%02d", hour, local.tm_min, local.tm_sec);
    } else {
        std::snprintf(text, sizeof(text), view.hours12 ? "%d:%02d" : "%02d:%02d", hour, local.tm_min);
    }
    set_text(view.value, text);
    if (view.suffix != nullptr) {
        set_text(view.suffix, local.tm_hour < 12 ? "AM" : "PM");
        set_hidden(view.suffix, false);
    }
    if (view.face != nullptr) {
        // in tenths of a degree: the hour hand moves on with the minutes, the minute hand with the
        // seconds when they show
        const int hour = ((local.tm_hour % 12) * 60 + local.tm_min) * 5;
        const int minute = view.seconds ? local.tm_min * 60 + local.tm_sec : local.tm_min * 60;
        set_hands(view, {static_cast<int16_t>(hour), static_cast<int16_t>(minute), static_cast<int16_t>(local.tm_sec * 60)},
                  true);
    }
    if (view.date != nullptr) {
        // "Tue 13 Oct"
        if (std::strftime(text, sizeof(text), "%a %d %b", &local) != 0) {
            set_text(view.date, text);
        }
        set_hidden(view.date, false);
    }
}

namespace {

void on_clock_tick(lv_timer_t *timer)
{
    tick_clock(*static_cast<View *>(timer->user_data), false);
}

// The digital clock: a column of the optional icon, the time (with AM/PM beside it), the date and
// the name. The time takes the largest font (the digit fonts of 72 and 48 px, then the text fonts)
// that fits the width and leaves the lines below their room; chosen once, so the digits stay put.
void create_digital_clock(View &view, const core::Tile &tile, lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *col = column(view.card);
    const bool with_icon = !tile.icon.empty() && tile.icon != "none";
    const bool with_name = !tile.name.empty();
    const lv_font_t &date_font = view.large ? t::font_normal() : t::font_small();
    const lv_font_t &suffix_font = view.large ? t::font_medium() : t::font_small();
    // the lines above and below the time, with the column's gaps
    lv_coord_t lines = 0;
    if (with_icon) {
        lines += lv_font_get_line_height(&icons::small()) + 2;
    }
    if (tile.date) {
        lines += lv_font_get_line_height(&date_font) + 2;
    }
    if (with_name) {
        lines += lv_font_get_line_height(&name_font(view)) + 2;
    }
    const char *sample = tile.seconds ? "00:00:00" : "00:00";
    lv_coord_t room = view.value_width;
    if (tile.hours12) {
        room -= text_width("PM", suffix_font) + 4;
    }
    const lv_font_t &font = largest_font({&t::font_digits_72(), &t::font_digits_48(), &t::font_huge(), &t::font_large(),
                                          &t::font_medium(), &t::font_normal(), &t::font_small()},
                                         sample, room, static_cast<lv_coord_t>(height - 14 - lines));
    static_cast<void>(width);
    if (with_icon) {
        view.icon = t::label(col, "", icons::small(), t::kTextLabel);
    }
    lv_obj_t *row = flex_row(col, 4);
    view.value = t::label(row, "", font, t::kText);
    if (tile.hours12) {
        view.suffix = t::label(row, "", suffix_font, t::kTextLabel);
    }
    if (tile.date) {
        view.date = text(col, date_font, t::kTextValue);
    }
    if (with_name) {
        view.name = name_label(view, col);
    }
}

// The face of an analog clock, `d` px across: a plain object that draws the ticks, the hands and
// the hub itself (on_draw_face), with `border` a rim around them.
lv_obj_t *analog_face(View &view, const core::Tile &tile, lv_obj_t *parent, lv_coord_t d)
{
    lv_obj_t *face = t::plain(parent);
    lv_obj_set_size(face, d, d);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(face, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_coord_t pad = 0;
    if (tile.border) {
        const lv_coord_t rim = d >= 200 ? 3 : 2;
        lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(face, rim, 0);
        lv_obj_set_style_border_color(face, t::color(t::kTextLabel), 0);
        pad = rim + 2;
    }
    lv_obj_set_style_pad_all(face, pad, 0);
    ClockFace &f = view.clock_face;
    f = {};
    f.r = static_cast<lv_coord_t>(d / 2 - pad);
    f.hub = static_cast<lv_coord_t>(std::max<lv_coord_t>(5, f.r / 8) | 1);
    f.seconds = tile.seconds;
    // a face under 30 px of radius shows at most the hour marks
    f.ticks = tile.ticks == core::ClockTicks::Minute && f.r < 30 ? core::ClockTicks::Hour : tile.ticks;
    lv_obj_add_event_cb(face, on_draw_face, LV_EVENT_DRAW_MAIN, &view);
    view.face = face;
    return face;
}

// The analog clock: the face with the date and the name below it; a wide tile puts them beside
// the face. A face that would be smaller than 40 px drops the date, then the name.
void create_analog_clock(View &view, const core::Tile &tile, lv_coord_t width, lv_coord_t height)
{
    const lv_coord_t inner_w = width - 14;
    const lv_coord_t inner_h = height - 14;
    bool with_name = !tile.name.empty();
    bool with_date = tile.date;
    const lv_font_t &date_font = view.large ? t::font_normal() : t::font_small();
    const lv_coord_t name_h = lv_font_get_line_height(&name_font(view)) + 2;
    const lv_coord_t date_h = lv_font_get_line_height(&date_font) + 2;
    constexpr lv_coord_t kSmallestFace = 40;

    if (view.wide && (with_name || with_date)) {
        // the face left, the texts right of it
        lv_obj_t *row = t::plain(view.card);
        lv_obj_set_size(row, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
        const lv_coord_t d = inner_h;
        analog_face(view, tile, row, d);
        lv_obj_t *col = t::plain(row);
        lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 2, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
        view.value_width = static_cast<lv_coord_t>(inner_w - d - 12);
        lv_obj_set_width(col, view.value_width);
        if (with_name) {
            view.name = t::label(col, "", name_font(view), t::kTextLabel);
            lv_obj_set_width(view.name, LV_PCT(100));
        }
        if (with_date) {
            view.date = t::label(col, "", date_font, t::kTextValue);
            lv_obj_set_width(view.date, LV_PCT(100));
        }
        return;
    }

    lv_coord_t d = std::min(inner_w, static_cast<lv_coord_t>(inner_h - (with_name ? name_h : 0) - (with_date ? date_h : 0)));
    if (d < kSmallestFace && with_date) {
        with_date = false;
        d = std::min(inner_w, static_cast<lv_coord_t>(inner_h - (with_name ? name_h : 0)));
    }
    if (d < kSmallestFace && with_name) {
        with_name = false;
        d = std::min(inner_w, inner_h);
    }
    lv_obj_t *col = column(view.card);
    analog_face(view, tile, col, d);
    if (with_date) {
        view.date = text(col, date_font, t::kTextValue);
    }
    if (with_name) {
        view.name = name_label(view, col);
    }
}

} // namespace

void create_clock(View &view, const core::Tile &tile, lv_coord_t width, lv_coord_t height)
{
    view.hours12 = tile.hours12;
    view.seconds = tile.seconds;
    if (const char *rule = tile.zone.empty() ? nullptr : core::time_zone_rule(tile.zone)) {
        view.has_zone = core::parse_tz_rule(rule, view.zone_rule); // finish() checked the name
    }
    if (tile.style == core::ClockStyle::Analog) {
        create_analog_clock(view, tile, width, height);
    } else {
        create_digital_clock(view, tile, width, height);
    }
    view.clock_timer = lv_timer_create(on_clock_tick, tile.seconds ? kClockSecondsTickMs : kClockTickMs, &view);
}

} // namespace hc::ui::tiles::detail
