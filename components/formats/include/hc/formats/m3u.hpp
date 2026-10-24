#pragma once

// A playlist of the player (docs/architecture.md section 9.6): M3U or M3U8, one entry per line.
// `#EXTINF:<seconds>,<title>` before an entry gives its title, other lines with `#` are comments.
// A path is relative to the playlist's folder, one that starts with '/' to the card's root; '\'
// counts as '/' (a list written on Windows). A line with "://" is an address. Plain C++, host
// tested.

#include <string_view>

#include "hc/memory/large.hpp"

namespace hc::formats {

struct PlaylistEntry {
    // a path relative to the card's root ("music/a.mp3"), or an address
    memory::String location;
    memory::String title; // from #EXTINF, empty without
    bool address = false;
};

// The entries of `text`, a playlist in the folder `folder` (relative to the card's root, "" the
// root). Paths that leave the card ("..") are left out.
memory::Vector<PlaylistEntry> parse_m3u(std::string_view text, std::string_view folder);

} // namespace hc::formats
