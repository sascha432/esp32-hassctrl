#include "history_view.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

#include "dashboard.hpp"
#include "panel.hpp"
#include "theme.hpp"

namespace hc::ui::history_view {

namespace {

namespace t = theme;
using panel::set_hidden;
using panel::set_text;

// the ranges of the buttons, in hours; the view opens with the one used last
constexpr uint32_t kRanges[] = {48, 24, 12};
constexpr int kRangeCount = static_cast<int>(std::size(kRanges));
// the history is asked for again this often while the view is open
constexpr uint32_t kRefreshMs = 60000;
// the length of a statistics bucket
constexpr uint32_t kBucketSeconds = 300;

// the row of "History" and the range buttons
constexpr lv_coord_t kChipGap = 6;
// between the title row and the card
constexpr lv_coord_t kCardGap = 8;
// the gap at the right of the graph, above the time labels
constexpr lv_coord_t kGraphRight = 8;
constexpr lv_coord_t kTimeTop = 2;
// the curve is mapped inside this inset, so a 2 px line on the top or the bottom is not cut
constexpr lv_coord_t kGraphInset = 2;
// LVGL's chart stores 16 bit values: the window's range is mapped to 0..kChartScale
constexpr lv_coord_t kChartScale = 10000;
constexpr uint32_t kTickHours[] = {1, 2, 3, 4, 6, 8, 12, 24};
constexpr int kMaxTicks = 25;
// the timeline of an on/off entity: a strip centered in the card above the time labels, the same
// gap left and right (it has no value labels), the legend centered in the space above it
constexpr lv_coord_t kStripHeight = 40;
constexpr lv_coord_t kStripSide = 16;
constexpr lv_coord_t kLegendDot = 12;

// The sizes that hold text grow with the fonts (theme::scaled), set by build(); the numbers are
// those of the normal fonts.
struct Sizes {
    // the row of "History" and the range buttons
    lv_coord_t title_height;
    lv_coord_t chip_width;
    lv_coord_t chip_height;
    // inside the card: the column of the value labels left of the graph, the strip of the time
    // labels below it
    lv_coord_t level_width;
    lv_coord_t level_height;
    lv_coord_t graph_x;
    // below the unit, so the label of the top line does not touch it
    lv_coord_t graph_y;
    lv_coord_t time_height;
    // the time labels keep at least this distance ("00:00" in 12 px is about 32 px wide)
    lv_coord_t tick_spacing;
};

Sizes sizes_now()
{
    Sizes s{};
    s.title_height = t::scaled(30);
    s.chip_width = t::scaled(52);
    s.chip_height = t::scaled(26);
    s.level_width = t::scaled(40);
    s.level_height = t::scaled(14);
    s.graph_x = static_cast<lv_coord_t>(s.level_width + 8);
    s.graph_y = static_cast<lv_coord_t>(s.level_height + 8);
    s.time_height = t::scaled(23);
    s.tick_spacing = t::scaled(44);
    return s;
}

Sizes sizes = sizes_now();

// unknown is a hole in the card; on, off and a problem have the colors of the Legend
constexpr uint32_t kColorUnknown = t::kBackground;
// the curve, in the accent color of the old firmware
constexpr uint32_t kColorCurve = 0x00ffff;

// the states of a column of the strip
enum : int8_t { kNone = -1, kOff = 0, kOn = 1, kUnknownState = 2, kProblemState = 3 };

struct State {
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    core::HistoryKind kind = core::HistoryKind::Statistics;
    Legend states; // the words and colors of the timeline
    lv_obj_t *chips[kRangeCount] = {};
    lv_obj_t *card = nullptr;
    // as built: LVGL lays the objects out only before the next frame
    lv_coord_t card_w = 0;
    lv_coord_t strip_y = 0;
    lv_coord_t strip_h = 0;
    lv_coord_t graph_x = 0; // the left of the curve or the strip
    lv_coord_t graph_w = 0;
    lv_coord_t graph_h = 0;
    lv_obj_t *unit = nullptr;
    lv_obj_t *levels[3] = {};
    lv_obj_t *chart = nullptr;
    lv_chart_series_t *series = nullptr;
    lv_obj_t *strip = nullptr;
    lv_obj_t *legend = nullptr;
    lv_obj_t *problem_item = nullptr; // in the legend while the window has a problem
    lv_obj_t *grid[kMaxTicks] = {};
    lv_obj_t *times[kMaxTicks] = {};
    lv_obj_t *message = nullptr;
    uint32_t generation = 0; // of the request on its way
    lv_timer_t *refresh = nullptr;
};

State g;
// kept between the views: the range used last, and the requests' generations
int g_range = 1;
uint32_t g_next_generation = 1;

// a curve (the statistics, a temperature) rather than the timeline of states
bool curve()
{
    return g.kind != core::HistoryKind::States;
}

uint32_t hours()
{
    return kRanges[g_range];
}

// ------------------------------------------------------------------------------- requests

void request()
{
    const core::Layout &layout = dashboard::layout();
    const core::Tile &tile = layout.tiles[g.tile];
    g.generation = g_next_generation++;
    dashboard::backend().fetch_history(layout.entity_id(tile), g.entity, g.kind, hours(), g.generation);
}

void show_message(const char *text)
{
    set_text(g.message, text);
    set_hidden(g.message, text[0] == 0);
}

// nothing drawn: the curve empty, the strip and its labels hidden
void clear_graph()
{
    if (g.chart != nullptr) {
        lv_chart_set_all_value(g.chart, g.series, LV_CHART_POINT_NONE);
        set_hidden(g.chart, true);
    }
    if (g.strip != nullptr) {
        lv_obj_clean(g.strip);
        set_hidden(g.strip, true);
        set_hidden(g.legend, true);
    }
    for (lv_obj_t *level : g.levels) {
        set_text(level, "");
    }
    for (int i = 0; i < kMaxTicks; i++) {
        set_hidden(g.grid[i], true);
        set_hidden(g.times[i], true);
    }
}

void on_chip(lv_event_t *event)
{
    const auto range = static_cast<int>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (range == g_range) {
        return;
    }
    g_range = range;
    clear_graph();
    show_message("Loading...");
    update();
    request();
}

// ------------------------------------------------------------------------------- the axes

// a step of 1, 2, 2.5 or 5 times a power of ten, at least `raw`
float nice_step(float raw)
{
    if (!(raw > 0) || raw > 1e30f) {
        return 1;
    }
    const float magnitude = std::pow(10.0f, std::floor(std::log10(raw)));
    for (const float factor : {1.0f, 2.0f, 2.5f, 5.0f, 10.0f}) {
        if (factor * magnitude >= raw * 0.9999f) {
            return factor * magnitude;
        }
    }
    return 10 * magnitude;
}

// decimals that show a step exactly: 2.5 needs one more than 2
int step_decimals(float step)
{
    int decimals = std::max(0, static_cast<int>(-std::floor(std::log10(step))));
    const float scaled = step * std::pow(10.0f, static_cast<float>(decimals));
    if (std::fabs(scaled - std::round(scaled)) > 0.01f) {
        decimals++;
    }
    return decimals;
}

std::string number_text(float value, int decimals)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%.*f", decimals, static_cast<double>(value));
    return text;
}

// The grid lines of the time axis at whole local hours, as far apart as the labels need, with
// "HH:00" under each one that fits in the card.
void draw_ticks(uint32_t start, uint32_t end, lv_coord_t top, lv_coord_t height)
{
    const Backend &backend = dashboard::backend();
    const float span = static_cast<float>(end - start);
    uint32_t every = kTickHours[std::size(kTickHours) - 1];
    for (const uint32_t candidate : kTickHours) {
        if (g.graph_w * static_cast<float>(candidate * 3600) / span >= sizes.tick_spacing) {
            every = candidate;
            break;
        }
    }
    std::tm local{};
    backend.local_time(start, local);
    // the first whole hour after the start
    uint32_t time = start - static_cast<uint32_t>(local.tm_min * 60 + local.tm_sec) + 3600;
    int count = 0;
    for (; time < end && count < kMaxTicks; time += 3600) {
        backend.local_time(time, local);
        if (local.tm_hour % static_cast<int>(every) != 0) {
            continue;
        }
        const auto x = static_cast<lv_coord_t>(g.graph_x + std::lround((time - start) / span * g.graph_w));
        lv_obj_set_pos(g.grid[count], x, top);
        lv_obj_set_height(g.grid[count], height);
        set_hidden(g.grid[count], false);
        char text[8];
        std::snprintf(text, sizeof(text), "%02d:00", local.tm_hour);
        lv_obj_t *label = g.times[count];
        lv_label_set_text(label, text);
        const lv_coord_t w = lv_txt_get_width(text, 5, &t::font_small(), 0, LV_TEXT_FLAG_NONE);
        lv_obj_set_x(label, static_cast<lv_coord_t>(x - w / 2));
        // a label that would stick out of the card is left out, its line stays
        set_hidden(label, x - w / 2 < 2 || x + w / 2 > g.card_w - 2);
        count++;
    }
    for (int i = count; i < kMaxTicks; i++) {
        set_hidden(g.grid[i], true);
        set_hidden(g.times[i], true);
    }
}

// ------------------------------------------------------------------------------- the curve

void draw_statistics(const core::History &history)
{
    // a temperature holds from its change to the next one: the value before the window counts too
    const bool holds = history.kind == core::HistoryKind::Temperature;
    float lo = INFINITY;
    float hi = -INFINITY;
    for (const auto &point : history.points) {
        if (!std::isnan(point.value) && (holds || point.time + kBucketSeconds > history.start)) {
            lo = std::min(lo, point.value);
            hi = std::max(hi, point.value);
        }
    }
    if (lo > hi) {
        // a sensor without a state_class has no statistics
        clear_graph();
        show_message(holds ? "No temperatures in this time"
                     : history.points.empty() ? "No statistics for this sensor" : "No values in this time");
        return;
    }
    // three round values: the bottom, the middle and the top line
    float span = hi - lo;
    if (span < std::max(std::fabs(hi) * 1e-4f, 1e-6f)) {
        span = std::max(std::fabs(hi) * 0.1f, 1.0f);
    }
    float step = nice_step(span / 2);
    float bottom = std::floor(lo / step) * step;
    for (int i = 0; i < 8 && bottom + 2 * step < hi; i++) {
        step = nice_step(step * 1.01f);
        bottom = std::floor(lo / step) * step;
    }
    const float top = bottom + 2 * step;
    const int decimals = step_decimals(step);
    set_text(g.levels[0], number_text(top, decimals));
    set_text(g.levels[1], number_text(bottom + step, decimals));
    set_text(g.levels[2], number_text(bottom, decimals));

    // one column per bucket; a bucket without a value is a gap in the curve
    const auto count = static_cast<uint16_t>(std::max<uint32_t>(1, (history.end - history.start) / kBucketSeconds));
    if (lv_chart_get_point_count(g.chart) != count) {
        lv_chart_set_point_count(g.chart, count);
    }
    lv_chart_set_all_value(g.chart, g.series, LV_CHART_POINT_NONE);
    const auto set = [&](uint32_t index, float value) {
        if (!std::isnan(value)) {
            const auto scaled = static_cast<lv_coord_t>(std::lround((value - bottom) / (top - bottom) * kChartScale));
            lv_chart_set_value_by_id(g.chart, g.series, static_cast<uint16_t>(index), scaled);
        }
    };
    if (holds) {
        // every bucket from a change to the next one has the value of the change (the points come
        // in time order); the last change before the window seeds the first buckets
        float last = core::kUnknown;
        uint32_t next = 0;
        for (const auto &point : history.points) {
            const uint32_t index = point.time > history.start ? (point.time - history.start) / kBucketSeconds : 0;
            if (index >= count) {
                break;
            }
            for (uint32_t i = next; i < index; i++) {
                set(i, last);
            }
            last = point.value;
            set(index, last);
            next = index + 1;
        }
        for (uint32_t i = next; i < count; i++) {
            set(i, last);
        }
    } else {
        // the bucket of each mean; a bucket without one is a gap in the curve
        for (const auto &point : history.points) {
            if (point.time + kBucketSeconds <= history.start) {
                continue;
            }
            const uint32_t index = point.time > history.start ? (point.time - history.start) / kBucketSeconds : 0;
            if (index < count) {
                set(index, point.value);
            }
        }
    }
    lv_chart_refresh(g.chart);
    set_hidden(g.chart, false);
    draw_ticks(history.start, history.end, sizes.graph_y, g.graph_h);
    show_message("");
}

// ------------------------------------------------------------------------------- the timeline

int8_t state_of(float value)
{
    if (std::isnan(value)) {
        return kUnknownState;
    }
    return value > 1.5f ? kProblemState : value > 0.5f ? kOn : kOff;
}

uint32_t color_of(int8_t state)
{
    switch (state) {
    case kOn:
        return g.states.on_color;
    case kOff:
        return g.states.off_color;
    case kProblemState:
        return g.states.problem_color;
    default:
        return kColorUnknown;
    }
}

// The strip of an on/off entity. Each column of pixels takes the state that matters most within
// its time (a problem before on before unknown before off), so a motion of two minutes in 48 hours
// is still seen; equal columns are one object.
void draw_states(const core::History &history)
{
    lv_obj_clean(g.strip);
    const lv_coord_t width = g.graph_w;
    const float span = static_cast<float>(history.end - history.start);
    memory::Vector<int8_t> columns(static_cast<size_t>(std::max<lv_coord_t>(width, 1)), kNone);
    const auto rank = [](int8_t state) {
        return state == kProblemState ? 4 : state == kOn ? 3 : state == kUnknownState ? 2 : state == kOff ? 1 : 0;
    };
    bool any = false;
    bool problem = false;
    for (size_t i = 0; i < history.points.size(); i++) {
        const auto &point = history.points[i];
        const uint32_t from = std::max(point.time, history.start);
        const uint32_t to = i + 1 < history.points.size() ? std::min(history.points[i + 1].time, history.end) : history.end;
        if (to <= from) {
            continue;
        }
        const int8_t state = state_of(point.value);
        problem = problem || state == kProblemState;
        const auto c1 = static_cast<int>(std::floor((from - history.start) / span * width));
        const auto c2 = static_cast<int>(std::ceil((to - history.start) / span * width));
        for (int c = std::max(0, c1); c < std::min<int>(width, std::max(c2, c1 + 1)); c++) {
            if (rank(state) > rank(columns[static_cast<size_t>(c)])) {
                columns[static_cast<size_t>(c)] = state;
            }
        }
        any = true;
    }
    if (!any) {
        clear_graph();
        show_message("No history in this time");
        return;
    }
    for (lv_coord_t x = 0; x < width;) {
        const int8_t state = columns[static_cast<size_t>(x)];
        lv_coord_t run = 1;
        while (x + run < width && columns[static_cast<size_t>(x + run)] == state) {
            run++;
        }
        if (state != kNone) {
            lv_obj_t *part = t::plain(g.strip);
            lv_obj_set_pos(part, x, 0);
            lv_obj_set_size(part, run, LV_PCT(100));
            lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(part, t::color(color_of(state)), 0);
            lv_obj_clear_flag(part, LV_OBJ_FLAG_CLICKABLE);
        }
        x = static_cast<lv_coord_t>(x + run);
    }
    set_hidden(g.strip, false);
    set_hidden(g.legend, false);
    set_hidden(g.problem_item, !problem);
    // the lines from the strip's bottom down to the labels: a time without history leaves the strip
    // empty, a line through it would show there
    const auto lines_top = static_cast<lv_coord_t>(g.strip_y + g.strip_h);
    draw_ticks(history.start, history.end, lines_top, static_cast<lv_coord_t>(sizes.graph_y + g.graph_h - lines_top));
    show_message("");
}

lv_obj_t *legend_item(lv_obj_t *parent, uint32_t color, const char *text)
{
    lv_obj_t *item = t::plain(parent);
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 6, 0);
    lv_obj_t *dot = t::plain(item);
    lv_obj_set_size(dot, kLegendDot, kLegendDot);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, t::color(color), 0);
    // the unknown color is the background's: the outline keeps the dot seen
    lv_obj_set_style_border_width(dot, 1, 0);
    lv_obj_set_style_border_color(dot, t::color(t::kTextMuted), 0);
    t::label(item, text, t::font_small(), t::kTextLabel);
    return item;
}

// ------------------------------------------------------------------------------- building

void build_title(lv_obj_t *parent, const lv_area_t &area)
{
    lv_obj_t *title = t::label(parent, "History", t::font_large(), t::kText);
    lv_obj_set_pos(title, area.x1,
                   static_cast<lv_coord_t>(area.y1 + (sizes.title_height - lv_font_get_line_height(&t::font_large())) / 2));
    auto x = static_cast<lv_coord_t>(area.x2 + 1 - (kRangeCount * sizes.chip_width + (kRangeCount - 1) * kChipGap));
    const auto y = static_cast<lv_coord_t>(area.y1 + (sizes.title_height - sizes.chip_height) / 2);
    for (int i = 0; i < kRangeCount; i++) {
        lv_obj_t *chip = panel::card(parent, {x, y, static_cast<lv_coord_t>(x + sizes.chip_width - 1),
                                              static_cast<lv_coord_t>(y + sizes.chip_height - 1)});
        lv_obj_set_style_radius(chip, sizes.chip_height / 2, 0);
        // a small target: the touch counts a little around it
        lv_obj_set_ext_click_area(chip, kChipGap / 2);
        char text[8];
        std::snprintf(text, sizeof(text), "%lu h", static_cast<unsigned long>(kRanges[i]));
        lv_obj_center(t::label(chip, text, t::font_normal(), t::kTextLabel));
        lv_obj_add_event_cb(chip, on_chip, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        g.chips[i] = chip;
        x = static_cast<lv_coord_t>(x + sizes.chip_width + kChipGap);
    }
}

void build_card(lv_obj_t *parent, const lv_area_t &area)
{
    g.card = panel::card(parent, area);
    lv_obj_clear_flag(g.card, LV_OBJ_FLAG_CLICKABLE);
    // the size is the one of the area: the objects are placed before LVGL lays them out
    const lv_coord_t card_w = lv_area_get_width(&area);
    g.card_w = card_w;
    const lv_coord_t card_h = lv_area_get_height(&area);
    const bool timeline = g.kind == core::HistoryKind::States;
    g.graph_x = timeline ? kStripSide : sizes.graph_x;
    // the children sit inside the card's 1 px border
    g.graph_w = static_cast<lv_coord_t>(timeline ? card_w - 2 - 2 * kStripSide : card_w - sizes.graph_x - kGraphRight);
    g.graph_h = static_cast<lv_coord_t>(card_h - sizes.graph_y - sizes.time_height);

    // the grid lines first, so the curve and the strip are drawn over them
    for (lv_obj_t *&line : g.grid) {
        line = t::plain(g.card);
        lv_obj_set_width(line, 1);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(line, t::color(t::kBorder), 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        set_hidden(line, true);
    }
    const auto time_y = static_cast<lv_coord_t>(sizes.graph_y + g.graph_h + kTimeTop);
    for (lv_obj_t *&label : g.times) {
        label = t::label(g.card, "", t::font_small(), t::kTextMuted);
        lv_obj_set_y(label, time_y);
        set_hidden(label, true);
    }

    if (curve()) {
        g.unit = t::label(g.card, "", t::font_small(), t::kTextMuted);
        lv_obj_set_pos(g.unit, 4, 2);
        // the three value lines are the chart's dividers, the labels sit on them
        const lv_coord_t line_top = sizes.graph_y + kGraphInset;
        const lv_coord_t line_h = static_cast<lv_coord_t>(g.graph_h - 2 * kGraphInset);
        const lv_coord_t ys[3] = {
            static_cast<lv_coord_t>(line_top - sizes.level_height / 2),
            static_cast<lv_coord_t>(line_top + line_h / 2 - sizes.level_height / 2),
            static_cast<lv_coord_t>(line_top + line_h - sizes.level_height / 2),
        };
        for (int i = 0; i < 3; i++) {
            g.levels[i] = t::label(g.card, "", t::font_small(), t::kTextMuted);
            lv_obj_set_width(g.levels[i], sizes.level_width - 4);
            lv_obj_set_style_text_align(g.levels[i], LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(g.levels[i], 2, std::max<lv_coord_t>(2, ys[i]));
        }
        g.chart = lv_chart_create(g.card);
        lv_obj_remove_style_all(g.chart);
        lv_obj_set_pos(g.chart, sizes.graph_x, sizes.graph_y);
        lv_obj_set_size(g.chart, g.graph_w, g.graph_h);
        lv_obj_set_style_pad_top(g.chart, kGraphInset, 0);
        lv_obj_set_style_pad_bottom(g.chart, kGraphInset, 0);
        lv_obj_set_style_line_color(g.chart, t::color(t::kBorder), LV_PART_MAIN);
        lv_obj_set_style_line_width(g.chart, 1, LV_PART_MAIN);
        lv_obj_set_style_line_width(g.chart, 2, LV_PART_ITEMS);
        // no dots on the points
        lv_obj_set_style_width(g.chart, 0, LV_PART_INDICATOR);
        lv_obj_set_style_height(g.chart, 0, LV_PART_INDICATOR);
        lv_obj_clear_flag(g.chart, LV_OBJ_FLAG_CLICKABLE);
        lv_chart_set_type(g.chart, LV_CHART_TYPE_LINE);
        lv_chart_set_div_line_count(g.chart, 3, 0);
        lv_chart_set_range(g.chart, LV_CHART_AXIS_PRIMARY_Y, 0, kChartScale);
        // the point count is set before the series: LVGL 8.4 leaves the x points of a line series
        // unset, a series is never removed (lv_chart_remove_series would free them)
        lv_chart_set_point_count(g.chart, static_cast<uint16_t>(hours() * 3600 / kBucketSeconds));
        g.series = lv_chart_add_series(g.chart, t::color(kColorCurve), LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_all_value(g.chart, g.series, LV_CHART_POINT_NONE);
        set_hidden(g.chart, true);
    } else {
        // the strip in the middle of the card above the time labels, the legend in the middle of
        // the space above the strip
        const auto above_times = static_cast<lv_coord_t>(card_h - 2 - sizes.time_height);
        const lv_coord_t strip_h = std::min<lv_coord_t>(kStripHeight, static_cast<lv_coord_t>(above_times / 3));
        g.strip_h = strip_h;
        g.strip = t::plain(g.card);
        g.strip_y = static_cast<lv_coord_t>((above_times - strip_h) / 2);
        lv_obj_set_pos(g.strip, g.graph_x, g.strip_y);
        lv_obj_set_size(g.strip, g.graph_w, strip_h);
        lv_obj_set_style_radius(g.strip, 4, 0);
        lv_obj_set_style_clip_corner(g.strip, true, 0);
        lv_obj_set_style_border_width(g.strip, 1, 0);
        lv_obj_set_style_border_color(g.strip, t::color(t::kBorder), 0);
        lv_obj_set_style_border_post(g.strip, true, 0);
        lv_obj_clear_flag(g.strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(g.strip, LV_OBJ_FLAG_SCROLLABLE);
        set_hidden(g.strip, true);
        g.legend = t::plain(g.card);
        lv_obj_set_size(g.legend, g.graph_w, g.strip_y);
        lv_obj_set_pos(g.legend, g.graph_x, 0);
        lv_obj_set_flex_flow(g.legend, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(g.legend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(g.legend, 16, 0);
        lv_obj_set_style_pad_row(g.legend, 4, 0);
        lv_obj_clear_flag(g.legend, LV_OBJ_FLAG_CLICKABLE);
        legend_item(g.legend, g.states.on_color, g.states.on);
        legend_item(g.legend, g.states.off_color, g.states.off);
        if (g.states.problem != nullptr) {
            g.problem_item = legend_item(g.legend, g.states.problem_color, g.states.problem);
            set_hidden(g.problem_item, true);
        }
        legend_item(g.legend, kColorUnknown, "Unknown");
        set_hidden(g.legend, true);
    }

    g.message = t::label(g.card, "", t::font_medium(), t::kTextMuted);
    lv_obj_set_width(g.message, g.graph_w);
    lv_obj_set_style_text_align(g.message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.message, g.graph_x,
                   static_cast<lv_coord_t>(sizes.graph_y + (g.graph_h - lv_font_get_line_height(&t::font_medium())) / 2));
}

} // namespace

void build(lv_obj_t *parent, const lv_area_t &area, core::TileIndex tile, core::HistoryKind kind, const Legend &legend)
{
    forget();
    sizes = sizes_now();
    g.tile = tile;
    g.entity = dashboard::layout().tiles[tile].entity;
    g.kind = kind;
    g.states = legend;
    build_title(parent, {area.x1, area.y1, area.x2, static_cast<lv_coord_t>(area.y1 + sizes.title_height - 1)});
    build_card(parent, {area.x1, static_cast<lv_coord_t>(area.y1 + sizes.title_height + kCardGap), area.x2, area.y2});
    show_message("Loading...");
    request();
    g.refresh = lv_timer_create([](lv_timer_t *) { request(); }, kRefreshMs, nullptr);
    update();
}

void update()
{
    if (g.card == nullptr) {
        return;
    }
    if (g.unit != nullptr) {
        const core::Layout &layout = dashboard::layout();
        // a climate has no unit attribute: degrees, as its panel writes the room temperature
        set_text(g.unit, g.kind == core::HistoryKind::Temperature
                             ? std::string("\xc2\xb0")
                             : std::string(core::display_unit(dashboard::state(g.entity), layout.tiles[g.tile])));
    }
    for (int i = 0; i < kRangeCount; i++) {
        const bool selected = i == g_range;
        lv_obj_set_style_bg_color(g.chips[i], t::color(selected ? t::kText : t::kCard), 0);
        lv_obj_set_style_border_color(g.chips[i], t::color(selected ? t::kText : t::kBorder), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(g.chips[i], 0), t::color(selected ? t::kBackground : t::kTextLabel), 0);
    }
}

void on_history(core::History history)
{
    if (g.card == nullptr || history.generation != g.generation) {
        return; // the answer to an older request (another range)
    }
    if (!history.error.empty()) {
        // a refresh that failed keeps what is drawn
        if ((g.chart == nullptr || lv_obj_has_flag(g.chart, LV_OBJ_FLAG_HIDDEN)) &&
            (g.strip == nullptr || lv_obj_has_flag(g.strip, LV_OBJ_FLAG_HIDDEN))) {
            std::string text = history.error.c_str();
            if (!text.empty()) {
                text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
            }
            show_message(text.c_str());
        }
        return;
    }
    if (curve()) {
        draw_statistics(history);
    } else {
        draw_states(history);
    }
}

void forget()
{
    if (g.refresh != nullptr) {
        lv_timer_del(g.refresh);
    }
    // the objects go with their parent
    g = {};
}

} // namespace hc::ui::history_view
