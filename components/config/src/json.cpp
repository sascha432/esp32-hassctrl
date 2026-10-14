// The stored dashboard, /fs/dashboard.json (docs/architecture.md section 5, "Storage"):
//
//   {"format": "hassctrl-dashboard", "version": 1, "revision": 12, "next_id": 131,
//    "defaults": {"grid": [4, 2]},
//    "tiles": [{"id": 1, "entity": "light.kitchen", "name": "Kitchen", "dimmer": true,
//               "landscape": {"size": [1, 2], "cell": [0, 0]}, "portrait": {"hidden": true}},
//              {"id": 2, "type": "area", "name": "Rooms", "grid": [4, 3], "tiles": [...]},
//              {"id": 3, "type": "spacer"}]}
//
// Only values that differ from the default are written. The file is written by the device, so
// the reader is strict: an unknown key is an error ("cells", which the web API adds to its answers,
// is skipped). A tile without "id" is new (the web UI adds
// tiles without one); finish() gives it the next id.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

#include "cJSON.h"
#include "hc/config/dashboard.hpp"
#include "hc/formats/json_writer.hpp"

namespace hc::config {

namespace {

using core::PageIndex;
using core::Tile;
using core::TileIndex;
using core::TileKind;

constexpr const char *kFormat = "hassctrl-dashboard";
constexpr int kVersion = 2;

memory::String format(const char *pattern, ...)
{
    char text[200];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(text, sizeof(text), pattern, args);
    va_end(args);
    return memory::String(text);
}

struct JsonDeleter {
    void operator()(cJSON *json) const
    {
        cJSON_Delete(json);
    }
};

class Reader {
public:
    std::expected<core::Layout, JsonProblem> run(std::string_view text)
    {
        const auto failed = [this](memory::String message) {
            return std::unexpected(JsonProblem{std::move(message), _problem_path});
        };
        std::unique_ptr<cJSON, JsonDeleter> root(cJSON_ParseWithLength(text.data(), text.size()));
        if (root == nullptr || !cJSON_IsObject(root.get())) {
            return failed(memory::String("the stored dashboard is not valid JSON"));
        }
        const cJSON *format_name = cJSON_GetObjectItemCaseSensitive(root.get(), "format");
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
        if (!cJSON_IsString(format_name) || std::strcmp(format_name->valuestring, kFormat) != 0) {
            return failed(memory::String("the stored file is not a dashboard"));
        }
        if (!cJSON_IsNumber(version) || version->valueint != kVersion) {
            // no older version is read: tools/convert_dashboard.py converts a file of version 1
            return failed(format("the dashboard has version %d, this firmware reads version %d (tools/convert_dashboard.py converts it)",
                                 cJSON_IsNumber(version) ? version->valueint : 0, kVersion));
        }
        core::Layout layout;
        const cJSON *tiles = nullptr;
        for (const cJSON *item = root->child; item != nullptr; item = item->next) {
            const std::string_view key = item->string;
            if (key == "format" || key == "version") {
                continue;
            }
            if (key == "revision" && cJSON_IsNumber(item) && item->valuedouble >= 0) {
                layout.revision = static_cast<uint32_t>(item->valuedouble);
            } else if (key == "next_id" && cJSON_IsNumber(item) && item->valuedouble >= 1) {
                layout.next_id = static_cast<uint32_t>(item->valuedouble);
            } else if (key == "defaults" && cJSON_IsObject(item)) {
                for (const cJSON *value = item->child; value != nullptr; value = value->next) {
                    if (std::strcmp(value->string, "grid") != 0 || !span(value, layout.grid)) {
                        return failed(format("defaults: bad key or value '%s'", value->string));
                    }
                }
            } else if (key == "tiles" && cJSON_IsArray(item)) {
                tiles = item;
            } else {
                return failed(format("bad key or value '%s'", item->string));
            }
        }
        if (tiles == nullptr) {
            return failed(memory::String("the stored dashboard has no tiles"));
        }
        if (!list(tiles, 0, layout)) {
            return failed(_error);
        }
        if (auto finished = core::finish(layout); !finished) {
            const core::Problem &problem = finished.error();
            if (problem.tile != core::kNone) {
                _problem_path = tile_path(layout, problem.tile);
                return failed(format("tile %u: %s", static_cast<unsigned>(layout.tiles[problem.tile].id),
                                     problem.message.c_str()));
            }
            if (problem.page != core::kNone && problem.page != 0) {
                const core::TileIndex area = layout.pages[problem.page].area;
                _problem_path = tile_path(layout, area);
                return failed(format("area %u: %s", static_cast<unsigned>(layout.tiles[area].id), problem.message.c_str()));
            }
            return failed(problem.message);
        }
        return layout;
    }

private:
    static bool span(const cJSON *value, core::Span &out)
    {
        if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) != 2) {
            return false;
        }
        const cJSON *cols = cJSON_GetArrayItem(value, 0);
        const cJSON *rows = cJSON_GetArrayItem(value, 1);
        if (!cJSON_IsNumber(cols) || !cJSON_IsNumber(rows) || cols->valueint < 1 || cols->valueint > core::kMaxGrid ||
            rows->valueint < 1 || rows->valueint > core::kMaxGrid) {
            return false;
        }
        out = {static_cast<uint8_t>(cols->valueint), static_cast<uint8_t>(rows->valueint)};
        return true;
    }

    // {"size": [cols, rows], "cell": [column, row], "hidden": true}, every key optional
    static bool placement(const cJSON *value, core::Placement &out)
    {
        if (!cJSON_IsObject(value)) {
            return false;
        }
        for (const cJSON *item = value->child; item != nullptr; item = item->next) {
            const std::string_view key = item->string;
            if (key == "size") {
                if (!span(item, out.size)) {
                    return false;
                }
                out.has_size = true;
            } else if (key == "cell") {
                if (!cell(item, out.cell)) {
                    return false;
                }
                out.has_cell = true;
            } else if (key == "hidden") {
                if (!cJSON_IsBool(item)) {
                    return false;
                }
                out.hidden = cJSON_IsTrue(item);
            } else {
                return false;
            }
        }
        return true;
    }

    // [column, row], 0 to 31 each
    static bool cell(const cJSON *value, core::Cell &out)
    {
        if (!cJSON_IsArray(value) || cJSON_GetArraySize(value) != 2) {
            return false;
        }
        const cJSON *col = cJSON_GetArrayItem(value, 0);
        const cJSON *row = cJSON_GetArrayItem(value, 1);
        if (!cJSON_IsNumber(col) || !cJSON_IsNumber(row) || col->valueint < 0 || col->valueint >= core::kMaxGrid ||
            row->valueint < 0 || row->valueint >= core::kMaxGrid) {
            return false;
        }
        out = {static_cast<uint8_t>(col->valueint), static_cast<uint8_t>(row->valueint)};
        return true;
    }

    bool fail(uint32_t id, const char *key)
    {
        _error = id == 0 ? format("a new tile: bad key or value '%s'", key)
                         : format("tile %u: bad key or value '%s'", static_cast<unsigned>(id), key);
        _problem_path = _path;
        return false;
    }

    bool list(const cJSON *array, PageIndex page, core::Layout &layout)
    {
        _path.push_back(0);
        for (const cJSON *object = array->child; object != nullptr; object = object->next) {
            if (!cJSON_IsObject(object)) {
                _error = "a tile is not an object";
                _problem_path = _path;
                return false;
            }
            if (!tile(object, page, layout)) {
                return false;
            }
            _path.back()++;
        }
        _path.pop_back();
        return true;
    }

    bool tile(const cJSON *object, PageIndex page, core::Layout &layout)
    {
        Tile tile;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
        if (id != nullptr && (!cJSON_IsNumber(id) || id->valuedouble < 1)) {
            _error = "a tile has a bad id";
            _problem_path = _path;
            return false;
        }
        tile.id = id != nullptr ? static_cast<uint32_t>(id->valuedouble) : 0;
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(object, "type");
        const cJSON *entity = cJSON_GetObjectItemCaseSensitive(object, "entity");
        if (cJSON_IsString(type) && std::strcmp(type->valuestring, "area") == 0) {
            tile.kind = TileKind::Area;
        } else if (cJSON_IsString(type) && std::strcmp(type->valuestring, "spacer") == 0) {
            tile.kind = TileKind::Spacer;
        } else if (type != nullptr) {
            return fail(tile.id, "type");
        } else if (cJSON_IsString(entity)) {
            tile.kind = core::kind_of_entity(entity->valuestring);
        } else {
            return fail(tile.id, "entity");
        }

        const cJSON *tiles = nullptr;
        core::Span grid;
        bool has_grid = false;
        for (const cJSON *item = object->child; item != nullptr; item = item->next) {
            const std::string_view key = item->string;
            bool ok = true;
            if (key == "id" || key == "type" || key == "cells") {
                continue;
            } else if (key == "entity") {
                ok = cJSON_IsString(item);
                if (ok) {
                    tile.entity = layout.intern(item->valuestring);
                }
            } else if (key == "name" || key == "icon" || key == "unit") {
                ok = cJSON_IsString(item);
                if (ok) {
                    (key == "name" ? tile.name : key == "icon" ? tile.icon : tile.unit) = item->valuestring;
                }
            } else if (key == "dimmer") {
                ok = cJSON_IsBool(item);
                tile.dimmer = cJSON_IsTrue(item);
            } else if (key == "swipe") {
                ok = cJSON_IsBool(item);
                tile.swipe = cJSON_IsTrue(item);
            } else if (key == "landscape") {
                ok = placement(item, tile.in(core::Orientation::Landscape));
            } else if (key == "portrait") {
                ok = placement(item, tile.in(core::Orientation::Portrait));
            } else if (key == "step") {
                ok = cJSON_IsNumber(item) && item->valuedouble > 0;
                tile.step = static_cast<float>(item->valuedouble);
            } else if (key == "decimals") {
                ok = cJSON_IsNumber(item) && item->valueint >= 0 && item->valueint <= 99;
                tile.decimals = static_cast<int8_t>(item->valueint);
            } else if (key == "refresh") {
                ok = cJSON_IsNumber(item) && item->valueint >= 1 && item->valueint <= 65535;
                tile.refresh_s = static_cast<uint16_t>(item->valueint);
            } else if (key == "stream") {
                ok = cJSON_IsString(item);
                if (ok) {
                    tile.stream = item->valuestring;
                }
            } else if (key == "grid" && tile.kind == TileKind::Area) {
                ok = span(item, grid);
                has_grid = true;
            } else if (key == "tiles" && tile.kind == TileKind::Area) {
                ok = cJSON_IsArray(item);
                tiles = item;
            } else {
                ok = false;
            }
            if (!ok) {
                return fail(tile.id, item->string);
            }
        }

        const TileIndex index = layout.add_tile(page, std::move(tile));
        if (layout.tiles[index].kind != TileKind::Area) {
            return true;
        }
        const PageIndex inner = layout.add_page(page, index);
        layout.pages[inner].has_grid = has_grid;
        layout.pages[inner].grid = grid;
        if (tiles == nullptr) {
            return fail(layout.tiles[index].id, "tiles");
        }
        return list(tiles, inner, layout);
    }

    memory::String _error;
    memory::Vector<uint16_t> _path;         // where the reader is: an index per level of tile lists
    memory::Vector<uint16_t> _problem_path; // where the problem is
};

void write_tile(formats::JsonWriter &json, const core::Layout &layout, const Tile &tile, bool with_cells)
{
    json.begin_object();
    json.number("id", tile.id);
    if (tile.kind == TileKind::Area || tile.kind == TileKind::Spacer) {
        json.string("type", core::kind_name(tile.kind));
    } else {
        const std::string_view entity = layout.entity_id(tile);
        json.string("entity", entity.data(), entity.size());
    }
    if (!tile.name.empty()) {
        json.string("name", tile.name.c_str());
    }
    if (tile.dimmer) {
        json.boolean("dimmer", true);
    }
    if (tile.swipe) {
        json.boolean("swipe", true);
    }
    if (!tile.icon.empty()) {
        json.string("icon", tile.icon.c_str());
    }
    for (const core::Orientation o : core::kOrientations) {
        const core::Placement &p = tile.in(o);
        if (!p.has_size && !p.has_cell && !p.hidden) {
            continue;
        }
        json.begin_object(o == core::Orientation::Landscape ? "landscape" : "portrait");
        if (p.has_size) {
            json.begin_array("size").number(nullptr, p.size.cols).number(nullptr, p.size.rows).end_array();
        }
        if (p.has_cell) {
            json.begin_array("cell").number(nullptr, p.cell.col).number(nullptr, p.cell.row).end_array();
        }
        if (p.hidden) {
            json.boolean("hidden", true);
        }
        json.end_object();
    }
    if (tile.step != 0) {
        json.decimal("step", tile.step);
    }
    if (!tile.unit.empty()) {
        json.string("unit", tile.unit.c_str());
    }
    if (tile.decimals >= 0) {
        json.number("decimals", tile.decimals);
    }
    if (tile.refresh_s != 0) {
        json.number("refresh", tile.refresh_s);
    }
    if (!tile.stream.empty()) {
        json.string("stream", tile.stream.c_str());
    }
    if (with_cells) {
        // where the device draws the tile; null: not drawn in that orientation
        json.begin_object("cells");
        for (const core::Orientation o : core::kOrientations) {
            const char *name = o == core::Orientation::Landscape ? "landscape" : "portrait";
            const core::Placement &p = tile.in(o);
            if (p.shown) {
                json.begin_array(name).number(nullptr, p.cell.col).number(nullptr, p.cell.row).end_array();
            } else {
                json.null(name);
            }
        }
        json.end_object();
    }
    if (tile.kind == TileKind::Area) {
        const core::Page &page = layout.pages[tile.opens];
        if (page.has_grid) {
            json.begin_array("grid").number(nullptr, page.grid.cols).number(nullptr, page.grid.rows).end_array();
        }
        json.begin_array("tiles");
        for (const TileIndex child : page.tiles) {
            write_tile(json, layout, layout.tiles[child], with_cells);
        }
        json.end_array();
    }
    json.end_object();
}

void write_layout(formats::JsonWriter &json, const core::Layout &layout, bool with_cells)
{
    json.begin_object();
    json.string("format", kFormat);
    json.number("version", kVersion);
    json.number("revision", layout.revision);
    json.number("next_id", layout.next_id);
    json.begin_object("defaults");
    json.begin_array("grid").number(nullptr, layout.grid.cols).number(nullptr, layout.grid.rows).end_array();
    json.end_object();
    json.begin_array("tiles");
    for (const TileIndex index : layout.pages[0].tiles) {
        write_tile(json, layout, layout.tiles[index], with_cells);
    }
    json.end_array();
    json.end_object();
}

} // namespace

std::expected<core::Layout, JsonProblem> read_json_at(std::string_view text)
{
    return Reader().run(text);
}

std::expected<core::Layout, memory::String> read_json(std::string_view text)
{
    auto layout = read_json_at(text);
    if (!layout) {
        return std::unexpected(std::move(layout.error().message));
    }
    return std::move(*layout);
}

memory::String write_json(const core::Layout &layout, bool with_cells)
{
    // once to measure, once into a buffer of that size
    formats::JsonWriter measure(nullptr, 0);
    write_layout(measure, layout, with_cells);
    memory::String out(measure.needed(), '\0');
    formats::JsonWriter json(out.data(), out.size() + 1);
    write_layout(json, layout, with_cells);
    return out;
}

memory::Vector<uint16_t> tile_path(const core::Layout &layout, core::TileIndex tile)
{
    memory::Vector<uint16_t> path;
    for (core::TileIndex at = tile; at != core::kNone;) {
        const core::Page &page = layout.pages[layout.tiles[at].page];
        uint16_t position = 0;
        while (position < page.tiles.size() && page.tiles[position] != at) {
            position++;
        }
        path.insert(path.begin(), position);
        at = page.area; // kNone on the main page
    }
    return path;
}

} // namespace hc::config
