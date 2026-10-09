#include "hc/ui/test_screen.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace hc::ui {

namespace {

// theme of the old firmware (docs/ui/design-system.md section 2)
constexpr uint32_t kColorBackground = 0x0b0f14;
constexpr uint32_t kColorBar = 0x12181f;
constexpr uint32_t kColorCard = 0x182029;
constexpr uint32_t kColorText = 0xffffff;
constexpr uint32_t kColorLabel = 0x7fa8c9;
constexpr uint32_t kColorActive = 0xff8000;

constexpr uint32_t kReportPeriodMs = 2000;
constexpr uint32_t kReportsPerRotation = 5;

constexpr lv_coord_t kTabBarHeight = 34;
constexpr int kRampSteps = 16;

// Refresh statistics of one report period. The monitor callback of the display driver runs in the
// LVGL task, like the timers and the events, so nothing here needs a lock.
struct Stats {
    uint32_t refreshes = 0;
    uint32_t time_ms = 0;
    uint32_t max_ms = 0;
    uint32_t pixels = 0;
};

struct State {
    lv_disp_t *disp = nullptr;
    TestScreenConfig config{};
    // display tab
    lv_obj_t *info = nullptr;
    lv_obj_t *stats = nullptr;
    lv_obj_t *touch_dot = nullptr;
    lv_obj_t *touch_label = nullptr;
    lv_obj_t *network_label = nullptr;
    Stats period;
    bool measure_full = false;
    uint32_t full_ms = 0;
    uint32_t full_pixels = 0;
    uint32_t reports = 0;
    // backlight tab
    lv_obj_t *brightness_label = nullptr;
    lv_obj_t *duty_label = nullptr;
    lv_obj_t *minimum_label = nullptr;
    lv_timer_t *minimum_timer = nullptr;
    uint8_t brightness = 100;
    bool perceptual = true;
    // raw duty of the running minimum test, 0 while none runs
    uint16_t test_duty = 0;
    // what hide_test_screen() removes
    lv_obj_t *tabs = nullptr;
    lv_timer_t *report_timer = nullptr;
    lv_timer_t *network_timer = nullptr;
    // the display's refresh monitor before this one, which on_refresh() hands on to
    void (*monitor)(lv_disp_drv_t *, uint32_t, uint32_t) = nullptr;
};

State g;

// ------------------------------------------------------------------------------- display tab

void on_refresh(lv_disp_drv_t *driver, uint32_t time_ms, uint32_t pixels)
{
    if (g.monitor != nullptr) {
        g.monitor(driver, time_ms, pixels);
    }
    if (g.measure_full) {
        g.measure_full = false;
        g.full_ms = time_ms;
        g.full_pixels = pixels;
        return;
    }
    g.period.refreshes++;
    g.period.time_ms += time_ms;
    g.period.max_ms = std::max(g.period.max_ms, time_ms);
    g.period.pixels += pixels;
}

int rotation_deg()
{
    return static_cast<int>(lv_disp_get_rotation(g.disp)) * 90;
}

void update_info()
{
    lv_label_set_text_fmt(g.info, "%s  %dx%d  %d deg", g.config.board_name, lv_disp_get_hor_res(g.disp),
                          lv_disp_get_ver_res(g.disp), rotation_deg());
}

// a dot and the coordinates where the display tab is touched, to check the touch in every
// rotation. The dot lives on the top layer, which covers the screen without padding, so the
// absolute touch point can be used as it is.
void on_touch(lv_event_t *event)
{
    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);
    lv_obj_clear_flag(g.touch_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(g.touch_dot, point.x - 10, point.y - 10);
    lv_label_set_text_fmt(g.touch_label, "touch %d, %d", point.x, point.y);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        LV_LOG_USER("touch %d, %d at %d deg", point.x, point.y, rotation_deg());
    }
}

void on_touch_released(lv_event_t *)
{
    lv_obj_add_flag(g.touch_dot, LV_OBJ_FLAG_HIDDEN);
}

// only the tab page takes touches, so every press reaches on_touch()
void make_passive(lv_obj_t *obj)
{
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        lv_obj_clear_flag(child, LV_OBJ_FLAG_CLICKABLE);
        make_passive(child);
    }
}

void update_network(lv_timer_t *)
{
    char text[256];
    g.config.network_text(text, sizeof(text));
    lv_label_set_text(g.network_label, text);
}

void report(lv_timer_t *)
{
    const Stats s = g.period;
    g.period = {};
    const uint32_t avg_ms = s.refreshes ? s.time_ms / s.refreshes : 0;
    [[maybe_unused]] const uint32_t avg_px = s.refreshes ? s.pixels / s.refreshes : 0; // log only

    lv_label_set_text_fmt(g.stats, "partial: %lu/s, avg %lu ms, max %lu ms\nfull screen: %lu ms",
                          static_cast<unsigned long>(s.refreshes * 1000 / kReportPeriodMs),
                          static_cast<unsigned long>(avg_ms), static_cast<unsigned long>(s.max_ms),
                          static_cast<unsigned long>(g.full_ms));
    LV_LOG_USER("%d deg %dx%d: partial %lu refreshes/s, avg %lu ms for %lu px, max %lu ms; full screen %lu ms for %lu px",
                rotation_deg(), lv_disp_get_hor_res(g.disp), lv_disp_get_ver_res(g.disp),
                static_cast<unsigned long>(s.refreshes * 1000 / kReportPeriodMs), static_cast<unsigned long>(avg_ms),
                static_cast<unsigned long>(avg_px), static_cast<unsigned long>(s.max_ms),
                static_cast<unsigned long>(g.full_ms), static_cast<unsigned long>(g.full_pixels));

    if (g.config.cycle_rotation && ++g.reports % kReportsPerRotation == 0) {
        lv_disp_set_rotation(g.disp, static_cast<lv_disp_rot_t>((lv_disp_get_rotation(g.disp) + 1) % 4));
    }
    // the rotation may also have been changed from outside (orientation sensor)
    update_info();

    // the next refresh redraws everything and is measured on its own
    g.measure_full = true;
    lv_obj_invalidate(lv_scr_act());
}

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    return obj;
}

lv_obj_t *row(lv_obj_t *parent, lv_coord_t gap, bool wrap = false)
{
    lv_obj_t *obj = plain(parent);
    lv_obj_set_size(obj, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, wrap ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_row(obj, gap, 0);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, gap, 0);
    return obj;
}

lv_obj_t *color_box(lv_obj_t *parent, uint32_t color, lv_coord_t height, lv_coord_t radius)
{
    lv_obj_t *box = plain(parent);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_height(box, height);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_radius(box, radius, 0);
    return box;
}

void build_display_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, 8, 0);

    g.info = lv_label_create(tab);
    lv_obj_set_style_text_font(g.info, &lv_font_montserrat_20, 0);
    update_info();

    // red, green, blue, white: a wrong color order swaps red and blue, a wrong byte order gives
    // noise colors
    lv_obj_t *bars = row(tab, 6);
    const struct {
        uint32_t color;
        const char *name;
        uint32_t text;
    } kBars[] = {{0xff0000, "R", 0xffffff}, {0x00ff00, "G", 0x000000}, {0x0000ff, "B", 0xffffff},
                 {0xffffff, "W", 0x000000}};
    for (const auto &bar : kBars) {
        lv_obj_t *label = lv_label_create(color_box(bars, bar.color, 40, 6));
        lv_label_set_text(label, bar.name);
        lv_obj_set_style_text_color(label, lv_color_hex(bar.text), 0);
        lv_obj_center(label);
    }

    lv_obj_t *motion = row(tab, 12);
    lv_obj_t *spinner = lv_spinner_create(motion, 1000, 60);
    lv_obj_set_size(spinner, 56, 56);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(kColorActive), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(kColorCard), LV_PART_MAIN);

    lv_obj_t *bar = lv_bar_create(motion);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_height(bar, 16);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorActive), LV_PART_INDICATOR);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, bar);
    lv_anim_set_values(&anim, 0, 100);
    lv_anim_set_time(&anim, 1500);
    lv_anim_set_playback_time(&anim, 1500);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&anim, [](void *obj, int32_t value) {
        lv_bar_set_value(static_cast<lv_obj_t *>(obj), value, LV_ANIM_OFF);
    });
    lv_anim_start(&anim);

    g.stats = lv_label_create(tab);
    lv_obj_set_style_text_color(g.stats, lv_color_hex(kColorLabel), 0);
    lv_label_set_text(g.stats, "measuring ...");

    g.touch_label = lv_label_create(tab);
    lv_obj_set_style_text_color(g.touch_label, lv_color_hex(kColorLabel), 0);
    lv_label_set_text(g.touch_label, "touch: -");

    if (g.config.network_text != nullptr) {
        g.network_label = lv_label_create(tab);
        lv_obj_set_width(g.network_label, lv_pct(100));
        lv_label_set_long_mode(g.network_label, LV_LABEL_LONG_WRAP);
        update_network(nullptr);
        g.network_timer = lv_timer_create(update_network, 1000, nullptr);
    }

    make_passive(tab);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tab, on_touch, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(tab, on_touch, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(tab, on_touch_released, LV_EVENT_RELEASED, nullptr);

    g.touch_dot = plain(lv_layer_top());
    lv_obj_add_flag(g.touch_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(g.touch_dot, 20, 20);
    lv_obj_set_style_radius(g.touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g.touch_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.touch_dot, lv_color_hex(kColorActive), 0);
}

// ----------------------------------------------------------------------------- backlight tab

// While the minimum test drives the raw duty, the screen may get too dark to read. Without a touch
// for this long the brightness of the slider comes back.
constexpr uint32_t kMinimumTestTimeoutMs = 10000;
// the raw duty the minimum test starts from
constexpr uint16_t kMinimumTestStartDuty = 32;

const BacklightControl &backlight()
{
    return *g.config.backlight;
}

lv_obj_t *white_label(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorText), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

void apply_backlight(bool log)
{
    backlight().set(g.brightness, g.perceptual);
    lv_label_set_text_fmt(g.brightness_label, "%u %%", g.brightness);
    if (log) {
        LV_LOG_USER("backlight %u %% %s, minimum duty %u", g.brightness, g.perceptual ? "perceptual" : "linear",
                    backlight().minimum());
    }
}

void update_minimum_label()
{
    const uint16_t minimum = backlight().minimum();
    if (minimum == 0) {
        lv_label_set_text(g.minimum_label, "minimum: none, 1 % is duty 1");
    } else {
        lv_label_set_text_fmt(g.minimum_label, "minimum: duty %u of %u", minimum, backlight().max_duty);
    }
}

void end_minimum_test(lv_timer_t *)
{
    if (g.minimum_timer != nullptr) {
        lv_timer_del(g.minimum_timer);
        g.minimum_timer = nullptr;
    }
    g.test_duty = 0;
    lv_label_set_text(g.duty_label, "duty -");
    apply_backlight(false);
}

void on_brightness(lv_event_t *event)
{
    if (g.test_duty != 0) {
        end_minimum_test(nullptr);
    }
    g.brightness = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
    apply_backlight(lv_event_get_code(event) == LV_EVENT_RELEASED);
}

void on_curve(lv_event_t *event)
{
    g.perceptual = lv_btnmatrix_get_selected_btn(lv_event_get_target(event)) == 1;
    apply_backlight(true);
}

constexpr uint32_t kFrequencies[] = {1000, 5000, 25000};
constexpr uint16_t kFrequencyCount = sizeof(kFrequencies) / sizeof(kFrequencies[0]);

void on_frequency(lv_event_t *event)
{
    const uint16_t index = lv_btnmatrix_get_selected_btn(lv_event_get_target(event));
    if (index >= kFrequencyCount) {
        return;
    }
    if (!backlight().set_frequency(kFrequencies[index])) {
        LV_LOG_WARN("backlight frequency %lu Hz rejected", static_cast<unsigned long>(kFrequencies[index]));
        return;
    }
    LV_LOG_USER("backlight frequency %lu Hz", static_cast<unsigned long>(kFrequencies[index]));
    apply_backlight(false);
}

// "-" and "+": steps of 1 in the dark range, coarser above it
void on_duty_step(lv_event_t *event)
{
    const int direction = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (g.test_duty == 0) {
        g.test_duty = std::max<uint16_t>(kMinimumTestStartDuty, backlight().minimum());
    } else {
        const int step = (g.test_duty < 32) ? 1 : g.test_duty / 16;
        g.test_duty = static_cast<uint16_t>(std::clamp<int>(g.test_duty + direction * step, 1, backlight().max_duty));
    }
    backlight().set_duty(g.test_duty);
    lv_label_set_text_fmt(g.duty_label, "duty %u", g.test_duty);

    if (g.minimum_timer == nullptr) {
        g.minimum_timer = lv_timer_create(end_minimum_test, kMinimumTestTimeoutMs, nullptr);
    } else {
        lv_timer_reset(g.minimum_timer);
    }
}

void on_use_minimum(lv_event_t *)
{
    if (g.test_duty == 0) {
        return;
    }
    backlight().set_minimum(g.test_duty);
    LV_LOG_USER("backlight minimum set to duty %u of %u", g.test_duty, backlight().max_duty);
    update_minimum_label();
    end_minimum_test(nullptr);
}

void on_clear_minimum(lv_event_t *)
{
    backlight().set_minimum(0);
    LV_LOG_USER("backlight minimum cleared");
    update_minimum_label();
    end_minimum_test(nullptr);
}

lv_obj_t *button(lv_obj_t *parent, const char *text, lv_coord_t width, lv_event_cb_t callback, void *user_data = nullptr)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, width, 44);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorCard), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorActive), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *label = white_label(btn, &lv_font_montserrat_14);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_add_event_cb(btn, callback, LV_EVENT_LONG_PRESSED_REPEAT, user_data);
    return btn;
}

lv_obj_t *choice(lv_obj_t *parent, const char *const *map, uint16_t checked, lv_event_cb_t callback)
{
    lv_obj_t *matrix = lv_btnmatrix_create(parent);
    lv_btnmatrix_set_map(matrix, const_cast<const char **>(map));
    lv_btnmatrix_set_btn_ctrl_all(matrix, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(matrix, true);
    lv_btnmatrix_set_btn_ctrl(matrix, checked, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_flex_grow(matrix, 1);
    // two of them side by side on a landscape screen, one per line in portrait (320 wide)
    lv_obj_set_style_min_width(matrix, 200, 0);
    lv_obj_set_height(matrix, 44);
    lv_obj_set_style_pad_all(matrix, 2, 0);
    lv_obj_set_style_bg_color(matrix, lv_color_hex(kColorBar), 0);
    lv_obj_set_style_border_width(matrix, 0, 0);
    lv_obj_set_style_bg_color(matrix, lv_color_hex(kColorCard), LV_PART_ITEMS);
    lv_obj_set_style_text_color(matrix, lv_color_hex(kColorText), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(matrix, lv_color_hex(kColorActive),
                              static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED);
    lv_obj_add_event_cb(matrix, callback, LV_EVENT_VALUE_CHANGED, nullptr);
    return matrix;
}

// one ramp from black to `color` in kRampSteps steps of equal pixel value
void ramp(lv_obj_t *parent, uint32_t color, lv_coord_t height)
{
    lv_obj_t *steps = row(parent, 0);
    const uint8_t r = (color >> 16) & 0xff;
    const uint8_t gr = (color >> 8) & 0xff;
    const uint8_t b = color & 0xff;
    for (int i = 0; i < kRampSteps; i++) {
        const uint32_t scale = i * 255 / (kRampSteps - 1);
        const uint32_t step = ((r * scale / 255) << 16) | ((gr * scale / 255) << 8) | (b * scale / 255);
        color_box(steps, step, height, 0);
    }
}

void build_backlight_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, 8, 0);

    lv_obj_t *brightness = row(tab, 12);
    lv_obj_t *slider = lv_slider_create(brightness);
    lv_obj_set_flex_grow(slider, 1);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, g.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorActive), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorText), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_brightness, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, on_brightness, LV_EVENT_RELEASED, nullptr);
    g.brightness_label = white_label(brightness, &lv_font_montserrat_20);
    lv_obj_set_width(g.brightness_label, 64);

    // the rows wrap: in portrait (320 wide) they do not fit on one line
    lv_obj_t *options = row(tab, 8, true);
    static const char *const kCurveMap[] = {"Linear", "Perceptual", ""};
    choice(options, kCurveMap, g.perceptual ? 1 : 0, on_curve);
    static const char *const kFrequencyMap[] = {"1 kHz", "5 kHz", "25 kHz", ""};
    uint16_t frequency = kFrequencyCount - 1;
    for (uint16_t i = 0; i < kFrequencyCount; i++) {
        if (kFrequencies[i] == backlight().frequency_hz) {
            frequency = i;
        }
    }
    choice(options, kFrequencyMap, frequency, on_frequency);

    // minimum brightness: step the raw duty down until the text is just readable, then keep it
    lv_obj_t *minimum = row(tab, 8, true);
    button(minimum, LV_SYMBOL_MINUS, 52, on_duty_step, reinterpret_cast<void *>(intptr_t{-1}));
    g.duty_label = white_label(minimum, &lv_font_montserrat_20);
    lv_obj_set_flex_grow(g.duty_label, 1);
    lv_obj_set_style_text_align(g.duty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(g.duty_label, "duty -");
    button(minimum, LV_SYMBOL_PLUS, 52, on_duty_step, reinterpret_cast<void *>(intptr_t{1}));
    button(minimum, "Use as minimum", 140, on_use_minimum);
    button(minimum, "Clear", 64, on_clear_minimum);

    g.minimum_label = white_label(tab, &lv_font_montserrat_14);
    update_minimum_label();

    // gray ramp for the black and white contrast, then the three primaries
    ramp(tab, 0xffffff, 30);
    ramp(tab, 0xff0000, 16);
    ramp(tab, 0x00ff00, 16);
    ramp(tab, 0x0000ff, 16);

    apply_backlight(false);
}

} // namespace

void show_test_screen(lv_disp_t *disp, const TestScreenConfig &config)
{
    g = {};
    g.disp = disp;
    g.config = config;
    g.monitor = disp->driver->monitor_cb;
    disp->driver->monitor_cb = on_refresh;

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColorBackground), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(kColorText), 0);

    lv_obj_t *tabs = lv_tabview_create(screen, LV_DIR_TOP, kTabBarHeight);
    g.tabs = tabs;
    lv_obj_set_style_bg_color(tabs, lv_color_hex(kColorBackground), 0);
    // the default theme draws text dark, which is hard to read on the dark background
    lv_obj_set_style_text_color(lv_tabview_get_content(tabs), lv_color_hex(kColorText), 0);
    lv_obj_t *buttons = lv_tabview_get_tab_btns(tabs);
    lv_obj_set_style_bg_color(buttons, lv_color_hex(kColorBar), 0);
    lv_obj_set_style_text_color(buttons, lv_color_hex(kColorLabel), 0);
    lv_obj_set_style_text_color(buttons, lv_color_hex(kColorText), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED);
    // the touch test drags across the page, so the pages do not swipe
    lv_obj_clear_flag(lv_tabview_get_content(tabs), LV_OBJ_FLAG_SCROLLABLE);

    build_display_tab(lv_tabview_add_tab(tabs, "Display"));
    if (config.backlight != nullptr) {
        build_backlight_tab(lv_tabview_add_tab(tabs, "Backlight"));
    }

    g.report_timer = lv_timer_create(report, kReportPeriodMs, nullptr);
}

void hide_test_screen()
{
    if (g.tabs == nullptr) {
        return;
    }
    if (g.test_duty != 0) {
        end_minimum_test(nullptr);
    }
    for (lv_timer_t *timer : {g.report_timer, g.network_timer}) {
        if (timer != nullptr) {
            lv_timer_del(timer);
        }
    }
    if (g.disp->driver->monitor_cb == on_refresh) {
        g.disp->driver->monitor_cb = g.monitor;
    }
    lv_obj_del(g.touch_dot);
    lv_obj_del(g.tabs);
    g = {};
}

} // namespace hc::ui
