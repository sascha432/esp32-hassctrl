#include "theme.hpp"

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "sound.hpp"

// the digit fonts of the clock tile (tools/build_icons.py), C files like the icon fonts
extern "C" {
LV_FONT_DECLARE(hc_digits_48)
LV_FONT_DECLARE(hc_digits_72)
}

namespace hc::ui::theme {

namespace {

constexpr char kDots[] = "...";

lv_coord_t text_width(const char *text, size_t length, const lv_font_t *font, lv_coord_t letter_space)
{
    return lv_txt_get_width(text, static_cast<uint32_t>(length), font, letter_space, LV_TEXT_FLAG_NONE);
}

// The width the text may take: the label's own for a width from the layout, its max width for a
// label as wide as its text (whose own width would shrink with every cut).
lv_coord_t available_width(lv_obj_t *label)
{
    if (lv_obj_get_style_width(label, LV_PART_MAIN) != LV_SIZE_CONTENT ||
        lv_obj_get_style_flex_grow(label, LV_PART_MAIN) > 0) {
        return lv_obj_get_content_width(label);
    }
    lv_coord_t max = lv_obj_get_style_max_width(label, LV_PART_MAIN);
    if (LV_COORD_IS_PCT(max)) {
        max = static_cast<lv_coord_t>(lv_obj_get_content_width(lv_obj_get_parent(label)) * LV_COORD_GET_PCT(max) / 100);
    }
    return max;
}

// The text of a label set with line_text(): its own copy, and the font to try before a cut.
struct Line {
    const lv_font_t *font;
    const lv_font_t *smaller;
    char text[1];
};

void fit_line(lv_obj_t *label)
{
    const auto *line = static_cast<const Line *>(lv_obj_get_user_data(label));
    const char *full = line->text;
    const lv_coord_t space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    const lv_coord_t width = available_width(label);
    size_t length = std::strlen(full);
    const lv_font_t *font = line->font;
    if (line->smaller != nullptr && width > 0 && text_width(full, length, font, space) > width) {
        font = line->smaller;
    }
    if (line->smaller != nullptr && lv_obj_get_style_text_font(label, LV_PART_MAIN) != font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    if (width <= 0 || text_width(full, length, font, space) <= width) {
        lv_label_set_text(label, full);
        return;
    }
    const lv_coord_t dots = text_width(kDots, sizeof(kDots) - 1, font, space);
    while (length > 0 && text_width(full, length, font, space) + dots > width) {
        // one UTF-8 character back
        do {
            length--;
        } while (length > 0 && (static_cast<unsigned char>(full[length]) & 0xc0) == 0x80);
    }
    lv_label_set_text_fmt(label, "%.*s%s", static_cast<int>(length), full, kDots);
}

void on_parent_size(lv_event_t *event)
{
    fit_line(static_cast<lv_obj_t *>(lv_event_get_user_data(event)));
}

void on_line_event(lv_event_t *event)
{
    lv_obj_t *label = lv_event_get_target(event);
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        lv_obj_remove_event_cb_with_user_data(lv_obj_get_parent(label), on_parent_size, label);
        lv_mem_free(lv_obj_get_user_data(label));
        lv_obj_set_user_data(label, nullptr);
    } else {
        fit_line(label);
    }
}

} // namespace

lv_color_t color(uint32_t hex)
{
    return lv_color_hex(hex);
}

namespace {

struct Fonts {
    // how much larger than the normal set, in percent
    int scale;
    const lv_font_t *small;
    const lv_font_t *normal;
    const lv_font_t *medium;
    const lv_font_t *large;
    const lv_font_t *huge;
    const lv_font_t *item; // the items of the panels' lists
};

constexpr Fonts kFonts{100, &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16, &lv_font_montserrat_20,
                       &lv_font_montserrat_28, &lv_font_montserrat_14};
// only in the builds of the boards that use them (their sdkconfig.defaults)
#if LV_FONT_MONTSERRAT_18 && LV_FONT_MONTSERRAT_24 && LV_FONT_MONTSERRAT_26 && LV_FONT_MONTSERRAT_36
// the list items a third larger than the normal font: the panels' lists read small on the P4
constexpr Fonts kLargeFonts{130, &lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20,
                            &lv_font_montserrat_26, &lv_font_montserrat_36, &lv_font_montserrat_24};
#endif

// Settings on the large-font boards: one font step up from the large set (about 155 % of the normal set)
#if LV_FONT_MONTSERRAT_22 && LV_FONT_MONTSERRAT_32 && LV_FONT_MONTSERRAT_34
constexpr Fonts kSettingsFonts{155, &lv_font_montserrat_18, &lv_font_montserrat_22, &lv_font_montserrat_24,
                               &lv_font_montserrat_32, &lv_font_montserrat_34, &lv_font_montserrat_28};
#endif
constexpr int kSettingsMetricPercent = 128;

const Fonts *g_fonts = &kFonts;
#if LV_FONT_MONTSERRAT_18 && LV_FONT_MONTSERRAT_24 && LV_FONT_MONTSERRAT_26 && LV_FONT_MONTSERRAT_36
bool g_large = false; // only the boards with the large fonts built in (the P4)
#endif
int g_metric = 100;

} // namespace

void use_large_fonts(bool large)
{
#if LV_FONT_MONTSERRAT_18 && LV_FONT_MONTSERRAT_24 && LV_FONT_MONTSERRAT_26 && LV_FONT_MONTSERRAT_36
    g_fonts = large ? &kLargeFonts : &kFonts;
    g_large = large;
#else
    if (large) {
        LV_LOG_WARN("large fonts are not built in (Montserrat 18, 24, 26, 36)");
    }
#endif
}

void set_settings_mode(bool on)
{
#if LV_FONT_MONTSERRAT_18 && LV_FONT_MONTSERRAT_24 && LV_FONT_MONTSERRAT_26 && LV_FONT_MONTSERRAT_36 &&     LV_FONT_MONTSERRAT_22 && LV_FONT_MONTSERRAT_32 && LV_FONT_MONTSERRAT_34
    if (g_large) {
        g_fonts = on ? &kSettingsFonts : &kLargeFonts;
        g_metric = on ? kSettingsMetricPercent : 100;
    }
#else
    (void)on;
#endif
}

lv_coord_t dim(lv_coord_t px)
{
    return static_cast<lv_coord_t>((px * g_metric + 50) / 100);
}

lv_coord_t status_bar_height()
{
    return dim(kStatusBarHeight);
}

lv_coord_t row_height()
{
    return dim(kRowHeight);
}

lv_coord_t pad()
{
    return dim(kPad);
}


lv_coord_t radius()
{
    return dim(kRadius);
}

const lv_font_t &font_small()
{
    return *g_fonts->small;
}

const lv_font_t &font_normal()
{
    return *g_fonts->normal;
}

const lv_font_t &font_medium()
{
    return *g_fonts->medium;
}

const lv_font_t &font_large()
{
    return *g_fonts->large;
}

const lv_font_t &font_huge()
{
    return *g_fonts->huge;
}

const lv_font_t &font_item()
{
    return *g_fonts->item;
}

const lv_font_t &font_digits_48()
{
    return hc_digits_48;
}

const lv_font_t &font_digits_72()
{
    return hc_digits_72;
}

lv_coord_t scaled(lv_coord_t px)
{
    return static_cast<lv_coord_t>((px * g_fonts->scale + 50) / 100);
}

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

void image_dsc(lv_img_dsc_t &dsc, const void *pixels, uint16_t width, uint16_t height)
{
    dsc = {};
    dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc.header.w = width;
    dsc.header.h = height;
    dsc.data_size = static_cast<uint32_t>(width) * height * sizeof(lv_color_t);
    dsc.data = static_cast<const uint8_t *>(pixels);
}

lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t &font, uint32_t text_color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, &font, 0);
    lv_obj_set_style_text_color(obj, color(text_color), 0);
    return obj;
}

void line_text(lv_obj_t *label, const char *text, const lv_font_t *smaller)
{
    auto *old = static_cast<Line *>(lv_obj_get_user_data(label));
    if (old != nullptr && old->smaller == smaller && std::strcmp(old->text, text) == 0) {
        return; // refreshes set the same text every second
    }
    if (old == nullptr) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_add_event_cb(label, on_line_event, LV_EVENT_SIZE_CHANGED, nullptr);
        lv_obj_add_event_cb(label, on_line_event, LV_EVENT_DELETE, nullptr);
        // a wider parent (the display turned) leaves room for more of a cut text
        lv_obj_add_event_cb(lv_obj_get_parent(label), on_parent_size, LV_EVENT_SIZE_CHANGED, label);
    }
    const size_t size = std::strlen(text) + 1;
    auto *line = static_cast<Line *>(lv_mem_alloc(offsetof(Line, text) + size));
    if (line == nullptr) {
        lv_label_set_text(label, text);
        return;
    }
    // the label's own font, also after fit_line() set the smaller one
    line->font = old != nullptr ? old->font : lv_obj_get_style_text_font(label, LV_PART_MAIN);
    line->smaller = smaller;
    std::memcpy(line->text, text, size);
    lv_mem_free(old);
    lv_obj_set_user_data(label, line);
    fit_line(label);
}

lv_obj_t *row(lv_obj_t *parent, const char *text, const char *value, bool opens_page, lv_event_cb_t callback,
              void *user_data)
{
    lv_obj_t *obj = plain(parent);
    lv_obj_set_size(obj, lv_pct(100), row_height());
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, color(kCard), 0);
    lv_obj_set_style_bg_color(obj, color(kBorder), LV_STATE_PRESSED);
    lv_obj_set_style_radius(obj, radius(), 0);
    lv_obj_set_style_pad_hor(obj, dim(12), 0);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, dim(8), 0);

    lv_obj_t *name = label(obj, "", font_medium(), kText);
    // The name is as wide as its text up to 60 % of the row, the value takes all the rest: a
    // short name leaves room for a long value (the Wi-Fi name), a long name still leaves 40 %.
    lv_obj_set_style_max_width(name, lv_pct(60), 0);
    line_text(name, text);

    lv_obj_t *val = label(obj, "", font_normal(), kTextValue);
    lv_obj_set_flex_grow(val, 1);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    line_text(val, value != nullptr ? value : "");

    if (opens_page) {
        label(obj, LV_SYMBOL_RIGHT, font_normal(), kTextLabel);
    }
    if (callback != nullptr) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        sound::tappable(obj);
        lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, user_data);
    } else {
        // a row without an action takes no touches: they go to the page below (it scrolls)
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
    return obj;
}

lv_obj_t *row_value(lv_obj_t *row)
{
    return lv_obj_get_child(row, 1);
}

lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *user_data, bool danger)
{
    lv_obj_t *obj = lv_btn_create(parent);
    lv_obj_set_height(obj, row_height());
    lv_obj_set_style_min_width(obj, dim(96), 0);
    lv_obj_set_style_radius(obj, radius(), 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, color(danger ? kError : kCard), 0);
    lv_obj_set_style_bg_color(obj, color(kActive), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(obj, danger ? 0 : 1, 0);
    lv_obj_set_style_border_color(obj, color(kBorder), 0);
    lv_obj_t *text_label = label(obj, text, font_normal(), kText);
    lv_obj_center(text_label);
    if (callback != nullptr) {
        lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, user_data);
    }
    return obj;
}

lv_obj_t *toggle(lv_obj_t *parent, bool on, lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *obj = lv_switch_create(parent);
    lv_obj_set_size(obj, dim(52), dim(28));
    lv_obj_set_style_bg_color(obj, color(kBorder), LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, color(kActive), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(obj, color(kText), LV_PART_KNOB);
    if (on) {
        lv_obj_add_state(obj, LV_STATE_CHECKED);
    }
    if (callback != nullptr) {
        lv_obj_add_event_cb(obj, callback, LV_EVENT_VALUE_CHANGED, user_data);
    }
    // A row without an action of its own flips the switch with a tap anywhere on it: the switch
    // alone was hard to hit, the row took the taps beside it.
    if (!lv_obj_has_flag(parent, LV_OBJ_FLAG_CLICKABLE)) {
        lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
        sound::tappable(parent);
        lv_obj_add_event_cb(
            parent,
            [](lv_event_t *event) {
                auto *sw = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
                if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
                    lv_obj_clear_state(sw, LV_STATE_CHECKED);
                } else {
                    lv_obj_add_state(sw, LV_STATE_CHECKED);
                }
                lv_event_send(sw, LV_EVENT_VALUE_CHANGED, nullptr);
            },
            LV_EVENT_CLICKED, obj);
    }
    return obj;
}

// past the slider's ends: the knob's overhang plus a margin for a finger
constexpr lv_coord_t kSliderTouchMargin = 16;

lv_obj_t *slider(lv_obj_t *parent, int32_t min, int32_t max, int32_t value)
{
    lv_obj_t *obj = lv_slider_create(parent);
    lv_slider_set_range(obj, min, max);
    lv_slider_set_value(obj, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(obj, color(kBorder), LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, color(kActive), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(obj, color(kText), LV_PART_KNOB);
    // the knob covers the end of the track, so the slider keeps a gap to the edges
    // settings mode: a thicker track and a larger round knob
    if (dim(100) != 100) {
        lv_obj_set_height(obj, dim(12));
        lv_obj_set_style_pad_all(obj, dim(8), LV_PART_KNOB);
    } else {
        lv_obj_set_style_pad_all(obj, 4, LV_PART_KNOB);
    }
    // At 0 and 100 % half of the knob lies outside the slider, where its container took the
    // touches: a drag that started there did nothing. The slider takes touches as far as the knob
    // reaches (and a little more for a finger).
    lv_obj_set_ext_click_area(obj, kSliderTouchMargin);
    return obj;
}

namespace {

// "00\n01\n...": the options of a roller from 0 to count - 1
void number_options(char *out, size_t size, int count)
{
    size_t used = 0;
    for (int i = 0; i < count && used + 4 < size; i++) {
        used += static_cast<size_t>(std::snprintf(out + used, size - used, i == 0 ? "%02d" : "\n%02d", i));
    }
}

// A roller follows the finger 1:1 (LVGL 8.4): 45 rows are about 1350 px. A quick drag moves it up to
// kRollerMaxGain times as far, a step of kRollerSlowPx or less per read stays exact; a fling on
// release goes kRollerThrow times as far as LVGL's prediction.
constexpr lv_coord_t kRollerSlowPx = 2;
constexpr lv_coord_t kRollerMaxGain = 4;
constexpr lv_coord_t kRollerThrow = 3;

// after the roller's own handler moved its label by the step
void on_roller_pressing(lv_event_t *event)
{
    lv_point_t step;
    lv_indev_get_vect(lv_indev_get_act(), &step);
    const lv_coord_t gain = std::clamp<lv_coord_t>(static_cast<lv_coord_t>(std::abs(step.y) / kRollerSlowPx), 1, kRollerMaxGain);
    if (step.y != 0 && gain > 1) {
        lv_obj_t *label = lv_obj_get_child(lv_event_get_target(event), 0);
        lv_obj_set_y(label, static_cast<lv_coord_t>(lv_obj_get_y(label) + step.y * (gain - 1)));
    }
}

// before the roller's own handler predicts where the fling ends (it reads the _ori vector, the
// scrolling of pages reads the other one)
void on_roller_released(lv_event_t *)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != nullptr && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        lv_coord_t &y = indev->proc.types.pointer.scroll_throw_vect_ori.y;
        y = static_cast<lv_coord_t>(std::clamp<int32_t>(y * kRollerThrow, -1000, 1000));
    }
}

} // namespace

lv_obj_t *roller(lv_obj_t *parent, int count, int value)
{
    char options[100 * 3 + 1];
    number_options(options, sizeof(options), count);
    lv_obj_t *obj = lv_roller_create(parent);
    // not LV_ROLLER_MODE_INFINITE: its seven copies of 60 minutes in the large font drew the
    // selected row from another place (LVGL 8.4)
    lv_roller_set_options(obj, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(obj, 3);
    lv_roller_set_selected(obj, static_cast<uint16_t>(value), LV_ANIM_OFF);
    lv_obj_set_width(obj, scaled(80));
    // the selected row in the same font, or it is drawn at another height than the others
    lv_obj_set_style_text_font(obj, &font_large(), 0);
    lv_obj_set_style_text_font(obj, &font_large(), LV_PART_SELECTED);
    lv_obj_set_style_text_color(obj, color(kTextLabel), 0);
    lv_obj_set_style_bg_color(obj, color(kCard), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, color(kActive), LV_PART_SELECTED);
    lv_obj_set_style_text_color(obj, color(kBackground), LV_PART_SELECTED);
    lv_obj_add_event_cb(obj, on_roller_pressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(obj, on_roller_released, static_cast<lv_event_code_t>(LV_EVENT_RELEASED | LV_EVENT_PREPROCESS), nullptr);
    return obj;
}

lv_obj_t *column(lv_obj_t *parent)
{
    lv_obj_t *obj = plain(parent);
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(obj, pad(), 0);
    lv_obj_set_style_pad_row(obj, dim(6), 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_ACTIVE);
    return obj;
}

lv_obj_t *caption_value(lv_obj_t *parent, const char *caption, const char *value, const lv_font_t &font)
{
    lv_obj_t *name = label(parent, caption, font_small(), kTextLabel);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_t *text = label(parent, value, font, kText);
    lv_obj_set_width(text, lv_pct(100));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_bottom(text, dim(4), 0);
    return text;
}

// A white card with a QR code: dark on light with a quiet zone, also in the dark theme
// (docs/ui/lvgl/lvgl-design.md section 2).
lv_obj_t *qr_card(lv_obj_t *parent, const char *data, lv_coord_t size)
{
    lv_obj_t *card = plain(parent);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_radius(card, dim(8), 0);
    lv_obj_set_style_pad_all(card, dim(10), 0);
    lv_obj_t *qr = lv_qrcode_create(card, size, lv_color_black(), lv_color_white());
    lv_qrcode_update(qr, data, static_cast<uint32_t>(std::strlen(data)));
    return card;
}

// the Wi-Fi QR code format: WIFI:T:WPA;S:<ssid>;P:<password>;; with \ ; , : " escaped
void wifi_qr_text(char *out, size_t size, const char *ssid, const char *password)
{
    auto escape = [](char *o, size_t room, const char *in) {
        size_t n = 0;
        for (; *in != 0 && n + 2 < room; in++) {
            if (std::strchr("\\;,:\"", *in) != nullptr) {
                o[n++] = '\\';
            }
            o[n++] = *in;
        }
        o[n] = 0;
    };
    char s[2 * 33]; // an SSID has up to 32 characters, each may need an escape
    char p[32];
    escape(s, sizeof(s), ssid);
    escape(p, sizeof(p), password);
    std::snprintf(out, size, "WIFI:T:WPA;S:%s;P:%s;;", s, p);
}

namespace {

void apply_split(lv_obj_t *wrap)
{
    lv_obj_t *info = lv_obj_get_child(wrap, 0);
    const bool wide = lv_obj_get_content_width(wrap) > lv_obj_get_content_height(wrap);
    lv_obj_set_flex_flow(wrap, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(info, wide ? 1 : 0);
    lv_obj_set_width(info, wide ? LV_SIZE_CONTENT : lv_pct(100));
}

} // namespace

// Landscape: the text on the left, the QR code (added to the returned object by the caller) on the
// right. Portrait: below each other. Switches when the display turns.
lv_obj_t *split(lv_obj_t *content)
{
    lv_obj_t *wrap = column(content);
    lv_obj_set_style_pad_column(wrap, dim(12), 0);
    lv_obj_set_style_pad_row(wrap, dim(12), 0);
    lv_obj_t *info = plain(wrap);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, dim(8), 0);
    lv_obj_add_event_cb(wrap, [](lv_event_t *event) { apply_split(lv_event_get_target(event)); },
                        LV_EVENT_SIZE_CHANGED, nullptr);
    lv_obj_update_layout(wrap);
    apply_split(wrap);
    return wrap;
}

} // namespace hc::ui::theme
