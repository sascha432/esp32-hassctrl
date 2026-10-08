#include "dashboard.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

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
// docs/ui/dashboard.md: the margin around the grid and the gap between tiles
constexpr lv_coord_t kMargin = 8;
constexpr lv_coord_t kGap = 4;

struct State {
    const Backend *backend = nullptr;
    lv_obj_t *screen = nullptr;
    void (*leave)() = nullptr;
    lv_obj_t *root = nullptr;
    lv_timer_t *timer = nullptr;
    uint32_t last_layout_check = 0;

    std::shared_ptr<const core::Layout> layout;
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
    g.views.clear();
}

void update_view(tiles::View &view)
{
    tiles::update(view, *g.layout, state(view.entity), pending(view.entity));
}

// The cells of the page in the current orientation: the portrait grid is the transposed one
// (docs/architecture.md section 5, "Orientation").
void build_page()
{
    clear_page();
    const core::Layout &layout = *g.layout;
    const core::Page &page = layout.pages[g.page];
    const lv_coord_t width = lv_obj_get_width(g.screen);
    const lv_coord_t height = lv_obj_get_height(g.screen);
    g.built_width = width;
    g.built_height = height;
    const bool portrait = height > width;
    const uint8_t cols = portrait ? page.grid.rows : page.grid.cols;
    const uint8_t rows = portrait ? page.grid.cols : page.grid.rows;
    const lv_coord_t cell_w = (width - 2 * kMargin - (cols - 1) * kGap) / cols;
    const lv_coord_t cell_h = (height - 2 * kMargin - (rows - 1) * kGap) / rows;
    const auto area_of = [&](core::Cell cell, uint8_t span_cols, uint8_t span_rows) {
        lv_area_t area;
        area.x1 = static_cast<lv_coord_t>(kMargin + cell.col * (cell_w + kGap));
        area.y1 = static_cast<lv_coord_t>(kMargin + cell.row * (cell_h + kGap));
        area.x2 = static_cast<lv_coord_t>(area.x1 + span_cols * cell_w + (span_cols - 1) * kGap - 1);
        area.y2 = static_cast<lv_coord_t>(area.y1 + span_rows * cell_h + (span_rows - 1) * kGap - 1);
        return area;
    };

    g.views.reserve(page.tiles.size() + 1);
    if (page.parent != core::kNone) {
        // the first cell of an area page goes back
        tiles::create(g.views.emplace_back(), g.root, layout, area_of({0, 0}, 1, 1));
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
        tiles::create(view, g.root, layout, area_of(cell, span_cols, span_rows));
    }
    for (auto &view : g.views) {
        update_view(view);
    }
    if (g.panel != core::kNone) {
        // the display turned under an open panel
        panel::build(g.root, g.panel);
    }
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
    g.layout = std::move(layout);
    g.store = std::make_unique<core::EntityStore>(g.layout->entities.size());
    g.pending.clear();
    g.page = 0;
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
        if (current && current != g.layout) {
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

void init(const Backend &backend, lv_obj_t *screen, void (*leave)())
{
    g.backend = &backend;
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

bool shown()
{
    return g.root != nullptr && g.layout != nullptr;
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
    g.page = page;
    g.panel = core::kNone;
    // a tap on a tile opens the page: the tile is deleted after its event, not in it
    lv_async_call(
        [](void *) {
            if (g.layout) {
                build_page();
                subscribe();
            }
        },
        nullptr);
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
    panel::build(g.root, tile);
    // the same page with the panel's fields
    subscribe();
}

core::TileIndex panel_tile()
{
    return g.panel;
}

uint32_t g_image_generation = 1;

uint32_t fetch_image(core::TileIndex tile, uint16_t width, uint16_t height)
{
    if (!g.layout || tile >= g.layout->tiles.size() || g.backend->fetch_image == nullptr) {
        return 0;
    }
    const core::Tile &t = g.layout->tiles[tile];
    const uint32_t generation = g_image_generation++;
    g.backend->fetch_image(g.layout->entity_id(t), t.stream, t.entity, width, height, generation);
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
                panel::destroy();
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
