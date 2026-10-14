#pragma once

// The dashboard configuration (docs/architecture.md section 5, "Dashboard configuration"): the
// stored JSON file, the YAML import with its includes, and the YAML export. Plain C++, host
// tested.
//
// Every reader builds a core::Layout and passes it through core::finish(), so a layout from the
// web UI, from the file system or from an import is checked by the same rules. Errors are one
// line of text that names where the problem is: "rooms/office.yaml:12: unknown tile key 'nmae'
// (included from rooms.yaml:9)" for an import, "tile 17: ..." for the stored file.

#include <expected>
#include <span>
#include <string_view>

#include "hc/core/layout.hpp"
#include "hc/memory/large.hpp"

namespace hc::config {

// One file of an upload: its path inside the uploaded set ("rooms/office.yaml") and its text.
struct File {
    std::string_view path;
    std::string_view text;
};

struct Imported {
    core::Layout layout;
    // Top-level keys of the main file that are not the dashboard's ("hass", "network", ...).
    // The dashboard import does not read them; the caller hands them to their owners or names
    // them as ignored.
    memory::Vector<memory::String> other_keys;
};

// Reads `defaults` and `tiles` of the file `entry` and every file its includes name. Accepts the
// old format as well: `type:` of the old tile types (checked against the entity), the old icon
// names, `grid:` with `cols:` and `rows:`.
std::expected<Imported, memory::String> import_yaml(std::span<const File> files, std::string_view entry);

// The stored dashboard (/fs/dashboard.json), and the dashboard the web UI sends (tiles it added
// have no "id"; finish() gives them one).
std::expected<core::Layout, memory::String> read_json(std::string_view text);

// A problem of a dashboard in JSON: the text, and where the tile it is about stands (tile_path).
struct JsonProblem {
    memory::String message;
    memory::Vector<uint16_t> path; // empty: not about one tile
};
std::expected<core::Layout, JsonProblem> read_json_at(std::string_view text);

// `with_cells`: every tile also gets "cells": {"landscape": [column, row] or null, "portrait":
// the same}, where the device draws it (null: hidden, no room, or an area tile a swipe replaces);
// the answers of the web API, whose editor draws its preview from them. The reader skips "cells".
memory::String write_json(const core::Layout &layout, bool with_cells = false);

// Where a tile stands in the JSON: its index in the tile list of the main page, then in the list
// of that area, and so on down to the tile.
memory::Vector<uint16_t> tile_path(const core::Layout &layout, core::TileIndex tile);

// `defaults:` and `tiles:` as YAML, all in one file: the dashboard part of the backup export.
memory::String write_yaml(const core::Layout &layout);

} // namespace hc::config
