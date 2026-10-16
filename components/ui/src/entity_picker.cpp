#include "entity_picker.hpp"

#include <cstdint>
#include <cstdio>

#include "dashboard.hpp"
#include "edit_mode.hpp"
#include "keyboard.hpp"
#include "settings.hpp"
#include "settings_pages.hpp"
#include "theme.hpp"

namespace hc::ui::picker {

namespace {

namespace t = theme;
namespace pages = settings::pages;

// the rows of one answer: two labels each, little for the LVGL heap of the ESP32
constexpr size_t kMaxRows = 30;
// a search goes out this long after the last key
constexpr uint32_t kSearchDelayMs = 400;

struct Picker {
    lv_obj_t *root = nullptr;
    lv_obj_t *area = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *list = nullptr;
    lv_obj_t *keyboard = nullptr;
    lv_timer_t *timer = nullptr;
    Picked on_pick = nullptr;
    void (*on_back)() = nullptr;
    // of the request on its way; the answers of older ones are dropped
    uint32_t generation = 0;
    bool adding = false; // a row was tapped, its details are on their way
    memory::Vector<memory::String> ids; // of the rows
};

Picker g;
// counts across pages, so an answer to an earlier page is never taken
uint32_t g_generation = 0;

const Backend &backend()
{
    return dashboard::backend();
}

// one line over the list ("Searching...", a problem), hidden when there is nothing to say
void set_status(const char *text)
{
    if (text == nullptr || text[0] == 0) {
        lv_obj_add_flag(g.status, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(g.status, text);
    lv_obj_clear_flag(g.status, LV_OBJ_FLAG_HIDDEN);
}

void cancel_timer()
{
    if (g.timer != nullptr) {
        lv_timer_del(g.timer);
        g.timer = nullptr;
    }
}

// a tile of the (edited) dashboard shows the entity already
bool on_dashboard(std::string_view entity_id)
{
    const core::Layout &layout = dashboard::layout();
    for (const core::Tile &tile : layout.tiles) {
        if (layout.entity_id(tile) == entity_id) {
            return true;
        }
    }
    return false;
}

void search()
{
    cancel_timer();
    g.adding = false;
    g.generation = ++g_generation;
    set_status("Searching...");
    backend().find_entities(lv_textarea_get_text(g.area), kMaxRows, g.generation);
}

void on_timer(lv_timer_t *)
{
    // a timer with one repeat is deleted after this call
    g.timer = nullptr;
    search();
}

void on_changed(lv_event_t *)
{
    cancel_timer();
    g.timer = lv_timer_create(on_timer, kSearchDelayMs, nullptr);
    lv_timer_set_repeat_count(g.timer, 1);
}

// OK or Enter: the keyboard goes, the list gets its height
void on_ready(lv_event_t *)
{
    lv_obj_add_flag(g.keyboard, LV_OBJ_FLAG_HIDDEN);
    search();
}

void on_area_clicked(lv_event_t *)
{
    lv_obj_clear_flag(g.keyboard, LV_OBJ_FLAG_HIDDEN);
}

// the button beside the field: the keyboard goes or comes (landscape has little room for the list)
void on_keyboard_button(lv_event_t *)
{
    if (lv_obj_has_flag(g.keyboard, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(g.keyboard, LV_OBJ_FLAG_HIDDEN);
        pages::focus_area(g.area);
    } else {
        lv_obj_add_flag(g.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void on_row(lv_event_t *event)
{
    const auto index = static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    if (g.adding || index >= g.ids.size()) {
        return;
    }
    // the search on its way is dropped with its generation
    cancel_timer();
    g.adding = true;
    g.generation = ++g_generation;
    char text[96];
    std::snprintf(text, sizeof(text), "Adding %s...", g.ids[index].c_str());
    set_status(text);
    backend().fetch_entity(g.ids[index], g.generation);
}

void on_left(lv_event_t *)
{
    void (*on_back)() = g.on_back;
    close();
    if (on_back != nullptr) {
        on_back();
    }
}

} // namespace

lv_obj_t *frame(lv_obj_t *parent, const char *title, const char *left, lv_event_cb_t on_left, const char *right,
                lv_event_cb_t on_right)
{
    lv_obj_t *root = t::plain(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, t::color(t::kBackground), 0);
    // the page takes every touch: nothing reaches the edit layer below
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, t::dim(6), 0);

    // the bar as the edit mode's
    lv_obj_t *bar = t::plain(root);
    lv_obj_set_size(bar, LV_PCT(100), edit::kBarHeight);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, t::color(t::kBar), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, t::color(t::kBorder), 0);
    lv_obj_set_style_pad_hor(bar, t::kPad, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *name = t::label(bar, "", t::font_normal(), t::kTextValue);
    lv_obj_set_flex_grow(name, 1);
    t::line_text(name, title);
    lv_obj_t *button = t::button(bar, left, on_left);
    lv_obj_set_height(button, edit::kBarHeight - 8);
    if (right != nullptr) {
        button = t::button(bar, right, on_right);
        lv_obj_set_height(button, edit::kBarHeight - 8);
        lv_obj_set_style_bg_color(button, t::color(t::kActive), 0);
        lv_obj_set_style_border_color(button, t::color(t::kActive), 0);
    }
    return root;
}

void open(lv_obj_t *parent, const char *title, Picked on_pick, void (*on_back)())
{
    close();
    g.on_pick = on_pick;
    g.on_back = on_back;
    g.root = frame(parent, title, on_back != nullptr ? "Back" : "Cancel", on_left, nullptr, nullptr);

    lv_obj_t *line = t::plain(g.root);
    lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(line, t::pad(), 0);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, t::dim(6), 0);
    g.area = pages::text_area(line, "name or entity id", false);
    lv_textarea_set_max_length(g.area, 48);
    lv_obj_t *hide = t::button(line, LV_SYMBOL_KEYBOARD, on_keyboard_button);
    lv_obj_set_style_min_width(hide, t::row_height(), 0);

    g.status = t::label(g.root, "", t::font_normal(), t::kTextLabel);
    lv_obj_set_width(g.status, LV_PCT(100));
    lv_obj_set_style_pad_hor(g.status, t::pad(), 0);
    lv_label_set_long_mode(g.status, LV_LABEL_LONG_WRAP);

    g.list = t::column(g.root);
    lv_obj_set_style_pad_top(g.list, 0, 0);
    lv_obj_set_flex_grow(g.list, 1);

    g.keyboard = keyboard::create(g.root);
    lv_obj_set_size(g.keyboard, LV_PCT(100), LV_PCT(45));
    // OK and Enter search at once and give the list the keyboard's height
    pages::set_form(g.keyboard, g.area, nullptr, on_ready);
    pages::focus_area(g.area);
    lv_obj_add_event_cb(g.area, on_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(g.area, on_area_clicked, LV_EVENT_CLICKED, nullptr);
    // an empty search: the first entities
    search();
}

void close()
{
    cancel_timer();
    if (g.root != nullptr) {
        // called from the events of its own buttons
        lv_obj_del_async(g.root);
        pages::set_form(nullptr, nullptr, nullptr, nullptr);
        // the field's input counts as a Settings form; Settings is not open while editing
        settings::set_form_dirty(false);
    }
    g = {};
}

bool is_open()
{
    return g.root != nullptr;
}

void on_list(core::EntityList list)
{
    if (!is_open() || g.adding || list.generation != g.generation) {
        return;
    }
    lv_obj_clean(g.list);
    g.ids.clear();
    if (!list.error.empty()) {
        char text[128];
        std::snprintf(text, sizeof(text), "No search: %s", list.error.c_str());
        set_status(text);
        return;
    }
    for (size_t i = 0; i < list.rows.size(); i++) {
        const core::EntityRow &row = list.rows[i];
        char value[96];
        std::snprintf(value, sizeof(value), "%s%s", on_dashboard(row.id) ? LV_SYMBOL_OK "  " : "", row.id.c_str());
        t::row(g.list, row.name.empty() ? row.id.c_str() : row.name.c_str(), value, false, on_row,
               reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        g.ids.push_back(std::move(list.rows[i].id));
    }
    set_status(list.rows.empty() ? "Nothing found." : list.more ? "More entities match: type more of the name." : nullptr);
    lv_obj_scroll_to_y(g.list, 0, LV_ANIM_OFF);
}

void on_details(core::EntityDetails details)
{
    if (!is_open() || !g.adding || details.generation != g.generation) {
        return;
    }
    g.adding = false;
    char text[128];
    if (!details.error.empty() || details.missing) {
        std::snprintf(text, sizeof(text), "Not added: %s",
                      details.missing ? "Home Assistant does not have this entity" : details.error.c_str());
        set_status(text);
        return;
    }
    const Picked on_pick = g.on_pick;
    close();
    on_pick(details.id.c_str(), details.dimmer);
}

} // namespace hc::ui::picker
