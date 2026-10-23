// Settings, the player of a WAV or MP3 file on the SD card (CONFIG_HC_UI_PLAYER,
// docs/ui/lvgl/lvgl-settings.md section 3): a spectrum analyser, the position with seeking,
// Play/Pause, Stop and the volume. It plays from the moment the page opens and stops when the page
// is left; while it plays, Settings do not close by themselves.

#include "settings_pages.hpp"

#if CONFIG_HC_UI_PLAYER

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "overlay.hpp"
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

const Backend &be()
{
    return backend();
}

struct Player {
    lv_obj_t *spectrum = nullptr;
    lv_obj_t *time = nullptr;
    lv_obj_t *slider = nullptr;
    lv_obj_t *play_label = nullptr;
    lv_obj_t *message = nullptr;
    lv_timer_t *timer = nullptr;
    float bars[kBands] = {};
    float peaks[kBands] = {};
    PlayerInfo info{};
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

void start()
{
    std::memset(g.bars, 0, sizeof(g.bars));
    std::memset(g.peaks, 0, sizeof(g.peaks));
    if (!be().player_open(selected_file())) {
        show_message("It does not play: the volume is 0.");
        return;
    }
    show_message("");
}

void on_play(lv_event_t *)
{
    switch (g.info.state) {
    case PlayerState::Playing:
        be().player_pause(true);
        break;
    case PlayerState::Paused:
        be().player_pause(false);
        break;
    case PlayerState::Idle:
    case PlayerState::Failed:
        start();
        break;
    }
}

void on_seek(lv_event_t *event)
{
    if (g.info.duration_ms == 0 || g.info.state == PlayerState::Idle || g.info.state == PlayerState::Failed) {
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
    const PlayerState before = g.info.state;
    g.info = info;
    if (info.state != before) {
        lv_label_set_text(g.play_label, info.state == PlayerState::Playing ? LV_SYMBOL_PAUSE "  Pause"
                                                                            : LV_SYMBOL_PLAY "  Play");
        keep_open(info.state == PlayerState::Playing || info.state == PlayerState::Paused);
        if (info.state == PlayerState::Failed) {
            show_message("This file does not play: not a 16 bit WAV or an MP3, or damaged.");
        }
    }
    if (!lv_obj_has_state(g.slider, LV_STATE_PRESSED)) {
        const uint32_t position = info.state == PlayerState::Idle ? 0 : info.position_ms;
        const int32_t value = info.duration_ms > 0
                                  ? static_cast<int32_t>(std::min<uint64_t>(
                                        uint64_t{position} * kSliderSteps / info.duration_ms, kSliderSteps))
                                  : 0;
        lv_slider_set_value(g.slider, value, LV_ANIM_OFF);
        show_time(position);
    }
}

} // namespace

Built player_page(lv_obj_t *content)
{
    g = Player{};
    lv_obj_t *col = t::column(content);

    g.spectrum = t::plain(col);
    lv_obj_set_size(g.spectrum, lv_pct(100), t::dim(160));
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

    lv_obj_t *buttons = button_row(col);
    lv_obj_t *play = t::button(buttons, LV_SYMBOL_PLAY "  Play", on_play);
    g.play_label = lv_obj_get_child(play, 0);
    t::button(buttons, LV_SYMBOL_STOP "  Stop", [](lv_event_t *) { be().player_stop(); });
    if (sound::available()) {
        volume_card(col);
    }

    // the page goes: the sound stops with it
    lv_obj_add_event_cb(
        col,
        [](lv_event_t *) {
            if (g.timer != nullptr) {
                lv_timer_del(g.timer);
                g.timer = nullptr;
            }
            be().player_stop();
        },
        LV_EVENT_DELETE, nullptr);
    g.timer = lv_timer_create(on_frame, kFrameMs, nullptr);
    // Idle before it starts: the first frame sets the button's text
    g.info.state = PlayerState::Idle;
    start();
    const char *path = selected_file();
    const char *slash = std::strrchr(path, '/');
    return {slash != nullptr ? slash + 1 : path, nullptr};
}

} // namespace hc::ui::settings::pages

#endif
