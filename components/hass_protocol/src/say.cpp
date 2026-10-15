// The hassctrl_say event and what follows from it: the device it names, the text to speech
// request and the address the sound is fetched from (docs/architecture.md section 9.6).

#include <algorithm>
#include <cctype>

#include "cJSON.h"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass_protocol {

namespace {

bool same_name(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// the event's `device`: missing (every device), a name or a list of names
bool for_us(const cJSON *device, std::string_view device_name)
{
    if (device == nullptr || cJSON_IsNull(device)) {
        return true;
    }
    if (const char *name = cJSON_GetStringValue(device)) {
        return same_name(name, device_name);
    }
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, device)
    {
        if (const char *name = cJSON_GetStringValue(item); name != nullptr && same_name(name, device_name)) {
            return true;
        }
    }
    return false;
}

const char *text_of(const cJSON *data, const char *key)
{
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, key));
    return text != nullptr ? text : "";
}

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.substr(0, prefix.size()) == prefix;
}

} // namespace

SayResult decode_say(const cJSON *event, std::string_view device_name, Say &out, memory::String &error)
{
    out = {};
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(event, "data");
    if (!for_us(cJSON_GetObjectItemCaseSensitive(data, "device"), device_name)) {
        return SayResult::NotForUs;
    }
    out.message = text_of(data, "message");
    out.engine = text_of(data, "engine");
    out.language = text_of(data, "language");
    out.url = text_of(data, "url");
    if (const cJSON *options = cJSON_GetObjectItemCaseSensitive(data, "options"); options != nullptr) {
        char *text = cJSON_IsObject(options) ? cJSON_PrintUnformatted(options) : nullptr;
        if (text == nullptr) {
            error = "options is not a map";
            return SayResult::Invalid;
        }
        out.options = text;
        cJSON_free(text);
    }
    if (out.message.empty() && out.url.empty()) {
        error = "neither a message nor a url";
        return SayResult::Invalid;
    }
    if (!out.message.empty() && out.engine.empty()) {
        error = "a message needs its engine (a tts entity)";
        return SayResult::Invalid;
    }
    return SayResult::Play;
}

memory::String tts_url_request(const Say &say)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "engine_id", say.engine.c_str());
    cJSON_AddStringToObject(body, "message", say.message.c_str());
    if (!say.language.empty()) {
        cJSON_AddStringToObject(body, "language", say.language.c_str());
    }
    // the engine's own options as they came; decode_say made sure they are a map
    cJSON *options = say.options.empty() ? nullptr : cJSON_Parse(say.options.c_str());
    if (!cJSON_IsObject(options)) {
        cJSON_Delete(options);
        options = cJSON_CreateObject();
    }
    // every engine takes the preferred_* options: HA converts after it (ffmpeg)
    cJSON_DeleteItemFromObjectCaseSensitive(options, "preferred_format");
    cJSON_DeleteItemFromObjectCaseSensitive(options, "preferred_sample_channels");
    cJSON_AddStringToObject(options, "preferred_format", "mp3");
    cJSON_AddNumberToObject(options, "preferred_sample_channels", 1);
    cJSON_AddItemToObject(body, "options", options);
    memory::String out;
    if (char *text = cJSON_PrintUnformatted(body)) {
        out = text;
        cJSON_free(text);
    }
    cJSON_Delete(body);
    return out;
}

bool decode_tts_url(const cJSON *answer, memory::String &path)
{
    if (const char *p = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(answer, "path")); p != nullptr && p[0] == '/') {
        path = p;
        return true;
    }
    // older versions answer with the whole address only
    const std::string_view url = text_of(answer, "url");
    const size_t scheme = url.find("://");
    const size_t slash = scheme == std::string_view::npos ? std::string_view::npos : url.find('/', scheme + 3);
    if (slash == std::string_view::npos) {
        return false;
    }
    path = memory::String(url.substr(slash));
    return true;
}

bool sound_url(std::string_view ha_url, std::string_view url, memory::String &out, bool &authorize)
{
    if (starts_with(url, "/")) {
        out = memory::String(ha_url);
        out += url;
        authorize = true;
        return true;
    }
    if (starts_with(url, "http://") || starts_with(url, "https://")) {
        out = memory::String(url);
        // the token goes to Home Assistant only; "http://h:8123" must not match "http://h:81234"
        authorize = starts_with(url, ha_url) && (url.size() == ha_url.size() || url[ha_url.size()] == '/');
        return true;
    }
    return false;
}

} // namespace hc::hass_protocol
