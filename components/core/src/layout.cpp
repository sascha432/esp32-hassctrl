#include "hc/core/layout.hpp"
#include "hc/core/timezone.hpp"

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

bool in_range(float value, float low, float high)
{
    return value >= low && value <= high;
}

const char *orientation_name(Orientation o)
{
    return o == Orientation::Landscape ? "landscape" : "portrait";
}

std::expected<void, Problem> check_tile(const Layout &layout, TileIndex index)
{
    const Tile &tile = layout.tiles[index];
    const TileKind kind = tile.kind;
    if (has_entity(kind)) {
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
    if (tile.swipe && kind != TileKind::Area) {
        return tile_problem(index, "swipe is only for areas");
    }
    for (const Orientation o : kOrientations) {
        const Placement &p = tile.in(o);
        if (p.has_size && (p.size.cols < 1 || p.size.cols > kMaxTileSize || p.size.rows < 1 || p.size.rows > kMaxTileSize)) {
            return tile_problem(index, format("the %s size is 1x1 to %ux%u", orientation_name(o),
                                              static_cast<unsigned>(kMaxTileSize), static_cast<unsigned>(kMaxTileSize)));
        }
        if (p.has_cell && (p.cell.col >= kMaxGrid || p.cell.row >= kMaxGrid)) {
            return tile_problem(index, format("the %s cell is 0,0 to %u,%u", orientation_name(o),
                                              static_cast<unsigned>(kMaxGrid - 1), static_cast<unsigned>(kMaxGrid - 1)));
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
    const bool clock_keys = tile.hours12 || tile.seconds || tile.date || tile.border || tile.style != ClockStyle::Digital ||
                            tile.ticks != ClockTicks::Hour || !tile.zone.empty();
    if (clock_keys && kind != TileKind::Clock) {
        return tile_problem(index, "hours, seconds, date, style, border, ticks and zone are only for clock tiles");
    }
    if (kind == TileKind::Clock) {
        if (tile.style == ClockStyle::Digital && (tile.border || tile.ticks != ClockTicks::Hour)) {
            return tile_problem(index, "border and ticks are only for analog clocks");
        }
        if (!tile.zone.empty() && (tile.zone.size() > kMaxZoneLength || time_zone_rule(tile.zone) == nullptr)) {
            return tile_problem(index, format("unknown time zone '%.48s' (an IANA name like Europe/Berlin)", tile.zone.c_str()));
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
                _used[row] |= 1u << col;
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
    uint32_t _used[kMaxGrid] = {}; // a bit per column
};

// the tile keeps `cell` in `o` as its own
void pin(Tile &tile, Orientation o, Cell cell)
{
    Placement &p = tile.in(o);
    p.cell = cell;
    p.has_cell = true;
}

// web/dashboard.js copies this placement, kind_of_entity(), has_back_tile() and swipe_chain() for
// its preview: a change here is a change there too (docs/architecture.md section 5).
//
// Places the tiles of a page for one orientation. Portrait shows the grid transposed (4x3 becomes
// 3x4); a tile's portrait size is its landscape size turned unless the file sets one, so a cell
// keeps its shape and the page reads in the same order. The tiles with a cell of their own take
// it first; the others fill the first free block in the order of the page. A tile that cannot
// take its cell (outside the grid, on the back tile, over another tile) or finds no free block
// is not shown in this orientation: the device draws nothing for it, the web editor lists it.
void place(Layout &layout, PageIndex index, Orientation o)
{
    const Page &page = layout.pages[index];
    const Span size = grid_in(page, o);
    Grid grid(size);
    if (has_back_tile(layout, index)) {
        grid.take({0, 0}, {1, 1});
    }
    for (const TileIndex t : page.tiles) {
        Placement &p = layout.tiles[t].in(o);
        p.shown = false;
        if (!p.has_cell || p.hidden) {
            continue;
        }
        if (grid.free(p.cell, p.size)) {
            grid.take(p.cell, p.size);
            p.shown = true;
        }
    }
    for (const TileIndex t : page.tiles) {
        Placement &p = layout.tiles[t].in(o);
        if (p.has_cell || p.hidden) {
            continue;
        }
        if (grid.first_fit(p.size, p.cell)) {
            grid.take(p.cell, p.size);
            p.shown = true;
        }
    }
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
    case TileKind::Clock:
        return "clock";
    case TileKind::Timer:
        return "timer";
    case TileKind::Automation:
        return "automation";
    }
    return "?";
}

const char *clock_style_name(ClockStyle style)
{
    return style == ClockStyle::Analog ? "analog" : "digital";
}

const char *clock_ticks_name(ClockTicks ticks)
{
    switch (ticks) {
    case ClockTicks::None:
        return "none";
    case ClockTicks::Quarter:
        return "quarter";
    case ClockTicks::Minute:
        return "minute";
    case ClockTicks::Hour:
        break;
    }
    return "hour";
}

bool clock_style_of(std::string_view text, ClockStyle &out)
{
    for (const ClockStyle style : {ClockStyle::Digital, ClockStyle::Analog}) {
        if (text == clock_style_name(style)) {
            out = style;
            return true;
        }
    }
    return false;
}

bool clock_ticks_of(std::string_view text, ClockTicks &out)
{
    for (const ClockTicks ticks : {ClockTicks::None, ClockTicks::Quarter, ClockTicks::Hour, ClockTicks::Minute}) {
        if (text == clock_ticks_name(ticks)) {
            out = ticks;
            return true;
        }
    }
    return false;
}

TileKind kind_of_entity(std::string_view entity_id)
{
    const std::string_view domain = entity_id.substr(0, entity_id.find('.'));
    if (domain == "light") {
        return TileKind::Light;
    }
    if (domain == "switch" || domain == "input_boolean" || domain == "fan") {
        return TileKind::Switch;
    }
    if (domain == "automation" || domain == "script") {
        return TileKind::Automation;
    }
    if (domain == "button" || domain == "input_button" || domain == "scene") {
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
    if (domain == "timer") {
        return TileKind::Timer;
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
        return page_problem(0, "the grid is 1x1 to 32x32");
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
            return page_problem(p, "the grid is 1x1 to 32x32");
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
        if (!tile.in(Orientation::Landscape).has_size) {
            tile.in(Orientation::Landscape).size = {1, 1};
        }
        if (!tile.in(Orientation::Portrait).has_size) {
            const Span landscape = tile.in(Orientation::Landscape).size;
            tile.in(Orientation::Portrait).size = {landscape.rows, landscape.cols};
        }
    }

    for (PageIndex p = 0; p < layout.pages.size(); p++) {
        for (const Orientation o : kOrientations) {
            place(layout, p, o);
        }
    }
    return {};
}

bool swipe_page(const Layout &layout, PageIndex page)
{
    const TileIndex area = layout.pages[page].area;
    return area != kNone && layout.tiles[area].swipe;
}

bool has_back_tile(const Layout &layout, PageIndex page)
{
    return layout.pages[page].parent != kNone && !swipe_page(layout, page);
}

memory::Vector<PageIndex> swipe_chain(const Layout &layout, PageIndex page)
{
    memory::Vector<PageIndex> chain;
    chain.push_back(page);
    for (PageIndex p = page; swipe_page(layout, p) && layout.pages[p].parent != kNone;) {
        p = layout.pages[p].parent;
        chain.insert(chain.begin(), p);
    }
    return chain;
}

Span grid_in(const Page &page, Orientation o)
{
    return o == Orientation::Landscape ? page.grid : Span{page.grid.rows, page.grid.cols};
}

bool move_tile(Layout &layout, TileIndex index, Orientation o, Cell cell)
{
    const PageIndex page_index = layout.tiles[index].page;
    const Page &page = layout.pages[page_index];
    const bool back_tile = has_back_tile(layout, page_index);
    const Span size = grid_in(page, o);
    const Span span = layout.tiles[index].in(o).size;
    if (!layout.tiles[index].in(o).shown || cell.col + span.cols > size.cols || cell.row + span.rows > size.rows ||
        (back_tile && cell == Cell{0, 0})) {
        return false;
    }
    const auto covered = [&](Cell at, Span other) {
        return at.col < cell.col + span.cols && at.col + other.cols > cell.col && at.row < cell.row + span.rows &&
               at.row + other.rows > cell.row;
    };
    // the page without the moved tile and the tiles it covers, then the moved tile in its place
    Grid grid(size);
    if (back_tile) {
        grid.take({0, 0}, {1, 1});
    }
    memory::Vector<TileIndex> pushed;
    for (const TileIndex t : page.tiles) {
        const Placement &p = layout.tiles[t].in(o);
        if (t == index || !p.shown) {
            continue;
        }
        if (covered(p.cell, p.size)) {
            pushed.push_back(t);
        } else {
            grid.take(p.cell, p.size);
        }
    }
    grid.take(cell, span);
    // every pushed tile: the first free block from `cell` on in reading order, wrapping
    const unsigned count = size.cols * size.rows;
    const unsigned start = cell.row * size.cols + cell.col;
    memory::Vector<Cell> targets;
    for (const TileIndex t : pushed) {
        const Span other = layout.tiles[t].in(o).size;
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
    // the page is arranged in `o` from now on: every shown tile keeps its cell
    for (const TileIndex t : page.tiles) {
        Tile &tile = layout.tiles[t];
        if (tile.in(o).shown) {
            pin(tile, o, tile.in(o).cell);
        }
    }
    for (size_t i = 0; i < pushed.size(); i++) {
        pin(layout.tiles[pushed[i]], o, targets[i]);
    }
    pin(layout.tiles[index], o, cell);
    // cannot fail: the data did not change, and the placement never fails
    return finish(layout).has_value();
}

void reset_arrangement(Layout &layout, PageIndex page, Orientation o)
{
    for (const TileIndex t : layout.pages[page].tiles) {
        layout.tiles[t].in(o).has_cell = false;
    }
    // the data did not change: the placement cannot fail
    static_cast<void>(finish(layout));
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

std::expected<Layout, Problem> add_tile(const Layout &layout, PageIndex page, NewTile added, Orientation o, Cell cell)
{
    Layout out = layout;
    // an entity's tile: its kind follows the domain (a light keeps `dimmer` only as a light)
    const auto bind = [&](Tile &tile, const memory::String &entity) -> std::expected<void, Problem> {
        if (entity.empty()) {
            return {};
        }
        if (!valid_entity_id(entity)) {
            return std::unexpected(Problem{kNone, page, format("'%s' is not an entity id", entity.c_str())});
        }
        tile.kind = kind_of_entity(entity);
        tile.entity = out.intern(entity);
        tile.dimmer = tile.dimmer && tile.kind == TileKind::Light;
        return {};
    };
    if (auto bound = bind(added.tile, added.entity); !bound) {
        return std::unexpected(std::move(bound.error()));
    }
    const TileIndex index = out.add_tile(page, std::move(added.tile));
    if (out.tiles[index].kind == TileKind::Area) {
        if (auto bound = bind(added.first, added.first_entity); !bound) {
            return std::unexpected(std::move(bound.error()));
        }
        const PageIndex inner = out.add_page(page, index);
        out.add_tile(inner, std::move(added.first));
    }
    if (auto finished = finish(out); !finished) {
        return std::unexpected(std::move(finished.error()));
    }
    // the new tile took the first free block (or none); the move keeps every other tile in its cell
    if (!move_tile(out, index, o, cell)) {
        return std::unexpected(Problem{index, page, memory::String("no room for the tile there")});
    }
    return out;
}

std::expected<Layout, Problem> change_tile(const Layout &layout, TileIndex index, const NewTile &changed, Orientation o)
{
    Layout out = layout;
    Tile &tile = out.tiles[index];
    const Tile &from = changed.tile;
    tile.name = from.name;
    if (has_entity(tile.kind) && !changed.entity.empty() && changed.entity != out.entity_id(tile)) {
        if (!valid_entity_id(changed.entity)) {
            return std::unexpected(Problem{index, tile.page, format("'%s' is not an entity id", changed.entity.c_str())});
        }
        const TileKind kind = kind_of_entity(changed.entity);
        if (kind != tile.kind) {
            // the keys of the old kind
            tile.kind = kind;
            tile.decimals = -1;
            tile.unit.clear();
            tile.step = 0;
            tile.refresh_s = 0;
            tile.stream.clear();
        }
        tile.entity = out.intern(changed.entity);
    }
    switch (tile.kind) {
    case TileKind::Light:
        tile.dimmer = from.dimmer;
        break;
    case TileKind::Sensor:
        tile.decimals = from.decimals;
        break;
    case TileKind::Clock:
        tile.style = from.style;
        tile.hours12 = from.hours12;
        tile.seconds = from.seconds;
        tile.date = from.date;
        if (tile.style == ClockStyle::Digital) {
            tile.border = false;
            tile.ticks = ClockTicks::Hour;
        }
        break;
    case TileKind::Area:
        tile.swipe = from.swipe;
        break;
    default:
        tile.dimmer = false;
        break;
    }
    if (auto finished = finish(out); !finished) {
        return std::unexpected(std::move(finished.error()));
    }
    const Span size = from.in(o).size;
    Placement &p = out.tiles[index].in(o);
    if (size == p.size) {
        return out;
    }
    p.has_size = true;
    p.size = size;
    if (!p.shown) {
        // not on the screen in `o`: it keeps its place in the list
        static_cast<void>(finish(out));
        return out;
    }
    // the same cell with the new size, the tiles in the way pushed aside
    if (!move_tile(out, index, o, p.cell)) {
        return std::unexpected(Problem{index, tile.page, format("no room for %ux%u there", static_cast<unsigned>(size.cols),
                                                                static_cast<unsigned>(size.rows))});
    }
    return out;
}

} // namespace hc::core
