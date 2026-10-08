#include "picture_panel.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

#include "dashboard.hpp"
#include "lvgl_compat.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::picture_panel {

namespace {

namespace t = theme;

// the frame rate of a stream is shown this often
constexpr uint32_t kRateMs = 1000;
constexpr lv_coord_t kSpinnerSize = 56;

struct State {
    lv_obj_t *image = nullptr;
    lv_obj_t *info = nullptr; // a stream: its frame rate, or why it stopped
    lv_obj_t *spinner = nullptr; // until the first frame of the full screen size arrives
    compat::ImageDsc dsc{};
    core::Pixels pixels;
    core::TileIndex tile = core::kNone;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t generation = 0;
    bool streaming = false;
    lv_timer_t *refresh = nullptr;
    // the frames shown since the rate was shown last
    uint32_t frames = 0;
    uint32_t rate_since = 0;
};

State g;

void request()
{
    g.generation = dashboard::fetch_image(g.tile, g.width, g.height);
}

// shows `pixels` (w x h), enlarged to cover the screen when it is smaller
void show(core::Pixels pixels, uint16_t w, uint16_t h)
{
    if (g.pixels) {
        compat::image_forget(g.dsc);
    }
    g.pixels = std::move(pixels);
    compat::image_dsc(g.dsc, g.pixels.get(), w, h);
    lv_img_set_src(g.image, &g.dsc);
    lv_obj_set_size(g.image, w, h);
    lv_obj_center(g.image);
    const float zoom = std::max(static_cast<float>(g.width) / w, static_cast<float>(g.height) / h);
    // 256 is 1:1
    lv_img_set_zoom(g.image, static_cast<uint16_t>(std::max(256.0f, zoom * 256 + 0.5f)));
    lv_obj_invalidate(g.image);
}

// any tap or swipe goes back: on the release, so a finger that moved a little still counts
void on_release(lv_event_t *)
{
    dashboard::close_panel();
}

void hide_spinner()
{
    if (g.spinner != nullptr) {
        lv_obj_del(g.spinner);
        g.spinner = nullptr;
    }
}

void on_rate(lv_timer_t *)
{
    const uint32_t elapsed = lv_tick_elaps(g.rate_since);
    char text[24];
    std::snprintf(text, sizeof(text), "%.1f fps", g.frames * 1000.0 / std::max<uint32_t>(elapsed, 1));
    lv_label_set_text(g.info, text);
    g.frames = 0;
    g.rate_since = lv_tick_get();
}

} // namespace

void build(lv_obj_t *root, core::TileIndex tile, lv_coord_t width, lv_coord_t height)
{
    forget();
    g.tile = tile;
    g.width = static_cast<uint16_t>(width);
    g.height = static_cast<uint16_t>(height);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_add_event_cb(root, on_release, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(root, on_release, LV_EVENT_PRESS_LOST, nullptr);
    // the panel takes the touches, no gesture of the screen starts on it
    lv_obj_clear_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    g.image = lv_img_create(root);
    lv_obj_clear_flag(g.image, LV_OBJ_FLAG_CLICKABLE);
    // the tile's frame meanwhile, enlarged (blocky for a moment, but not an empty screen)
    if (const tiles::View *view = dashboard::view_of(tile); view != nullptr && view->pixels) {
        show(view->pixels, view->image_width, view->image_height);
    }
    // the first frame takes a while (a stream's server may first start its encoder)
    g.spinner = compat::spinner(root);
    lv_obj_set_size(g.spinner, kSpinnerSize, kSpinnerSize);
    lv_obj_center(g.spinner);
    lv_obj_set_style_arc_color(g.spinner, t::color(t::kActive), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g.spinner, t::color(t::kCard), LV_PART_MAIN);
    lv_obj_clear_flag(g.spinner, LV_OBJ_FLAG_CLICKABLE);
    const core::Tile &config = dashboard::layout().tiles[tile];
    if (!config.stream.empty()) {
        // live: the frames come as fast as the device decodes them; the rate in the corner
        g.streaming = true;
        g.info = t::label(root, "", t::font_normal(), t::kText);
        lv_obj_set_style_bg_opa(g.info, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(g.info, lv_color_black(), 0);
        lv_obj_set_style_pad_hor(g.info, 6, 0);
        lv_obj_set_style_pad_ver(g.info, 2, 0);
        lv_obj_align(g.info, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_label_set_text(g.info, "connecting...");
        g.rate_since = lv_tick_get();
        g.generation = dashboard::play_stream(tile, g.width, g.height);
        g.refresh = lv_timer_create(on_rate, kRateMs, nullptr);
        return;
    }
    request();
    const uint32_t seconds = config.refresh_s > 0 ? config.refresh_s : core::kDefaultRefresh;
    g.refresh = lv_timer_create([](lv_timer_t *) { request(); }, seconds * 1000, nullptr);
}

void on_image(const core::Image &image)
{
    if (g.image == nullptr || image.generation != g.generation) {
        return;
    }
    hide_spinner();
    if (!image.error.empty() || !image.pixels) {
        // the last frame stays; a stream that stopped says why
        LV_LOG_WARN("camera: %s", image.error.c_str());
        if (g.streaming && g.refresh != nullptr) {
            lv_timer_del(g.refresh);
            g.refresh = nullptr;
            lv_label_set_text(g.info, image.error.c_str());
        }
        return;
    }
    g.frames++;
    show(image.pixels, image.width, image.height);
    if (g.info != nullptr) {
        // over the new frame
        lv_obj_move_foreground(g.info);
    }
}

void forget()
{
    if (g.refresh != nullptr) {
        lv_timer_del(g.refresh);
    }
    if (g.streaming) {
        dashboard::stop_stream();
    }
    if (g.pixels) {
        compat::image_forget(g.dsc);
    }
    // the objects go with the panel's root
    g = {};
}

} // namespace hc::ui::picture_panel
