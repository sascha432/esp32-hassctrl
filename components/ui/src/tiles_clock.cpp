// The clock tile (docs/ui/lvgl/lvgl-dashboard.md section 3): digital or analog, ticked by its own
// timer, in the tile's time zone or the device's.

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "dashboard.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "tiles_parts.hpp"

namespace hc::ui::tiles::detail {

namespace {

namespace t = theme;

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
            lv_meter_set_indicator_value(view.face, view.hour_hand, 0);
            lv_meter_set_indicator_value(view.face, view.minute_hand, 0);
            if (view.second_hand != nullptr) {
                lv_meter_set_indicator_value(view.face, view.second_hand, 0);
            }
            lv_obj_set_style_bg_color(view.face, t::color(t::kTextMuted), LV_PART_INDICATOR);
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
        // the hour hand moves on with the minutes, the minute hand with the seconds when they show
        lv_meter_set_indicator_value(view.face, view.hour_hand, (local.tm_hour % 12) * 60 + local.tm_min);
        lv_meter_set_indicator_value(view.face, view.minute_hand,
                                     view.seconds ? local.tm_min * 60 + local.tm_sec : local.tm_min);
        if (view.second_hand != nullptr) {
            lv_meter_set_indicator_value(view.face, view.second_hand, local.tm_sec);
        }
        lv_obj_set_style_bg_color(view.face, t::color(t::kText), LV_PART_INDICATOR);
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

// The face of an analog clock, `d` px across: an lv_meter (the hands are its needles, the ticks
// its scales; a tick of the hands redraws only their strips). Three scales: the hours (0 to 720
// minutes of twelve hours), the minutes (0 to 3600 seconds with the second hand, so it creeps
// on; else 0 to 60) and the seconds. The ticks follow the tile's `ticks`: the hour marks on the
// hours scale, the minute marks on the minutes scale below them. No major ticks: they draw labels.
lv_obj_t *analog_face(View &view, const core::Tile &tile, lv_obj_t *parent, lv_coord_t d)
{
    lv_obj_t *face = lv_meter_create(parent);
    lv_obj_remove_style_all(face);
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
    const lv_coord_t r = d / 2 - pad;
    // the hands have round ends; the hub is the meter's own middle
    lv_obj_set_style_line_rounded(face, true, LV_PART_ITEMS);
    const lv_coord_t hub = std::max<lv_coord_t>(5, r / 8) | 1;
    lv_obj_set_style_width(face, hub, LV_PART_INDICATOR);
    lv_obj_set_style_height(face, hub, LV_PART_INDICATOR);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(face, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(face, t::color(t::kText), LV_PART_INDICATOR);

    // scales: 360 degrees from twelve o'clock (270 in LVGL's terms), clockwise. The minutes scale
    // first, so the hour marks of the hours scale draw over its minute marks.
    const auto scale = [&](int32_t max) {
        lv_meter_scale_t *s = lv_meter_add_scale(face);
        lv_meter_set_scale_range(face, s, 0, max, 360, 270);
        lv_meter_set_scale_ticks(face, s, 0, 0, 0, t::color(t::kTextMuted));
        return s;
    };
    lv_meter_scale_t *minutes = scale(tile.seconds ? 3600 : 60);
    lv_meter_scale_t *hours = scale(720);
    lv_meter_scale_t *seconds = tile.seconds ? scale(60) : nullptr;
    // ticks: a face under 30 px of radius shows at most the hour marks
    core::ClockTicks ticks = tile.ticks;
    if (ticks == core::ClockTicks::Minute && r < 30) {
        ticks = core::ClockTicks::Hour;
    }
    switch (ticks) {
    case core::ClockTicks::Quarter:
        lv_meter_set_scale_ticks(face, hours, 5, std::max<lv_coord_t>(2, r / 25), r / 5, t::color(t::kTextLabel));
        break;
    case core::ClockTicks::Minute:
        lv_meter_set_scale_ticks(face, minutes, 61, 1, r / 10, t::color(t::kTextMuted));
        [[fallthrough]];
    case core::ClockTicks::Hour:
        lv_meter_set_scale_ticks(face, hours, 13, std::max<lv_coord_t>(2, r / 40), r / 6, t::color(t::kTextLabel));
        break;
    case core::ClockTicks::None:
        break;
    }
    // the hands, hour first: a later needle draws over an earlier one; lengths as parts of r
    view.hour_hand = lv_meter_add_needle_line(face, hours, static_cast<uint16_t>(std::max<lv_coord_t>(3, r / 14)),
                                              t::color(t::kText), static_cast<int16_t>(-r * 45 / 100));
    view.minute_hand = lv_meter_add_needle_line(face, minutes, static_cast<uint16_t>(std::max<lv_coord_t>(2, r / 20)),
                                                t::color(t::kText), static_cast<int16_t>(-r * 20 / 100));
    if (seconds != nullptr) {
        view.second_hand = lv_meter_add_needle_line(face, seconds, r >= 100 ? 2 : 1, t::color(t::kActive),
                                                    static_cast<int16_t>(-r * 10 / 100));
    }
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
