#include "splash.hpp"

#include <cstdint>

#include "dashboard.hpp"
#include "runner.hpp"
#include "settings.hpp"
#include "theme.hpp"

namespace hc::ui::splash {

namespace {

namespace t = theme;

#include "logo.inc"

constexpr uint32_t kCheckMs = 100;
// Home Assistant is connected but the page's values do not come (a page without entities sends
// none): the logo goes after this long anyway
constexpr uint32_t kValuesTimeoutMs = 10000;
// the colors of the logo
constexpr uint32_t kTreeColor = 0xf2f4f9;
constexpr uint32_t kFaceColor = 0x18bcf2;
// between the logo and the line below it
constexpr lv_coord_t kGap = 8;

constexpr runner::Outline kTrack{kLogoTrack, static_cast<uint16_t>(sizeof(kLogoTrack) / sizeof(kLogoTrack[0])),
                                 kLogoTrackStep};

struct State {
    const Backend *backend = nullptr;
    lv_obj_t *root = nullptr;
    // the logo and the room round it for the runner
    lv_obj_t *box = nullptr;
    // "No dashboard yet ...", hidden while there is one
    lv_obj_t *hint = nullptr;
    // "Setup Home Assistant", while no address and token are stored and there is Wi-Fi
    lv_obj_t *setup = nullptr;
    lv_timer_t *timer = nullptr;
    bool connected = false;
    uint32_t connected_at = 0;
};

State g;

// the masks as LVGL images; they have to live as long as the logo is drawn
lv_img_dsc_t g_tree{};
lv_img_dsc_t g_face{};

void set_mask(lv_img_dsc_t &image, const uint8_t *data, uint32_t size)
{
    image.header.cf = LV_IMG_CF_ALPHA_4BIT;
    image.header.w = kLogoSize;
    image.header.h = kLogoSize;
    image.data_size = size;
    image.data = data;
}

// an alpha mask is drawn in the image's recolor
void add_mask(lv_obj_t *parent, const lv_img_dsc_t &image, uint32_t color)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &image);
    lv_obj_set_pos(img, kLogoMargin, kLogoMargin);
    lv_obj_set_style_img_recolor(img, t::color(color), 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
}

// the logo runs while the splash waits, not while it shows the hint or the button
void update_runner()
{
    const bool waiting = lv_obj_has_flag(g.hint, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(g.setup, LV_OBJ_FLAG_HIDDEN);
    runner::set(g.box, waiting, &kTrack);
}

// Without a dashboard nothing more comes from Home Assistant: the runner stops and the line says
// where a dashboard is made.
void show_hint(bool on)
{
    if (on == !lv_obj_has_flag(g.hint, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (on) {
        char address[64] = "";
        g.backend->web_address(address, sizeof(address));
        if (address[0] != '\0') {
            lv_label_set_text_fmt(g.hint, "No dashboard yet.\nCreate one in the web UI at %s", address);
        } else {
            lv_label_set_text(g.hint, "No dashboard yet.\nCreate one in the web UI.");
        }
        lv_obj_clear_flag(g.hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g.hint, LV_OBJ_FLAG_HIDDEN);
    }
    update_runner();
}

// the button "Setup Home Assistant" below the logo, hidden while there is something to wait for
void show_setup(bool on)
{
    if (on == !lv_obj_has_flag(g.setup, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(g.setup, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g.setup, LV_OBJ_FLAG_HIDDEN);
    }
    update_runner();
}

void close()
{
    LV_LOG_USER("start screen done after %lu ms", static_cast<unsigned long>(lv_tick_get()));
    runner::set(g.box, false);
    lv_obj_del(g.root);
    lv_timer_del(g.timer);
    g = {};
}

void on_check(lv_timer_t *)
{
    HassInfo info{};
    g.backend->hass_info(info);
    if (!info.configured) {
        // no address or token (a backup carries none): once there is Wi-Fi the button leads to the
        // page with the QR code of the web UI, where they are entered; until then the logo runs
        WifiInfo wifi{};
        g.backend->wifi_info(wifi);
        g.connected = false;
        show_hint(false);
        show_setup(wifi.connected);
        return;
    }
    show_setup(false);
    if (!info.connected) {
        g.connected = false;
        show_hint(false);
        return;
    }
    if (!g.connected) {
        g.connected = true;
        g.connected_at = lv_tick_get();
    }
    if (g.backend->dashboard == nullptr || g.backend->dashboard() == nullptr) {
        show_hint(true);
        return;
    }
    show_hint(false);
    if (dashboard::loaded() || lv_tick_elaps(g.connected_at) >= kValuesTimeoutMs) {
        close();
    }
}

} // namespace

void init(const Backend &backend, lv_obj_t *screen)
{
    g = {};
    g.backend = &backend;
    set_mask(g_tree, kLogoTree, sizeof(kLogoTree));
    set_mask(g_face, kLogoFace, sizeof(kLogoFace));

    // over the dashboard, which is built below it; it takes the touches meant for the tiles
    g.root = t::plain(screen);
    lv_obj_set_size(g.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(g.root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.root, t::color(t::kBackground), 0);
    lv_obj_add_flag(g.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g.root, LV_OBJ_FLAG_SCROLLABLE);
    // the logo at the center, the line below it; the layout follows when the display turns
    lv_obj_set_flex_flow(g.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g.root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g.root, kGap, 0);

    g.box = t::plain(g.root);
    lv_obj_set_size(g.box, kLogoBox, kLogoBox);
    lv_obj_clear_flag(g.box, LV_OBJ_FLAG_CLICKABLE);
    add_mask(g.box, g_tree, kTreeColor);
    add_mask(g.box, g_face, kFaceColor);

    g.hint = t::label(g.root, "", t::font_normal(), t::kTextLabel);
    lv_obj_set_width(g.hint, LV_PCT(90));
    lv_label_set_long_mode(g.hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g.hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(g.hint, LV_OBJ_FLAG_HIDDEN);

    // the normal Settings with the page on top: `<` leads to quick settings and all the others
    g.setup = t::button(g.root, "Setup Home Assistant",
                        [](lv_event_t *) { settings::open(settings::Page::HomeAssistant); });
    lv_obj_add_flag(g.setup, LV_OBJ_FLAG_HIDDEN);

    runner::set(g.box, true, &kTrack);
    g.timer = lv_timer_create(on_check, kCheckMs, nullptr);
}

} // namespace hc::ui::splash
