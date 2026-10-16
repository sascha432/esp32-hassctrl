// The YAML import of the dashboard (docs/architecture.md section 5, "YAML import and export").

#include <cstdarg>
#include <cstdio>

#include "hc/config/dashboard.hpp"
#include "hc/formats/yaml.hpp"
#include "values.hpp"

namespace hc::config {

namespace {

namespace yaml = formats::yaml;
using core::PageIndex;
using core::Tile;
using core::TileIndex;
using core::TileKind;

constexpr uint8_t kMaxIncludeDepth = 4;

memory::String format(const char *pattern, ...)
{
    char text[200];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(text, sizeof(text), pattern, args);
    va_end(args);
    return memory::String(text);
}

// One file being read, and the include entry that named it.
struct Frame {
    const File *file;
    int parent;          // index into the frames, -1 for the main file
    uint32_t parent_line; // the line of the include entry in the parent
};

// Where a tile came from.
struct Origin {
    int frame;
    uint32_t line;
};

class Importer {
public:
    Importer(std::span<const File> files) : _files(files)
    {
    }

    std::expected<Imported, memory::String> run(std::string_view entry)
    {
        const File *file = find(entry);
        if (file == nullptr) {
            return std::unexpected(format("%.*s is not in the uploaded files", static_cast<int>(entry.size()), entry.data()));
        }
        _frames.push_back({file, -1, 0});
        yaml::Node tree;
        if (!parse(0, tree)) {
            return std::unexpected(_error);
        }
        const yaml::Node *root = &tree;
        if (root->kind != yaml::Kind::Map) {
            fail(0, root->line, "the main file is a map with defaults: and tiles:");
            return std::unexpected(_error);
        }
        Imported result;
        for (const auto &entry_ : root->map) {
            if (entry_.key == "defaults") {
                if (!defaults(entry_.value, result.layout)) {
                    return std::unexpected(_error);
                }
            } else if (entry_.key == "tiles") {
                _tiles_line = entry_.line;
            } else {
                result.other_keys.push_back(entry_.key);
            }
        }
        const yaml::Node *tiles = root->find("tiles");
        if (tiles == nullptr || tiles->kind != yaml::Kind::List) {
            fail(0, tiles != nullptr ? tiles->line : root->line, "tiles: is a list of tiles");
            return std::unexpected(_error);
        }
        if (!list(*tiles, 0, 0, result.layout)) {
            return std::unexpected(_error);
        }
        if (auto finished = core::finish(result.layout); !finished) {
            report(finished.error(), result.layout);
            return std::unexpected(_error);
        }
        return result;
    }

private:
    const File *find(std::string_view path) const
    {
        for (const auto &file : _files) {
            if (file.path == path) {
                return &file;
            }
        }
        return nullptr;
    }

    // The tree of a frame's file. The caller keeps it while its tiles are read, so a tree lives
    // only as long as it is needed; a file that is included twice is parsed twice.
    bool parse(int frame, yaml::Node &out)
    {
        auto parsed = yaml::parse(_frames[frame].file->text);
        if (!parsed) {
            return fail(frame, parsed.error().line, parsed.error().message.c_str());
        }
        out = std::move(*parsed);
        return true;
    }

    bool fail(int frame, uint32_t line, const char *message)
    {
        const File &file = *_frames[frame].file;
        _error = format("%.*s:%u: %s", static_cast<int>(file.path.size()), file.path.data(), static_cast<unsigned>(line),
                        message);
        for (int f = frame; _frames[f].parent >= 0; f = _frames[f].parent) {
            const File &parent = *_frames[_frames[f].parent].file;
            _error += f == frame ? " (included from " : ", included from ";
            _error += format("%.*s:%u", static_cast<int>(parent.path.size()), parent.path.data(),
                             static_cast<unsigned>(_frames[f].parent_line));
        }
        if (_frames[frame].parent >= 0) {
            _error += ")";
        }
        return false;
    }

    void report(const core::Problem &problem, const core::Layout &layout)
    {
        if (problem.tile != core::kNone) {
            const Origin &origin = _origins[problem.tile];
            fail(origin.frame, origin.line, problem.message.c_str());
        } else if (problem.page != core::kNone && problem.page != 0) {
            const Origin &origin = _origins[layout.pages[problem.page].area];
            fail(origin.frame, origin.line, problem.message.c_str());
        } else {
            fail(0, _tiles_line, problem.message.c_str());
        }
    }

    bool grid(const yaml::Node &node, int frame, core::Span &out)
    {
        if (node.kind != yaml::Kind::Scalar || !detail::parse_span(node.text, out)) {
            return fail(frame, node.line, "grid is columns x rows, for example 4x3");
        }
        return true;
    }

    bool defaults(const yaml::Node &node, core::Layout &layout)
    {
        if (node.kind != yaml::Kind::Map) {
            return fail(0, node.line, "defaults: is a map");
        }
        for (const auto &entry : node.map) {
            if (entry.key == "grid") {
                if (!grid(entry.value, 0, layout.grid)) {
                    return false;
                }
            } else {
                return fail(0, entry.line, format("unknown key '%s' in defaults", entry.key.c_str()).c_str());
            }
        }
        return true;
    }

    bool list(const yaml::Node &node, PageIndex page, int frame, core::Layout &layout)
    {
        for (const auto &item : node.list) {
            if (item.kind != yaml::Kind::Map) {
                return fail(frame, item.line, "a tile is a map of keys");
            }
            if (const yaml::Node *path = item.find("include"); path != nullptr) {
                if (!include(item, *path, page, frame, layout)) {
                    return false;
                }
            } else if (!tile(item, page, frame, layout)) {
                return false;
            }
        }
        return true;
    }

    bool include(const yaml::Node &item, const yaml::Node &path, PageIndex page, int frame, core::Layout &layout)
    {
        if (item.map.size() != 1) {
            return fail(frame, item.line, "an include entry has no other keys");
        }
        if (path.kind != yaml::Kind::Scalar || path.text.empty()) {
            return fail(frame, item.line, "include: names a file");
        }
        int depth = 0;
        for (int f = frame; _frames[f].parent >= 0; f = _frames[f].parent) {
            depth++;
        }
        if (depth >= kMaxIncludeDepth) {
            return fail(frame, item.line, "includes are nested more than 4 levels deep");
        }
        memory::String target;
        if (!resolve(_frames[frame].file->path, path.text, target)) {
            return fail(frame, item.line, "the include leaves the uploaded files");
        }
        const File *file = find(target);
        if (file == nullptr) {
            return fail(frame, item.line, format("%s is not in the uploaded files", target.c_str()).c_str());
        }
        for (int f = frame; f >= 0; f = _frames[f].parent) {
            if (_frames[f].file == file) {
                return fail(frame, item.line, format("%s includes itself", target.c_str()).c_str());
            }
        }
        _frames.push_back({file, frame, item.line});
        const int child = static_cast<int>(_frames.size() - 1);
        yaml::Node root;
        if (!parse(child, root)) {
            return false;
        }
        if (root.kind != yaml::Kind::List) {
            return fail(child, root.line, "an included file is a list of tiles");
        }
        return list(root, page, child, layout);
    }

    // `path` relative to the folder of `from`; false when it leaves the set
    static bool resolve(std::string_view from, std::string_view path, memory::String &out)
    {
        if (path.empty() || path.front() == '/' || path.front() == '\\' || path.find(':') != std::string_view::npos) {
            return false;
        }
        memory::Vector<std::string_view> parts;
        const size_t slash = from.find_last_of('/');
        std::string_view folder = slash == std::string_view::npos ? std::string_view() : from.substr(0, slash);
        const auto split = [&parts](std::string_view text) {
            while (!text.empty()) {
                const size_t end = text.find_first_of("/\\");
                const std::string_view part = text.substr(0, end);
                if (part == "..") {
                    if (parts.empty()) {
                        return false;
                    }
                    parts.pop_back();
                } else if (!part.empty() && part != ".") {
                    parts.push_back(part);
                }
                text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
            }
            return true;
        };
        if (!split(folder) || !split(path) || parts.empty()) {
            return false;
        }
        out.clear();
        for (const auto part : parts) {
            if (!out.empty()) {
                out += '/';
            }
            out.append(part.data(), part.size());
        }
        return true;
    }

    // `type:` is only for the kinds without an entity; every other tile is made by its entity
    bool kind_of(const yaml::Node &item, std::string_view type, std::string_view entity, int frame, Tile &tile)
    {
        if (type == "area" || type == "spacer" || type == "clock") {
            if (!entity.empty()) {
                return fail(frame, item.line, format("a%s %.*s has no entity", type == "area" ? "n" : "",
                                                     static_cast<int>(type.size()), type.data())
                                                  .c_str());
            }
            tile.kind = type == "area" ? TileKind::Area : type == "spacer" ? TileKind::Spacer : TileKind::Clock;
            return true;
        }
        if (!type.empty()) {
            return fail(frame, item.line,
                        "type: is only for area, spacer and clock, the entity decides what the tile is");
        }
        if (entity.empty()) {
            return fail(frame, item.line, "a tile needs an entity, or type: area, spacer or clock");
        }
        tile.kind = core::kind_of_entity(entity);
        return true;
    }

    // `landscape:` / `portrait:` of a tile: size, cell and hidden
    bool placement(const yaml::Node &node, int frame, core::Placement &out)
    {
        for (const auto &entry : node.map) {
            const std::string_view key = entry.key;
            const std::string_view text = entry.value.kind == yaml::Kind::Scalar ? std::string_view(entry.value.text)
                                                                                  : std::string_view();
            if (key == "size") {
                if (!detail::parse_span(text, out.size)) {
                    return fail(frame, entry.line, "size is columns x rows, for example 1x2");
                }
                out.has_size = true;
            } else if (key == "cell") {
                if (!detail::parse_cell(text, out.cell)) {
                    return fail(frame, entry.line, "cell is column,row from 0, for example 2,1");
                }
                out.has_cell = true;
            } else if (key == "hidden") {
                if (text != "true" && text != "false") {
                    return fail(frame, entry.line, "hidden is true or false");
                }
                out.hidden = text == "true";
            } else {
                return fail(frame, entry.line,
                            format("unknown key '%.*s' (landscape: and portrait: take size, cell and hidden)",
                                   static_cast<int>(key.size()), key.data())
                                .c_str());
            }
        }
        return true;
    }

    bool tile(const yaml::Node &item, PageIndex page, int frame, core::Layout &layout)
    {
        const auto scalar = [&item](const char *key) -> std::string_view {
            const yaml::Node *value = item.find(key);
            return value != nullptr && value->kind == yaml::Kind::Scalar ? std::string_view(value->text)
                                                                          : std::string_view();
        };
        Tile tile;
        if (!kind_of(item, scalar("type"), scalar("entity"), frame, tile)) {
            return false;
        }
        const yaml::Node *tiles = nullptr;
        const yaml::Node *area_grid = nullptr;
        for (const auto &entry : item.map) {
            const std::string_view key = entry.key;
            const yaml::Node &value = entry.value;
            if (key == "tiles" || key == "grid") {
                if (tile.kind != TileKind::Area) {
                    return fail(frame, entry.line, format("%.*s is only for areas", static_cast<int>(key.size()), key.data()).c_str());
                }
                (key == "tiles" ? tiles : area_grid) = &value;
                continue;
            }
            if (key == "landscape" || key == "portrait") {
                if (value.kind != yaml::Kind::Map) {
                    return fail(frame, entry.line,
                                format("%.*s: is a map with size, cell and hidden (version 2 of the format)",
                                       static_cast<int>(key.size()), key.data())
                                    .c_str());
                }
                if (!placement(value, frame, tile.in(key == "landscape" ? core::Orientation::Landscape
                                                                         : core::Orientation::Portrait))) {
                    return false;
                }
                continue;
            }
            if (key == "size" || key == "position") {
                return fail(frame, entry.line,
                            format("%.*s: is gone: landscape: and portrait: take size, cell and hidden (version 2 of the format)",
                                   static_cast<int>(key.size()), key.data())
                                .c_str());
            }
            if (value.kind != yaml::Kind::Scalar) {
                return fail(frame, entry.line, format("%.*s takes a single value", static_cast<int>(key.size()), key.data()).c_str());
            }
            const std::string_view text = value.text;
            if (key == "type") {
                continue; // read by kind_of()
            }
            if (key == "entity") {
                if (!core::valid_entity_id(text)) {
                    return fail(frame, entry.line,
                                format("'%.*s' is not an entity id (domain.name: lowercase letters, digits and _)",
                                       static_cast<int>(text.size()), text.data())
                                    .c_str());
                }
                tile.entity = layout.intern(text);
            } else if (key == "name") {
                tile.name.assign(text.data(), text.size());
            } else if (key == "icon") {
                tile.icon.assign(text.data(), text.size());
            } else if (key == "dimmer") {
                if (text != "true" && text != "false") {
                    return fail(frame, entry.line, "dimmer is true or false");
                }
                tile.dimmer = tile.dimmer || text == "true";
            } else if (key == "swipe") {
                if (text != "true" && text != "false") {
                    return fail(frame, entry.line, "swipe is true or false");
                }
                tile.swipe = text == "true";
            } else if (key == "step") {
                if (!detail::parse_decimal(text, tile.step) || tile.step == 0) {
                    return fail(frame, entry.line, "step is a number above 0, for example 0.5");
                }
            } else if (key == "unit") {
                tile.unit.assign(text.data(), text.size());
            } else if (key == "stream") {
                tile.stream.assign(text.data(), text.size());
            } else if (key == "decimals") {
                long decimals = 0;
                if (!detail::parse_whole(text, 99, decimals)) {
                    return fail(frame, entry.line, "decimals is a whole number");
                }
                tile.decimals = static_cast<int8_t>(decimals);
            } else if (key == "refresh") {
                long refresh = 0;
                if (!detail::parse_whole(text, 99999, refresh) || refresh == 0) {
                    return fail(frame, entry.line, "refresh is a number of seconds, 1 to 3600");
                }
                tile.refresh_s = static_cast<uint16_t>(refresh);
            } else if (key == "hours") {
                long hours = 0;
                if (!detail::parse_whole(text, 99, hours) || (hours != 12 && hours != 24)) {
                    return fail(frame, entry.line, "hours is 12 or 24");
                }
                tile.hours12 = hours == 12;
            } else if (key == "seconds" || key == "date" || key == "border") {
                if (text != "true" && text != "false") {
                    return fail(frame, entry.line, format("%.*s is true or false", static_cast<int>(key.size()), key.data()).c_str());
                }
                (key == "seconds" ? tile.seconds : key == "date" ? tile.date : tile.border) = text == "true";
            } else if (key == "style") {
                if (!core::clock_style_of(text, tile.style)) {
                    return fail(frame, entry.line, "style is digital or analog");
                }
            } else if (key == "ticks") {
                if (!core::clock_ticks_of(text, tile.ticks)) {
                    return fail(frame, entry.line, "ticks is none, quarter, hour or minute");
                }
            } else if (key == "zone") {
                tile.zone.assign(text.data(), text.size());
            } else {
                return fail(frame, entry.line, format("unknown tile key '%.*s'", static_cast<int>(key.size()), key.data()).c_str());
            }
        }

        const TileIndex index = layout.add_tile(page, std::move(tile));
        _origins.push_back({frame, item.line});
        if (layout.tiles[index].kind != TileKind::Area) {
            return true;
        }
        const PageIndex inner = layout.add_page(page, index);
        if (area_grid != nullptr) {
            core::Page &area_page = layout.pages[inner];
            if (!grid(*area_grid, frame, area_page.grid)) {
                return false;
            }
            area_page.has_grid = true;
        }
        if (tiles == nullptr || tiles->kind != yaml::Kind::List) {
            return fail(frame, item.line, "an area needs tiles: with a list of tiles");
        }
        return list(*tiles, inner, frame, layout);
    }

    std::span<const File> _files;
    memory::Vector<Frame> _frames;
    memory::Vector<Origin> _origins; // by tile index
    uint32_t _tiles_line = 1;
    memory::String _error;
};

} // namespace

std::expected<Imported, memory::String> import_yaml(std::span<const File> files, std::string_view entry)
{
    return Importer(files).run(entry);
}

} // namespace hc::config
