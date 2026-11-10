#include "weather_panel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <string>

#include "dashboard.hpp"
#include "forecast_page.hpp"
#include "hc/core/weather.hpp"
#include "icons.hpp"
#include "sound.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::weather_panel {

namespace {

namespace t = theme;
using core::ForecastType;
using panel::set_hidden;
using panel::set_text;

// the rows and the forecast keep this much more from the sides than the frame's margin (8):
// 12 px, 24 in the P4's dialog (and from its bottom), where the content sat on the edge
constexpr lv_coord_t kInset = 4;
constexpr lv_coord_t kInsetLarge = 16;
// the rows of the current values: a glyph, the name, the value right aligned. They are spread
// over the space above the forecast, kRowGap to kRowGapMax (theme::scaled) apart.
constexpr int kMaxRows = 9;
constexpr lv_coord_t kRowGap = 4;
constexpr lv_coord_t kRowGapMax = 18;
// two columns of rows in a control at least this wide (when one is not tall enough), this far apart
constexpr lv_coord_t kTwoColumns = 400;
constexpr lv_coord_t kColumnGap = 16;
// the name of a row this far right of its glyph
constexpr lv_coord_t kGlyphGap = 6;
// the forecast: "Forecast" and the chips above the card, the card below
constexpr lv_coord_t kChipGap = 6;
constexpr lv_coord_t kCardGap = 8;
// above the forecast, below the rows
constexpr lv_coord_t kSectionGap = 8;
// inside the card: its padding and the gaps of a column's lines
constexpr lv_coord_t kCardPad = 6;
constexpr lv_coord_t kLineGap = 2;

// The sizes that hold text grow with the fonts (theme::scaled); the numbers are those of the
// normal fonts.
struct Sizes {
    lv_coord_t title_height; // the row of "Forecast" and the chips
    lv_coord_t chip_width;
    lv_coord_t chip_height;
    lv_coord_t column_width; // a day or an hour of the forecast
};

Sizes sizes_now()
{
    return {t::scaled(30), t::scaled(64), t::scaled(26), t::scaled(54)};
}

struct Row {
    lv_obj_t *glyph = nullptr;
    lv_obj_t *name = nullptr;
    lv_obj_t *value = nullptr;
    lv_coord_t width = 0; // of the name and the value together
};

// the texts of a column of the strip, made once per answer
struct Column {
    char top[8];     // the weekday, "14:00"
    char value[16];  // the high, the hour's temperature, "--"
    char second[16]; // the low, the chance of precipitation; "" for none
    const char *glyph;
    bool strong;     // the weekday at midnight: white
};

struct State {
    panel::Frame frame;
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    Sizes sizes{};
    lv_area_t control{}; // the rows and the forecast below them, or the forecast page
    // The view: the current values with the forecast below them, or the forecast page
    // (forecast_page.hpp). Its objects are children of `content`; a switch deletes it and builds
    // the other one.
    lv_obj_t *content = nullptr;
    // the header
    lv_obj_t *icon = nullptr;
    lv_obj_t *value = nullptr;
    lv_obj_t *condition = nullptr;
    lv_coord_t text_x = 0;
    // the name of the forecast page beside the chips (the frame's, placed above the temperature,
    // is hidden there)
    lv_obj_t *page_name = nullptr;
    // the rows that fit, placed by update() in `rows_area`
    Row rows[kMaxRows];
    int row_count = 0;
    lv_area_t rows_area{};
    int columns = 1;
    lv_coord_t column_w = 0;
    lv_coord_t row_h = 0;
    lv_coord_t glyph_w = 0;
    // the forecast
    lv_obj_t *chips[2] = {}; // daily, hourly
    lv_obj_t *card = nullptr;
    lv_obj_t *strip = nullptr; // scrolls sideways
    // In the strip: the columns, one object drawn by on_draw_columns() (an object per value took
    // 0.6 s for the 48 hours on the JC3248W535 each time the view was built)
    lv_obj_t *strip_columns = nullptr;
    memory::Vector<Column> column_texts;
    lv_obj_t *message = nullptr;
    ForecastType subscribed = ForecastType::None;
    uint32_t generation = 0; // of the subscription
    // the last answer, drawn again when the view changes
    ForecastType entries_type = ForecastType::None;
    memory::Vector<core::ForecastEntry> entries;
    bool has_entries = false;
    std::string message_text;    // in place of the forecast, shown again when the view changes
    lv_timer_t *retry = nullptr; // asks for the forecast again after an error
};

State g;
// kept between the panels: the forecast shown last, and the subscriptions' generations
ForecastType g_type = ForecastType::Daily;
uint32_t g_next_generation = 1;
// the forecast page is open; kept when the panel is built again (the display turned)
bool g_page = false;

// ------------------------------------------------------------------------------- texts

// a value as HA shows it: no decimals from 100 up, else up to two without trailing zeros
std::string number(float value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%.*f", std::fabs(value) >= 100 ? 0 : 2, static_cast<double>(value));
    std::string out = text;
    if (out.find('.') != std::string::npos) {
        out.erase(out.find_last_not_of('0') + 1);
        if (out.back() == '.') {
            out.pop_back();
        }
    }
    return out;
}

std::string with_unit(float value, std::string_view unit)
{
    std::string out = number(value);
    if (!unit.empty()) {
        out += ' ';
        out.append(unit.data(), unit.size());
    }
    return out;
}

std::string temperature_text(float value, std::string_view unit)
{
    if (std::isnan(value)) {
        return "--";
    }
    return tiles::temperature(value) + " " + std::string(unit.empty() ? std::string_view("°") : unit);
}

struct Item {
    const char *glyph;
    const char *name;
    std::string value;
};

// the current values the entity reports, in the order of the rows
int items_of(const core::EntityState &s, Item (&out)[kMaxRows])
{
    int count = 0;
    const auto add = [&](const char *glyph, const char *name, std::string value) {
        if (count < kMaxRows) {
            out[count++] = {glyph, name, std::move(value)};
        }
    };
    const std::string_view unit = s.temperature_unit;
    if (!std::isnan(s.apparent_temperature)) {
        add("thermometer", "Feels like", temperature_text(s.apparent_temperature, unit));
    }
    if (!std::isnan(s.humidity)) {
        add("water-percent", "Humidity", number(s.humidity) + "%");
    }
    if (!std::isnan(s.pressure)) {
        add("gauge", "Pressure", with_unit(s.pressure, s.pressure_unit));
    }
    if (!std::isnan(s.wind_speed)) {
        std::string wind = with_unit(s.wind_speed, s.wind_speed_unit);
        if (const char *point = core::compass(s.wind_bearing); point[0] != 0) {
            wind += " (";
            wind += point;
            wind += ')';
        }
        add("weather-windy", "Wind", std::move(wind));
    }
    if (!std::isnan(s.wind_gust)) {
        add("weather-windy-variant", "Gusts", with_unit(s.wind_gust, s.wind_speed_unit));
    }
    if (!std::isnan(s.visibility)) {
        add("eye", "Visibility", with_unit(s.visibility, s.visibility_unit));
    }
    if (!std::isnan(s.cloud_coverage)) {
        add("weather-cloudy", "Clouds", number(s.cloud_coverage) + "%");
    }
    if (!std::isnan(s.dew_point)) {
        add("thermometer-water", "Dew point", temperature_text(s.dew_point, unit));
    }
    if (!std::isnan(s.uv_index)) {
        add("weather-sunny", "UV index", number(s.uv_index));
    }
    return count;
}

const char *weekday(int day)
{
    static constexpr const char *kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return kDays[(day % 7 + 7) % 7];
}

forecast_page::Units units_of(const core::EntityState &s)
{
    return {s.temperature_unit.c_str(), s.precipitation_unit.c_str(), s.wind_speed_unit.c_str()};
}

// ------------------------------------------------------------------------------- the forecast

bool supports(const core::EntityState &s, ForecastType type)
{
    // not known yet, or the entity is unavailable (Home Assistant restarting reports no attributes,
    // so no features): both are offered, a forecast that fails is tried again
    if (!s.has(core::Field::Features) || s.unavailable()) {
        return true;
    }
    return (s.features & (type == ForecastType::Daily ? core::kForecastDaily : core::kForecastHourly)) != 0;
}

void show_message(const char *text)
{
    g.message_text = text;
    if (g_page) {
        forecast_page::message(text);
    } else {
        set_text(g.message, text);
        set_hidden(g.message, text[0] == 0);
    }
}

void subscribe(ForecastType type)
{
    const Backend &backend = dashboard::backend();
    if (backend.subscribe_forecast == nullptr || (type == ForecastType::None && g.subscribed == ForecastType::None)) {
        g.subscribed = ForecastType::None;
        return;
    }
    g.subscribed = type;
    g.generation = g_next_generation++;
    if (type == ForecastType::None) {
        // not from the layout: the panel goes when a new one replaced it
        backend.subscribe_forecast({}, g.entity, type, g.generation);
        return;
    }
    const core::Layout &layout = dashboard::layout();
    backend.subscribe_forecast(layout.entity_id(layout.tiles[g.tile]), g.entity, type, g.generation);
}

std::string degrees(float value)
{
    return tiles::temperature(value) + "°";
}

// the texts of the strip's columns
void prepare_columns()
{
    const bool daily = g.entries_type == ForecastType::Daily;
    g.column_texts.clear();
    g.column_texts.reserve(g.entries.size());
    for (const core::ForecastEntry &entry : g.entries) {
        Column c{};
        std::tm local{};
        dashboard::backend().local_time(entry.time, local);
        if (daily || local.tm_hour == 0) {
            // midnight: the new day's name
            std::snprintf(c.top, sizeof(c.top), "%s", weekday(local.tm_wday));
            c.strong = !daily;
        } else {
            std::snprintf(c.top, sizeof(c.top), "%02d:00", local.tm_hour);
        }
        const char *glyph = icons::glyph(core::condition_icon(entry.condition));
        c.glyph = glyph != nullptr ? glyph : "";
        std::snprintf(c.value, sizeof(c.value), "%s", std::isnan(entry.temperature) ? "--" : degrees(entry.temperature).c_str());
        if (daily) {
            if (!std::isnan(entry.templow)) {
                std::snprintf(c.second, sizeof(c.second), "%s", degrees(entry.templow).c_str());
            }
        } else if (entry.precipitation_probability > 0) {
            std::snprintf(c.second, sizeof(c.second), "%d%%", entry.precipitation_probability);
        }
        g.column_texts.push_back(c);
    }
}

// The columns in the area to draw: the weekday or the hour, the glyph, the temperature, the low or
// the chance, a line each, centered in the column; the lines centered in the strip's height.
void on_draw_columns(lv_event_t *event)
{
    const int n = static_cast<int>(g.column_texts.size());
    const int w = g.sizes.column_width;
    if (n == 0 || w <= 0) {
        return;
    }
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_target(event), &c);
    const lv_area_t &clip = *ctx->clip_area;
    const int first = std::max(0, (clip.x1 - c.x1) / w);
    const int last = std::min(n - 1, (clip.x2 - c.x1) / w);
    const lv_font_t *fonts[] = {&t::font_normal(), &icons::small(), &t::font_medium(), &t::font_small()};
    int height = static_cast<int>(std::size(fonts) - 1) * kLineGap;
    for (const lv_font_t *font : fonts) {
        height += lv_font_get_line_height(font);
    }
    const int top = c.y1 + (lv_area_get_height(&c) - height) / 2;
    for (int i = first; i <= last; i++) {
        const Column &col = g.column_texts[static_cast<size_t>(i)];
        const char *lines[] = {col.top, col.glyph, col.value, col.second};
        const uint32_t colors[] = {col.strong ? t::kText : t::kTextLabel, t::kTextValue, t::kText, t::kTextLabel};
        int y = top;
        for (size_t k = 0; k < std::size(fonts); k++) {
            t::draw_text(ctx, lines[k], *fonts[k], colors[k], c.x1 + i * w, y, w, LV_TEXT_ALIGN_CENTER);
            y += lv_font_get_line_height(fonts[k]) + kLineGap;
        }
    }
}

// draws the entries in the view: the panel's strip (an update keeps where it was scrolled to), or
// the forecast page
void draw_entries()
{
    if (g_page) {
        forecast_page::show(g.entries_type, g.entries, units_of(dashboard::state(g.entity)));
        return;
    }
    if (g.strip == nullptr) {
        return;
    }
    const lv_coord_t scrolled = lv_obj_get_scroll_x(g.strip);
    prepare_columns();
    lv_obj_set_width(g.strip_columns, static_cast<lv_coord_t>(static_cast<int>(g.column_texts.size()) * g.sizes.column_width));
    lv_obj_invalidate(g.strip_columns);
    lv_obj_update_layout(g.strip);
    lv_obj_scroll_to_x(g.strip, scrolled, LV_ANIM_OFF);
}

void clear_entries()
{
    g.entries.clear();
    g.has_entries = false;
    if (g_page) {
        forecast_page::clear();
    } else if (g.strip != nullptr) {
        g.column_texts.clear();
        lv_obj_set_width(g.strip_columns, 0);
        lv_obj_invalidate(g.strip);
    }
}

// the forecast `type` from now on: the view empty until it arrives
// after an error the forecast is asked for again every this long while the panel is open: Home
// Assistant answers "not supported" or "not found" for a weather entity that is not set up yet
// after its restart
constexpr uint32_t kRetryMs = 10000;

void show(ForecastType type);

void on_retry(lv_timer_t *)
{
    g.retry = nullptr; // a timer with a repeat count of 1 deletes itself
    if (g.content != nullptr && g.subscribed != ForecastType::None) {
        const ForecastType type = g.subscribed;
        // the view keeps what it shows meanwhile
        if (g.has_entries) {
            subscribe(type);
        } else {
            show(type);
        }
    }
}

void show(ForecastType type)
{
    g_type = type;
    clear_entries();
    if (dashboard::backend().subscribe_forecast == nullptr || type == ForecastType::None) {
        subscribe(ForecastType::None);
        show_message("No forecast");
        return;
    }
    show_message("Loading...");
    subscribe(type);
}

void on_chip(lv_event_t *event)
{
    const auto type = static_cast<ForecastType>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (type != g.subscribed) {
        show(type);
        update();
    }
}

// ------------------------------------------------------------------------------- the views

void build_view(int focus);

void drop_view()
{
    g.content = nullptr;
    g.icon = g.value = g.condition = g.page_name = nullptr;
    for (Row &row : g.rows) {
        row = {};
    }
    g.row_count = 0;
    g.chips[0] = g.chips[1] = nullptr;
    g.card = g.strip = g.strip_columns = g.message = nullptr;
    g.column_texts.clear();
}

// the values or the forecast page; the subscription and what it brought stay
void switch_view(bool page, int focus)
{
    if (g.content == nullptr || g_page == page) {
        return;
    }
    g_page = page;
    forecast_page::forget();
    lv_obj_del(g.content);
    drop_view();
    build_view(focus);
    update();
}

void open_page(void *focus)
{
    switch_view(true, static_cast<int>(reinterpret_cast<intptr_t>(focus)));
}

// A tap on the panel's forecast (a drag scrolls it) opens the page, at the hour tapped.
void on_strip(lv_event_t *)
{
    int focus = -1;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != nullptr && g.strip != nullptr && g.sizes.column_width > 0) {
        lv_point_t point{};
        lv_indev_get_point(indev, &point);
        lv_area_t coords;
        lv_obj_get_coords(g.strip, &coords);
        const int x = point.x - coords.x1 - lv_obj_get_style_pad_left(g.strip, LV_PART_MAIN) + lv_obj_get_scroll_x(g.strip);
        focus = x >= 0 ? x / g.sizes.column_width : -1;
    }
    // the strip goes with the view: switched after its event
    lv_async_call(open_page, reinterpret_cast<void *>(static_cast<intptr_t>(focus)));
}

// ------------------------------------------------------------------------------- building

void build_header()
{
    const lv_area_t &header = g.frame.header;
    const lv_coord_t height = lv_area_get_height(&header);
    const lv_coord_t icon_h = lv_font_get_line_height(&icons::large());
    g.icon = t::label(g.content, "", icons::large(), t::kTextLabel);
    lv_obj_set_pos(g.icon, header.x1, static_cast<lv_coord_t>(header.y1 + (height - icon_h) / 2));
    g.text_x = static_cast<lv_coord_t>(header.x1 + icon_h + 6);
    const lv_coord_t text_w = static_cast<lv_coord_t>(header.x2 + 1 - g.text_x);
    // the name above the temperature, the condition right of the temperature
    const lv_coord_t name_h = lv_font_get_line_height(&t::font_medium());
    const lv_coord_t value_h = lv_font_get_line_height(&t::font_huge());
    const auto top = static_cast<lv_coord_t>(header.y1 + (height - name_h - value_h) / 2);
    lv_obj_set_style_text_font(g.frame.name, &t::font_medium(), 0);
    lv_obj_set_style_text_color(g.frame.name, t::color(t::kTextValue), 0);
    lv_obj_set_pos(g.frame.name, g.text_x, top);
    lv_obj_set_width(g.frame.name, text_w);
    g.value = t::label(g.content, "", t::font_huge(), t::kText);
    lv_obj_set_pos(g.value, g.text_x, static_cast<lv_coord_t>(top + name_h));
    g.condition = t::label(g.content, "", t::font_medium(), t::kTextValue);
    // on the baseline of the temperature
    const lv_coord_t condition_h = lv_font_get_line_height(&t::font_medium());
    lv_obj_set_y(g.condition, static_cast<lv_coord_t>(top + name_h + value_h - condition_h - 3));
}

// the rows in the area above the forecast: as many as fit, one or two columns; placed by
// place_rows() once the values say how many there are
void build_rows(const lv_area_t &area)
{
    const lv_coord_t width = lv_area_get_width(&area);
    g.rows_area = area;
    g.glyph_w = lv_font_get_line_height(&icons::small());
    g.row_h = std::max(g.glyph_w, lv_font_get_line_height(&t::font_normal()));
    const int lines = std::max(0, (lv_area_get_height(&area) + kRowGap) / (g.row_h + kRowGap));
    // two columns only when one does not hold every row (the P4's portrait dialog has the room)
    g.columns = width >= kTwoColumns && lines < kMaxRows ? 2 : 1;
    g.column_w = static_cast<lv_coord_t>((width - (g.columns - 1) * kColumnGap) / g.columns);
    g.row_count = std::min(kMaxRows, lines * g.columns);
    for (int i = 0; i < g.row_count; i++) {
        Row &row = g.rows[i];
        row.glyph = t::label(g.content, "", icons::small(), t::kTextLabel);
        row.name = t::label(g.content, "", t::font_normal(), t::kTextLabel);
        row.width = static_cast<lv_coord_t>(g.column_w - g.glyph_w - kGlyphGap);
        row.value = t::label(g.content, "", t::font_normal(), t::kText);
        lv_obj_set_style_text_align(row.value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(row.value, row.width);
        set_hidden(row.glyph, true);
        set_hidden(row.name, true);
        set_hidden(row.value, true);
    }
}

// the first `count` rows spread over the area, in reading order (left, right, next line)
void place_rows(int count)
{
    const int lines = (count + g.columns - 1) / g.columns;
    lv_coord_t gap = kRowGap;
    if (lines > 1) {
        gap = std::clamp<lv_coord_t>(static_cast<lv_coord_t>((lv_area_get_height(&g.rows_area) - lines * g.row_h) / (lines - 1)),
                                     kRowGap, t::scaled(kRowGapMax));
    }
    const auto text_dy = static_cast<lv_coord_t>((g.row_h - lv_font_get_line_height(&t::font_normal())) / 2);
    for (int i = 0; i < count; i++) {
        const auto x = static_cast<lv_coord_t>(g.rows_area.x1 + (i % g.columns) * (g.column_w + kColumnGap));
        const auto y = static_cast<lv_coord_t>(g.rows_area.y1 + (i / g.columns) * (g.row_h + gap));
        Row &row = g.rows[i];
        lv_obj_set_pos(row.glyph, x, y);
        lv_obj_set_pos(row.name, static_cast<lv_coord_t>(x + g.glyph_w + kGlyphGap), static_cast<lv_coord_t>(y + text_dy));
        lv_obj_set_pos(row.value, static_cast<lv_coord_t>(x + g.glyph_w + kGlyphGap), static_cast<lv_coord_t>(y + text_dy));
    }
}

// `Daily` and `Hourly` from x on
void build_chips(lv_coord_t x, lv_coord_t y)
{
    const Sizes &s = g.sizes;
    static constexpr const char *kNames[] = {"Daily", "Hourly"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *chip = panel::card(g.content, {x, y, static_cast<lv_coord_t>(x + s.chip_width - 1),
                                                 static_cast<lv_coord_t>(y + s.chip_height - 1)});
        lv_obj_set_style_radius(chip, s.chip_height / 2, 0);
        // a small target: the touch counts a little around it
        lv_obj_set_ext_click_area(chip, kChipGap / 2);
        lv_obj_center(t::label(chip, kNames[i], t::font_normal(), t::kTextLabel));
        const auto type = i == 0 ? ForecastType::Daily : ForecastType::Hourly;
        lv_obj_add_event_cb(chip, on_chip, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(type)));
        g.chips[i] = chip;
        x = static_cast<lv_coord_t>(x + s.chip_width + kChipGap);
    }
}

void build_forecast(const lv_area_t &area)
{
    const Sizes &s = g.sizes;
    lv_obj_t *title = t::label(g.content, "Forecast", t::font_large(), t::kText);
    lv_obj_set_pos(title, area.x1,
                   static_cast<lv_coord_t>(area.y1 + (s.title_height - lv_font_get_line_height(&t::font_large())) / 2));
    build_chips(static_cast<lv_coord_t>(area.x2 + 1 - (2 * s.chip_width + kChipGap)),
                static_cast<lv_coord_t>(area.y1 + (s.title_height - s.chip_height) / 2));
    const lv_area_t card = {area.x1, static_cast<lv_coord_t>(area.y1 + s.title_height + kCardGap), area.x2, area.y2};
    g.card = panel::card(g.content, card);
    lv_obj_clear_flag(g.card, LV_OBJ_FLAG_CLICKABLE);
    // the columns scroll sideways inside the card's border; a tap opens the forecast page
    g.strip = t::plain(g.card);
    lv_obj_set_size(g.strip, static_cast<lv_coord_t>(lv_area_get_width(&card) - 2),
                    static_cast<lv_coord_t>(lv_area_get_height(&card) - 2));
    lv_obj_set_style_pad_ver(g.strip, kCardPad, 0);
    lv_obj_set_style_pad_hor(g.strip, 2, 0);
    lv_obj_set_scroll_dir(g.strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(g.strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g.strip, LV_OBJ_FLAG_SCROLLABLE);
    // the drag scrolls the strip, it is not a gesture of the screen
    lv_obj_clear_flag(g.strip, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(g.strip, LV_OBJ_FLAG_CLICKABLE);
    sound::tappable(g.strip);
    lv_obj_add_event_cb(g.strip, on_strip, LV_EVENT_CLICKED, nullptr);
    // as wide as the columns (draw_entries()); the touches go to the strip
    g.strip_columns = t::plain(g.strip);
    lv_obj_clear_flag(g.strip_columns, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(g.strip_columns, 0, LV_PCT(100));
    lv_obj_add_event_cb(g.strip_columns, on_draw_columns, LV_EVENT_DRAW_MAIN, nullptr);
    g.message = t::label(g.card, "", t::font_normal(), t::kTextLabel);
    lv_obj_center(g.message);
}

// the height of the card: the padding, a column's four lines and their gaps
lv_coord_t card_height()
{
    return static_cast<lv_coord_t>(2 + 2 * kCardPad + lv_font_get_line_height(&t::font_normal()) +
                                   lv_font_get_line_height(&icons::small()) + lv_font_get_line_height(&t::font_medium()) +
                                   lv_font_get_line_height(&t::font_small()) + 3 * kLineGap);
}

// the current values above the forecast
void build_values()
{
    build_header();
    const lv_area_t &control = g.control;
    const auto forecast_y = static_cast<lv_coord_t>(control.y2 + 1 - card_height() - kCardGap - g.sizes.title_height);
    build_rows({control.x1, control.y1, control.x2, static_cast<lv_coord_t>(forecast_y - kSectionGap - 1)});
    build_forecast({control.x1, forecast_y, control.x2, control.y2});
}

// The forecast page: the name and the chips in the header, the forecast in the whole area below.
void build_page(int focus)
{
    const lv_area_t &header = g.frame.header;
    const Sizes &s = g.sizes;
    const lv_coord_t height = lv_area_get_height(&header);
    const auto chips_x = static_cast<lv_coord_t>(header.x2 + 1 - (2 * s.chip_width + kChipGap));
    build_chips(chips_x, static_cast<lv_coord_t>(header.y1 + (height - s.chip_height) / 2));
    g.page_name = t::label(g.content, "", t::font_large(), t::kText);
    lv_obj_set_width(g.page_name, std::max<lv_coord_t>(0, static_cast<lv_coord_t>(chips_x - 2 * kChipGap - header.x1)));
    lv_obj_set_pos(g.page_name, header.x1,
                   static_cast<lv_coord_t>(header.y1 + (height - lv_font_get_line_height(&t::font_large())) / 2));
    if (g.frame.large) {
        // the dialog's round corners cut the page's background at the bottom
        lv_obj_set_style_clip_corner(g.frame.root, true, 0);
    }
    forecast_page::build(g.content, g.control, g.frame.portrait, g.frame.large, focus);
}

void build_view(int focus)
{
    g.content = t::plain(g.frame.root);
    lv_obj_set_size(g.content, LV_PCT(100), LV_PCT(100));
    // the taps go to the parts, and between them to the panel (the back tile stays usable)
    lv_obj_clear_flag(g.content, LV_OBJ_FLAG_CLICKABLE);
    set_hidden(g.frame.name, g_page);
    if (g_page) {
        build_page(focus);
    } else {
        build_values();
    }
    // what arrived meanwhile
    if (g.has_entries) {
        draw_entries();
    }
    const std::string text = g.message_text;
    show_message(text.c_str());
}

void update_values(const core::Layout &layout, const core::Tile &tile, const core::EntityState &s, bool known,
                   bool available)
{
    // the header
    const char *glyph = icons::for_tile(tile, layout.entity_id(tile), s, true);
    set_text(g.icon, glyph != nullptr ? glyph : "");
    const std::string value = !known ? std::string("...") : !available ? std::string("--")
                                                                        : temperature_text(s.temperature, s.temperature_unit);
    set_text(g.value, value);
    const lv_coord_t value_w = lv_txt_get_width(value.c_str(), static_cast<uint32_t>(value.size()), &t::font_huge(), 0,
                                                LV_TEXT_FLAG_NONE);
    const auto condition_x = static_cast<lv_coord_t>(g.text_x + value_w + 12);
    lv_obj_set_x(g.condition, condition_x);
    lv_obj_set_width(g.condition, std::max<lv_coord_t>(0, static_cast<lv_coord_t>(g.frame.header.x2 + 1 - condition_x)));
    t::line_text(g.condition, available ? core::condition_words(core::condition_of(s.state)) : "");

    // the rows
    Item items[kMaxRows];
    const int count = std::min(g.row_count, available ? items_of(s, items) : 0);
    place_rows(count);
    for (int i = 0; i < g.row_count; i++) {
        Row &row = g.rows[i];
        const bool shown = i < count;
        set_hidden(row.glyph, !shown);
        set_hidden(row.name, !shown);
        set_hidden(row.value, !shown);
        if (shown) {
            const char *row_glyph = icons::glyph(items[i].glyph);
            set_text(row.glyph, row_glyph != nullptr ? row_glyph : "");
            set_text(row.value, items[i].value);
            // the value whole, the name ends in "..." when the two do not fit
            const std::string &text = items[i].value;
            const lv_coord_t text_w = lv_txt_get_width(text.c_str(), static_cast<uint32_t>(text.size()), &t::font_normal(),
                                                       0, LV_TEXT_FLAG_NONE);
            lv_obj_set_width(row.name, std::max<lv_coord_t>(0, static_cast<lv_coord_t>(row.width - text_w - kGlyphGap)));
            t::line_text(row.name, items[i].name);
        }
    }
}

} // namespace

void build(const panel::Frame &frame, core::TileIndex tile, bool keep_page)
{
    forget();
    if (!keep_page) {
        g_page = false;
    }
    g.frame = frame;
    g.tile = tile;
    g.entity = dashboard::layout().tiles[tile].entity;
    g.sizes = sizes_now();
    const lv_coord_t inset = frame.large ? kInsetLarge : kInset;
    g.control = {static_cast<lv_coord_t>(frame.control.x1 + inset), frame.control.y1,
                 static_cast<lv_coord_t>(frame.control.x2 - inset),
                 static_cast<lv_coord_t>(frame.control.y2 - (frame.large ? inset : 0))};
    build_view(-1);

    const core::EntityState &s = dashboard::state(g.entity);
    ForecastType type = g_type;
    if (!supports(s, type)) {
        type = type == ForecastType::Daily ? ForecastType::Hourly : ForecastType::Daily;
    }
    show(supports(s, type) ? type : ForecastType::None);
}

void update()
{
    if (g.content == nullptr) {
        return;
    }
    const core::Layout &layout = dashboard::layout();
    const core::Tile &tile = layout.tiles[g.tile];
    const core::EntityState &s = dashboard::state(g.entity);
    const bool known = s.known != 0;
    const bool available = known && !s.unavailable();
    if (g_page) {
        // beside the chips: a long name takes the next smaller font before it is cut
        const std::string name(core::display_name(s, tile, layout.entity_id(tile)));
        t::line_text(g.page_name, name.c_str(), &t::font_medium());
        forecast_page::set_current(available ? s.temperature : core::kUnknown);
    } else {
        update_values(layout, tile, s, known, available);
    }

    // the chips: only the forecasts the entity has, none when there is one or none to choose
    const bool daily = supports(s, ForecastType::Daily);
    const bool hourly = supports(s, ForecastType::Hourly);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *chip = g.chips[i];
        const auto type = i == 0 ? ForecastType::Daily : ForecastType::Hourly;
        const bool selected = type == g.subscribed;
        set_hidden(chip, !(daily && hourly));
        lv_obj_set_style_bg_color(chip, t::color(selected ? t::kText : t::kCard), 0);
        lv_obj_set_style_border_color(chip, t::color(selected ? t::kText : t::kBorder), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(chip, 0), t::color(selected ? t::kBackground : t::kTextLabel), 0);
    }
    // the features arrived with the panel's fields: the forecast shown may not be one of them (an
    // unavailable entity has none: nothing is dropped then)
    if (available && g.subscribed != ForecastType::None && !supports(s, g.subscribed)) {
        const ForecastType other = g.subscribed == ForecastType::Daily ? ForecastType::Hourly : ForecastType::Daily;
        show(supports(s, other) ? other : ForecastType::None);
        update();
    }
}

bool back()
{
    if (g.content == nullptr || !g_page) {
        return false;
    }
    switch_view(false, -1);
    return true;
}

void on_forecast(core::Forecast forecast)
{
    if (g.content == nullptr || forecast.generation != g.generation || forecast.entity != g.entity) {
        return; // the answer of an older subscription (the other forecast)
    }
    if (!forecast.error.empty()) {
        if (g.retry == nullptr) {
            g.retry = lv_timer_create(on_retry, kRetryMs, nullptr);
            lv_timer_set_repeat_count(g.retry, 1);
        }
        // an update that failed keeps what is shown
        if (!g.has_entries) {
            std::string text = forecast.error.c_str();
            text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
            show_message(text.c_str());
        }
        return;
    }
    if (g.retry != nullptr) {
        lv_timer_del(g.retry);
        g.retry = nullptr;
    }
    g.entries = std::move(forecast.entries);
    g.entries_type = forecast.type;
    g.has_entries = !g.entries.empty();
    draw_entries();
    show_message(g.has_entries ? "" : "No forecast");
}

void forget()
{
    if (g.retry != nullptr) {
        lv_timer_del(g.retry);
    }
    if (g.content != nullptr) {
        subscribe(ForecastType::None);
    }
    forecast_page::forget();
    // the objects go with the panel's root
    g = {};
}

} // namespace hc::ui::weather_panel
