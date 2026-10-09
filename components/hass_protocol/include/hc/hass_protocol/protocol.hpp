#pragma once

// What goes over the WebSocket of Home Assistant, as text (docs/architecture.md sections 5 and 6).
// Plain C++ with cJSON, host tested; the hass task sends and receives it.
//
// The visible page is one `render_template` subscription. Its template renders one compact JSON
// object: the key of an entity is its position in the subscription, the keys of its values are the
// one or two letter codes of the field table, so a full page stays small:
//
//   {"0":{"s":"on","b":180},"1":{"s":"heat","t":21.5,"c":20.8,"a":"heating"}}
//
// Which fields an entity gets follows from its tiles on the page (a light without `dimmer` does
// not need its brightness, a tile with its own name not the friendly_name). The fields of the open
// panel are added while it is open. Template and decoder read the same table, so they cannot
// drift apart.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"
#include "hc/memory/large.hpp"

struct cJSON;

namespace hc::hass_protocol {

// The fields a tile needs; `panel` adds the ones its panel shows.
core::FieldMask fields_of(const core::Tile &tile, bool panel);

// The subscription of one page: the entities in the order of their keys in the result.
struct PageSubscription {
    memory::Vector<core::EntityIndex> entities;
    memory::Vector<core::FieldMask> fields;
    memory::String template_text;

    bool empty() const
    {
        return entities.empty();
    }
};

// The template of `page`, with the panel fields of the tile `panel` (core::kNone: no panel open).
PageSubscription build_page(const core::Layout &layout, core::PageIndex page, core::TileIndex panel);

// Decodes the `result` of a render_template event (an object, or the same as a JSON string) into
// one update per entity of the subscription. False with `error` set when it is not the object
// the template renders.
bool decode_page(const cJSON *result, const PageSubscription &subscription, memory::Vector<core::EntityUpdate> &out,
                 memory::String &error);

// {"id":1,"type":"render_template","template":"...","report_errors":true}
memory::String render_template_message(uint32_t id, std::string_view template_text);

// {"id":2,"type":"unsubscribe_events","subscription":1}
memory::String unsubscribe_message(uint32_t id, uint32_t subscription);

// The call_service message of an intent for the entity `entity_id`. False with `error` set when
// the intent does not fit the entity's domain (a press of a light).
bool call_service_message(uint32_t id, const core::Intent &intent, std::string_view entity_id, memory::String &out,
                          memory::String &error);

// ------------------------------------------------------------------------- the entity picker

// the most rows of one entity list (docs/ui/web-dashboard.md section 2.3)
constexpr size_t kEntityListMax = 100;

// A render_template whose result is {"more": bool, "rows": [[id, name, domain, device_class,
// unit], ...]}: the entities of `domain` (empty: all) whose id or friendly name contains `query`
// (case-insensitive, empty: all), in order of the id, at most kEntityListMax and about 10 KB.
memory::String entity_list_message(uint32_t id, std::string_view domain, std::string_view query);

// A render_template whose result is {"id", "name", "domain", "state", "icon", "device_class",
// "unit", "color_modes", "temp_step"} of `entity_id`, or {"id", "missing": true}. The id has to
// be a valid entity id (core::valid_entity_id).
memory::String entity_details_message(uint32_t id, std::string_view entity_id);

// The `result` of a render_template event as JSON text: an object or a list, or the same as a
// JSON string. False when it is neither.
bool template_result_json(const cJSON *result, memory::String &out);

// ------------------------------------------------------------------------------- history

// The history of a sensor panel is read in windows this long, one after the other: a 5 minute
// bucket of the statistics is about 60 bytes, so 6 hours stay far below the 16 KB a message may
// have, and a busy motion sensor's state changes too.
constexpr uint32_t kHistoryChunkSeconds = 6 * 3600;

// "2026-10-07T12:00:00+00:00"
void iso_time(uint32_t epoch, char *out, size_t size);

// The request of [start, end) of one entity: `recorder/statistics_during_period` with the 5 minute
// means, or `history/history_during_period` with the state changes (minimal, no attributes).
memory::String history_message(uint32_t id, core::HistoryKind kind, std::string_view entity_id, uint32_t start,
                               uint32_t end);

// Appends the points of the `result` of such a request to `out`. No list for the entity is no
// data in the window, not an error.
bool decode_history(const cJSON *result, core::HistoryKind kind, std::string_view entity_id,
                    memory::Vector<core::HistoryPoint> &out, memory::String &error);

} // namespace hc::hass_protocol
