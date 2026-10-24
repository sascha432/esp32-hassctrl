// Home Assistant's media library (docs/architecture.md section 9.6): browsed folder by folder,
// a file resolved to an address the player streams from.

#include "cJSON.h"
#include "hc/formats/json_writer.hpp"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass_protocol {

namespace {

using formats::JsonWriter;

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

memory::String media_message(uint32_t id, const char *type, std::string_view media_id)
{
    return write([&](JsonWriter &w) {
        w.begin_object().number("id", id).string("type", type);
        if (!media_id.empty()) {
            w.string("media_content_id", media_id.data(), media_id.size());
        }
        w.end_object();
    });
}

const char *text(const cJSON *object, const char *key)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
    return value != nullptr ? value : "";
}

} // namespace

memory::String browse_media_message(uint32_t id, std::string_view media_id)
{
    return media_message(id, "media_source/browse_media", media_id);
}

memory::String resolve_media_message(uint32_t id, std::string_view media_id)
{
    return media_message(id, "media_source/resolve_media", media_id);
}

bool decode_browse_media(const cJSON *result, core::MediaList &out, memory::String &error)
{
    out.items.clear();
    if (!cJSON_IsObject(result)) {
        error = "the answer is not a folder";
        return false;
    }
    out.title = text(result, "title");
    const cJSON *children = cJSON_GetObjectItemCaseSensitive(result, "children");
    if (children != nullptr && !cJSON_IsArray(children)) {
        error = "the folder's children are not a list";
        return false;
    }
    const cJSON *child = nullptr;
    cJSON_ArrayForEach(child, children)
    {
        core::MediaItem item;
        item.id = text(child, "media_content_id");
        if (item.id.empty()) {
            continue;
        }
        item.title = text(child, "title");
        item.type = text(child, "media_content_type");
        item.folder = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(child, "can_expand"));
        item.playable = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(child, "can_play"));
        out.items.push_back(std::move(item));
    }
    return true;
}

bool decode_resolve_media(const cJSON *result, memory::String &url, memory::String &mime, memory::String &error)
{
    url = text(result, "url");
    mime = text(result, "mime_type");
    if (url.empty()) {
        error = "Home Assistant gave no address for it";
        return false;
    }
    return true;
}

} // namespace hc::hass_protocol
