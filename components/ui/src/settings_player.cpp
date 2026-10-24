// Settings, the media player (CONFIG_HC_UI_PLAYER, docs/ui/lvgl/lvgl-settings.md section 3): the
// cover and the tags of what plays, a spectrum analyser, the position with seeking, previous,
// play/pause, next and stop, shuffle and repeat, and the volume. The player plays on in the
// background (components/player): the page only shows and controls it, leaving it changes
// nothing. It opens from the file manager, the media library and the play icon of the status bar.

#include "settings_pages.hpp"

#if CONFIG_HC_UI_PLAYER

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "sound.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

constexpr size_t kBands = 16;
// the segments of a bar (scaled), and the hue of the highest band (0 is red, 280 violet)
constexpr lv_coord_t kSegmentPx = 6;
constexpr lv_coord_t kSegmentGapPx = 3;
constexpr uint32_t kLastHue = 280;
// the spectrum's frame time; the bars fall this much (of 1) per frame, the peaks slower
constexpr uint32_t kFrameMs = 33;
constexpr float kBarFall = 0.06f;
constexpr float kPeakFall = 0.012f;
// the position slider's steps
constexpr int32_t kSliderSteps = 1000;
// the cover's side and the spectrum's height (scaled); smaller on a screen less than
// kShortScreen high (480x320 in landscape), so the controls stay in sight
constexpr lv_coord_t kCoverPx = 120;
constexpr lv_coord_t kSpectrumPx = 110;
constexpr lv_coord_t kShortCoverPx = 88;
constexpr lv_coord_t kShortSpectrumPx = 72;
constexpr lv_coord_t kShortScreen = 400;

bool short_screen()
{
    return lv_disp_get_ver_res(nullptr) < kShortScreen;
}

const Backend &be()
{
    return backend();
}

struct Player {
    lv_obj_t *cover_box = nullptr;
    lv_obj_t *cover = nullptr;
    lv_obj_t *cover_none = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *artist = nullptr;
    lv_obj_t *album = nullptr;
    lv_obj_t *place = nullptr;
    lv_obj_t *spectrum = nullptr;
    lv_obj_t *time = nullptr;
    lv_obj_t *slider = nullptr;
    lv_obj_t *play_label = nullptr;
    lv_obj_t *shuffle_label = nullptr;
    lv_obj_t *repeat_label = nullptr;
    lv_obj_t *message = nullptr;
    lv_timer_t *timer = nullptr;
    float bars[kBands] = {};
    float peaks[kBands] = {};
    PlayerInfo info{};
    // what the page shows: built from the first info, then updated when it changes
    bool shown = false;
    uint32_t error_serial = 0;
    uint32_t cover_serial = 0;
    core::Image cover_image;
    lv_img_dsc_t cover_dsc{};
};
Player g;

void time_text(uint32_t ms, char *out, size_t size)
{
    const uint32_t s = ms / 1000;
    std::snprintf(out, size, "%lu:%02lu", static_cast<unsigned long>(s / 60), static_cast<unsigned long>(s % 60));
}

// "1:23 / 4:56", or the position alone while the length is not known
void show_time(uint32_t position_ms)
{
    char position[16];
    time_text(position_ms, position, sizeof(position));
    char text[40];
    if (g.info.duration_ms > 0) {
        char duration[16];
        time_text(g.info.duration_ms, duration, sizeof(duration));
        std::snprintf(text, sizeof(text), "%s / %s", position, duration);
    } else {
        std::snprintf(text, sizeof(text), "%s", position);
    }
    lv_label_set_text(g.time, text);
}

void show_message(const char *text)
{
    lv_label_set_text(g.message, text);
    if (text[0] == 0) {
        lv_obj_add_flag(g.message, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(g.message, LV_OBJ_FLAG_HIDDEN);
    }
}

bool loaded(PlayerState state)
{
    return state != PlayerState::Idle && state != PlayerState::Failed;
}

void on_seek(lv_event_t *event)
{
    if (g.info.duration_ms == 0 || !loaded(g.info.state)) {
        return;
    }
    const auto value = static_cast<uint64_t>(lv_slider_get_value(lv_event_get_target(event)));
    be().player_seek(static_cast<uint32_t>(value * g.info.duration_ms / kSliderSteps));
}

// the time while the slider is dragged
void on_slide(lv_event_t *event)
{
    if (g.info.duration_ms > 0) {
        const auto value = static_cast<uint64_t>(lv_slider_get_value(lv_event_get_target(event)));
        show_time(static_cast<uint32_t>(value * g.info.duration_ms / kSliderSteps));
    }
}

void show_options()
{
    lv_label_set_text(g.shuffle_label, LV_SYMBOL_SHUFFLE "  Shuffle");
    lv_obj_set_style_text_color(g.shuffle_label, t::color(g.info.shuffle ? t::kActive : t::kTextMuted), 0);
    const char *repeat = g.info.repeat == PlayerRepeat::All   ? LV_SYMBOL_LOOP "  Repeat all"
                         : g.info.repeat == PlayerRepeat::One ? LV_SYMBOL_LOOP "  Repeat one"
                                                              : LV_SYMBOL_LOOP "  Repeat";
    lv_label_set_text(g.repeat_label, repeat);
    lv_obj_set_style_text_color(g.repeat_label,
                                t::color(g.info.repeat != PlayerRepeat::Off ? t::kActive : t::kTextMuted), 0);
}

void show_cover()
{
    core::Image image;
    const lv_coord_t size = lv_obj_get_width(g.cover_box);
    const bool have = size > 0 && be().player_cover(static_cast<uint16_t>(size), image) && image.pixels;
    // LVGL keeps what it read of the old pixels by their address
    lv_img_cache_invalidate_src(&g.cover_dsc);
    g.cover_image = std::move(image);
    if (have) {
        t::image_dsc(g.cover_dsc, g.cover_image.pixels.get(), g.cover_image.width, g.cover_image.height);
        lv_img_set_src(g.cover, &g.cover_dsc);
        lv_obj_clear_flag(g.cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g.cover_none, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g.cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g.cover_none, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_text(lv_obj_t *label, const char *text)
{
    if (std::strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
    if (text[0] == 0) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

// what changed of the info since the last frame
void show_info(const PlayerInfo &info)
{
    const PlayerInfo before = g.info;
    g.info = info;
    if (!g.shown || info.state != before.state) {
        const bool pause = info.state == PlayerState::Playing || info.state == PlayerState::Buffering ||
                           info.state == PlayerState::Interrupted;
        lv_label_set_text(g.play_label, pause ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    set_text(g.title, info.count > 0 ? info.title : "Nothing plays");
    set_text(g.artist, info.artist);
    set_text(g.album, info.album);
    char place[48] = {};
    if (info.count > 1) {
        std::snprintf(place, sizeof(place), "%u / %u", info.index + 1u, static_cast<unsigned>(info.count));
    }
    if (info.state == PlayerState::Buffering) {
        const size_t used = std::strlen(place);
        std::snprintf(place + used, sizeof(place) - used, "%sLoading ...", used > 0 ? "   " : "");
    }
    set_text(g.place, place);
    if (!g.shown || info.shuffle != before.shuffle || info.repeat != before.repeat) {
        show_options();
    }
    if (!g.shown || info.cover_serial != g.cover_serial) {
        g.cover_serial = info.cover_serial;
        show_cover();
    }
    if (g.shown && info.error_serial != g.error_serial) {
        show_message(info.error);
    }
    g.error_serial = info.error_serial;
    if (!lv_obj_has_state(g.slider, LV_STATE_PRESSED)) {
        const uint32_t position = loaded(info.state) ? info.position_ms : 0;
        const int32_t value = info.duration_ms > 0
                                  ? static_cast<int32_t>(std::min<uint64_t>(
                                        uint64_t{position} * kSliderSteps / info.duration_ms, kSliderSteps))
                                  : 0;
        lv_slider_set_value(g.slider, value, LV_ANIM_OFF);
        show_time(position);
    }
    g.shown = true;
}

// The look of an LED meter (your example, 2026-10-23): each band a column of rounded segments,
// the bands in the colors of the rainbow from red (bass) to violet, the peak a white segment.
void on_draw(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target(event);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(event);
    lv_area_t box;
    lv_obj_get_content_coords(obj, &box);
    const lv_coord_t width = lv_area_get_width(&box);
    const lv_coord_t height = lv_area_get_height(&box);
    const lv_coord_t gap = std::max<lv_coord_t>(2, t::dim(4));
    const lv_coord_t bar = std::max<lv_coord_t>(1, (width - gap * (kBands - 1)) / kBands);
    // the segments: their height and the gap between them
    const lv_coord_t segment = std::max<lv_coord_t>(2, t::dim(kSegmentPx));
    const lv_coord_t segment_gap = std::max<lv_coord_t>(1, t::dim(kSegmentGapPx));
    const int segments = std::max(1, (height + segment_gap) / (segment + segment_gap));
    // the bars as wide as the box, the rest of the division to both sides
    const lv_coord_t left = box.x1 + (width - (bar * kBands + gap * (kBands - 1))) / 2;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = std::max<lv_coord_t>(1, segment / 3);
    dsc.bg_opa = LV_OPA_COVER;
    for (size_t i = 0; i < kBands; i++) {
        const auto hue = static_cast<uint16_t>(kLastHue * i / (kBands - 1));
        const lv_color_t color = lv_color_hsv_to_rgb(hue, 100, 100);
        // the lowest segment stays lit where a band is silent, so the bars are seen
        const int lit = std::max(1, static_cast<int>(g.bars[i] * static_cast<float>(segments) + 0.5f));
        const int peak = static_cast<int>(g.peaks[i] * static_cast<float>(segments) + 0.5f);
        lv_area_t area;
        area.x1 = left + static_cast<lv_coord_t>(i) * (bar + gap);
        area.x2 = area.x1 + bar - 1;
        for (int s = 0; s < segments; s++) {
            const bool on = s < lit;
            const bool top = s == peak - 1 && peak > lit;
            if (!on && !top) {
                continue;
            }
            area.y2 = box.y2 - s * (segment + segment_gap);
            area.y1 = area.y2 - segment + 1;
            dsc.bg_color = top ? lv_color_white() : color;
            lv_draw_rect(ctx, &dsc, &area);
        }
    }
}

void on_frame(lv_timer_t *)
{
    float levels[kBands] = {};
    const bool sounding = g.info.state == PlayerState::Playing && be().player_spectrum(levels, kBands);
    bool moved = false;
    for (size_t i = 0; i < kBands; i++) {
        const float level = sounding ? levels[i] : 0.0f;
        const float bar = std::max(level, g.bars[i] - kBarFall);
        const float peak = std::max(bar, g.peaks[i] - kPeakFall);
        moved = moved || bar != g.bars[i] || peak != g.peaks[i];
        g.bars[i] = std::max(bar, 0.0f);
        g.peaks[i] = std::max(peak, 0.0f);
    }
    if (moved) {
        lv_obj_invalidate(g.spectrum);
    }
    PlayerInfo info{};
    be().player_info(info);
    show_info(info);
}

// a round button with a symbol, the play button larger
lv_obj_t *control(lv_obj_t *parent, const char *symbol, lv_event_cb_t callback, bool large)
{
    lv_obj_t *button = t::button(parent, symbol, callback);
    const lv_coord_t size = t::dim(large ? 64 : 52);
    // round: not the width of a text button
    lv_obj_set_style_min_width(button, 0, 0);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    if (large) {
        lv_obj_set_style_bg_color(button, t::color(t::kActive), 0);
        lv_obj_set_style_border_width(button, 0, 0);
    }
    lv_obj_t *label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(label, large ? &t::font_large() : &t::font_medium(), 0);
    lv_obj_center(label);
    return button;
}

// the cover and the tags beside it
void build_header(lv_obj_t *col)
{
    lv_obj_t *header = t::plain(col);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, t::dim(14), 0);

    const lv_coord_t cover = t::dim(short_screen() ? kShortCoverPx : kCoverPx);
    g.cover_box = t::plain(header);
    lv_obj_set_size(g.cover_box, cover, cover);
    lv_obj_set_style_bg_opa(g.cover_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.cover_box, t::color(t::kCard), 0);
    lv_obj_set_style_radius(g.cover_box, t::radius(), 0);
    lv_obj_set_style_clip_corner(g.cover_box, true, 0);
    g.cover = lv_img_create(g.cover_box);
    lv_obj_center(g.cover);
    lv_obj_add_flag(g.cover, LV_OBJ_FLAG_HIDDEN);
    g.cover_none = t::label(g.cover_box, LV_SYMBOL_AUDIO, t::font_huge(), t::kTextMuted);
    lv_obj_center(g.cover_none);

    lv_obj_t *text = t::plain(header);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, t::dim(4), 0);
    g.title = t::label(text, "", t::font_medium(), t::kText);
    lv_obj_set_width(g.title, lv_pct(100));
    lv_label_set_long_mode(g.title, LV_LABEL_LONG_WRAP);
    g.artist = t::label(text, "", t::font_normal(), t::kTextValue);
    lv_obj_set_width(g.artist, lv_pct(100));
    lv_label_set_long_mode(g.artist, LV_LABEL_LONG_DOT);
    g.album = t::label(text, "", t::font_small(), t::kTextMuted);
    lv_obj_set_width(g.album, lv_pct(100));
    lv_label_set_long_mode(g.album, LV_LABEL_LONG_DOT);
    g.place = t::label(text, "", t::font_small(), t::kTextMuted);
}

} // namespace

Built player_page(lv_obj_t *content)
{
    g = Player{};
    lv_obj_t *col = t::column(content);
    build_header(col);

    g.spectrum = t::plain(col);
    lv_obj_set_size(g.spectrum, lv_pct(100), t::dim(short_screen() ? kShortSpectrumPx : kSpectrumPx));
    lv_obj_set_style_bg_opa(g.spectrum, LV_OPA_COVER, 0);
    // black as the meter of the example: the colors stand out
    lv_obj_set_style_bg_color(g.spectrum, lv_color_black(), 0);
    lv_obj_set_style_radius(g.spectrum, t::radius(), 0);
    lv_obj_set_style_pad_all(g.spectrum, t::dim(10), 0);
    lv_obj_add_event_cb(g.spectrum, on_draw, LV_EVENT_DRAW_MAIN, nullptr);

    g.message = text_block(col, "", t::kAlert);
    lv_obj_add_flag(g.message, LV_OBJ_FLAG_HIDDEN);

    // the position, a card like the volume's: the time above the slider, which seeks when it is
    // let go
    lv_obj_t *card = t::plain(col);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, t::color(t::kCard), 0);
    lv_obj_set_style_radius(card, t::radius(), 0);
    lv_obj_set_style_pad_hor(card, t::dim(12), 0);
    lv_obj_set_style_pad_top(card, t::dim(10), 0);
    lv_obj_set_style_pad_bottom(card, t::dim(6), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    g.time = t::label(card, "0:00", t::font_medium(), t::kText);
    lv_obj_t *track = t::plain(card);
    lv_obj_set_size(track, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(track, t::dim(16), 0);
    lv_obj_set_style_pad_ver(track, t::dim(16), 0);
    g.slider = t::slider(track, 0, kSliderSteps, 0);
    lv_obj_set_width(g.slider, lv_pct(100));
    lv_obj_add_event_cb(g.slider, on_slide, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(g.slider, on_seek, LV_EVENT_RELEASED, nullptr);

    // previous, play/pause, next, stop: centered
    lv_obj_t *controls = t::plain(col);
    lv_obj_set_size(controls, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(controls, t::dim(16), 0);
    lv_obj_set_style_pad_ver(controls, t::dim(6), 0);
    control(controls, LV_SYMBOL_PREV, [](lv_event_t *) { be().player_previous(); }, false);
    g.play_label = lv_obj_get_child(control(controls, LV_SYMBOL_PLAY, [](lv_event_t *) { be().player_toggle(); }, true), 0);
    control(controls, LV_SYMBOL_NEXT, [](lv_event_t *) { be().player_next(); }, false);
    control(controls, LV_SYMBOL_STOP, [](lv_event_t *) { be().player_stop(); }, false);

    lv_obj_t *options = button_row(col);
    lv_obj_set_flex_align(options, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    g.shuffle_label = lv_obj_get_child(
        t::button(options, LV_SYMBOL_SHUFFLE "  Shuffle", [](lv_event_t *) { be().player_set_shuffle(!g.info.shuffle); }),
        0);
    g.repeat_label = lv_obj_get_child(t::button(options, LV_SYMBOL_LOOP "  Repeat",
                                                [](lv_event_t *) {
                                                    // off, all, one, off
                                                    const auto next = static_cast<PlayerRepeat>(
                                                        (static_cast<int>(g.info.repeat) + 1) % 3);
                                                    be().player_set_repeat(next);
                                                }),
                                      0);
    // what plays and its controls first, the spectrum after them
    lv_obj_move_to_index(g.spectrum, static_cast<int32_t>(lv_obj_get_index(options)));
    if (sound::available()) {
        volume_card(col);
    }

    lv_obj_add_event_cb(
        col,
        [](lv_event_t *) {
            if (g.timer != nullptr) {
                lv_timer_del(g.timer);
                g.timer = nullptr;
            }
            lv_img_cache_invalidate_src(&g.cover_dsc);
            g.cover_image = {};
        },
        LV_EVENT_DELETE, nullptr);
    g.timer = lv_timer_create(on_frame, kFrameMs, nullptr);
    // the layout first, so the cover knows its size
    lv_obj_update_layout(col);
    PlayerInfo info{};
    be().player_info(info);
    show_info(info);
    return {"Player", nullptr};
}

} // namespace hc::ui::settings::pages

#endif
