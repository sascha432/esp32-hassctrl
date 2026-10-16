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

// The template of `page` as shown in `o` (the tiles hidden or without room there are left out),
// with the panel fields of the tile `panel` (core::kNone: no panel open).
PageSubscription build_page(const core::Layout &layout, core::PageIndex page, core::Orientation o, core::TileIndex panel);

// Decodes the `result` of a render_template event (an object, or the same as a JSON string) into
// one update per entity of the subscription. False with `error` set when it is not the object
// the template renders.
bool decode_page(const cJSON *result, const PageSubscription &subscription, memory::Vector<core::EntityUpdate> &out,
                 memory::String &error);

// {"id":1,"type":"render_template","template":"...","report_errors":true}
memory::String render_template_message(uint32_t id, std::string_view template_text);

// {"id":2,"type":"unsubscribe_events","subscription":1}
memory::String unsubscribe_message(uint32_t id, uint32_t subscription);

// {"id":3,"type":"weather/subscribe_forecast","entity_id":"weather.home","forecast_type":"daily"}:
// Home Assistant answers with an empty result, then an event with the forecast now and with every
// update of the integration. It is ended with unsubscribe_message(). `type` Daily or Hourly.
memory::String forecast_subscribe_message(uint32_t id, std::string_view entity_id, core::ForecastType type);

// Decodes the `event` of that subscription ({"type":"daily","forecast":[...]}) into the first
// core::kMaxForecastEntries entries. A null forecast is no entries.
bool decode_forecast(const cJSON *event, memory::Vector<core::ForecastEntry> &out, memory::String &error);

// "2026-10-13T20:00:00+00:00" (also with fractions of a second, "Z" and +HHMM) -> epoch seconds
bool parse_iso_time(const char *text, uint32_t &epoch);

// The call_service message of an intent for the entity `entity_id`. False with `error` set when
// the intent does not fit the entity's domain (a press of a light).
bool call_service_message(uint32_t id, const core::Intent &intent, std::string_view entity_id, memory::String &out,
                          memory::String &error);

// ------------------------------------------------------------------------- the entity picker

// the most rows of one entity list (docs/ui/web/web-dashboard-editor.md section 3)
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
// have, and a busy motion sensor's state changes too. A climate's states come with their
// attributes (about 300 bytes each, a thermostat reports every few minutes): one hour at a time.
constexpr uint32_t kHistoryChunkSeconds = 6 * 3600;
constexpr uint32_t kTemperatureChunkSeconds = 3600;
inline uint32_t history_chunk_seconds(core::HistoryKind kind)
{
    return kind == core::HistoryKind::Temperature ? kTemperatureChunkSeconds : kHistoryChunkSeconds;
}

// "2026-10-07T12:00:00+00:00"
void iso_time(uint32_t epoch, char *out, size_t size);

// The request of [start, end) of one entity: `recorder/statistics_during_period` with the 5 minute
// means, or `history/history_during_period` with the state changes (minimal, no attributes; with
// the attributes for Temperature, which reads `current_temperature`).
memory::String history_message(uint32_t id, core::HistoryKind kind, std::string_view entity_id, uint32_t start,
                               uint32_t end);

// Appends the points of the `result` of such a request to `out`. No list for the entity is no
// data in the window, not an error.
bool decode_history(const cJSON *result, core::HistoryKind kind, std::string_view entity_id,
                    memory::Vector<core::HistoryPoint> &out, memory::String &error);

// --------------------------------------------------------------------- sounds (hassctrl_say)

// The event Home Assistant fires to make a device speak or play a sound (docs/architecture.md
// section 9.6): its data names the device (optional, the host name or a list of them), and a
// `message` for text to speech with its `engine` (a tts entity), `language` and the engine's
// `options` (both optional), or a `url` of an MP3: a path of Home Assistant ("/local/door.mp3") or a whole http(s) address.
constexpr const char *kSayEvent = "hassctrl_say";

// {"id":9,"type":"subscribe_events","event_type":"hassctrl_say"}
memory::String subscribe_events_message(uint32_t id, std::string_view event_type);

struct Say {
    memory::String message;
    memory::String engine;
    memory::String language;
    // the event's `options` as JSON text ("{\"tld\":\"co.uk\"}"), empty: none
    memory::String options;
    memory::String url;
};

enum class SayResult : uint8_t {
    Play,     // for this device: `out` holds what to play
    NotForUs, // it names other devices
    Invalid,  // `error` says what is missing
};

// Decodes the `event` of the subscription ({"event_type":..., "data":{...}}) for the device
// `device_name` (compared without case).
SayResult decode_say(const cJSON *event, std::string_view device_name, Say &out, memory::String &error);

// The body of POST /api/tts_get_url: the message with its engine, language and options, as a mono
// MP3 (HA converts what the engine makes): `preferred_format` and `preferred_sample_channels` are
// always the device's, whatever the options say.
memory::String tts_url_request(const Say &say);

// ------------------------------------------------------------------------------------ relays

// The relays of a board (docs/architecture.md section 9.7a) are switched by entities found by
// name: input_boolean.<device>_relay_<n> or switch.<device>_relay_<n>, <device> the host name
// with "-" as "_" ("hassctrl-7e7d": hassctrl_7e7d_relay_1).
constexpr unsigned kMaxRelays = 4;

// The device's name as the first part of its entity ids: "hassctrl_7e7d" for "hassctrl-7e7d"
// (letters and digits in lower case, anything else "_").
memory::String device_entity_name(std::string_view device_name);

// "hassctrl_7e7d_relay_" for "hassctrl-7e7d"
memory::String relay_entity_prefix(std::string_view device_name);

// The template of one render_template subscription: a JSON object of relay number -> state of
// the entities of the device ({"1":"on","2":"off"}), "{}" without any.
memory::String relay_template(std::string_view device_name);

// Bit n-1 of both: relay n. `seen`: an entity has this relay; `on`: its state is "on".
struct RelayStates {
    uint8_t seen = 0;
    uint8_t on = 0;
};

// Decodes the `result` of that subscription (an object, or the same as a JSON string); numbers
// beyond kMaxRelays are ignored. False when it is not an object.
bool decode_relays(const cJSON *result, RelayStates &out);

// ----------------------------------------------------------------------------- motion sensor

// The motion sensor of a board (docs/architecture.md section 9.7b) sets the text helper
// input_text.<device>_motion, which the user creates in Home Assistant, to one of these (the
// words of Home Assistant's own motion sensors).
constexpr const char *kMotionDetected = "Detected";
constexpr const char *kMotionClear = "Clear";

// "input_text.hassctrl_7e7d_motion" for "hassctrl-7e7d"
memory::String motion_entity_id(std::string_view device_name);

// The call_service message that sets the helper: input_text.set_value with the value for `detected`.
memory::String motion_message(uint32_t id, std::string_view device_name, bool detected);

// The path of the answer to it ({"url": "http://.../api/tts_proxy/x.mp3", "path": "/api/..."});
// the device fetches it from its own address of Home Assistant, not from the one HA thinks it has.
bool decode_tts_url(const cJSON *answer, memory::String &path);

// The address to fetch a sound from: a path is Home Assistant's (`ha_url` "http://h:8123"),
// fetched with the token; a whole address as it is, with the token only when it is Home
// Assistant's. False when it is neither.
bool sound_url(std::string_view ha_url, std::string_view url, memory::String &out, bool &authorize);

} // namespace hc::hass_protocol
