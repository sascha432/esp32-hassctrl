#include "add_dialog.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "dashboard.hpp"
#include "entity_picker.hpp"
#include "keyboard.hpp"
#include "settings.hpp"
#include "settings_pages.hpp"
#include "theme.hpp"

namespace hc::ui::add {

namespace {

namespace t = theme;
namespace pages = settings::pages;

enum class Type : uint8_t { Tile, Area, Clock, Media };

constexpr const char *kTypes[] = {"Tile", "Area", "Clock", "Media"};
constexpr core::Span kSizes[] = {{1, 1}, {2, 1}, {1, 2}, {2, 2}};
constexpr int8_t kDecimals[] = {-1, 0, 1, 2};
// the choices of a step: a size or decimals beyond the usual ones (set in the web editor) is one more
constexpr size_t kMaxChoices = 5;

// what was chosen so far; kept while the steps go back and forth
struct State {
    lv_obj_t *parent = nullptr;
    core::PageIndex page = 0;
    core::Orientation o = core::Orientation::Landscape;
    core::Cell cell{0, 0};
    // the tile that is changed; kNone: a new one at `cell`
    core::TileIndex tile = core::kNone;
    lv_obj_t *step = nullptr; // the page of the step shown (not the search's)
    lv_obj_t *area = nullptr;
    lv_obj_t *keyboard = nullptr;
    lv_obj_t *status = nullptr;

    char title[core::kMaxNameLength + 1] = {};
    Type type = Type::Tile;
    memory::String entity; // the tile's
    core::Span size{1, 1};
    core::Span sizes[kMaxChoices] = {}; // offered, in the order of the buttons
    bool dimmer = false;
    int8_t decimals = -1;
    int8_t decimal_choices[kMaxChoices] = {};
    bool analog = false;
    bool hours12 = false;
    bool seconds = false;
    bool date = false;
    bool swipe = false;
    memory::String folder; // the media tile's; the web editor sets it, kept here
};

State g;

void step_title();
void step_options();

bool changing()
{
    return g.tile != core::kNone;
}

void close_step()
{
    if (g.step != nullptr) {
        // called from the events of its own buttons
        lv_obj_del_async(g.step);
        g.step = nullptr;
        g.area = nullptr;
        g.keyboard = nullptr;
        g.status = nullptr;
        pages::set_form(nullptr, nullptr, nullptr, nullptr);
        settings::set_form_dirty(false);
    }
}

// one line under the bar: what is missing or what went wrong
void set_status(const char *text)
{
    lv_label_set_text(g.status, text);
    lv_obj_clear_flag(g.status, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *status_label(lv_obj_t *parent)
{
    lv_obj_t *label = t::label(parent, "", t::font_normal(), t::kAlert);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_pad_hor(label, t::pad(), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return label;
}

// A row of buttons of which one is chosen (shown in the action color). `on_click` gets the
// button's index as user data and calls choose() for it.
lv_obj_t *choices(lv_obj_t *parent, const char *const *names, size_t count, size_t chosen, lv_event_cb_t on_click)
{
    lv_obj_t *row = t::plain(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, t::dim(6), 0);
    for (size_t i = 0; i < count; i++) {
        lv_obj_t *button = t::button(row, names[i], on_click, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
        lv_obj_set_style_min_width(button, 0, 0);
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_style_bg_color(button, t::color(t::kActive), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(button, t::color(t::kActive), LV_STATE_CHECKED);
        if (i == chosen) {
            lv_obj_add_state(button, LV_STATE_CHECKED);
        }
    }
    return row;
}

// the clicked button of a choices() row becomes the chosen one; returns its index
size_t choose(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_current_target(event);
    lv_obj_t *row = lv_obj_get_parent(button);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(row); i++) {
        lv_obj_clear_state(lv_obj_get_child(row, static_cast<int32_t>(i)), LV_STATE_CHECKED);
    }
    lv_obj_add_state(button, LV_STATE_CHECKED);
    return static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

// a row with a switch; the switch sets `*flag`
void switch_row(lv_obj_t *parent, const char *text, bool *flag)
{
    lv_obj_t *row = t::row(parent, text, nullptr, false, nullptr);
    t::toggle(row, *flag,
              [](lv_event_t *event) {
                  *static_cast<bool *>(lv_event_get_user_data(event)) =
                      lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
              },
              flag);
}

bool connected()
{
    HassInfo info{};
    dashboard::backend().hass_info(info);
    return info.connected;
}

// what the options show for the kind of tile
core::TileKind kind()
{
    switch (g.type) {
    case Type::Area:
        return core::TileKind::Area;
    case Type::Clock:
        return core::TileKind::Clock;
    case Type::Media:
        return core::TileKind::Media;
    default:
        return core::kind_of_entity(g.entity);
    }
}

// ------------------------------------------------------------------------- adding, changing

// Puts the chosen tile into the edited copy, or the changes of the tile; `first` is the first
// tile of a new area. False with the problem when it does not go.
bool put(const char *first, bool first_dimmer, char *problem, size_t size)
{
    core::NewTile added;
    core::Tile &tile = added.tile;
    tile.name = g.title;
    tile.in(g.o).size = g.size;
    tile.in(g.o).has_size = !(g.size == core::Span{1, 1});
    // only the options of the kind: a type changed in step 1 leaves the others behind
    tile.dimmer = kind() == core::TileKind::Light && g.dimmer;
    tile.decimals = kind() == core::TileKind::Sensor ? g.decimals : -1;
    tile.swipe = g.type == Type::Area && g.swipe;
    if (g.type == Type::Clock) {
        tile.style = g.analog ? core::ClockStyle::Analog : core::ClockStyle::Digital;
        tile.hours12 = g.hours12;
        tile.seconds = g.seconds;
        tile.date = g.date;
    }
    if (g.type == Type::Media) {
        tile.folder = g.folder;
    }
    added.entity = g.entity;
    if (changing()) {
        auto result = core::change_tile(dashboard::layout(), g.tile, added, g.o);
        if (!result) {
            std::snprintf(problem, size, "Not changed: %s", result.error().message.c_str());
            return false;
        }
        dashboard::set_edited(std::move(*result));
        return true;
    }
    tile.kind = kind();
    if (g.type == Type::Area) {
        added.first_entity = first != nullptr ? first : "";
        added.first.dimmer = first_dimmer;
    }
    auto result = core::add_tile(dashboard::layout(), g.page, std::move(added), g.o, g.cell);
    if (!result) {
        std::snprintf(problem, size, "Not added: %s", result.error().message.c_str());
        return false;
    }
    dashboard::set_edited(std::move(*result));
    return true;
}

// ------------------------------------------------------------------------------- 4. first tile

void on_first(const char *entity_id, bool dimmer)
{
    char problem[160];
    if (put(entity_id, dimmer, problem, sizeof(problem))) {
        close();
        return;
    }
    // back to the options, with the reason
    step_options();
    set_status(problem);
}

// ---------------------------------------------------------------------------------- 3. options

void on_entity(const char *entity_id, bool dimmer)
{
    g.entity = entity_id;
    g.dimmer = dimmer;
    step_options();
}

// a tile's entity: the search, the one picked comes back to the options
void open_search(void (*on_back)())
{
    close_step();
    char title[80];
    std::snprintf(title, sizeof(title), "Entity of %s", g.title[0] != 0 ? g.title : "the tile");
    picker::open(g.parent, title, on_entity, on_back);
}

void on_options_back(lv_event_t *)
{
    if (g.type == Type::Tile && !changing()) {
        open_search(step_title);
    } else {
        step_title();
    }
}

void on_options_done(lv_event_t *)
{
    if (g.type == Type::Area && !changing()) {
        if (!connected()) {
            set_status("Home Assistant is not connected: no entity search for the first tile.");
            return;
        }
        close_step();
        char title[80];
        std::snprintf(title, sizeof(title), "First tile of %s", g.title);
        picker::open(g.parent, title, on_first, step_options);
        return;
    }
    char problem[160];
    if (put(nullptr, false, problem, sizeof(problem))) {
        close();
    } else {
        set_status(problem);
    }
}

void on_entity_row(lv_event_t *)
{
    if (!connected()) {
        set_status("Home Assistant is not connected: no entity search.");
        return;
    }
    open_search(step_options);
}

void step_options()
{
    close_step();
    char title[80];
    std::snprintf(title, sizeof(title), "Options of %s",
                  g.title[0] != 0          ? g.title
                  : g.type == Type::Clock ? "the clock"
                  : g.type == Type::Media ? "the media tile"
                                          : g.entity.c_str());
    g.step = picker::frame(g.parent, title, "Back", on_options_back,
                           changing() ? "Save" : g.type == Type::Area ? "Next" : "Add", on_options_done);
    g.status = status_label(g.step);
    lv_obj_t *list = t::column(g.step);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_flex_grow(list, 1);

    if (changing() && g.type == Type::Tile) {
        pages::section(list, "Entity");
        t::row(list, g.entity.c_str(), nullptr, true, on_entity_row);
    }

    // the sizes that fit the page's grid in this orientation, and the tile's own
    const dashboard::Grid &grid = dashboard::grid();
    static char size_names[kMaxChoices][8];
    const char *names[kMaxChoices];
    size_t count = 0;
    size_t chosen = 0;
    bool listed = false;
    for (const core::Span size : kSizes) {
        if (size.cols <= grid.cols && size.rows <= grid.rows) {
            listed = listed || size == g.size;
            g.sizes[count++] = size;
        }
    }
    if (!listed) {
        g.sizes[count++] = g.size;
    }
    for (size_t i = 0; i < count; i++) {
        std::snprintf(size_names[i], sizeof(size_names[i]), "%ux%u", static_cast<unsigned>(g.sizes[i].cols),
                      static_cast<unsigned>(g.sizes[i].rows));
        names[i] = size_names[i];
        chosen = g.sizes[i] == g.size ? i : chosen;
    }
    pages::section(list, g.o == core::Orientation::Landscape ? "Size in landscape" : "Size in portrait");
    choices(list, names, count, chosen, [](lv_event_t *event) { g.size = g.sizes[choose(event)]; });

    switch (kind()) {
    case core::TileKind::Light:
        pages::section(list, "Light");
        switch_row(list, "Dimmer", &g.dimmer);
        break;
    case core::TileKind::Sensor: {
        static char decimal_names[kMaxChoices][8];
        count = 0;
        chosen = 0;
        listed = false;
        for (const int8_t decimals : kDecimals) {
            listed = listed || decimals == g.decimals;
            g.decimal_choices[count++] = decimals;
        }
        if (!listed) {
            g.decimal_choices[count++] = g.decimals;
        }
        for (size_t i = 0; i < count; i++) {
            if (g.decimal_choices[i] < 0) {
                std::snprintf(decimal_names[i], sizeof(decimal_names[i]), "Auto");
            } else {
                std::snprintf(decimal_names[i], sizeof(decimal_names[i]), "%d", g.decimal_choices[i]);
            }
            names[i] = decimal_names[i];
            chosen = g.decimal_choices[i] == g.decimals ? i : chosen;
        }
        pages::section(list, "Decimals");
        choices(list, names, count, chosen,
                [](lv_event_t *event) { g.decimals = g.decimal_choices[choose(event)]; });
        break;
    }
    case core::TileKind::Clock:
        pages::section(list, "Clock");
        switch_row(list, "Analog", &g.analog);
        switch_row(list, "12 hours", &g.hours12);
        switch_row(list, "Seconds", &g.seconds);
        switch_row(list, "Date", &g.date);
        break;
    case core::TileKind::Area:
        pages::section(list, "Area");
        switch_row(list, "Swipe navigation", &g.swipe);
        break;
    case core::TileKind::Media: {
        // the folder is picked in the web editor, which browses both
        pages::section(list, "Media");
        t::caption_value(list, "Folder (web editor)",
                         g.folder.empty() ? "Home Assistant and SD card" : g.folder.c_str());
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------------------- 1. title, type

void on_next(lv_event_t *)
{
    std::snprintf(g.title, sizeof(g.title), "%s", lv_textarea_get_text(g.area));
    if (g.type == Type::Area && g.title[0] == 0) {
        set_status("An area needs a title.");
        return;
    }
    if (g.type == Type::Tile && !changing()) {
        if (!connected()) {
            set_status("Home Assistant is not connected: no entity search.");
            return;
        }
        open_search(step_title);
        return;
    }
    step_options();
}

void on_cancel(lv_event_t *)
{
    close();
}

void on_keyboard_button(lv_event_t *)
{
    if (lv_obj_has_flag(g.keyboard, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(g.keyboard, LV_OBJ_FLAG_HIDDEN);
        pages::focus_area(g.area);
    } else {
        lv_obj_add_flag(g.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void step_title()
{
    close_step();
    char title[80];
    if (changing()) {
        std::snprintf(title, sizeof(title), "Change %s", dashboard::tile_name(g.tile).c_str());
    }
    g.step = picker::frame(g.parent, changing() ? title : "New tile", "Cancel", on_cancel, "Next", on_next);
    lv_obj_t *body = t::plain(g.step);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(body, t::pad(), 0);
    lv_obj_set_style_pad_row(body, t::dim(6), 0);
    // the type of a tile that is there stays: an entity's domain decides its kind
    if (!changing()) {
        choices(body, kTypes, 4, static_cast<size_t>(g.type),
                [](lv_event_t *event) { g.type = static_cast<Type>(choose(event)); });
    }

    lv_obj_t *line = t::plain(body);
    lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, t::dim(6), 0);
    // a tile that is there shows the name it has without a title of its own
    g.area = pages::text_area(line, changing() ? dashboard::tile_name(g.tile).c_str() : "title", false);
    lv_textarea_set_max_length(g.area, core::kMaxNameLength);
    lv_textarea_set_text(g.area, g.title);
    lv_obj_t *hide = t::button(line, LV_SYMBOL_KEYBOARD, on_keyboard_button);
    lv_obj_set_style_min_width(hide, t::row_height(), 0);
    lv_obj_add_event_cb(g.area, [](lv_event_t *) { lv_obj_clear_flag(g.keyboard, LV_OBJ_FLAG_HIDDEN); },
                        LV_EVENT_CLICKED, nullptr);

    g.status = status_label(g.step);
    // the keyboard at the bottom, the free room above it
    lv_obj_t *room = t::plain(g.step);
    lv_obj_set_width(room, LV_PCT(100));
    lv_obj_set_flex_grow(room, 1);
    g.keyboard = keyboard::create(g.step);
    lv_obj_set_size(g.keyboard, LV_PCT(100), LV_PCT(45));
    // OK and Enter: Next
    pages::set_form(g.keyboard, g.area, nullptr, on_next);
    pages::focus_area(g.area);
}

} // namespace

void open(lv_obj_t *parent, core::PageIndex page, core::Orientation o, core::Cell cell)
{
    close();
    g.parent = parent;
    g.page = page;
    g.o = o;
    g.cell = cell;
    step_title();
}

void change(lv_obj_t *parent, core::TileIndex index, core::Orientation o)
{
    close();
    const core::Layout &layout = dashboard::layout();
    const core::Tile &tile = layout.tiles[index];
    g.parent = parent;
    g.page = tile.page;
    g.o = o;
    g.tile = index;
    g.type = tile.kind == core::TileKind::Area    ? Type::Area
             : tile.kind == core::TileKind::Clock ? Type::Clock
             : tile.kind == core::TileKind::Media ? Type::Media
                                                  : Type::Tile;
    std::snprintf(g.title, sizeof(g.title), "%s", tile.name.c_str());
    g.entity = memory::String(layout.entity_id(tile));
    g.size = tile.in(o).size;
    g.dimmer = tile.dimmer;
    g.decimals = tile.decimals;
    g.analog = tile.style == core::ClockStyle::Analog;
    g.hours12 = tile.hours12;
    g.seconds = tile.seconds;
    g.date = tile.date;
    g.swipe = tile.swipe;
    g.folder = tile.folder;
    step_title();
}

void close()
{
    picker::close();
    close_step();
    g = {};
}

} // namespace hc::ui::add
