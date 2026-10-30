#include "hc/player/kinds.hpp"

#include <cctype>
#include <cstring>

namespace hc::player {

namespace {

// strcasecmp is not on MSVC (the simulator)
bool has_extension(const char *name, const char *extension)
{
    const size_t length = std::strlen(name);
    const size_t wanted = std::strlen(extension);
    if (length <= wanted) {
        return false;
    }
    const char *end = name + length - wanted;
    for (size_t i = 0; i < wanted; i++) {
        if (std::tolower(static_cast<unsigned char>(end[i])) != extension[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

bool playable(const char *name)
{
    return has_extension(name, ".mp3") || has_extension(name, ".wav") || is_playlist(name);
}

bool is_playlist(const char *name)
{
    return has_extension(name, ".m3u") || has_extension(name, ".m3u8");
}

bool playable_media(const core::MediaItem &item)
{
    if (item.folder || !item.playable) {
        return false;
    }
    return item.type == "audio/mpeg" || item.type == "audio/mp3" || has_extension(item.title.c_str(), ".mp3");
}

bool is_media_playlist(const core::MediaItem &item)
{
    if (item.folder || !item.playable) {
        return false;
    }
    // audio/x-mpegurl, audio/mpegurl, application/vnd.apple.mpegurl
    return item.type.find("mpegurl") != memory::String::npos || is_playlist(item.title.c_str());
}

} // namespace hc::player
