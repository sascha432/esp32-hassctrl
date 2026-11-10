#include "forecast_page.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "dashboard.hpp"
#include "hc/core/weather.hpp"
#include "icons.hpp"
#include "panel.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::forecast_page {

namespace {

namespace t = theme;
using core::ForecastEntry;
using core::ForecastType;
using panel::set_hidden;

// Sizes at the normal fonts; the ones that hold text grow with the large fonts (theme::scaled).
// The content keeps this far from the card's sides and top; in the dialog from its bottom more,
// where its round corners cut.
constexpr int kSide = 8;
constexpr int kTop = 6;
constexpr int kBottomDialog = 12;
// the graphs: the units in a column left of the days or hours
constexpr int kLegendWidth = 36;
constexpr int kHourWidth = 44;   // a column of the hourly graph
constexpr int kDayWidthMin = 48; // a column of the daily graph at least; wider when they fit
constexpr int kBarWidth = 16;
constexpr int kBarRadius = 3;
// the bar of a 100 % chance: this part of the card's height, within these limits
constexpr int kBarPct = 16;
constexpr int kBarMin = 26;
constexpr int kBarMax = 56;
constexpr int kLineGap = 2;  // between the lines of a column or a row
constexpr int kLabelGap = 3; // between a point of a curve and its value
constexpr int kCurveGap = 6; // between the curves and the values of the bars
constexpr int kDot = 6;
constexpr int kCurveWidth = 2;
constexpr int kScrollbar = 4;
constexpr int kScrollbarRoom = 6;
// the lists
constexpr int kGapMin = 4;  // between the columns of a row
constexpr int kGap = 8;     // the daily list: the range takes the rest
constexpr int kGapMax = 24; // the hourly list: the rest stays at the right
constexpr int kRowPad = 8;  // the lines of a row from its top and its bottom, together
constexpr int kHeadingPad = 10;
constexpr int kRangeHeight = 6; // the day's range on the week's scale
constexpr int kRangeMin = 40;
constexpr int kNowDot = 12;     // the temperature now on today's range
constexpr lv_opa_t kBarOpa = LV_OPA_60;
constexpr lv_opa_t kBandOpa = LV_OPA_60;

constexpr const char *kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct Box {
    int left, top, right, bottom; // in the card, inside its border; right and bottom exclusive
};

// the texts of an entry, made once per answer
struct Cells {
    char top[12];    // "Today", "Mon"; "Now", "14:00", the weekday at midnight
    char time[8];    // the hourly list: "Now", "00:00" (the day is in the heading)
    char date[12];   // "9 Nov"
    char head[20];   // the heading of its day in the hourly list: "Today", "Tomorrow", "Tue 10 Nov"
    char high[12];   // the day's high, the hour's temperature
    char low[12];    // the day's low
    char chance[8];  // "80%"
    char amount[12]; // "1.8"
    char wind[8];    // "24"
    const char *glyph;
    int day;         // the local day (year * 366 + day of the year)
    bool strong;     // today, now, the first hour of a day: white
    bool current;    // today, now
    bool midnight;   // an hour that starts a day
};

struct Heading {
    int y;     // in the list
    int entry; // the first of its day
};

enum DayColumn { kDay, kDayGlyph, kDayChance, kDayLow, kDayRange, kDayHigh, kDayColumns };
enum HourColumn { kTime, kHourGlyph, kTemp, kChance, kAmount, kWind, kHourColumns };
constexpr int kColumns = 6;

struct State {
    lv_obj_t *card = nullptr;
    lv_obj_t *scroll = nullptr; // the view's viewport
    lv_obj_t *canvas = nullptr; // the view, drawn by draw_graph(), draw_days() or draw_hours()
    lv_obj_t *legend = nullptr; // the units of a graph
    lv_obj_t *sticky = nullptr; // the hourly list: the heading of the day at the top
    lv_obj_t *message = nullptr;
    bool portrait = false;
    int inner_w = 0; // the card inside its border
    int inner_h = 0;
    Box box{};
    int focus = -1;
    ForecastType type = ForecastType::None;
    memory::Vector<ForecastEntry> entries;
    memory::Vector<Cells> cells;
    Units units;
    float current = core::kUnknown;
    int today = -1; // the entry of today, or of this hour
    bool has_chance = false;
    bool has_amount = false;
    bool has_wind = false;
    float lo = 0; // the temperatures' range, the scale of the curves and the day ranges
    float hi = 1;
    // the graphs
    int col_w = 0;
    int top_y = 0;
    int glyph_y = 0;
    int plot_top = 0;
    int plot_bottom = 0;
    int bar_bottom = 0;
    int bar_max = 0;
    int amount_y = 0;
    int wind_y = 0;
    // the lists
    int row_h = 0;
    int head_h = 0;
    bool two_lines = false;
    int col_x[kColumns] = {};
    int col_wd[kColumns] = {};
    memory::Vector<int> row_y; // the hourly list: the top of each entry's row
    memory::Vector<Heading> headings;
};

State g;

lv_coord_t cv(int value)
{
    return static_cast<lv_coord_t>(value);
}

int sc(int px)
{
    return t::scaled(cv(px));
}

int line_h(const lv_font_t &font)
{
    return lv_font_get_line_height(&font);
}

int text_w(const char *text, const lv_font_t &font)
{
    return text[0] == 0 ? 0 : lv_txt_get_width(text, static_cast<uint32_t>(std::strlen(text)), &font, 0, LV_TEXT_FLAG_NONE);
}

// the size of the arrow of the wind beside a text in `font`
int arrow_size(const lv_font_t &font)
{
    return line_h(font) * 2 / 3;
}

// "8.9 mm"; "" without a value
void with_unit(char (&out)[24], const char *value, const std::string &unit)
{
    if (value[0] == 0 || unit.empty()) {
        std::snprintf(out, sizeof(out), "%s", value);
    } else {
        std::snprintf(out, sizeof(out), "%s %s", value, unit.c_str());
    }
}

// ------------------------------------------------------------------------------- drawing

using t::draw_text;

void draw_rect(lv_draw_ctx_t *ctx, int x, int y, int w, int h, uint32_t color, lv_opa_t opa, int radius)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = t::color(color);
    dsc.bg_opa = opa;
    dsc.radius = cv(radius);
    const lv_area_t area = {cv(x), cv(y), cv(x + w - 1), cv(y + h - 1)};
    lv_draw_rect(ctx, &dsc, &area);
}

void draw_line(lv_draw_ctx_t *ctx, int x1, int y1, int x2, int y2, uint32_t color)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = t::color(color);
    dsc.width = cv(kCurveWidth);
    dsc.round_start = 1;
    dsc.round_end = 1;
    const lv_point_t a = {cv(x1), cv(y1)};
    const lv_point_t b = {cv(x2), cv(y2)};
    lv_draw_line(ctx, &dsc, &a, &b);
}

void draw_dot(lv_draw_ctx_t *ctx, int x, int y, uint32_t color)
{
    draw_rect(ctx, x - kDot / 2, y - kDot / 2, kDot, kDot, color, LV_OPA_COVER, LV_RADIUS_CIRCLE);
}

// An arrow of `size` centered at x, y that points where the wind goes: `bearing` is where it comes
// from (0 north, clockwise).
void draw_arrow(lv_draw_ctx_t *ctx, int x, int y, int size, float bearing, uint32_t color)
{
    constexpr float kShape[3][2] = {{0.0f, -1.0f}, {0.6f, 0.85f}, {-0.6f, 0.85f}};
    const float angle = (bearing + 180.0f) * 3.14159265f / 180.0f;
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    const float r = static_cast<float>(size) / 2.0f;
    lv_point_t points[3];
    for (int i = 0; i < 3; i++) {
        const float px = kShape[i][0] * r;
        const float py = kShape[i][1] * r;
        points[i].x = cv(x + static_cast<int>(std::lround(px * c - py * s)));
        points[i].y = cv(y + static_cast<int>(std::lround(px * s + py * c)));
    }
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = t::color(color);
    lv_draw_polygon(ctx, &dsc, points, 3);
}

// the arrow and the speed from x on (the arrow left out when the bearing is unknown); its width
int draw_wind(lv_draw_ctx_t *ctx, const char *speed, float bearing, const lv_font_t &font, uint32_t color, int x, int y)
{
    const int a = arrow_size(font);
    int width = 0;
    if (!std::isnan(bearing)) {
        draw_arrow(ctx, x + a / 2, y + line_h(font) / 2, a, bearing, color);
        width = a + 3;
    }
    const int w = text_w(speed, font);
    draw_text(ctx, speed, font, color, x + width, y, w + 2, LV_TEXT_ALIGN_LEFT);
    return width + w;
}

// the same centered on x
void draw_wind_centered(lv_draw_ctx_t *ctx, const char *speed, float bearing, const lv_font_t &font, uint32_t color,
                        int x, int y)
{
    const int width = (std::isnan(bearing) ? 0 : arrow_size(font) + 3) + text_w(speed, font);
    draw_wind(ctx, speed, bearing, font, color, x - width / 2, y);
}

// ------------------------------------------------------------------------------- the texts

void temperature_text(char (&out)[12], float value)
{
    out[0] = 0;
    if (!std::isnan(value)) {
        std::snprintf(out, sizeof(out), "%s°", tiles::temperature(value).c_str());
    }
}

// the texts of the entries, the range of their temperatures, and what they report
void prepare()
{
    const Backend &backend = dashboard::backend();
    const bool daily = g.type == ForecastType::Daily;
    const int64_t now = backend.clock != nullptr ? backend.clock() : 0;
    std::tm local_now{};
    std::tm local_tomorrow{};
    if (now > 0) {
        backend.local_time(now, local_now);
        backend.local_time(now + 86400, local_tomorrow);
    }
    const int today = now > 0 ? (local_now.tm_year + 1900) * 366 + local_now.tm_yday : -1;
    const int tomorrow = now > 0 ? (local_tomorrow.tm_year + 1900) * 366 + local_tomorrow.tm_yday : -1;

    g.cells.clear();
    g.cells.reserve(g.entries.size());
    g.today = -1;
    g.has_chance = g.has_amount = g.has_wind = false;
    float lo = INFINITY;
    float hi = -INFINITY;
    for (size_t i = 0; i < g.entries.size(); i++) {
        const ForecastEntry &e = g.entries[i];
        Cells c{};
        std::tm local{};
        backend.local_time(e.time, local);
        c.day = (local.tm_year + 1900) * 366 + local.tm_yday;
        const bool current = now > 0 && (daily ? c.day == today : now >= e.time && now < static_cast<int64_t>(e.time) + 3600);
        if (current && g.today < 0) {
            g.today = static_cast<int>(i);
        }
        const char *weekday = kWeekdays[(local.tm_wday % 7 + 7) % 7];
        const char *month = kMonths[(local.tm_mon % 12 + 12) % 12];
        c.midnight = !daily && local.tm_hour == 0;
        if (daily) {
            std::snprintf(c.top, sizeof(c.top), "%s", current ? "Today" : weekday);
        } else if (current) {
            std::snprintf(c.top, sizeof(c.top), "Now");
        } else if (c.midnight) {
            std::snprintf(c.top, sizeof(c.top), "%s", weekday);
        } else {
            std::snprintf(c.top, sizeof(c.top), "%02d:00", local.tm_hour);
        }
        if (current) {
            std::snprintf(c.time, sizeof(c.time), "Now");
        } else {
            std::snprintf(c.time, sizeof(c.time), "%02d:00", local.tm_hour);
        }
        std::snprintf(c.date, sizeof(c.date), "%d %s", local.tm_mday, month);
        if (c.day == today) {
            std::snprintf(c.head, sizeof(c.head), "Today");
        } else if (c.day == tomorrow) {
            std::snprintf(c.head, sizeof(c.head), "Tomorrow");
        } else {
            std::snprintf(c.head, sizeof(c.head), "%s %d %s", weekday, local.tm_mday, month);
        }
        c.strong = current || c.midnight;
        c.current = current;
        temperature_text(c.high, e.temperature);
        temperature_text(c.low, daily ? e.templow : core::kUnknown);
        if (e.precipitation_probability > 0) {
            std::snprintf(c.chance, sizeof(c.chance), "%d%%", e.precipitation_probability);
        }
        // nothing for none: the rows of a dry day stay calm
        if (!std::isnan(e.precipitation) && e.precipitation >= 0.05f) {
            std::snprintf(c.amount, sizeof(c.amount), "%.*f", e.precipitation >= 10 ? 0 : 1,
                          static_cast<double>(e.precipitation));
        }
        if (!std::isnan(e.wind_speed)) {
            std::snprintf(c.wind, sizeof(c.wind), "%.0f", static_cast<double>(e.wind_speed));
        }
        const char *glyph = icons::glyph(core::condition_icon(e.condition));
        c.glyph = glyph != nullptr ? glyph : "";
        g.has_chance = g.has_chance || e.precipitation_probability >= 0;
        g.has_amount = g.has_amount || !std::isnan(e.precipitation);
        g.has_wind = g.has_wind || !std::isnan(e.wind_speed);
        for (const float value : {e.temperature, daily ? e.templow : core::kUnknown}) {
            if (!std::isnan(value)) {
                lo = std::min(lo, value);
                hi = std::max(hi, value);
            }
        }
        g.cells.push_back(c);
    }
    if (lo > hi) {
        lo = 0;
        hi = 1;
    }
    // a flat forecast is a line in the middle
    if (hi - lo < 2) {
        const float middle = (hi + lo) / 2;
        lo = middle - 1;
        hi = middle + 1;
    }
    g.lo = lo;
    g.hi = hi;
}

// ------------------------------------------------------------------------------- the graphs

// the height of `value` in the plot
int plot_y(float value)
{
    return g.plot_bottom - static_cast<int>(std::lround((value - g.lo) / (g.hi - g.lo) * static_cast<float>(g.plot_bottom - g.plot_top)));
}

void layout_graph()
{
    const bool daily = g.type == ForecastType::Daily;
    const int n = static_cast<int>(g.cells.size());
    const int ls = line_h(t::font_small());
    const int legend_w = sc(kLegendWidth);
    // the columns start right of the legend and end at the box; scrolled, they pass under the
    // card's right edge (the dialog's)
    const int left = g.box.left + legend_w;
    const int usable = g.box.right - left;
    g.col_w = daily ? std::max(sc(kDayWidthMin), usable / std::max(n, 1)) : sc(kHourWidth);
    const bool scrolls = n * g.col_w > usable;

    // from the top: the day or the time, the condition, the curves; from the bottom: the wind, the
    // amount, the bars with their chance above them
    g.top_y = g.box.top;
    g.glyph_y = g.top_y + line_h(daily ? t::font_normal() : t::font_small()) + kLineGap;
    int bottom = g.box.bottom - (scrolls ? kScrollbarRoom : 0);
    g.wind_y = bottom - (g.has_wind ? ls : 0);
    bottom = g.has_wind ? g.wind_y - kLineGap : bottom;
    g.amount_y = bottom - (g.has_amount ? ls : 0);
    bottom = g.has_amount ? g.amount_y - kLineGap : bottom;
    g.bar_bottom = bottom;
    g.bar_max = g.has_chance ? std::clamp(g.inner_h * kBarPct / 100, sc(kBarMin), sc(kBarMax)) : 0;
    const int bars_top = g.bar_bottom - g.bar_max - (g.has_chance ? ls : 0);
    // the values above the points (and the lows below them)
    g.plot_top = g.glyph_y + line_h(icons::small()) + kLineGap + ls + kLabelGap + kDot / 2;
    g.plot_bottom = bars_top - kCurveGap - (daily ? ls + kLabelGap : 0) - kDot / 2;
    if (g.plot_bottom <= g.plot_top) {
        g.plot_bottom = g.plot_top + 1;
    }

    lv_obj_set_pos(g.legend, cv(g.box.left), 0);
    lv_obj_set_size(g.legend, cv(legend_w), cv(g.inner_h));
    lv_obj_set_pos(g.scroll, cv(left), 0);
    lv_obj_set_size(g.scroll, cv(g.inner_w - left), cv(g.inner_h));
    lv_obj_set_size(g.canvas, cv(n * g.col_w + (g.inner_w - g.box.right)), cv(g.inner_h));
}

void draw_graph(lv_draw_ctx_t *ctx, const lv_area_t &c)
{
    const int n = static_cast<int>(g.cells.size());
    if (n == 0 || g.col_w <= 0) {
        return;
    }
    const bool daily = g.type == ForecastType::Daily;
    const int ls = line_h(t::font_small());
    const int ox = c.x1;
    const int oy = c.y1;
    // the columns in the area to draw, and one more each side for the segments of the curves
    const lv_area_t &clip = *ctx->clip_area;
    const int first = std::max(0, (clip.x1 - ox) / g.col_w - 1);
    const int last = std::min(n - 1, (clip.x2 - ox) / g.col_w + 1);
    const auto mid = [&](int i) { return ox + i * g.col_w + g.col_w / 2; };

    // a line where a day starts
    for (int i = std::max(first, 1); i <= last; i++) {
        if (g.cells[static_cast<size_t>(i)].midnight) {
            draw_rect(ctx, ox + i * g.col_w, oy, 1, g.inner_h, t::kBorder, LV_OPA_COVER, 0);
        }
    }
    // the band between the days' highs and lows, then the curves
    for (int i = first; i < last; i++) {
        const ForecastEntry &a = g.entries[static_cast<size_t>(i)];
        const ForecastEntry &b = g.entries[static_cast<size_t>(i + 1)];
        if (daily && !std::isnan(a.temperature) && !std::isnan(b.temperature) && !std::isnan(a.templow) &&
            !std::isnan(b.templow)) {
            const lv_point_t band[4] = {{cv(mid(i)), cv(oy + plot_y(a.temperature))},
                                        {cv(mid(i + 1)), cv(oy + plot_y(b.temperature))},
                                        {cv(mid(i + 1)), cv(oy + plot_y(b.templow))},
                                        {cv(mid(i)), cv(oy + plot_y(a.templow))}};
            lv_draw_rect_dsc_t dsc;
            lv_draw_rect_dsc_init(&dsc);
            dsc.bg_color = t::color(t::kBorder);
            dsc.bg_opa = kBandOpa;
            lv_draw_polygon(ctx, &dsc, band, 4);
        }
    }
    for (int i = first; i < last; i++) {
        const ForecastEntry &a = g.entries[static_cast<size_t>(i)];
        const ForecastEntry &b = g.entries[static_cast<size_t>(i + 1)];
        if (!std::isnan(a.temperature) && !std::isnan(b.temperature)) {
            draw_line(ctx, mid(i), oy + plot_y(a.temperature), mid(i + 1), oy + plot_y(b.temperature), t::kText);
        }
        if (daily && !std::isnan(a.templow) && !std::isnan(b.templow)) {
            draw_line(ctx, mid(i), oy + plot_y(a.templow), mid(i + 1), oy + plot_y(b.templow), t::kTextLabel);
        }
    }

    const lv_font_t &top_font = daily ? t::font_normal() : t::font_small();
    for (int i = first; i <= last; i++) {
        const Cells &cell = g.cells[static_cast<size_t>(i)];
        const ForecastEntry &e = g.entries[static_cast<size_t>(i)];
        const int x = ox + i * g.col_w;
        const uint32_t top_color = cell.strong ? t::kText : daily ? t::kTextValue : t::kTextLabel;
        draw_text(ctx, cell.top, top_font, top_color, x, oy + g.top_y, g.col_w, LV_TEXT_ALIGN_CENTER);
        draw_text(ctx, cell.glyph, icons::small(), t::kTextValue, x, oy + g.glyph_y, g.col_w, LV_TEXT_ALIGN_CENTER);
        if (!std::isnan(e.temperature)) {
            const int y = oy + plot_y(e.temperature);
            draw_dot(ctx, mid(i), y, t::kText);
            draw_text(ctx, cell.high, t::font_small(), t::kText, x, y - kDot / 2 - kLabelGap - ls, g.col_w,
                      LV_TEXT_ALIGN_CENTER);
        }
        if (daily && !std::isnan(e.templow)) {
            const int y = oy + plot_y(e.templow);
            draw_dot(ctx, mid(i), y, t::kTextLabel);
            draw_text(ctx, cell.low, t::font_small(), t::kTextLabel, x, y + kDot / 2 + kLabelGap, g.col_w,
                      LV_TEXT_ALIGN_CENTER);
        }
        if (g.has_chance && e.precipitation_probability > 0) {
            const int h = std::max(1, e.precipitation_probability * g.bar_max / 100);
            const int bar_w = sc(kBarWidth);
            draw_rect(ctx, mid(i) - bar_w / 2, oy + g.bar_bottom - h, bar_w, h, t::kRain, kBarOpa, kBarRadius);
            draw_text(ctx, cell.chance, t::font_small(), t::kRain, x, oy + g.bar_bottom - h - ls, g.col_w,
                      LV_TEXT_ALIGN_CENTER);
        }
        if (g.has_amount) {
            draw_text(ctx, cell.amount, t::font_small(), t::kTextLabel, x, oy + g.amount_y, g.col_w, LV_TEXT_ALIGN_CENTER);
        }
        if (g.has_wind && cell.wind[0] != 0) {
            draw_wind_centered(ctx, cell.wind, e.wind_bearing, t::font_small(), t::kTextLabel, mid(i), oy + g.wind_y);
        }
    }
}

// the units left of the columns, a line between them and the columns
void on_draw_legend(lv_event_t *event)
{
    if (g.cells.empty()) {
        return;
    }
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(event), &c);
    const int w = lv_area_get_width(&c) - 1;
    const int ls = line_h(t::font_small());
    draw_rect(ctx, c.x2, c.y1, 1, lv_area_get_height(&c), t::kBorder, LV_OPA_COVER, 0);
    draw_text(ctx, g.units.temperature.c_str(), t::font_small(), t::kTextLabel, c.x1,
              c.y1 + (g.plot_top + g.plot_bottom) / 2 - ls / 2, w, LV_TEXT_ALIGN_CENTER);
    if (g.has_chance) {
        draw_text(ctx, "%", t::font_small(), t::kRain, c.x1, c.y1 + g.bar_bottom - g.bar_max / 2 - ls / 2, w,
                  LV_TEXT_ALIGN_CENTER);
    }
    if (g.has_amount) {
        draw_text(ctx, g.units.precipitation.c_str(), t::font_small(), t::kTextLabel, c.x1, c.y1 + g.amount_y, w,
                  LV_TEXT_ALIGN_CENTER);
    }
    if (g.has_wind) {
        draw_text(ctx, g.units.wind.c_str(), t::font_small(), t::kTextLabel, c.x1, c.y1 + g.wind_y, w, LV_TEXT_ALIGN_CENTER);
    }
}

// ------------------------------------------------------------------------------- the lists

// the columns from the box's left, `gap` apart; a column of width 0 is left out
void place_columns(const int (&widths)[kColumns], int gap)
{
    int x = g.box.left;
    for (int i = 0; i < kColumns; i++) {
        g.col_x[i] = x;
        g.col_wd[i] = widths[i];
        if (widths[i] > 0) {
            x += widths[i] + gap;
        }
    }
}

void place_list(int content_h)
{
    lv_obj_set_pos(g.scroll, 0, cv(g.box.top));
    lv_obj_set_size(g.scroll, cv(g.inner_w), cv(g.inner_h - g.box.top));
    lv_obj_set_size(g.canvas, cv(g.inner_w), cv(content_h + (g.inner_h - g.box.bottom)));
}

// A row per day, as tall as the area allows: two lines (the date, the amount and the wind below)
// when they fit. The day's range is a bar on the scale of the whole week.
void layout_days()
{
    const int n = static_cast<int>(g.cells.size());
    const int ln = line_h(t::font_normal());
    const int ls = line_h(t::font_small());
    const int gh = line_h(icons::small());
    int row = (g.box.bottom - g.box.top) / std::max(n, 1);
    g.two_lines = row >= ln + kLineGap + ls + sc(kRowPad);
    if (!g.two_lines) {
        row = std::max(row, std::max(ln, gh) + sc(kRowPad));
    }
    g.row_h = row;
    int w[kColumns] = {};
    w[kDayGlyph] = gh;
    for (const Cells &c : g.cells) {
        char amount[24];
        with_unit(amount, c.amount, g.units.precipitation);
        w[kDay] = std::max({w[kDay], text_w(c.top, t::font_normal()), g.two_lines ? text_w(c.date, t::font_small()) : 0});
        w[kDayChance] = std::max({w[kDayChance], text_w(c.chance, t::font_normal()),
                                  g.two_lines ? text_w(amount, t::font_small()) : 0});
        w[kDayLow] = std::max(w[kDayLow], text_w(c.low, t::font_normal()));
        w[kDayHigh] = std::max(w[kDayHigh], text_w(c.high, t::font_normal()));
    }
    const int width = g.box.right - g.box.left;
    int fixed = 0;
    for (const int value : w) {
        fixed += value;
    }
    const int gap = std::clamp((width - fixed - sc(kRangeMin)) / (kDayColumns - 1), kGapMin, sc(kGap));
    w[kDayRange] = std::max(0, width - fixed - (kDayColumns - 1) * gap);
    place_columns(w, gap);
    place_list(n * row);
}

void draw_days(lv_draw_ctx_t *ctx, const lv_area_t &c)
{
    const int n = static_cast<int>(g.cells.size());
    const int ln = line_h(t::font_normal());
    const int ls = line_h(t::font_small());
    const int gh = line_h(icons::small());
    const lv_area_t &clip = *ctx->clip_area;
    const float range = g.hi - g.lo;
    const int bar_h = sc(kRangeHeight);
    for (int i = 0; i < n; i++) {
        const int top = c.y1 + i * g.row_h;
        if (top > clip.y2 || top + g.row_h <= clip.y1) {
            continue;
        }
        const Cells &cell = g.cells[static_cast<size_t>(i)];
        const ForecastEntry &e = g.entries[static_cast<size_t>(i)];
        const auto x = [&](int column) { return c.x1 + g.col_x[column]; };
        const auto w = [&](int column) { return g.col_wd[column]; };
        if (i > 0) {
            draw_rect(ctx, c.x1 + g.box.left, top, g.box.right - g.box.left, 1, t::kBorder, LV_OPA_COVER, 0);
        }
        const int line1 = g.two_lines ? top + (g.row_h - ln - kLineGap - ls) / 2 : top + (g.row_h - ln) / 2;
        const int line2 = line1 + ln + kLineGap;
        draw_text(ctx, cell.top, t::font_normal(), cell.strong ? t::kText : t::kTextValue, x(kDay), line1, w(kDay) + 4,
                  LV_TEXT_ALIGN_LEFT);
        draw_text(ctx, cell.glyph, icons::small(), t::kTextValue, x(kDayGlyph), top + (g.row_h - gh) / 2, w(kDayGlyph),
                  LV_TEXT_ALIGN_CENTER);
        draw_text(ctx, cell.chance, t::font_normal(), t::kRain, x(kDayChance), line1, w(kDayChance), LV_TEXT_ALIGN_RIGHT);
        draw_text(ctx, cell.low, t::font_normal(), t::kTextLabel, x(kDayLow), line1, w(kDayLow), LV_TEXT_ALIGN_RIGHT);
        draw_text(ctx, cell.high, t::font_normal(), t::kText, x(kDayHigh), line1, w(kDayHigh), LV_TEXT_ALIGN_RIGHT);

        // the day's range on the week's scale, the temperature now on today's
        const int bar_x = x(kDayRange);
        const int bar_w = w(kDayRange);
        const int bar_y = line1 + ln / 2 - bar_h / 2;
        draw_rect(ctx, bar_x, bar_y, bar_w, bar_h, t::kBorder, LV_OPA_COVER, bar_h / 2);
        if (!std::isnan(e.temperature)) {
            const float low = std::isnan(e.templow) ? e.temperature : e.templow;
            const int x1 = bar_x + static_cast<int>(std::lround((low - g.lo) / range * static_cast<float>(bar_w)));
            const int x2 = bar_x + static_cast<int>(std::lround((e.temperature - g.lo) / range * static_cast<float>(bar_w)));
            draw_rect(ctx, x1, bar_y, std::max(x2 - x1, bar_h), bar_h, t::kTextValue, LV_OPA_COVER, bar_h / 2);
        }
        if (i == g.today && !std::isnan(g.current)) {
            const float value = std::clamp(g.current, g.lo, g.hi);
            const int dot_x = bar_x + static_cast<int>(std::lround((value - g.lo) / range * static_cast<float>(bar_w)));
            const int d = sc(kNowDot);
            // white with a ring of the card's color, so it stands out of the range under it
            draw_rect(ctx, dot_x - d / 2, bar_y + bar_h / 2 - d / 2, d, d, t::kCard, LV_OPA_COVER, LV_RADIUS_CIRCLE);
            draw_rect(ctx, dot_x - d / 2 + 2, bar_y + bar_h / 2 - d / 2 + 2, d - 4, d - 4, t::kText, LV_OPA_COVER,
                      LV_RADIUS_CIRCLE);
        }
        if (!g.two_lines) {
            continue;
        }
        draw_text(ctx, cell.date, t::font_small(), t::kTextLabel, x(kDay), line2, w(kDay) + 4, LV_TEXT_ALIGN_LEFT);
        char amount[24];
        with_unit(amount, cell.amount, g.units.precipitation);
        draw_text(ctx, amount, t::font_small(), t::kTextLabel, x(kDayChance) - 20, line2, w(kDayChance) + 20,
                  LV_TEXT_ALIGN_RIGHT);
        if (cell.wind[0] != 0) {
            // below the range, from the low to the high
            char wind[24];
            with_unit(wind, cell.wind, g.units.wind);
            const int span_x = x(kDayLow);
            const int span_w = x(kDayHigh) + w(kDayHigh) - span_x;
            draw_wind_centered(ctx, wind, e.wind_bearing, t::font_small(), t::kTextLabel, span_x + span_w / 2, line2);
        }
    }
}

// A row per hour under the heading of its day; the heading of the day at the top stays (the
// sticky object). The units of the amount and the wind are in the headings.
void layout_hours()
{
    const int n = static_cast<int>(g.cells.size());
    const int ln = line_h(t::font_normal());
    const int ls = line_h(t::font_small());
    const int gh = line_h(icons::small());
    g.row_h = std::max(ln, gh) + sc(kRowPad);
    g.head_h = ls + sc(kHeadingPad);
    g.row_y.clear();
    g.headings.clear();
    int y = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0 || g.cells[static_cast<size_t>(i)].day != g.cells[static_cast<size_t>(i - 1)].day) {
            g.headings.push_back({y, i});
            y += g.head_h;
        }
        g.row_y.push_back(y);
        y += g.row_h;
    }
    const int arrow = arrow_size(t::font_normal()) + 3;
    int w[kColumns] = {};
    w[kHourGlyph] = gh;
    for (const Cells &c : g.cells) {
        w[kTime] = std::max(w[kTime], text_w(c.time, t::font_normal()));
        w[kTemp] = std::max(w[kTemp], text_w(c.high, t::font_normal()));
        w[kChance] = std::max(w[kChance], text_w(c.chance, t::font_normal()));
        w[kAmount] = std::max(w[kAmount], text_w(c.amount, t::font_normal()));
        w[kWind] = std::max(w[kWind], arrow + text_w(c.wind, t::font_normal()));
    }
    if (g.has_amount) {
        w[kAmount] = std::max(w[kAmount], text_w(g.units.precipitation.c_str(), t::font_small()));
    } else {
        w[kAmount] = 0;
    }
    if (g.has_wind) {
        w[kWind] = std::max(w[kWind], arrow + text_w(g.units.wind.c_str(), t::font_small()));
    } else {
        w[kWind] = 0;
    }
    const int width = g.box.right - g.box.left;
    const auto spread = [&]() {
        int fixed = 0;
        int columns = 0;
        for (const int value : w) {
            fixed += value;
            columns += value > 0 ? 1 : 0;
        }
        return columns > 1 ? (width - fixed) / (columns - 1) : 0;
    };
    // too narrow: the amount goes first
    if (spread() < kGapMin && w[kAmount] > 0) {
        w[kAmount] = 0;
    }
    place_columns(w, std::clamp(spread(), kGapMin, sc(kGapMax)));
    place_list(y);
    lv_obj_set_pos(g.sticky, 0, cv(g.box.top));
    lv_obj_set_size(g.sticky, cv(g.inner_w), cv(g.head_h));
}

// the heading of the day of `entry` at y: the day, the units over their columns, a line below
void draw_heading(lv_draw_ctx_t *ctx, int x0, int y, int entry)
{
    const int ls = line_h(t::font_small());
    const int ty = y + (g.head_h - ls) / 2;
    // opaque: the heading at the top covers the rows scrolled under it
    draw_rect(ctx, x0, y, g.inner_w, g.head_h, t::kCard, LV_OPA_COVER, 0);
    draw_text(ctx, g.cells[static_cast<size_t>(entry)].head, t::font_small(), t::kTextLabel, x0 + g.box.left, ty,
              g.box.right - g.box.left, LV_TEXT_ALIGN_LEFT);
    if (g.col_wd[kAmount] > 0) {
        draw_text(ctx, g.units.precipitation.c_str(), t::font_small(), t::kTextLabel, x0 + g.col_x[kAmount] - 20, ty,
                  g.col_wd[kAmount] + 20, LV_TEXT_ALIGN_RIGHT);
    }
    if (g.col_wd[kWind] > 0) {
        const int arrow = arrow_size(t::font_normal()) + 3;
        draw_text(ctx, g.units.wind.c_str(), t::font_small(), t::kTextLabel, x0 + g.col_x[kWind] + arrow, ty,
                  g.col_wd[kWind] + 20, LV_TEXT_ALIGN_LEFT);
    }
    draw_rect(ctx, x0 + g.box.left, y + g.head_h - 1, g.box.right - g.box.left, 1, t::kBorder, LV_OPA_COVER, 0);
}

void draw_hours(lv_draw_ctx_t *ctx, const lv_area_t &c)
{
    const int n = static_cast<int>(g.cells.size());
    const int ln = line_h(t::font_normal());
    const int gh = line_h(icons::small());
    const lv_area_t &clip = *ctx->clip_area;
    for (const Heading &h : g.headings) {
        const int y = c.y1 + h.y;
        if (y <= clip.y2 && y + g.head_h > clip.y1) {
            draw_heading(ctx, c.x1, y, h.entry);
        }
    }
    const auto x = [&](int column) { return c.x1 + g.col_x[column]; };
    const auto w = [&](int column) { return g.col_wd[column]; };
    for (int i = 0; i < n; i++) {
        const int top = c.y1 + g.row_y[static_cast<size_t>(i)];
        if (top > clip.y2 || top + g.row_h <= clip.y1) {
            continue;
        }
        const Cells &cell = g.cells[static_cast<size_t>(i)];
        const ForecastEntry &e = g.entries[static_cast<size_t>(i)];
        // a line between the hours of a day (the heading has its own)
        if (i > 0 && cell.day == g.cells[static_cast<size_t>(i - 1)].day) {
            draw_rect(ctx, c.x1 + g.box.left, top, g.box.right - g.box.left, 1, t::kBorder, LV_OPA_COVER, 0);
        }
        const int ty = top + (g.row_h - ln) / 2;
        draw_text(ctx, cell.time, t::font_normal(), cell.current ? t::kText : t::kTextValue, x(kTime), ty, w(kTime) + 4,
                  LV_TEXT_ALIGN_LEFT);
        draw_text(ctx, cell.glyph, icons::small(), t::kTextValue, x(kHourGlyph), top + (g.row_h - gh) / 2, w(kHourGlyph),
                  LV_TEXT_ALIGN_CENTER);
        draw_text(ctx, cell.high, t::font_normal(), t::kText, x(kTemp), ty, w(kTemp), LV_TEXT_ALIGN_RIGHT);
        draw_text(ctx, cell.chance, t::font_normal(), t::kRain, x(kChance), ty, w(kChance), LV_TEXT_ALIGN_RIGHT);
        if (w(kAmount) > 0) {
            draw_text(ctx, cell.amount, t::font_normal(), t::kTextLabel, x(kAmount), ty, w(kAmount), LV_TEXT_ALIGN_RIGHT);
        }
        if (w(kWind) > 0 && cell.wind[0] != 0) {
            draw_wind(ctx, cell.wind, e.wind_bearing, t::font_normal(), t::kTextLabel, x(kWind), ty);
        }
    }
}

// ------------------------------------------------------------------------------- events

void on_draw_canvas(lv_event_t *event)
{
    if (g.cells.empty()) {
        return;
    }
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(event), &c);
    if (!g.portrait) {
        draw_graph(ctx, c);
    } else if (g.type == ForecastType::Daily) {
        draw_days(ctx, c);
    } else {
        draw_hours(ctx, c);
    }
}

// the heading of the day whose rows are at the top of the hourly list
void on_draw_sticky(lv_event_t *event)
{
    if (g.headings.empty() || g.type != ForecastType::Hourly) {
        return;
    }
    const int scrolled = lv_obj_get_scroll_y(g.scroll);
    int entry = g.headings[0].entry;
    for (const Heading &h : g.headings) {
        if (h.y > scrolled) {
            break;
        }
        entry = h.entry;
    }
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(event), &c);
    draw_heading(lv_event_get_draw_ctx(event), c.x1, c.y1, entry);
}

void on_scroll(lv_event_t *)
{
    if (g.sticky != nullptr) {
        lv_obj_invalidate(g.sticky);
    }
}

lv_obj_t *drawn(lv_obj_t *parent, lv_event_cb_t callback)
{
    lv_obj_t *obj = t::plain(parent);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_DRAW_MAIN, nullptr);
    set_hidden(obj, true);
    return obj;
}

} // namespace

void build(lv_obj_t *parent, const lv_area_t &area, bool portrait, bool dialog, int focus)
{
    forget();
    g.portrait = portrait;
    g.focus = focus;
    lv_area_t a = area;
    if (dialog) {
        // no card in the dialog: the background runs to its edges below the header, a line on top
        // separates it from the header (like the history view)
        lv_obj_update_layout(parent);
        a = {0, area.y1, cv(lv_obj_get_content_width(parent) - 1), cv(lv_obj_get_content_height(parent) - 1)};
    }
    g.card = panel::card(parent, a);
    lv_obj_clear_flag(g.card, LV_OBJ_FLAG_CLICKABLE);
    if (dialog) {
        lv_obj_set_style_radius(g.card, 0, 0);
        lv_obj_set_style_border_side(g.card, LV_BORDER_SIDE_TOP, 0);
    }
    // the children sit inside the card's 1 px border
    g.inner_w = lv_area_get_width(&a) - 2;
    g.inner_h = lv_area_get_height(&a) - 2;
    // in the dialog the content lines up with the header's text
    const int side = dialog ? area.x1 - 1 : sc(kSide);
    g.box = {side, sc(kTop), g.inner_w - side, g.inner_h - (dialog ? sc(kBottomDialog) : sc(kTop))};

    g.scroll = t::plain(g.card);
    lv_obj_add_flag(g.scroll, LV_OBJ_FLAG_SCROLLABLE);
    // no bounce: the heading at the top of the hourly list would stand apart from its rows
    lv_obj_clear_flag(g.scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(g.scroll, portrait ? LV_DIR_VER : LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(g.scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(g.scroll, t::color(t::kBorder), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(g.scroll, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(g.scroll, cv(kScrollbar), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(g.scroll, cv(kScrollbar / 2), LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(g.scroll, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(g.scroll, 2, LV_PART_SCROLLBAR);
    // the drag scrolls the view, it is not a gesture of the screen
    lv_obj_clear_flag(g.scroll, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(g.scroll, on_scroll, LV_EVENT_SCROLL, nullptr);
    set_hidden(g.scroll, true);
    g.canvas = drawn(g.scroll, on_draw_canvas);
    set_hidden(g.canvas, false);
    if (portrait) {
        g.sticky = drawn(g.card, on_draw_sticky);
    } else {
        g.legend = drawn(g.card, on_draw_legend);
    }
    g.message = t::label(g.card, "", t::font_normal(), t::kTextLabel);
    lv_obj_center(g.message);
    set_hidden(g.message, true);
}

void show(ForecastType type, const memory::Vector<ForecastEntry> &entries, const Units &units)
{
    if (g.card == nullptr) {
        return;
    }
    // an update of the forecast shown keeps where it was scrolled to
    const bool again = type == g.type && !g.cells.empty();
    const lv_coord_t scroll_x = lv_obj_get_scroll_x(g.scroll);
    const lv_coord_t scroll_y = lv_obj_get_scroll_y(g.scroll);
    g.type = type;
    g.entries = entries;
    g.units = units;
    prepare();
    if (g.cells.empty()) {
        clear();
        return;
    }
    if (!g.portrait) {
        layout_graph();
    } else if (type == ForecastType::Daily) {
        layout_days();
    } else {
        layout_hours();
    }
    set_hidden(g.scroll, false);
    set_hidden(g.legend, false);
    set_hidden(g.sticky, !g.portrait || type != ForecastType::Hourly);
    lv_obj_invalidate(g.card);
    lv_obj_update_layout(g.scroll);
    const int focus = g.focus;
    g.focus = -1;
    if (again) {
        lv_obj_scroll_to(g.scroll, scroll_x, scroll_y, LV_ANIM_OFF);
    } else if (type == ForecastType::Hourly && focus >= 0 && focus < static_cast<int>(g.cells.size())) {
        // the hour tapped in the panel at the left, or under the heading at the top
        if (g.portrait) {
            lv_obj_scroll_to_y(g.scroll, cv(g.row_y[static_cast<size_t>(focus)] - g.head_h), LV_ANIM_OFF);
        } else {
            lv_obj_scroll_to_x(g.scroll, cv(focus * g.col_w), LV_ANIM_OFF);
        }
    } else {
        lv_obj_scroll_to(g.scroll, 0, 0, LV_ANIM_OFF);
    }
}

void clear()
{
    if (g.card == nullptr) {
        return;
    }
    g.entries.clear();
    g.cells.clear();
    g.headings.clear();
    g.row_y.clear();
    set_hidden(g.scroll, true);
    set_hidden(g.legend, true);
    set_hidden(g.sticky, true);
}

void message(const char *text)
{
    if (g.message == nullptr) {
        return;
    }
    panel::set_text(g.message, text);
    set_hidden(g.message, text[0] == 0);
}

void set_current(float temperature)
{
    if (temperature == g.current || (std::isnan(temperature) && std::isnan(g.current))) {
        return;
    }
    g.current = temperature;
    if (g.canvas != nullptr && g.portrait && g.type == ForecastType::Daily) {
        lv_obj_invalidate(g.canvas);
    }
}

void forget()
{
    // the objects go with their parent
    g = {};
}

} // namespace hc::ui::forecast_page
