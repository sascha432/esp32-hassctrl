#include "dashboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "edit_mode.hpp"
#include "overlay.hpp"
#include "panel.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::dashboard {

namespace {

namespace t = theme;

// the values of Home Assistant are taken this often
constexpr uint32_t kPollMs = 20;
// a new layout is looked for this often
constexpr uint32_t kLayoutCheckMs = 500;
// docs/ui/lvgl/lvgl-dashboard.md: the margin around the grid (the gap between tiles is Options::tile_gap)
constexpr lv_coord_t kMargin = 8;
// Options::slide_pages: how long a page takes to slide in
constexpr uint32_t kSlideMs = 250;

// where the page that opens comes from
enum class Slide : uint8_t {
    None,
    FromRight, // an area page of the current one
    FromLeft,  // the parent of the current page
};

struct State {
    const Backend *backend = nullptr;
    lv_obj_t *screen = nullptr;
    void (*leave)() = nullptr;
    Options options;
    lv_obj_t *root = nullptr;
    // the tiles of the page, a child of root under the panel
    lv_obj_t *page_obj = nullptr;
    // the page that slides out and its views, deleted when the slide is done
    lv_obj_t *leaving = nullptr;
    memory::Vector<tiles::View> leaving_views;
    lv_timer_t *timer = nullptr;
    uint32_t last_layout_check = 0;

    std::shared_ptr<const core::Layout> layout;
    // while editing: the layout the edit started from (`layout` is the edited copy); null otherwise
    std::shared_ptr<const core::Layout> base;
    bool edited = false;
    Grid grid;
    std::unique_ptr<core::EntityStore> store;
    core::PendingTracker pending;
    core::PageIndex page = 0;
    core::TileIndex panel = core::kNone; // the tile whose panel is open
    uint32_t generation = 0;
    // the large heap (PSRAM): reserved for the whole page before the first view is made, so the
    // views do not move (their LVGL events point at them)
    memory::Vector<tiles::View> views;
    lv_coord_t built_width = 0;
    lv_coord_t built_height = 0;
    // the values of a page came from Home Assistant (the start screen waits for them)
    bool loaded = false;
};

State g;
const core::EntityState kNoState{};

void subscribe()
{
    g.generation++;
    g.backend->subscribe_page(g.layout, g.layout ? g.page : core::kNone, g.panel, g.generation);
}

void clear_page()
{
    // the views' cards go with the root; the views go after them (the delete events use them)
    if (g.root != nullptr) {
        lv_obj_clean(g.root);
    }
    g.page_obj = nullptr;
    g.leaving = nullptr;
    g.views.clear();
    g.leaving_views.clear();
}

// A slide that runs ends now: the page that left goes, the new one stands where it belongs.
void finish_slide()
{
    if (g.leaving == nullptr) {
        return;
    }
    lv_anim_del(g.page_obj, nullptr);
    lv_obj_set_x(g.page_obj, 0);
    lv_obj_del(g.leaving);
    g.leaving = nullptr;
    g.leaving_views.clear();
}

// the new page from `width` (or -width) to 0, the leaving one a screen width beside it
void start_slide(Slide slide, lv_coord_t width)
{
    const lv_coord_t from = slide == Slide::FromRight ? width : static_cast<lv_coord_t>(-width);
    lv_obj_set_x(g.page_obj, from);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, g.page_obj);
    lv_anim_set_values(&anim, from, 0);
    lv_anim_set_time(&anim, kSlideMs);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, [](void *obj, int32_t x) {
        const lv_coord_t width = lv_obj_get_width(static_cast<lv_obj_t *>(obj));
        lv_obj_set_x(static_cast<lv_obj_t *>(obj), static_cast<lv_coord_t>(x));
        if (g.leaving != nullptr) {
            lv_obj_set_x(g.leaving, static_cast<lv_coord_t>(x > 0 ? x - width : x + width));
        }
    });
    lv_anim_set_ready_cb(&anim, [](lv_anim_t *) { finish_slide(); });
    lv_anim_start(&anim);
}

void update_view(tiles::View &view)
{
    tiles::update(view, *g.layout, state(view.entity), pending(view.entity));
}

// The cells of the page in the current orientation: the portrait grid is the transposed one
// (docs/architecture.md section 5, "Orientation"). With a slide the page that is shown stays
// until the new one has slid over it.
void build_page(Slide slide = Slide::None)
{
    const core::Layout &layout = *g.layout;
    const core::Page &page = layout.pages[g.page];
    const lv_coord_t width = lv_obj_get_width(g.screen);
    const lv_coord_t height = lv_obj_get_height(g.screen);
    if (slide != Slide::None && g.page_obj != nullptr && width == g.built_width && height == g.built_height) {
        finish_slide();
        panel::destroy();
        g.leaving = g.page_obj;
        g.leaving_views = std::move(g.views);
        g.views.clear();
        // a tap on the leaving page goes nowhere
        lv_obj_t *cover = t::plain(g.leaving);
        lv_obj_set_size(cover, width, height);
        lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    } else {
        slide = Slide::None;
        clear_page();
    }
    g.page_obj = t::plain(g.root);
    lv_obj_set_size(g.page_obj, width, height);
    g.built_width = width;
    g.built_height = height;
    const bool portrait = height > width;
    // while editing the bar takes the top and the grid shrinks below it
    const lv_coord_t top = editing() ? edit::kBarHeight : 0;
    const uint8_t cols = portrait ? page.grid.rows : page.grid.cols;
    const uint8_t rows = portrait ? page.grid.cols : page.grid.rows;
    const lv_coord_t gap = g.options.tile_gap;
    const lv_coord_t cell_w = (width - 2 * kMargin - (cols - 1) * gap) / cols;
    const lv_coord_t cell_h = (height - top - 2 * kMargin - (rows - 1) * gap) / rows;
    g.grid = {kMargin, static_cast<lv_coord_t>(top + kMargin), cell_w, cell_h, gap, cols, rows,
              portrait ? core::Orientation::Portrait : core::Orientation::Landscape};
    const auto area_of = [&](core::Cell cell, uint8_t span_cols, uint8_t span_rows) {
        lv_area_t area;
        area.x1 = static_cast<lv_coord_t>(kMargin + cell.col * (cell_w + gap));
        area.y1 = static_cast<lv_coord_t>(top + kMargin + cell.row * (cell_h + gap));
        area.x2 = static_cast<lv_coord_t>(area.x1 + span_cols * cell_w + (span_cols - 1) * gap - 1);
        area.y2 = static_cast<lv_coord_t>(area.y1 + span_rows * cell_h + (span_rows - 1) * gap - 1);
        return area;
    };

    g.views.reserve(page.tiles.size() + 1);
    if (page.parent != core::kNone) {
        // the first cell of an area page goes back
        tiles::create(g.views.emplace_back(), g.page_obj, layout, area_of({0, 0}, 1, 1));
    }
    for (const core::TileIndex index : page.tiles) {
        const core::Tile &tile = layout.tiles[index];
        if (tile.kind == core::TileKind::Spacer) {
            continue;
        }
        const core::Cell cell = portrait ? tile.portrait : tile.landscape;
        const uint8_t span_cols = portrait ? tile.span.rows : tile.span.cols;
        const uint8_t span_rows = portrait ? tile.span.cols : tile.span.rows;
        tiles::View &view = g.views.emplace_back();
        view.tile = index;
        tiles::create(view, g.page_obj, layout, area_of(cell, span_cols, span_rows));
    }
    for (auto &view : g.views) {
        update_view(view);
    }
    if (editing()) {
        // the tiles take no touches (the edit layer lies over them), an outline shows that they move
        for (auto &view : g.views) {
            lv_obj_set_style_outline_width(view.card, 2, 0);
            lv_obj_set_style_outline_color(view.card, t::color(view.tile == core::kNone ? t::kBorder : t::kTextLabel), 0);
        }
        edit::refresh();
    } else {
        // a long press anywhere on the page starts edit mode
        lv_obj_add_flag(g.page_obj, LV_OBJ_FLAG_CLICKABLE);
        edit::watch(g.page_obj);
        for (auto &view : g.views) {
            for (lv_obj_t *obj : {view.card, view.plus, view.minus}) {
                if (obj != nullptr) {
                    edit::watch(obj);
                }
            }
        }
    }
    if (g.panel != core::kNone) {
        // the display turned under an open panel
        panel::build(g.root, g.panel);
    }
    if (slide != Slide::None) {
        start_slide(slide, width);
    }
}

// The page of `layout` that shows what `page` of `old` showed: the page of the same area tile (by
// its id), else of the nearest parent that is still there, else the main page. A new layout (a
// save in the web UI, an import, Done) keeps the page that is open.
core::PageIndex same_page(const core::Layout &old, core::PageIndex page, const core::Layout &layout)
{
    for (; page != core::kNone && page < old.pages.size(); page = old.pages[page].parent) {
        const core::TileIndex area = old.pages[page].area;
        if (area == core::kNone || old.tiles[area].id == 0) {
            break;
        }
        for (core::PageIndex p = 1; p < layout.pages.size(); p++) {
            const core::TileIndex other = layout.pages[p].area;
            if (other != core::kNone && layout.tiles[other].id == old.tiles[area].id) {
                return p;
            }
        }
    }
    return 0;
}

void show_layout(std::shared_ptr<const core::Layout> layout)
{
    if (!g.root) {
        if (g.leave != nullptr) {
            g.leave();
            g.leave = nullptr;
        }
        g.root = t::plain(g.screen);
        lv_obj_set_size(g.root, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_opa(g.root, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(g.root, t::color(t::kBackground), 0);
        lv_obj_clear_flag(g.root, LV_OBJ_FLAG_SCROLLABLE);
        // the tiles are drawn under the pull-down zone and the alert strip of the top layer
        lv_obj_move_background(g.root);
    }
    clear_page();
    const core::PageIndex page = g.layout ? same_page(*g.layout, g.page, *layout) : 0;
    g.layout = std::move(layout);
    g.store = std::make_unique<core::EntityStore>(g.layout->entities.size());
    g.pending.clear();
    g.page = page;
    g.panel = core::kNone;
    build_page();
    subscribe();
}

void take_events()
{
    core::HassEvent event;
    while (g.backend->take_event(event)) {
        if (!g.store) {
            continue;
        }
        if (const auto *values = std::get_if<core::PageValues>(&event)) {
            if (values->generation != g.generation) {
                continue; // a page that is not shown any more
            }
            g.loaded = true;
            for (const core::EntityUpdate &update : values->updates) {
                const bool changed = g.store->apply(update);
                const bool resolved = g.pending.resolve(update.entity, g.store->state(update.entity));
                if (changed || resolved) {
                    redraw(update.entity);
                }
            }
        } else if (auto *history = std::get_if<core::History>(&event)) {
            panel::on_history(std::move(*history));
        } else if (const auto *image = std::get_if<core::Image>(&event)) {
            for (auto &view : g.views) {
                if (view.entity == image->entity) {
                    tiles::show_image(view, *image);
                }
            }
            panel::on_image(*image);
        } else {
            const auto &failed = std::get<core::ServiceFailed>(event);
            LV_LOG_WARN("service call failed: %s", failed.message.c_str());
            if (g.pending.fail(failed.entity)) {
                redraw(failed.entity);
            }
        }
    }
    g.pending.expire(lv_tick_get(), [](core::EntityIndex entity) { redraw(entity); });
}

void on_timer(lv_timer_t *)
{
    const uint32_t now = lv_tick_get();
    if (now - g.last_layout_check >= kLayoutCheckMs) {
        g.last_layout_check = now;
        auto current = g.backend->dashboard();
        if (current && current != (g.base ? g.base : g.layout)) {
            if (editing()) {
                // an import or a save in the web UI while editing: the edit would conflict
                stop_editing();
                edit::dropped();
            }
            show_layout(std::move(current));
        }
    }
    if (g.root != nullptr && (lv_obj_get_width(g.screen) != g.built_width || lv_obj_get_height(g.screen) != g.built_height)) {
        // the display turned
        build_page();
    }
    take_events();
}

} // namespace

void init(const Backend &backend, lv_obj_t *screen, void (*leave)(), const Options &options)
{
    g.backend = &backend;
    g.options = options;
    g.screen = screen;
    g.leave = leave;
    if (backend.dashboard == nullptr) {
        return;
    }
    if (auto current = backend.dashboard()) {
        show_layout(std::move(current));
    }
    g.timer = lv_timer_create(on_timer, kPollMs, nullptr);
}

const Backend &backend()
{
    return *g.backend;
}

const Options &options()
{
    return g.options;
}

bool shown()
{
    return g.root != nullptr && g.layout != nullptr;
}

bool loaded()
{
    return g.loaded;
}

const core::Layout &layout()
{
    return *g.layout;
}

const core::EntityState &state(core::EntityIndex entity)
{
    return g.store && entity < g.store->size() ? g.store->state(entity) : kNoState;
}

const core::Expectation *pending(core::EntityIndex entity)
{
    return entity == core::kNone ? nullptr : g.pending.find(entity);
}

void open_page(core::PageIndex page)
{
    if (!g.layout || page >= g.layout->pages.size()) {
        return;
    }
    Slide slide = Slide::None;
    if (g.options.slide_pages && page != g.page) {
        if (g.layout->pages[page].parent == g.page) {
            slide = Slide::FromRight;
        } else if (g.layout->pages[g.page].parent == page) {
            slide = Slide::FromLeft;
        }
    }
    g.page = page;
    g.panel = core::kNone;
    // a tap on a tile opens the page: the tile is deleted after its event, not in it
    lv_async_call(
        [](void *slide) {
            if (g.layout) {
                build_page(static_cast<Slide>(reinterpret_cast<uintptr_t>(slide)));
                subscribe();
            }
        },
        reinterpret_cast<void *>(static_cast<uintptr_t>(slide)));
}

void back()
{
    if (g.layout && g.layout->pages[g.page].parent != core::kNone) {
        open_page(g.layout->pages[g.page].parent);
    }
}

void open_panel(core::TileIndex tile)
{
    if (!g.layout || tile >= g.layout->tiles.size() || !panel::has_panel(g.layout->tiles[tile])) {
        return;
    }
    g.panel = tile;
    panel::build(g.root, tile, true);
    // the same page with the panel's fields
    subscribe();
}

core::TileIndex panel_tile()
{
    return g.panel;
}

uint32_t g_image_generation = 1;

uint32_t fetch_image(core::TileIndex tile, uint16_t width, uint16_t height, bool whole)
{
    if (!g.layout || tile >= g.layout->tiles.size() || g.backend->fetch_image == nullptr) {
        return 0;
    }
    const core::Tile &t = g.layout->tiles[tile];
    const uint32_t generation = g_image_generation++;
    g.backend->fetch_image(g.layout->entity_id(t), t.stream, t.entity, width, height, whole, generation);
    return generation;
}

uint32_t play_stream(core::TileIndex tile, uint16_t width, uint16_t height)
{
    if (!g.layout || tile >= g.layout->tiles.size() || g.backend->play_stream == nullptr) {
        return 0;
    }
    const core::Tile &t = g.layout->tiles[tile];
    const uint32_t generation = g_image_generation++;
    g.backend->play_stream(g.layout->entity_id(t), t.stream, t.entity, width, height, generation);
    return generation;
}

void stop_stream()
{
    if (g.backend->stop_stream != nullptr) {
        g.backend->stop_stream();
    }
}

const tiles::View *view_of(core::TileIndex tile)
{
    for (const auto &view : g.views) {
        if (view.tile == tile && view.card != nullptr) {
            return &view;
        }
    }
    return nullptr;
}

void close_panel()
{
    if (g.panel == core::kNone) {
        return;
    }
    g.panel = core::kNone;
    // the back tile of the panel is pressed: the panel is deleted after its event, not in it
    lv_async_call(
        [](void *) {
            if (g.panel == core::kNone) {
                panel::close();
                // the cameras of the page waited while the panel covered it (and lost their image
                // when the display turned meanwhile)
                for (auto &view : g.views) {
                    tiles::request_image(view);
                }
            }
        },
        nullptr);
    subscribe();
}

void act(const core::Intent &intent, core::Expectation expected)
{
    if (!g.layout || intent.entity >= g.layout->entities.size()) {
        return;
    }
    g.backend->call_service(intent, g.layout->entities[intent.entity]);
    if (expected.says_something()) {
        g.pending.start(intent.entity, std::move(expected), lv_tick_get());
    }
    redraw(intent.entity);
}

void expect(core::EntityIndex entity, core::Expectation expected)
{
    g.pending.start(entity, std::move(expected), lv_tick_get());
    redraw(entity);
}

memory::String tile_name(core::TileIndex tile)
{
    const core::Tile &t = g.layout->tiles[tile];
    if (!t.name.empty()) {
        return t.name;
    }
    const core::EntityState &entity = state(t.entity);
    if (!entity.name.empty()) {
        return entity.name;
    }
    const std::string_view id = g.layout->entity_id(t);
    return memory::String(id.data(), id.size());
}

bool editing()
{
    return g.base != nullptr;
}

void start_editing()
{
    if (!g.layout || editing()) {
        return;
    }
    finish_slide();
    panel::destroy();
    g.panel = core::kNone;
    g.base = g.layout;
    g.layout = std::make_shared<const core::Layout>(*g.base);
    g.edited = false;
    edit::open(g.screen);
    overlay::update(); // no pull-down while editing
    build_page();
    subscribe();
}

void set_edited(core::Layout layout)
{
    if (!editing()) {
        return;
    }
    g.layout = std::make_shared<const core::Layout>(std::move(layout));
    g.edited = true;
    // a removal only takes pages below the shown one: its index stays
    if (g.page >= g.layout->pages.size()) {
        g.page = 0;
    }
    build_page();
    subscribe();
}

void redraw_page()
{
    if (g.layout) {
        build_page();
    }
}

bool save_editing(char *error, size_t size)
{
    if (!editing()) {
        return true;
    }
    if (!g.edited) {
        stop_editing();
        return true;
    }
    if (g.backend->save_dashboard == nullptr ||
        !g.backend->save_dashboard(*g.layout, g.base->revision, error, size)) {
        if (g.backend->save_dashboard == nullptr) {
            std::snprintf(error, size, "this device cannot store the dashboard");
        }
        return false;
    }
    g.base.reset();
    edit::close();
    overlay::update();
    if (auto stored = g.backend->dashboard()) {
        show_layout(std::move(stored));
    }
    return true;
}

void stop_editing()
{
    if (!editing()) {
        return;
    }
    g.layout = std::move(g.base);
    g.base.reset();
    edit::close();
    overlay::update();
    if (g.page >= g.layout->pages.size()) {
        g.page = 0;
    }
    build_page();
    subscribe();
}

core::PageIndex current_page()
{
    return g.page;
}

const memory::Vector<tiles::View> &views()
{
    return g.views;
}

const Grid &grid()
{
    return g.grid;
}

void redraw(core::EntityIndex entity)
{
    if (!g.layout) {
        return;
    }
    for (auto &view : g.views) {
        if (view.entity == entity) {
            update_view(view);
        }
    }
    panel::update(entity);
}

} // namespace hc::ui::dashboard
