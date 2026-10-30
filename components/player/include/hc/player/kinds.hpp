#pragma once

// What the player takes (docs/architecture.md section 9.6): the one rule for the walk of a folder,
// the queue of a tapped file and the icons of the media browser and the file manager. Plain C++,
// host tested.

#include "hc/core/entity.hpp"

namespace hc::player {

// the name is a file the player plays: .mp3, .wav, or a playlist (any case)
bool playable(const char *name);
bool is_playlist(const char *name);
// an entry of the media library the player plays: an MP3
bool playable_media(const core::MediaItem &item);
// an entry of the media library that is a playlist (audio/x-mpegurl, .m3u, .m3u8)
bool is_media_playlist(const core::MediaItem &item);

} // namespace hc::player
