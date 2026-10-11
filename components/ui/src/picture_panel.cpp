#include "picture_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "dashboard.hpp"
#include "lvgl_compat.hpp"
#include "overlay.hpp"
#include "sound.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::picture_panel {

namespace {

namespace t = theme;

// the frame rate of a stream is shown this often
constexpr uint32_t kRateMs = 1000;
constexpr lv_coord_t kSpinnerSize = 56;
// The zoom in percent: 100 % covers the screen. A pinch zooms freely, the buttons go from step to
// step; the smallest zoom shows the whole picture (its width or its height fills the screen), the
// largest is kMaxZoom.
constexpr float kSteps[] = {50, 75, 100, 150, 200};
constexpr float kMaxZoom = 200;
constexpr float kDefaultZoom = 100;
// the buttons hide this long after the last touch
constexpr uint32_t kControlsMs = 5000;
// a finger that moved this far drags the picture
constexpr int32_t kDragStart = 8;
constexpr lv_coord_t kButtonSize = 48;
constexpr lv_coord_t kButtonGap = 12;

struct State {
    lv_obj_t *image = nullptr;
    lv_obj_t *info = nullptr; // a stream: its frame rate, or why it stopped
    lv_obj_t *spinner = nullptr; // until the first frame of the full screen size arrives
    lv_obj_t *controls = nullptr; // the buttons and the hint at the top
    lv_obj_t *zoom_in = nullptr;
    lv_obj_t *zoom_out = nullptr;
    lv_timer_t *hide = nullptr;
    compat::ImageDsc dsc{};
    core::Pixels pixels;
    uint16_t frame_width = 0;
    uint16_t frame_height = 0;
    // the part of the whole picture the frame shows (centered): 1 but for a tile's frame, which
    // is the middle of the picture in the tile's shape
    float part_x = 1;
    float part_y = 1;
    core::TileIndex tile = core::kNone;
    // the screen
    uint16_t width = 0;
    uint16_t height = 0;
    float zoom = kDefaultZoom;
    // the picture's center from the screen's, as a part of the picture's shown size: stays on
    // the same spot when the zoom or the frame's size changes
    float pan_x = 0;
    float pan_y = 0;
    // where the finger came down and where it was last
    lv_point_t pressed_at{};
    lv_point_t last{};
    bool dragging = false;
    // Two fingers zoom by their distance and keep the spot between them under them; LVGL zooms the
    // frame there until a finger lifts, then the frames in that size are asked for. The picture
    // does not move with the finger that stays (until it lifts).
    bool pinching = false;
    bool pinched = false; // since the finger came down
    float pinch_zoom = 0; // the zoom and the fingers' distance when the pinch began
    float pinch_distance = 0;
    // the spot of the picture between the fingers, from its center, as a part of its shown size
    float pinch_spot_x = 0;
    float pinch_spot_y = 0;
    uint32_t generation = 0;
    bool streaming = false;
    lv_timer_t *refresh = nullptr;
    // the frames shown since the rate was shown last
    uint32_t frames = 0;
    uint32_t rate_since = 0;
    memory::String key; // the camera's entity and stream (Kept)
};

State g;

// The zoom and the position of a camera's view, kept while the device runs: the view opens as it
// was left (also after a turn of the display).
struct Kept {
    memory::String key;
    float zoom = kDefaultZoom;
    float pan_x = 0;
    float pan_y = 0;
};

memory::Vector<Kept> g_kept;

Kept *kept(const memory::String &key)
{
    for (Kept &k : g_kept) {
        if (k.key == key) {
            return &k;
        }
    }
    return nullptr;
}

// the size the frames are asked for: the screen at the zoom (100 % covers it)
uint16_t box_width()
{
    return static_cast<uint16_t>(std::lround(g.width * g.zoom / 100));
}

uint16_t box_height()
{
    return static_cast<uint16_t>(std::lround(g.height * g.zoom / 100));
}

// the whole picture's shown width and height (LVGL's zoom of the frame)
float shown_width()
{
    return g.frame_width / g.part_x * lv_img_get_zoom(g.image) / 256.0f;
}

float shown_height()
{
    return g.frame_height / g.part_y * lv_img_get_zoom(g.image) / 256.0f;
}

void request()
{
    g.generation = dashboard::fetch_image(g.tile, box_width(), box_height(), true);
}

// Scales the frame to cover the screen at the zoom and moves it by the pan, which is kept inside
// the picture: no black beside a picture that is larger than the screen. A frame of that size
// already (the P4's PPA scales to it) is shown 1:1: LVGL's zoom of a full screen takes 100 ms.
void place()
{
    if (!g.pixels) {
        return;
    }
    // the whole picture in the frame's pixels covers the box
    const float picture_width = g.frame_width / g.part_x;
    const float picture_height = g.frame_height / g.part_y;
    const float scale = std::max(box_width() / picture_width, box_height() / picture_height);
    // 256 is 1:1; the PPA's scale is rounded up to sixteenths (up to 1/16 larger) and its rounding
    // of the size can leave a pixel or two
    const uint16_t zoom = scale >= 0.93f && scale <= 1.01f
                              ? static_cast<uint16_t>(LV_IMG_ZOOM_NONE)
                              : static_cast<uint16_t>(std::clamp(scale * 256 + 0.5f, 1.0f, 65535.0f));
    // the whole picture as shown: the pan keeps it on the screen
    const float shown_width = picture_width * zoom / 256.0f;
    const float shown_height = picture_height * zoom / 256.0f;
    const float max_x = std::max(0.0f, (shown_width - g.width) / 2);
    const float max_y = std::max(0.0f, (shown_height - g.height) / 2);
    const float x = std::clamp(g.pan_x * shown_width, -max_x, max_x);
    const float y = std::clamp(g.pan_y * shown_height, -max_y, max_y);
    g.pan_x = x / shown_width;
    g.pan_y = y / shown_height;
    lv_img_set_zoom(g.image, zoom);
    // LVGL zooms around the image's center
    lv_obj_set_pos(g.image, static_cast<lv_coord_t>(std::lround(g.width / 2.0f + x - g.frame_width / 2.0f)),
                   static_cast<lv_coord_t>(std::lround(g.height / 2.0f + y - g.frame_height / 2.0f)));
    // the whole screen: what LVGL invalidates of a zoomed image that changes its size, zoom and
    // place at once left parts of the larger one on the black beside a smaller one (zoom out); a
    // frame covers most of the screen anyway
    lv_obj_invalidate(lv_obj_get_parent(g.image));
}

// shows `pixels` (w x h) where the pan has it
void show(core::Pixels pixels, uint16_t w, uint16_t h, uint16_t source_w, uint16_t source_h)
{
    g.part_x = g.part_y = 1;
    if (source_w > 0 && source_h > 0) {
        // the frame's shape against the picture's: a wider frame is cut from the picture's middle
        // rows, a narrower one from its middle columns; a whole frame differs only by rounding
        const float frame_shape = static_cast<float>(w) / h;
        const float picture_shape = static_cast<float>(source_w) / source_h;
        const float part = std::min(frame_shape, picture_shape) / std::max(frame_shape, picture_shape);
        if (part < 0.98f) {
            (frame_shape > picture_shape ? g.part_y : g.part_x) = part;
        }
    }
    if (g.pixels) {
        compat::image_forget(g.dsc);
    }
    g.pixels = std::move(pixels);
    g.frame_width = w;
    g.frame_height = h;
    compat::image_dsc(g.dsc, g.pixels.get(), w, h);
    lv_img_set_src(g.image, &g.dsc);
    lv_obj_set_size(g.image, w, h);
    place();
    lv_obj_invalidate(g.image);
}

// the buttons for 5 s more
void show_controls()
{
    lv_obj_clear_flag(g.controls, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(g.hide);
    lv_timer_resume(g.hide);
}

void on_hide(lv_timer_t *timer)
{
    lv_obj_add_flag(g.controls, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

// the smallest zoom: the whole picture on the screen, its width or its height filling it; none
// until a frame tells the picture's shape
float min_zoom()
{
    if (!g.pixels) {
        return 0;
    }
    const float across = g.width / (g.frame_width / g.part_x);
    const float down = g.height / (g.frame_height / g.part_y);
    return 100 * std::min(across, down) / std::max(across, down);
}

void update_zoom_buttons()
{
    if (g.zoom <= min_zoom() + 1) {
        lv_obj_add_state(g.zoom_out, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(g.zoom_out, LV_STATE_DISABLED);
    }
    if (g.zoom >= kMaxZoom - 1) {
        lv_obj_add_state(g.zoom_in, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(g.zoom_in, LV_STATE_DISABLED);
    }
}

// another zoom (kept between the smallest and the largest): the frame there is zoomed by LVGL at
// once, the frames in the new size are asked for (a stream goes on in it)
void set_zoom(float zoom)
{
    g.zoom = std::clamp(zoom, min_zoom(), kMaxZoom);
    update_zoom_buttons();
    place();
    if (g.streaming) {
        if (g.refresh != nullptr) {
            // a stream that stopped stays stopped
            g.generation = dashboard::play_stream(g.tile, box_width(), box_height());
        }
    } else {
        request();
    }
}

// the buttons: the next step up or down from the zoom (below the smallest step the smallest zoom)
void step_zoom(int step)
{
    float zoom = step > 0 ? kMaxZoom : 0;
    for (float s : kSteps) {
        if (step > 0 && s > g.zoom + 1) {
            zoom = std::min(zoom, s);
        } else if (step < 0 && s < g.zoom - 1) {
            zoom = s;
        }
    }
    set_zoom(zoom);
}

// Follows two fingers on the picture; false when there are not two (any more). The pinch ends
// where the fingers left the zoom, and the frames in that size are asked for.
bool follow_pinch()
{
    Finger fingers[2];
    const auto count_fingers = dashboard::backend().touch_fingers;
    const bool two = g.pixels && count_fingers != nullptr && count_fingers(fingers) == 2;
    if (!two) {
        if (g.pinching) {
            g.pinching = false;
            if (std::fabs(g.zoom - g.pinch_zoom) >= 0.5f) {
                set_zoom(g.zoom);
            }
        }
        return false;
    }
    const float distance = std::max(1.0f, std::hypot(static_cast<float>(fingers[0].x - fingers[1].x),
                                                     static_cast<float>(fingers[0].y - fingers[1].y)));
    const float between_x = (fingers[0].x + fingers[1].x) / 2.0f - g.width / 2.0f;
    const float between_y = (fingers[0].y + fingers[1].y) / 2.0f - g.height / 2.0f;
    if (!g.pinching) {
        g.pinching = g.pinched = true;
        g.pinch_zoom = g.zoom;
        g.pinch_distance = distance;
        g.pinch_spot_x = between_x / shown_width() - g.pan_x;
        g.pinch_spot_y = between_y / shown_height() - g.pan_y;
    }
    g.zoom = std::clamp(g.pinch_zoom * distance / g.pinch_distance, min_zoom(), kMaxZoom);
    place(); // the zoom, for the shown size
    g.pan_x = between_x / shown_width() - g.pinch_spot_x;
    g.pan_y = between_y / shown_height() - g.pinch_spot_y;
    place();
    return true;
}

// a touch on the picture shows the buttons; a finger that moves drags the picture, two zoom it
void on_touch(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED) {
        g.pressed_at = g.last = point;
        g.dragging = g.pinched = false;
        show_controls();
        return;
    }
    if (follow_pinch()) {
        show_controls();
        return;
    }
    if (g.pinched) {
        return;
    }
    // the release brings the last point of a fast swipe
    if (!g.dragging) {
        if (std::abs(point.x - g.pressed_at.x) + std::abs(point.y - g.pressed_at.y) < kDragStart) {
            return;
        }
        g.dragging = true;
    }
    show_controls();
    if (!g.pixels || (point.x == g.last.x && point.y == g.last.y)) {
        return;
    }
    const float zoom = lv_img_get_zoom(g.image) / 256.0f;
    g.pan_x += (point.x - g.last.x) / (g.frame_width / g.part_x * zoom);
    g.pan_y += (point.y - g.last.y) / (g.frame_height / g.part_y * zoom);
    g.last = point;
    place();
}

void on_button(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        show_controls();
        return;
    }
    const int action = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (action == 0) {
        dashboard::close_panel();
        return;
    }
    show_controls();
    step_zoom(action);
}

// a round button with a border: `action` 0 closes, 1 zooms in, -1 out
lv_obj_t *round_button(lv_obj_t *parent, const char *text, int action)
{
    lv_obj_t *button = t::plain(parent);
    const lv_coord_t size = t::scaled(kButtonSize);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(button, lv_color_black(), 0);
    lv_obj_set_style_bg_color(button, t::color(t::kActive), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, t::color(t::kText), 0);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    sound::tappable(button);
    void *user_data = reinterpret_cast<void *>(static_cast<intptr_t>(action));
    lv_obj_add_event_cb(button, on_button, LV_EVENT_PRESSED, user_data);
    lv_obj_add_event_cb(button, on_button, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = t::label(button, text, t::font_large(), t::kText);
    lv_obj_center(label);
    return button;
}

// "X" "+" "-" and the hint, centered at the top below the pull-down zone of Settings (it lies
// above the panel and takes the taps there); the stream's rate keeps the upper left corner
void build_controls(lv_obj_t *root)
{
    g.controls = t::plain(root);
    lv_obj_set_size(g.controls, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g.controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g.controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g.controls, t::kPad, 0);
    lv_obj_align(g.controls, LV_ALIGN_TOP_MID, 0, overlay::kZoneHeight + t::kPad);
    // a touch beside the buttons is one on the picture
    lv_obj_clear_flag(g.controls, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *buttons = t::plain(g.controls);
    lv_obj_set_size(buttons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, t::scaled(kButtonGap), 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_CLICKABLE);
    round_button(buttons, "X", 0);
    g.zoom_in = round_button(buttons, "+", 1);
    g.zoom_out = round_button(buttons, "-", -1);
    lv_obj_t *hint = t::label(g.controls,
                              dashboard::backend().touch_fingers != nullptr ? "swipe to move, pinch to zoom"
                                                                            : "swipe to move",
                              t::font_normal(), t::kText);
    lv_obj_set_style_bg_opa(hint, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(hint, lv_color_black(), 0);
    lv_obj_set_style_radius(hint, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(hint, 10, 0);
    lv_obj_set_style_pad_ver(hint, 4, 0);
    update_zoom_buttons();
    g.hide = lv_timer_create(on_hide, kControlsMs, nullptr);
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
    const core::Tile &config = dashboard::layout().tiles[tile];
    g.key = memory::String(dashboard::layout().entity_id(config));
    g.key += ' ';
    g.key += config.stream;
    if (const Kept *k = kept(g.key); k != nullptr) {
        g.zoom = k->zoom;
        g.pan_x = k->pan_x;
        g.pan_y = k->pan_y;
    }
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_add_event_cb(root, on_touch, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(root, on_touch, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(root, on_touch, LV_EVENT_RELEASED, nullptr);
    // the panel takes the touches: no gesture of the screen and no scroll of the page under it
    // starts on it
    lv_obj_clear_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLL_CHAIN);
    g.image = lv_img_create(root);
    lv_obj_clear_flag(g.image, LV_OBJ_FLAG_CLICKABLE);
    // the tile's frame meanwhile, enlarged (blocky for a moment, but not an empty screen)
    if (const tiles::View *view = dashboard::view_of(tile); view != nullptr && view->pixels) {
        show(view->pixels, view->image_width, view->image_height, view->source_width, view->source_height);
    }
    // the first frame takes a while (a stream's server may first start its encoder)
    g.spinner = compat::spinner(root);
    lv_obj_set_size(g.spinner, kSpinnerSize, kSpinnerSize);
    lv_obj_center(g.spinner);
    lv_obj_set_style_arc_color(g.spinner, t::color(t::kActive), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g.spinner, t::color(t::kCard), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g.spinner, LV_OPA_50, LV_PART_MAIN); // the image shows through the track
    lv_obj_clear_flag(g.spinner, LV_OBJ_FLAG_CLICKABLE);
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
        g.generation = dashboard::play_stream(tile, box_width(), box_height());
        g.refresh = lv_timer_create(on_rate, kRateMs, nullptr);
    } else {
        request();
        const uint32_t seconds = config.refresh_s > 0 ? config.refresh_s : core::kDefaultRefresh;
        g.refresh = lv_timer_create([](lv_timer_t *) { request(); }, seconds * 1000, nullptr);
    }
    // over the picture; shown for a while, as the tap that opened the view would
    build_controls(root);
    show_controls();
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
    show(image.pixels, image.width, image.height, image.source_width, image.source_height);
    // the first frame tells the picture's shape: a kept zoom below the smallest goes up to it
    if (!g.pinching && g.zoom < min_zoom() - 1) {
        set_zoom(min_zoom());
    } else {
        update_zoom_buttons();
    }
}

void forget()
{
    if (!g.key.empty()) {
        Kept *k = kept(g.key);
        if (k == nullptr) {
            k = &g_kept.emplace_back();
            k->key = g.key;
        }
        k->zoom = g.zoom;
        k->pan_x = g.pan_x;
        k->pan_y = g.pan_y;
    }
    if (g.refresh != nullptr) {
        lv_timer_del(g.refresh);
    }
    if (g.hide != nullptr) {
        lv_timer_del(g.hide);
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
