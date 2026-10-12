#pragma once

// The dashboard model (docs/architecture.md section 5): pages, tiles and the grid of every page.
// Plain C++, host tested. It lives on the large heap (PSRAM).
//
// A reader (the stored JSON, the YAML import) adds pages and tiles and then calls finish(), which
// checks every rule and places the tiles. Both readers use the same finish(), so a layout passes
// the same checks however it arrived. A finished layout does not change; an edit builds a new one
// (the edit functions at the end work on a copy).
//
// Placement (docs/ui/lvgl/lvgl-dashboard.md section 6): per orientation, the tiles with a cell of their
// own take it first, then the others fill the first free block in list order.

#include <cstdint>
#include <expected>
#include <string_view>

#include "hc/memory/large.hpp"

namespace hc::core {

constexpr uint8_t kMaxGrid = 8;          // columns and rows of a grid
constexpr uint8_t kMaxAreaDepth = 5;     // areas inside areas, below the main page
constexpr size_t kMaxNameLength = 32;    // bytes of UTF-8
constexpr size_t kMaxEntityLength = 64;
constexpr size_t kMaxUnitLength = 12;
constexpr uint8_t kMaxDecimals = 6;
constexpr uint16_t kDefaultRefresh = 60; // seconds between two camera images
constexpr uint16_t kMaxRefresh = 3600;
constexpr float kDefaultLightStep = 10;  // percent per + or - in the light panel
constexpr float kMaxLightStep = 50;
constexpr float kMinClimateStep = 0.1f;  // degrees
constexpr float kMaxClimateStep = 5;

// What a tile shows and does. Every kind but Area and Spacer has an entity, and the entity's
// domain decides the kind (kind_of_entity).
enum class TileKind : uint8_t {
    Area,    // opens a page of its own
    Spacer,  // keeps a cell free
    Switch,  // switch, input_boolean, fan, automation: a tap toggles
    Light,   // a tap toggles; with `dimmer` a level fill, drag and the light panel
    Button,  // button, input_button, scene, script: a tap presses
    Climate,
    Picture, // camera
    Sensor,  // every other domain: shows the state, a tap opens the history
    Lock,    // lock: shows the state, a tap opens the lock panel (never locks or unlocks by itself)
};

const char *kind_name(TileKind kind);

// "light.kitchen" -> Light. The entity id is not checked here.
TileKind kind_of_entity(std::string_view entity_id);

// lowercase letters, digits and _ on both sides of one dot, at most kMaxEntityLength
bool valid_entity_id(std::string_view entity_id);

struct Span {
    uint8_t cols = 1;
    uint8_t rows = 1;

    bool operator==(const Span &) const = default;
};

struct Cell {
    uint8_t col = 0;
    uint8_t row = 0;

    bool operator==(const Cell &) const = default;
};

enum class Orientation : uint8_t { Landscape, Portrait };

using TileIndex = uint16_t;
using PageIndex = uint16_t;
using EntityIndex = uint16_t;
constexpr uint16_t kNone = 0xFFFF;
// Tile::stream: the MJPEG stream of Home Assistant's camera proxy (/api/camera_proxy_stream)
constexpr std::string_view kHassStream = "hass";

struct Tile {
    // stable, given by the device; the web UI and the screen name a tile by it
    uint32_t id = 0;
    TileKind kind = TileKind::Sensor;
    PageIndex page = 0;          // the page the tile is on
    PageIndex opens = kNone;     // Area: the page it opens
    EntityIndex entity = kNone;  // into Layout::entities
    bool dimmer = false;         // Light
    bool has_size = false;       // `span` was set by the configuration
    Span span;                   // cells in landscape; finish() sets the default of the kind
    int8_t decimals = -1;        // Sensor; -1: the display precision set in Home Assistant
    uint16_t refresh_s = 0;      // Picture; 0: kDefaultRefresh
    float step = 0;              // Light (percent) and Climate (degrees); 0: the default
    memory::String name;         // empty: the entity's friendly_name
    memory::String icon;         // empty: automatic; "none": no icon
    memory::String unit;         // Sensor; empty: the unit Home Assistant reports
    memory::String stream;       // Picture: an MJPEG stream (http://... or kHassStream); empty: none
    bool has_landscape = false;  // `landscape` was set by the configuration (the page is arranged)
    bool has_portrait = false;
    Cell landscape;              // set by finish() unless has_landscape
    Cell portrait;               // set by finish() unless has_portrait
    bool portrait_fallback = false; // set by finish(): no free block in portrait, drawn at the first cell

    bool has_cell(Orientation o) const
    {
        return o == Orientation::Landscape ? has_landscape : has_portrait;
    }
    Cell cell(Orientation o) const
    {
        return o == Orientation::Landscape ? landscape : portrait;
    }
    // the span in that orientation: portrait turns it
    Span span_in(Orientation o) const
    {
        return o == Orientation::Landscape ? span : Span{span.rows, span.cols};
    }
};

struct Page {
    PageIndex parent = kNone;    // the main page has none
    TileIndex area = kNone;      // the area tile that opens it
    bool has_grid = false;       // false: the grid of the defaults
    Span grid;                   // set by finish() when !has_grid
    uint8_t depth = 0;           // 0: the main page
    memory::Vector<TileIndex> tiles;
};

struct Layout {
    Span grid{4, 2};             // `defaults.grid`
    uint32_t revision = 0;       // counts the stored versions
    uint32_t next_id = 1;
    memory::Vector<Page> pages;  // [0] is the main page
    memory::Vector<Tile> tiles;
    memory::Vector<memory::String> entities;

    Layout();

    // a new area page below `parent`, opened by the area tile `area`
    PageIndex add_page(PageIndex parent, TileIndex area);
    // appends `tile` to `page`; the tile's page is set here
    TileIndex add_tile(PageIndex page, Tile tile);
    // the index of `entity_id`, added when it is new
    EntityIndex intern(std::string_view entity_id);

    std::string_view entity_id(const Tile &tile) const
    {
        return tile.entity == kNone ? std::string_view() : std::string_view(entities[tile.entity]);
    }
};

// A broken rule: the tile or page it is about (kNone when neither) and what is wrong.
struct Problem {
    TileIndex tile = kNone;
    PageIndex page = kNone;
    memory::String message;
};

// Checks every rule, sets the default sizes and grids, gives tiles without an id one, and places
// the tiles of every page for landscape and for portrait.
std::expected<void, Problem> finish(Layout &layout);

// The grid of a page in that orientation (portrait is the landscape grid turned).
Span grid_in(const Page &page, Orientation o);

// ---- edits of a finished layout (the edit mode of the screen, docs/ui/lvgl/lvgl-dashboard.md section 7)

// Moves `tile` to `cell` in orientation `o`. Every tile of its page gets its current cell (the page
// becomes arranged in `o`), and the tiles the moved tile covers are pushed aside: each takes the
// first free block in reading order from `cell` on, wrapping to the start, in their old order.
// False when the tile does not fit there or a pushed tile finds no block; the layout is unchanged
// then. The layout stays finished.
bool move_tile(Layout &layout, TileIndex tile, Orientation o, Cell cell);

// The tiles of `page` lose their cells in `o`: automatic order again. A problem when the tiles do
// not fit in automatic order (an arrangement can use the room better); the layout is unchanged
// then. The layout stays finished.
std::expected<void, Problem> reset_arrangement(Layout &layout, PageIndex page, Orientation o);

// The layout without `tile` (an area takes its pages and their tiles with it), finished. The
// entity table stays as it is, so entity indices stay valid. A page left without tiles is a
// problem, as in finish().
std::expected<Layout, Problem> remove_tile(const Layout &layout, TileIndex tile);

} // namespace hc::core
