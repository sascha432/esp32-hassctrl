#include "sensor_panel.hpp"

#include <string>

#include "dashboard.hpp"
#include "history_view.hpp"
#include "icons.hpp"
#include "theme.hpp"
#include "tiles.hpp"

namespace hc::ui::sensor_panel {

namespace {

namespace t = theme;
using panel::set_text;

struct State {
    panel::Frame frame;
    core::TileIndex tile = core::kNone;
    core::EntityIndex entity = core::kNone;
    core::HistoryKind kind = core::HistoryKind::Statistics;
    lv_obj_t *icon = nullptr;
    lv_obj_t *value = nullptr;
};

State g;

// on/off history for a binary sensor and anything that reports on or off, statistics else
core::HistoryKind kind_of(const core::EntityState &state, std::string_view entity_id)
{
    if (entity_id.substr(0, entity_id.find('.')) == "binary_sensor" || state.state == "on" || state.state == "off") {
        return core::HistoryKind::States;
    }
    return core::HistoryKind::Statistics;
}

void build_header()
{
    const lv_area_t &header = g.frame.header;
    const lv_coord_t height = lv_area_get_height(&header);
    g.icon = t::label(g.frame.root, "", icons::large(), t::kTextLabel);
    lv_obj_set_pos(g.icon, header.x1, static_cast<lv_coord_t>(header.y1 + (height - lv_font_get_line_height(&icons::large())) / 2));
    const lv_coord_t text_x = static_cast<lv_coord_t>(header.x1 + lv_font_get_line_height(&icons::large()) + 6);
    const lv_coord_t text_w = static_cast<lv_coord_t>(header.x2 + 1 - text_x);
    // the name above the value, both left aligned beside the icon
    const lv_coord_t name_h = lv_font_get_line_height(&t::font_medium());
    const lv_coord_t value_h = lv_font_get_line_height(&t::font_huge());
    const auto top = static_cast<lv_coord_t>(header.y1 + (height - name_h - value_h) / 2);
    lv_obj_set_style_text_font(g.frame.name, &t::font_medium(), 0);
    lv_obj_set_style_text_color(g.frame.name, t::color(t::kTextValue), 0);
    lv_obj_set_pos(g.frame.name, text_x, top);
    lv_obj_set_width(g.frame.name, text_w);
    g.value = t::label(g.frame.root, "", t::font_huge(), t::kText);
    lv_obj_set_pos(g.value, text_x, static_cast<lv_coord_t>(top + name_h));
    lv_obj_set_width(g.value, text_w);
    lv_label_set_long_mode(g.value, LV_LABEL_LONG_CLIP);
}

} // namespace

void build(const panel::Frame &frame, core::TileIndex tile)
{
    forget();
    g.frame = frame;
    g.tile = tile;
    const core::Layout &layout = dashboard::layout();
    g.entity = layout.tiles[tile].entity;
    const core::EntityState &s = dashboard::state(g.entity);
    g.kind = kind_of(s, layout.entity_id(layout.tiles[tile]));

    build_header();
    // the words of the device class ("Detected" / "Clear")
    history_view::Legend legend;
    legend.on = tiles::binary_words(s.device_class, true);
    legend.off = tiles::binary_words(s.device_class, false);
    history_view::build(frame.root, frame.control, tile, g.kind, legend);
}

void update()
{
    if (g.value == nullptr) {
        return;
    }
    const core::Layout &layout = dashboard::layout();
    const core::Tile &tile = layout.tiles[g.tile];
    const core::EntityState &s = dashboard::state(g.entity);
    const bool known = s.known != 0;
    const bool available = known && !s.unavailable();
    const bool on = s.is_on();
    const char *glyph = icons::for_tile(tile, layout.entity_id(tile), s, g.kind == core::HistoryKind::States ? on : true);
    set_text(g.icon, glyph != nullptr ? glyph : "");
    lv_obj_set_style_text_color(g.icon, t::color(g.kind == core::HistoryKind::States && on ? t::kActive : t::kTextLabel), 0);
    set_text(g.value, !known ? std::string("...") : !available ? std::string("--") : tiles::sensor_text(tile, layout.entity_id(tile), s));
    history_view::update();
}

void forget()
{
    history_view::forget();
    // the objects go with the panel's root
    g = {};
}

} // namespace hc::ui::sensor_panel
