// The motion sensor of a board and the entity that shows its state (docs/architecture.md section
// 9.7b): the call that switches input_boolean.<device>_motion, and the template that tells whether
// Home Assistant has it.

#include <cstring>

#include "cJSON.h"
#include "hc/formats/json_writer.hpp"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass_protocol {

memory::String motion_entity_id(std::string_view device_name)
{
    memory::String id = "input_boolean.";
    id += device_entity_name(device_name);
    id += "_motion";
    return id;
}

memory::String motion_message(uint32_t id, std::string_view device_name, bool detected)
{
    const memory::String entity_id = motion_entity_id(device_name);
    const auto body = [&](formats::JsonWriter &w) {
        w.begin_object()
            .number("id", id)
            .string("type", "call_service")
            .string("domain", "input_boolean")
            .string("service", detected ? "turn_on" : "turn_off")
            .begin_object("target")
            .string("entity_id", entity_id.c_str())
            .end_object()
            .end_object();
    };
    // written twice: once to measure it, once into a string of that size
    formats::JsonWriter measure(nullptr, 0);
    body(measure);
    memory::String out(measure.needed(), '\0');
    formats::JsonWriter writer(out.data(), out.size() + 1);
    body(writer);
    return out;
}

memory::String motion_template(std::string_view device_name)
{
    memory::String text = "{{ states('";
    text += motion_entity_id(device_name);
    text += "') }}";
    return text;
}

bool motion_helper_exists(const cJSON *result)
{
    const char *state = cJSON_GetStringValue(result);
    return state != nullptr && std::strcmp(state, "unknown") != 0;
}

} // namespace hc::hass_protocol
