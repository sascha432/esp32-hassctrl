// The YAML export of the dashboard: one file with `defaults:` and `tiles:`, the dashboard part of
// the backup (docs/architecture.md section 5, "YAML import and export"). The import reads it back
// to the same layout; ids are not written, the import gives new ones in the same order.

#include <algorithm>

#include "hc/config/dashboard.hpp"
#include "hc/formats/yaml.hpp"
#include "values.hpp"

namespace hc::config {

namespace {

using core::Tile;
using core::TileIndex;
using core::TileKind;

// A stream's address without the password of its login: http://user:secret@host/ becomes
// http://user@host/. No secrets in an export; after an import the stream says "password missing".
memory::String without_password(std::string_view url)
{
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string_view::npos ? 0 : scheme + 3;
    const size_t end = std::min(url.find('/', start), url.size());
    const size_t at = url.rfind('@', end);
    if (at == std::string_view::npos || at < start) {
        return memory::String(url);
    }
    const size_t colon = url.find(':', start);
    if (colon == std::string_view::npos || colon > at) {
        return memory::String(url);
    }
    memory::String out(url.substr(0, colon));
    out += url.substr(at);
    return out;
}

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

    // a key of a map inside a tile, one level deeper than the tile's keys
    void sub(int indent, const char *key, std::string_view value)
    {
        _out.append(static_cast<size_t>(indent + 4), ' ');
        _out += key;
        _out += ": ";
        _out.append(value.data(), value.size());
        _out += '\n';
    }

    void tile(const Tile &tile, int indent)
    {
        bool first = true;
        const auto put = [&](const char *key, std::string_view value) {
            line(indent, first, key, value);
            first = false;
        };
        if (!core::has_entity(tile.kind)) {
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
        if (tile.swipe) {
            put("swipe", "true");
        }
        if (!tile.icon.empty()) {
            put("icon", formats::yaml::quote(tile.icon));
        }
        char text[24];
        for (const core::Orientation o : core::kOrientations) {
            const core::Placement &p = tile.in(o);
            if (!p.has_size && !p.has_cell && !p.hidden) {
                continue;
            }
            put(o == core::Orientation::Landscape ? "landscape" : "portrait", {});
            char value[8];
            if (p.has_size) {
                detail::format_span(p.size, value);
                sub(indent, "size", value);
            }
            if (p.has_cell) {
                detail::format_cell(p.cell, value);
                sub(indent, "cell", value);
            }
            if (p.hidden) {
                sub(indent, "hidden", "true");
            }
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
            put("stream", formats::yaml::quote(without_password(tile.stream)));
        }
        if (tile.hours12) {
            put("hours", "12");
        }
        if (tile.seconds) {
            put("seconds", "true");
        }
        if (tile.date) {
            put("date", "true");
        }
        if (tile.style != core::ClockStyle::Digital) {
            put("style", core::clock_style_name(tile.style));
        }
        if (tile.border) {
            put("border", "true");
        }
        if (tile.ticks != core::ClockTicks::Hour) {
            put("ticks", core::clock_ticks_name(tile.ticks));
        }
        if (!tile.zone.empty()) {
            put("zone", formats::yaml::quote(tile.zone));
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
