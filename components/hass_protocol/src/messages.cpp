// The commands the device sends: the subscription of a page, its end, and the service calls.

#include <algorithm>
#include <cmath>

#include "hc/formats/json_writer.hpp"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass_protocol {

namespace {

using formats::JsonWriter;

// Writes a message twice: once to measure it, once into a string of that size.
template <class Body>
memory::String write(Body body)
{
    JsonWriter measure(nullptr, 0);
    body(measure);
    memory::String out(measure.needed(), '\0');
    JsonWriter writer(out.data(), out.size() + 1);
    body(writer);
    return out;
}

std::string_view domain_of(std::string_view entity_id)
{
    return entity_id.substr(0, entity_id.find('.'));
}

bool one_of(std::string_view domain, std::initializer_list<std::string_view> domains)
{
    return std::ranges::find(domains, domain) != domains.end();
}

struct Call {
    const char *domain = nullptr;
    const char *service = nullptr;
};

// The service of an intent for a domain; an empty call when it does not fit.
Call service_of(const core::Intent &intent, std::string_view domain)
{
    using core::IntentKind;
    const bool light = domain == "light";
    const bool climate = domain == "climate";
    switch (intent.kind) {
    case IntentKind::Toggle:
        if (one_of(domain, {"light", "switch", "input_boolean", "fan", "automation"})) {
            return {nullptr, "toggle"};
        }
        break;
    case IntentKind::Press:
        if (one_of(domain, {"button", "input_button"})) {
            return {nullptr, "press"};
        }
        if (domain == "scene") {
            return {nullptr, "turn_on"};
        }
        break;
    case IntentKind::Run:
        if (domain == "automation" && intent.value == core::kRun) {
            return {"automation", "trigger"};
        }
        if (domain == "script") {
            return {"script", intent.value == core::kStop ? "turn_off" : "turn_on"};
        }
        break;
    case IntentKind::SetLevel:
        if (light) {
            return {"light", intent.value < 0.5f ? "turn_off" : "turn_on"};
        }
        break;
    case IntentKind::SetTemperature:
        if (climate) {
            return {"climate", "set_temperature"};
        }
        break;
    case IntentKind::SetHvacMode:
        if (climate) {
            return {"climate", "set_hvac_mode"};
        }
        break;
    case IntentKind::SetPreset:
        if (climate) {
            return {"climate", "set_preset_mode"};
        }
        break;
    case IntentKind::SetFanMode:
        if (climate) {
            return {"climate", "set_fan_mode"};
        }
        break;
    case IntentKind::SetLock:
        if (domain == "lock") {
            return {"lock", intent.value == core::kOpen ? "open" : intent.value == core::kUnlock ? "unlock" : "lock"};
        }
        break;
    case IntentKind::SetEffect:
    case IntentKind::SetColor:
    case IntentKind::SetColorTemp:
        if (light) {
            return {"light", "turn_on"};
        }
        break;
    case IntentKind::SetTimer:
        if (domain == "timer") {
            return {"timer", intent.value == core::kTimerPause    ? "pause"
                             : intent.value == core::kTimerCancel ? "cancel"
                             : intent.value == core::kTimerFinish ? "finish"
                                                                   : "start"};
        }
        break;
    }
    return {};
}

} // namespace

memory::String render_template_message(uint32_t id, std::string_view template_text)
{
    return write([&](JsonWriter &w) {
        w.begin_object()
            .number("id", id)
            .string("type", "render_template")
            .string("template", template_text.data(), template_text.size())
            .boolean("report_errors", true)
            .end_object();
    });
}

memory::String forecast_subscribe_message(uint32_t id, std::string_view entity_id, core::ForecastType type)
{
    return write([&](JsonWriter &w) {
        w.begin_object()
            .number("id", id)
            .string("type", "weather/subscribe_forecast")
            .string("entity_id", entity_id.data(), entity_id.size())
            .string("forecast_type", type == core::ForecastType::Hourly ? "hourly" : "daily")
            .end_object();
    });
}

memory::String unsubscribe_message(uint32_t id, uint32_t subscription)
{
    return write([&](JsonWriter &w) {
        w.begin_object()
            .number("id", id)
            .string("type", "unsubscribe_events")
            .number("subscription", subscription)
            .end_object();
    });
}

bool call_service_message(uint32_t id, const core::Intent &intent, std::string_view entity_id, memory::String &out,
                          memory::String &error)
{
    using core::IntentKind;
    const std::string_view domain = domain_of(entity_id);
    const Call call = service_of(intent, domain);
    if (call.service == nullptr) {
        error = "this action does not fit ";
        error.append(entity_id.data(), entity_id.size());
        return false;
    }
    out = write([&](JsonWriter &w) {
        w.begin_object().number("id", id).string("type", "call_service");
        if (call.domain != nullptr) {
            w.string("domain", call.domain);
        } else {
            w.string("domain", domain.data(), domain.size());
        }
        w.string("service", call.service);
        w.begin_object("service_data");
        switch (intent.kind) {
        case IntentKind::SetLevel:
            if (intent.value >= 0.5f) {
                w.number("brightness_pct", std::clamp<int64_t>(std::lround(intent.value), 1, 100));
            }
            break;
        case IntentKind::SetTemperature:
            w.decimal("temperature", std::round(intent.value * 10) / 10);
            break;
        case IntentKind::SetHvacMode:
            w.string("hvac_mode", intent.text.c_str());
            break;
        case IntentKind::SetPreset:
            w.string("preset_mode", intent.text.c_str());
            break;
        case IntentKind::SetFanMode:
            w.string("fan_mode", intent.text.c_str());
            break;
        case IntentKind::SetEffect:
            w.string("effect", intent.text.c_str());
            break;
        case IntentKind::SetColor:
            w.begin_array("hs_color")
                .decimal(nullptr, std::round(intent.value * 10) / 10)
                .decimal(nullptr, std::round(intent.value2 * 10) / 10)
                .end_array();
            break;
        case IntentKind::SetColorTemp:
            w.number("color_temp_kelvin", std::lround(intent.value));
            break;
        case IntentKind::Run:
            // the actions of an automation run whatever its conditions say, as "Run actions" in
            // Home Assistant does
            if (domain == "automation") {
                w.boolean("skip_condition", true);
            }
            break;
        case IntentKind::SetTimer:
            // a start with a duration of its own ("0:45:00"); without one the entity's, or the
            // rest of a paused timer
            if (intent.value == core::kTimerStart && intent.value2 > 0) {
                w.string("duration", core::duration_text(intent.value2).c_str());
            }
            break;
        default:
            break;
        }
        w.end_object();
        w.begin_object("target").string("entity_id", entity_id.data(), entity_id.size()).end_object();
        w.end_object();
    });
    return true;
}

memory::String subscribe_events_message(uint32_t id, std::string_view event_type)
{
    return write([&](JsonWriter &w) {
        w.begin_object()
            .number("id", id)
            .string("type", "subscribe_events")
            .string("event_type", event_type.data(), event_type.size())
            .end_object();
    });
}

} // namespace hc::hass_protocol
