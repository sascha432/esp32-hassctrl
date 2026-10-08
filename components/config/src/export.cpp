// The YAML export of the dashboard: one file with `defaults:` and `tiles:`, the dashboard part of
// the backup (docs/architecture.md section 5, "YAML import and export"). The import reads it back
// to the same layout; ids are not written, the import gives new ones in the same order.

#include "hc/config/dashboard.hpp"
#include "hc/formats/yaml.hpp"
#include "values.hpp"

namespace hc::config {

namespace {

using core::Tile;
using core::TileIndex;
using core::TileKind;

class Writer {
public:
    explicit Writer(const core::Layout &layout) : _layout(layout)
    {
    }

    memory::String run()
    {
        char span[8];
        detail::format_span(_layout.grid, span);
        _out += "defaults:\n  grid: ";
        _out += span;
        _out += "\n\ntiles:\n";
        for (const TileIndex index : _layout.pages[0].tiles) {
            tile(_layout.tiles[index], 2);
        }
        return std::move(_out);
    }

private:
    void line(int indent, bool first, const char *key, std::string_view value)
    {
        _out.append(static_cast<size_t>(indent), ' ');
        _out += first ? "- " : "  ";
        _out += key;
        _out += ':';
        if (!value.empty()) {
            _out += ' ';
            _out.append(value.data(), value.size());
        }
        _out += '\n';
    }

    void tile(const Tile &tile, int indent)
    {
        bool first = true;
        const auto put = [&](const char *key, std::string_view value) {
            line(indent, first, key, value);
            first = false;
        };
        if (tile.kind == TileKind::Area || tile.kind == TileKind::Spacer) {
            put("type", core::kind_name(tile.kind));
        } else {
            put("entity", _layout.entity_id(tile));
        }
        if (!tile.name.empty()) {
            put("name", formats::yaml::quote(tile.name));
        }
        if (tile.dimmer) {
            put("dimmer", "true");
        }
        if (!tile.icon.empty()) {
            put("icon", formats::yaml::quote(tile.icon));
        }
        char text[24];
        if (tile.has_size) {
            char span[8];
            detail::format_span(tile.span, span);
            put("size", span);
        }
        if (tile.step != 0) {
            detail::format_decimal(tile.step, text);
            put("step", text);
        }
        if (!tile.unit.empty()) {
            put("unit", formats::yaml::quote(tile.unit));
        }
        if (tile.decimals >= 0) {
            std::snprintf(text, sizeof(text), "%d", tile.decimals);
            put("decimals", text);
        }
        if (tile.refresh_s != 0) {
            std::snprintf(text, sizeof(text), "%u", static_cast<unsigned>(tile.refresh_s));
            put("refresh", text);
        }
        if (!tile.stream.empty()) {
            put("stream", formats::yaml::quote(tile.stream));
        }
        if (tile.kind == TileKind::Area) {
            const core::Page &page = _layout.pages[tile.opens];
            if (page.has_grid) {
                char span[8];
                detail::format_span(page.grid, span);
                put("grid", span);
            }
            put("tiles", {});
            for (const TileIndex child : page.tiles) {
                this->tile(_layout.tiles[child], indent + 4);
            }
        }
    }

    const core::Layout &_layout;
    memory::String _out;
};

} // namespace

memory::String write_yaml(const core::Layout &layout)
{
    return Writer(layout).run();
}

} // namespace hc::config
