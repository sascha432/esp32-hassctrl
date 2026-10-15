// The relays of a board and the entities that switch them (docs/architecture.md section 9.7a): the
// template that finds the entities by their names, and the decoder of its result.

#include <cctype>
#include <cstdio>
#include <memory>
#include <string>

#include "cJSON.h"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass_protocol {

namespace {

struct JsonDeleter {
    void operator()(cJSON *json) const
    {
        cJSON_Delete(json);
    }
};

} // namespace

memory::String device_entity_name(std::string_view device_name)
{
    memory::String name;
    for (const char c : device_name) {
        name += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
    }
    return name;
}

memory::String relay_entity_prefix(std::string_view device_name)
{
    memory::String prefix = device_entity_name(device_name);
    prefix += "_relay_";
    return prefix;
}

// The result is written as JSON text, relay number -> state: {"1":"on","2":"off"}; Home Assistant
// parses it into an object. The entity ids are listed, not searched (states.<domain>): Home Assistant
// delays a template that watches a whole domain by up to a second, one that names its entities
// renders at once. An entity that does not exist reads "unknown" and is left out.
memory::String relay_template(std::string_view device_name)
{
    const memory::String prefix = relay_entity_prefix(device_name);
    memory::String text = "{% set ns = namespace(r=[]) %}{% for i in [";
    for (unsigned relay = 1; relay <= kMaxRelays; relay++) {
        for (const char *domain : {"input_boolean", "switch"}) {
            text += relay > 1 || domain[0] != 'i' ? ",'" : "'";
            text += domain;
            text += '.';
            text += prefix;
            text += std::to_string(relay).c_str();
            text += '\'';
        }
    }
    text += "] %}{% set v = states(i) %}{% if v != 'unknown' %}{% set ns.r = ns.r + ['\"' ~ i.split('_relay_')[1] ~ '\":\"' ~ v ~ '\"'] %}"
            "{% endif %}{% endfor %}{{ '{' ~ ns.r | join(',') ~ '}' }}";
    return text;
}

bool decode_relays(const cJSON *result, RelayStates &out)
{
    out = {};
    // Home Assistant parses a result that looks like JSON; older versions send the text
    std::unique_ptr<cJSON, JsonDeleter> parsed;
    if (cJSON_IsString(result)) {
        parsed.reset(cJSON_Parse(result->valuestring));
        result = parsed.get();
    }
    if (!cJSON_IsObject(result)) {
        return false;
    }
    char key[4];
    for (unsigned relay = 1; relay <= kMaxRelays; relay++) {
        std::snprintf(key, sizeof(key), "%u", relay);
        const char *state = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, key));
        if (state == nullptr) {
            continue;
        }
        out.seen |= static_cast<uint8_t>(1u << (relay - 1));
        // unavailable, unknown and everything else is off
        if (std::string_view(state) == "on") {
            out.on |= static_cast<uint8_t>(1u << (relay - 1));
        }
    }
    return true;
}

} // namespace hc::hass_protocol
