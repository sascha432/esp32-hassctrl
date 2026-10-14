#include "hc/core/layout.hpp"

#include <cstdarg>
#include <cstdio>

#include "hc/core/icons.hpp"

namespace hc::core {

namespace {

memory::String format(const char *pattern, ...)
{
    char text[160];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(text, sizeof(text), pattern, args);
    va_end(args);
    return memory::String(text);
}

std::expected<void, Problem> tile_problem(TileIndex tile, memory::String message)
{
    return std::unexpected(Problem{tile, kNone, std::move(message)});
}

std::expected<void, Problem> page_problem(PageIndex page, memory::String message)
{
    return std::unexpected(Problem{kNone, page, std::move(message)});
}

Span default_span(const Tile &tile)
{
    switch (tile.kind) {
    case TileKind::Light:
        return tile.dimmer ? Span{1, 2} : Span{1, 1};
    case TileKind::Climate:
        return {1, 2};
    case TileKind::Picture:
        return {2, 2};
    default:
        return {1, 1};
    }
}

bool in_range(float value, float low, float high)
{
    return value >= low && value <= high;
}

std::expected<void, Problem> check_tile(const Layout &layout, TileIndex index)
{
    const Tile &tile = layout.tiles[index];
    const TileKind kind = tile.kind;
    const bool has_entity = kind != TileKind::Area && kind != TileKind::Spacer;

    if (has_entity) {
        const std::string_view entity = layout.entity_id(tile);
        if (entity.empty()) {
            return tile_problem(index, format("a %s tile needs an entity", kind_name(kind)));
        }
        if (!valid_entity_id(entity)) {
            return tile_problem(index, format("'%.*s' is not an entity id (domain.name: lowercase letters, digits and _)",
                                              static_cast<int>(entity.size()), entity.data()));
        }
        if (kind_of_entity(entity) != kind) {
            return tile_problem(index, format("%.*s does not make a %s tile", static_cast<int>(entity.size()),
                                              entity.data(), kind_name(kind)));
        }
    } else if (tile.entity != kNone) {
        return tile_problem(index, format("a%s %s has no entity", kind == TileKind::Area ? "n" : "", kind_name(kind)));
    }

    if (kind == TileKind::Area && tile.name.empty()) {
        return tile_problem(index, "an area needs a name");
    }
    if (kind == TileKind::Spacer && (!tile.name.empty() || !tile.icon.empty())) {
        return tile_problem(index, "a spacer has no name and no icon");
    }
    if (tile.name.size() > kMaxNameLength) {
        return tile_problem(index, format("the name is longer than %u bytes", static_cast<unsigned>(kMaxNameLength)));
    }
    if (!tile.icon.empty() && tile.icon != "none" && !known_icon(tile.icon)) {
        return tile_problem(index, format("unknown icon '%s'", tile.icon.c_str()));
    }

    if (tile.dimmer && kind != TileKind::Light) {
        return tile_problem(index, "dimmer is only for lights");
    }
    if (tile.has_size) {
        const Span size = tile.span;
        switch (kind) {
        case TileKind::Light:
            if (!tile.dimmer) {
                return tile_problem(index, "a light has a size only with dimmer: true");
            }
            [[fallthrough]];
        case TileKind::Climate:
            if (size.cols != 1 || size.rows < 1 || size.rows > 2) {
                return tile_problem(index, format("a %s tile is 1x1 or 1x2", kind_name(kind)));
            }
            break;
        case TileKind::Weather:
            // 1x1, 1x2 (tall) and 2x1 (wide)
            if (!((size.cols == 1 && size.rows >= 1 && size.rows <= 2) || (size.cols == 2 && size.rows == 1))) {
                return tile_problem(index, "a weather tile is 1x1, 1x2 or 2x1");
            }
            break;
        case TileKind::Picture:
            if (size.cols < 1 || size.cols > 2 || size.rows < 1 || size.rows > 2) {
                return tile_problem(index, "a picture tile is 1x1, 1x2, 2x1 or 2x2");
            }
            break;
        default:
            return tile_problem(index, "size is only for dimmer lights, climate, weather and picture tiles");
        }
    }

    if (tile.step != 0) {
        if (kind == TileKind::Light) {
            if (!in_range(tile.step, 1, kMaxLightStep)) {
                return tile_problem(index, format("the step of a light is 1 to %d percent", static_cast<int>(kMaxLightStep)));
            }
        } else if (kind == TileKind::Climate) {
            if (!in_range(tile.step, kMinClimateStep - 0.0001f, kMaxClimateStep)) {
                return tile_problem(index, "the step of a climate tile is 0.1 to 5 degrees");
            }
        } else {
            return tile_problem(index, "step is only for lights and climate tiles");
        }
    }
    if (!tile.unit.empty()) {
        if (kind != TileKind::Sensor) {
            return tile_problem(index, "unit is only for sensors");
        }
        if (tile.unit.size() > kMaxUnitLength) {
            return tile_problem(index, format("the unit is longer than %u bytes", static_cast<unsigned>(kMaxUnitLength)));
        }
    }
    if (tile.decimals != -1) {
        if (kind != TileKind::Sensor) {
            return tile_problem(index, "decimals is only for sensors");
        }
        if (tile.decimals < 0 || tile.decimals > kMaxDecimals) {
            return tile_problem(index, format("decimals is 0 to %u", static_cast<unsigned>(kMaxDecimals)));
        }
    }
    if (tile.refresh_s != 0) {
        if (kind != TileKind::Picture) {
            return tile_problem(index, "refresh is only for picture tiles");
        }
        if (tile.refresh_s > kMaxRefresh) {
            return tile_problem(index, format("refresh is 1 to %u seconds", static_cast<unsigned>(kMaxRefresh)));
        }
    }
    if (!tile.stream.empty()) {
        if (kind != TileKind::Picture) {
            return tile_problem(index, "stream is only for picture tiles");
        }
        // https needs a certificate the bundle may not have; a camera stream on the network is http
        if (tile.stream != kHassStream && (tile.stream.rfind("http://", 0) != 0 || tile.stream.size() > 255)) {
            return tile_problem(index, "stream is hass or an http:// address of an MJPEG stream");
        }
    }
    return {};
}

bool grid_ok(Span grid)
{
    return grid.cols >= 1 && grid.cols <= kMaxGrid && grid.rows >= 1 && grid.rows <= kMaxGrid;
}

// The cells of one page, for one orientation.
class Grid {
public:
    Grid(Span size) : _size(size)
    {
    }

    bool free(Cell at, Span span) const
    {
        if (at.col + span.cols > _size.cols || at.row + span.rows > _size.rows) {
            return false;
        }
        for (uint8_t row = at.row; row < at.row + span.rows; row++) {
            for (uint8_t col = at.col; col < at.col + span.cols; col++) {
                if (_used[row] & (1u << col)) {
                    return false;
                }
            }
        }
        return true;
    }

    void take(Cell at, Span span)
    {
        for (uint8_t row = at.row; row < at.row + span.rows; row++) {
            for (uint8_t col = at.col; col < at.col + span.cols; col++) {
                _used[row] |= static_cast<uint8_t>(1u << col);
            }
        }
    }

    // the first free block in reading order (row by row, left to right)
    bool first_fit(Span span, Cell &out) const
    {
        for (uint8_t row = 0; row + span.rows <= _size.rows; row++) {
            for (uint8_t col = 0; col + span.cols <= _size.cols; col++) {
                if (free({col, row}, span)) {
                    out = {col, row};
                    return true;
                }
            }
        }
        return false;
    }

private:
    Span _size;
    uint8_t _used[kMaxGrid] = {};
};

const char *orientation_name(Orientation o)
{
    return o == Orientation::Landscape ? "landscape" : "portrait";
}

Cell &cell_of(Tile &tile, Orientation o)
{
    return o == Orientation::Landscape ? tile.landscape : tile.portrait;
}

// the tile keeps `cell` in `o` as its own
void pin(Tile &tile, Orientation o, Cell cell)
{
    cell_of(tile, o) = cell;
    (o == Orientation::Landscape ? tile.has_landscape : tile.has_portrait) = true;
}

// web/dashboard.js copies this placement, kind_of_entity(), default_span() and the sizes of
// check_tile() for its preview: a change here is a change there too (docs/architecture.md section 5).
//
// Places the tiles of a page for one orientation. Portrait shows the grid transposed (4x3 becomes
// 3x4) and swaps the span of every tile with it, so a cell keeps its shape, and the page reads in
// the same order (transposing the positions instead would turn the first row into the left
// column). The tiles with a cell of their own take it first; the others fill the first free block
// in the order of the page. The configuration is written for landscape: there a tile that finds no
// block is an error, in portrait it falls back to the first cell.
std::expected<void, Problem> place(Layout &layout, PageIndex index, Orientation o)
{
    const Page &page = layout.pages[index];
    const Span size = grid_in(page, o);
    const char *name = orientation_name(o);
    Grid grid(size);
    if (index != 0) {
        // the first cell of an area page is its back tile
        grid.take({0, 0}, {1, 1});
    }
    for (const TileIndex t : page.tiles) {
        Tile &tile = layout.tiles[t];
        if (!tile.has_cell(o)) {
            continue;
        }
        const Span span = tile.span_in(o);
        const Cell at = tile.cell(o);
        if (at.col + span.cols > size.cols || at.row + span.rows > size.rows) {
            return tile_problem(t, format("the %s cell %u,%u of a %ux%u tile lies outside the %ux%u grid of its page", name,
                                          static_cast<unsigned>(at.col), static_cast<unsigned>(at.row),
                                          static_cast<unsigned>(span.cols), static_cast<unsigned>(span.rows),
                                          static_cast<unsigned>(size.cols), static_cast<unsigned>(size.rows)));
        }
        if (index != 0 && at == Cell{0, 0}) {
            return tile_problem(t, format("the %s cell 0,0 of an area page is its back tile", name));
        }
        if (!grid.free(at, span)) {
            return tile_problem(t, format("the %s cell %u,%u overlaps another tile", name, static_cast<unsigned>(at.col),
                                          static_cast<unsigned>(at.row)));
        }
        grid.take(at, span);
    }
    for (const TileIndex t : page.tiles) {
        Tile &tile = layout.tiles[t];
        if (tile.has_cell(o)) {
            continue;
        }
        const Span span = tile.span_in(o);
        Cell &at = cell_of(tile, o);
        if (o == Orientation::Portrait) {
            tile.portrait_fallback = !grid.first_fit(span, at);
            if (tile.portrait_fallback) {
                at = {0, 0};
                continue;
            }
        } else if (span.cols > size.cols || span.rows > size.rows) {
            return tile_problem(t, format("a %ux%u tile does not fit into the %s %ux%u grid of its page",
                                          static_cast<unsigned>(span.cols), static_cast<unsigned>(span.rows), name,
                                          static_cast<unsigned>(size.cols), static_cast<unsigned>(size.rows)));
        } else if (!grid.first_fit(span, at)) {
            return tile_problem(t, format("no free %ux%u block left in the %s %ux%u grid of its page",
                                          static_cast<unsigned>(span.cols), static_cast<unsigned>(span.rows), name,
                                          static_cast<unsigned>(size.cols), static_cast<unsigned>(size.rows)));
        }
        grid.take(at, span);
    }
    return {};
}

// the tiles of page `from` (without `removed`, an area with its pages) appended to page `to`
void copy_page(const Layout &source, PageIndex from, Layout &target, PageIndex to, TileIndex removed)
{
    for (const TileIndex t : source.pages[from].tiles) {
        if (t == removed) {
            continue;
        }
        const Tile &original = source.tiles[t];
        Tile tile = original;
        tile.opens = kNone;
        const TileIndex index = target.add_tile(to, std::move(tile));
        if (original.kind == TileKind::Area) {
            const Page &page = source.pages[original.opens];
            const PageIndex inner = target.add_page(to, index);
            target.pages[inner].has_grid = page.has_grid;
            target.pages[inner].grid = page.grid;
            copy_page(source, original.opens, target, inner, removed);
        }
    }
}

} // namespace

const char *kind_name(TileKind kind)
{
    switch (kind) {
    case TileKind::Area:
        return "area";
    case TileKind::Spacer:
        return "spacer";
    case TileKind::Switch:
        return "switch";
    case TileKind::Light:
        return "light";
    case TileKind::Button:
        return "button";
    case TileKind::Climate:
        return "climate";
    case TileKind::Picture:
        return "picture";
    case TileKind::Sensor:
        return "sensor";
    case TileKind::Lock:
        return "lock";
    case TileKind::Weather:
        return "weather";
    }
    return "?";
}

TileKind kind_of_entity(std::string_view entity_id)
{
    const std::string_view domain = entity_id.substr(0, entity_id.find('.'));
    if (domain == "light") {
        return TileKind::Light;
    }
    if (domain == "switch" || domain == "input_boolean" || domain == "fan" || domain == "automation") {
        return TileKind::Switch;
    }
    if (domain == "button" || domain == "input_button" || domain == "scene" || domain == "script") {
        return TileKind::Button;
    }
    if (domain == "climate") {
        return TileKind::Climate;
    }
    if (domain == "camera") {
        return TileKind::Picture;
    }
    if (domain == "lock") {
        return TileKind::Lock;
    }
    if (domain == "weather") {
        return TileKind::Weather;
    }
    return TileKind::Sensor;
}

bool valid_entity_id(std::string_view entity_id)
{
    if (entity_id.size() > kMaxEntityLength) {
        return false;
    }
    const size_t dot = entity_id.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == entity_id.size()) {
        return false;
    }
    for (size_t i = 0; i < entity_id.size(); i++) {
        const char c = entity_id[i];
        if (i == dot) {
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

Layout::Layout()
{
    pages.emplace_back();
}

PageIndex Layout::add_page(PageIndex parent, TileIndex area)
{
    Page page;
    page.parent = parent;
    page.area = area;
    page.depth = static_cast<uint8_t>(pages[parent].depth + 1);
    pages.push_back(std::move(page));
    const auto index = static_cast<PageIndex>(pages.size() - 1);
    tiles[area].opens = index;
    return index;
}

TileIndex Layout::add_tile(PageIndex page, Tile tile)
{
    tile.page = page;
    tiles.push_back(std::move(tile));
    const auto index = static_cast<TileIndex>(tiles.size() - 1);
    pages[page].tiles.push_back(index);
    return index;
}

EntityIndex Layout::intern(std::string_view entity_id)
{
    for (size_t i = 0; i < entities.size(); i++) {
        if (entities[i] == entity_id) {
            return static_cast<EntityIndex>(i);
        }
    }
    entities.emplace_back(entity_id.data(), entity_id.size());
    return static_cast<EntityIndex>(entities.size() - 1);
}

std::expected<void, Problem> finish(Layout &layout)
{
    if (!grid_ok(layout.grid)) {
        return page_problem(0, "the grid is 1x1 to 8x8");
    }
    if (layout.tiles.size() >= kNone || layout.pages.size() >= kNone || layout.entities.size() >= kNone) {
        return page_problem(0, "the dashboard has too many tiles");
    }

    // ids: kept when given, unique, new ones after the highest
    uint32_t highest = 0;
    for (TileIndex i = 0; i < layout.tiles.size(); i++) {
        const uint32_t id = layout.tiles[i].id;
        if (id == 0) {
            continue;
        }
        for (TileIndex k = 0; k < i; k++) {
            if (layout.tiles[k].id == id) {
                return tile_problem(i, format("the id %u is used twice", static_cast<unsigned>(id)));
            }
        }
        highest = id > highest ? id : highest;
    }
    if (layout.next_id <= highest) {
        layout.next_id = highest + 1;
    }
    for (auto &tile : layout.tiles) {
        if (tile.id == 0) {
            tile.id = layout.next_id++;
        }
    }

    for (PageIndex p = 0; p < layout.pages.size(); p++) {
        Page &page = layout.pages[p];
        if (page.depth > kMaxAreaDepth) {
            return page_problem(p, format("areas are nested more than %u levels deep", static_cast<unsigned>(kMaxAreaDepth)));
        }
        if (page.has_grid && !grid_ok(page.grid)) {
            return page_problem(p, "the grid is 1x1 to 8x8");
        }
        if (!page.has_grid) {
            page.grid = layout.grid;
        }
        if (page.tiles.empty()) {
            return page_problem(p, p == 0 ? "the dashboard has no tiles" : "the area has no tiles");
        }
    }

    for (TileIndex i = 0; i < layout.tiles.size(); i++) {
        if (auto checked = check_tile(layout, i); !checked) {
            return checked;
        }
        Tile &tile = layout.tiles[i];
        if (!tile.has_size) {
            tile.span = default_span(tile);
        }
    }

    for (PageIndex p = 0; p < layout.pages.size(); p++) {
        for (const Orientation o : {Orientation::Landscape, Orientation::Portrait}) {
            if (auto placed = place(layout, p, o); !placed) {
                return placed;
            }
        }
    }
    return {};
}

Span grid_in(const Page &page, Orientation o)
{
    return o == Orientation::Landscape ? page.grid : Span{page.grid.rows, page.grid.cols};
}

bool move_tile(Layout &layout, TileIndex index, Orientation o, Cell cell)
{
    const Page &page = layout.pages[layout.tiles[index].page];
    const bool area_page = layout.tiles[index].page != 0;
    const Span size = grid_in(page, o);
    const Span span = layout.tiles[index].span_in(o);
    if (cell.col + span.cols > size.cols || cell.row + span.rows > size.rows || (area_page && cell == Cell{0, 0})) {
        return false;
    }
    const auto covered = [&](Cell at, Span other) {
        return at.col < cell.col + span.cols && at.col + other.cols > cell.col && at.row < cell.row + span.rows &&
               at.row + other.rows > cell.row;
    };
    // the page without the moved tile and the tiles it covers, then the moved tile in its place
    Grid grid(size);
    if (area_page) {
        grid.take({0, 0}, {1, 1});
    }
    memory::Vector<TileIndex> pushed;
    for (const TileIndex t : page.tiles) {
        const Tile &tile = layout.tiles[t];
        if (t == index || (o == Orientation::Portrait && tile.portrait_fallback)) {
            continue;
        }
        if (covered(tile.cell(o), tile.span_in(o))) {
            pushed.push_back(t);
        } else {
            grid.take(tile.cell(o), tile.span_in(o));
        }
    }
    grid.take(cell, span);
    // every pushed tile: the first free block from `cell` on in reading order, wrapping
    const unsigned count = size.cols * size.rows;
    const unsigned start = cell.row * size.cols + cell.col;
    memory::Vector<Cell> targets;
    for (const TileIndex t : pushed) {
        const Span other = layout.tiles[t].span_in(o);
        bool found = false;
        for (unsigned i = 0; i < count && !found; i++) {
            const unsigned at = (start + i) % count;
            const Cell candidate{static_cast<uint8_t>(at % size.cols), static_cast<uint8_t>(at / size.cols)};
            if (grid.free(candidate, other)) {
                grid.take(candidate, other);
                targets.push_back(candidate);
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    // the page is arranged in `o` from now on: every tile keeps its cell
    for (const TileIndex t : page.tiles) {
        Tile &tile = layout.tiles[t];
        if (!(o == Orientation::Portrait && tile.portrait_fallback)) {
            pin(tile, o, tile.cell(o));
        }
    }
    for (size_t i = 0; i < pushed.size(); i++) {
        pin(layout.tiles[pushed[i]], o, targets[i]);
    }
    pin(layout.tiles[index], o, cell);
    // cannot fail: the cells of this page do not overlap, the other pages did not change
    return finish(layout).has_value();
}

std::expected<void, Problem> reset_arrangement(Layout &layout, PageIndex page, Orientation o)
{
    // automatic order may need more room than the arrangement, so it is tried on a copy
    Layout copy = layout;
    for (const TileIndex t : copy.pages[page].tiles) {
        (o == Orientation::Landscape ? copy.tiles[t].has_landscape : copy.tiles[t].has_portrait) = false;
    }
    if (auto finished = finish(copy); !finished) {
        return finished;
    }
    layout = std::move(copy);
    return {};
}

std::expected<Layout, Problem> remove_tile(const Layout &layout, TileIndex tile)
{
    Layout out;
    out.grid = layout.grid;
    out.revision = layout.revision;
    out.next_id = layout.next_id;
    // the same entity table: the entity store of the screen stays valid while editing; the next
    // read of the stored file leaves the unused ones out
    out.entities = layout.entities;
    copy_page(layout, 0, out, 0, tile);
    if (auto finished = finish(out); !finished) {
        return std::unexpected(std::move(finished.error()));
    }
    return out;
}

} // namespace hc::core
