#include "settings.hpp"

#include <cstdio>
#include <cstring>

#include "settings_pages.hpp"
#include "sound.hpp"
#include "theme.hpp"

namespace hc::ui::settings {

namespace {

namespace t = theme;

constexpr uint32_t kTimeoutMs = 5 * 60 * 1000;
constexpr uint32_t kAnimationMs = 200;
constexpr int kMaxDepth = 6;
constexpr size_t kTextSize = 40;

struct Entry {
    Page page;
    char text[kTextSize];
    int index;
};

struct State {
    lv_disp_t *disp = nullptr;
    const Backend *backend = nullptr;
    lv_obj_t *dashboard = nullptr;
    // open_alone(): closing goes here instead of the dashboard
    lv_obj_t *return_to = nullptr;
    lv_obj_t *screen = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *wifi_icon = nullptr;
    lv_obj_t *hotspot_icon = nullptr;
    lv_obj_t *ha_icon = nullptr;
    lv_obj_t *clock = nullptr;
    lv_obj_t *content = nullptr;
    lv_timer_t *timer = nullptr;
    pages::Refresh refresh = nullptr;
    Entry stack[kMaxDepth];
    int depth = 0;
    bool form_dirty = false;
};

State g;

void update_bar()
{
    WifiInfo wifi{};
    g.backend->wifi_info(wifi);
    lv_obj_set_style_text_color(g.wifi_icon,
                                t::color(wifi.connected ? t::kText : (wifi.error[0] != 0 ? t::kAlert : t::kTextMuted)), 0);
    HassInfo hass{};
    g.backend->hass_info(hass);
    lv_obj_set_style_text_color(g.ha_icon,
                                t::color(hass.connected ? t::kText : (hass.failed ? t::kAlert : t::kTextMuted)), 0);
    if (wifi.hotspot_on) {
        lv_obj_clear_flag(g.hotspot_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g.hotspot_icon, LV_OBJ_FLAG_HIDDEN);
    }
    char time[8];
    lv_label_set_text(g.clock, g.backend->time_text(time, sizeof(time)) ? time : "--:--");
}

void on_tick(lv_timer_t *)
{
    if (!g.form_dirty && lv_disp_get_inactive_time(g.disp) > kTimeoutMs) {
        close();
        return;
    }
    update_bar();
    if (g.refresh != nullptr) {
        g.refresh();
    }
}

// `<` leaves a page; with unsaved input it asks first
void leave()
{
    if (!g.form_dirty) {
        pop();
        return;
    }
    confirm("Discard your input?", "What you typed on this page is lost.", "Discard", true,
            [](void *) {
                g.form_dirty = false;
                pop();
            },
            nullptr);
}

void show_top()
{
    const Entry &top = g.stack[g.depth - 1];
    lv_obj_clean(g.content);
    lv_obj_scroll_to_y(g.content, 0, LV_ANIM_OFF);
    g.form_dirty = false;
    const pages::Built built = pages::build(top.page, g.content, top.text, top.index);
    g.refresh = built.refresh;
    t::line_text(g.title, built.title);
}

// Page changes come from events of objects on the page, which the change deletes: it runs after
// the event, in the next LVGL cycle.
bool g_show_pending = false;

void show_async(void *)
{
    g_show_pending = false;
    if (g.screen != nullptr && g.depth > 0) {
        show_top();
    }
}

void schedule_show()
{
    if (!g_show_pending) {
        g_show_pending = true;
        lv_async_call(show_async, nullptr);
    }
}

void on_back(lv_event_t *)
{
    leave();
}

lv_obj_t *build_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = t::plain(parent);
    lv_obj_set_size(bar, lv_pct(100), t::status_bar_height());
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, t::color(t::kBar), 0);
    lv_obj_set_style_pad_hor(bar, t::pad(), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, t::dim(10), 0);

    // `<` and the title are one touch target, as wide as the title
    lv_obj_t *back = t::plain(bar);
    lv_obj_set_height(back, lv_pct(100));
    lv_obj_set_flex_grow(back, 1);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(back, t::dim(8), 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    sound::tappable(back);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, nullptr);
    t::label(back, LV_SYMBOL_LEFT, t::font_normal(), t::kText);
    g.title = t::label(back, "", t::font_normal(), t::kText);
    lv_obj_set_flex_grow(g.title, 1);
    t::line_text(g.title, "Settings");

    g.hotspot_icon = t::label(bar, "Hotspot", t::font_small(), t::kActive);
    g.wifi_icon = t::label(bar, LV_SYMBOL_WIFI, t::font_normal(), t::kTextMuted);
    g.ha_icon = t::label(bar, "HA", t::font_normal(), t::kTextMuted);
    g.clock = t::label(bar, "--:--", t::font_normal(), t::kText);
    return bar;
}

void build_screen()
{
    g.screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(g.screen);
    lv_obj_set_style_bg_opa(g.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g.screen, t::color(t::kBackground), 0);
    lv_obj_set_style_text_color(g.screen, t::color(t::kText), 0);
    lv_obj_set_flex_flow(g.screen, LV_FLEX_FLOW_COLUMN);

    build_bar(g.screen);
    g.content = t::plain(g.screen);
    lv_obj_set_width(g.content, lv_pct(100));
    lv_obj_set_flex_grow(g.content, 1);
}

// overlay dialog of confirm()
struct Dialog {
    void (*on_confirm)(void *) = nullptr;
    void *context = nullptr;
    lv_obj_t *backdrop = nullptr;
    // popup(): closes it after this long without popup_touch()
    lv_timer_t *timeout = nullptr;
    void (*on_close)() = nullptr;
};
Dialog g_dialog;

void close_dialog()
{
    if (g_dialog.timeout != nullptr) {
        lv_timer_del(g_dialog.timeout);
        g_dialog.timeout = nullptr;
    }
    if (g_dialog.backdrop != nullptr) {
        // called from the events of the dialog's own buttons
        lv_obj_del_async(g_dialog.backdrop);
        g_dialog.backdrop = nullptr;
    }
    void (*on_close)() = g_dialog.on_close;
    g_dialog.on_close = nullptr;
    if (on_close != nullptr) {
        on_close();
    }
}

// the side of a popup: fits the smallest display (320 px high) with room around it
constexpr lv_coord_t kPopupSize = 200;

// the dark layer under a dialog: nothing below it takes touches
lv_obj_t *backdrop()
{
    lv_obj_t *obj = t::plain(lv_layer_top());
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(obj, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t *dialog_card(lv_obj_t *parent)
{
    lv_obj_t *card = t::plain(parent);
    lv_obj_set_width(card, lv_pct(85));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(card, t::dim(420), 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, t::color(t::kCard), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, t::color(t::kBorder), 0);
    lv_obj_set_style_radius(card, t::radius(), 0);
    lv_obj_set_style_pad_all(card, t::dim(16), 0);
    lv_obj_set_style_pad_row(card, t::dim(10), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    // taps on the card do not reach the backdrop, which closes a popup
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

void on_dialog_cancel(lv_event_t *)
{
    close_dialog();
}

void on_dialog_confirm(lv_event_t *)
{
    const Dialog dialog = g_dialog;
    close_dialog();
    if (dialog.on_confirm != nullptr) {
        dialog.on_confirm(dialog.context);
    }
}

} // namespace

void init(lv_disp_t *disp, const Backend &backend, lv_obj_t *dashboard)
{
    g = {};
    g.disp = disp;
    g.backend = &backend;
    g.dashboard = dashboard;
}

void open(Page page)
{
    if (g.screen == nullptr) {
        t::set_settings_mode(true);
        build_screen();
        g.timer = lv_timer_create(on_tick, 1000, nullptr);
        lv_scr_load_anim(g.screen, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, kAnimationMs, 0, false);
    }
    g.return_to = nullptr;
    g.depth = 0;
    push(Page::Quick);
    if (page != Page::Quick) {
        push(page);
    }
    update_bar();
}

void open_alone(Page page, lv_obj_t *return_to, const char *text)
{
    if (g.screen == nullptr) {
        t::set_settings_mode(true);
        build_screen();
        g.timer = lv_timer_create(on_tick, 1000, nullptr);
        lv_scr_load_anim(g.screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, kAnimationMs, 0, false);
    }
    g.return_to = return_to;
    g.depth = 0;
    push(page, text);
    update_bar();
}

void close()
{
    if (g.screen == nullptr) {
        return;
    }
    close_dialog();
    t::set_settings_mode(false);
    lv_timer_del(g.timer);
    g.timer = nullptr;
    g.refresh = nullptr;
    // the Settings screen is deleted when the animation is done
    if (g.return_to != nullptr) {
        lv_scr_load_anim(g.return_to, LV_SCR_LOAD_ANIM_MOVE_RIGHT, kAnimationMs, 0, true);
        g.return_to = nullptr;
    } else {
        lv_scr_load_anim(g.dashboard, LV_SCR_LOAD_ANIM_MOVE_TOP, kAnimationMs, 0, true);
    }
    g.screen = nullptr;
    g.content = nullptr;
    g.depth = 0;
}

bool is_open()
{
    return g.screen != nullptr;
}

void push(Page page, const char *text, int index)
{
    if (g.depth >= kMaxDepth) {
        return;
    }
    Entry &entry = g.stack[g.depth++];
    entry.page = page;
    std::snprintf(entry.text, sizeof(entry.text), "%s", text != nullptr ? text : "");
    entry.index = index;
    schedule_show();
}

void pop(int pages)
{
    if (g.depth - pages <= 0) {
        close();
        return;
    }
    g.depth -= pages;
    schedule_show();
}

void reload()
{
    if (g.depth > 0) {
        schedule_show();
    }
}

void set_form_dirty(bool dirty)
{
    g.form_dirty = dirty;
}

const Backend &backend()
{
    return *g.backend;
}

void confirm(const char *title, const char *text, const char *action, bool danger, void (*on_confirm)(void *),
             void *context)
{
    close_dialog();
    g_dialog.on_confirm = on_confirm;
    g_dialog.context = context;

    g_dialog.backdrop = backdrop();
    lv_obj_t *card = dialog_card(g_dialog.backdrop);

    t::label(card, title, t::font_large(), t::kText);
    lv_obj_t *body = t::label(card, text, t::font_normal(), t::kTextValue);
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);

    lv_obj_t *buttons = t::plain(card);
    lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(buttons, t::dim(6), 0);
    t::button(buttons, "Cancel", on_dialog_cancel);
    t::button(buttons, action, on_dialog_confirm, nullptr, danger);
}

lv_obj_t *popup(const char *title, uint32_t timeout_ms, void (*on_close)())
{
    close_dialog();
    g_dialog.backdrop = backdrop();
    lv_obj_add_event_cb(g_dialog.backdrop, [](lv_event_t *) { close_dialog(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *card = dialog_card(g_dialog.backdrop);
    // A square of a fixed size, centered, with its content placed by hand (no flow): when the
    // display turns, the card and everything in it stay where they are.
    lv_obj_set_style_max_width(card, LV_COORD_MAX, 0);
    lv_obj_set_size(card, t::dim(kPopupSize), t::dim(kPopupSize));
    lv_obj_set_layout(card, 0);
    lv_obj_t *heading = t::label(card, title, t::font_large(), t::kText);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 0);
    g_dialog.on_close = on_close;
    g_dialog.timeout = lv_timer_create([](lv_timer_t *) { close_dialog(); }, timeout_ms, nullptr);
    return card;
}

void popup_touch()
{
    if (g_dialog.timeout != nullptr) {
        lv_timer_reset(g_dialog.timeout);
    }
}

void close_popup()
{
    close_dialog();
}

} // namespace hc::ui::settings
