// The motion sensor of a board and the entity that shows its state (docs/architecture.md section
// 9.7b): the call that sets input_text.<device>_motion.

#include "hc/formats/json_writer.hpp"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass_protocol {

memory::String motion_entity_id(std::string_view device_name)
{
    memory::String id = "input_text.";
    id += device_entity_name(device_name);
    id += "_motion";
    return id;
}

memory::String motion_message(uint32_t id, std::string_view device_name, bool occupied)
{
    const memory::String entity_id = motion_entity_id(device_name);
    const auto body = [&](formats::JsonWriter &w) {
        w.begin_object()
            .number("id", id)
            .string("type", "call_service")
            .string("domain", "input_text")
            .string("service", "set_value")
            .begin_object("service_data")
            .string("value", occupied ? kMotionOccupied : kMotionClear)
            .end_object()
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

} // namespace hc::hass_protocol
