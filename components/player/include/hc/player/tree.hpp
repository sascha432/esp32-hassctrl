#pragma once

// A folder with its subfolders as one queue (docs/architecture.md section 9.6, the ▶ of a folder in
// the media browser): depth first, each folder's files before its subfolders, both in natural order
// (core::natural_less). A folder with a playlist (.m3u, .m3u8) gives the playlist's entries in place
// of its files, the first playlist by name; its subfolders follow as usual. The same walk serves the
// SD card and Home Assistant's media library; the caller lists a folder. Plain C++, host tested.

#include <cstddef>
#include <functional>

#include "hc/memory/large.hpp"
#include "hc/player/queue.hpp"

namespace hc::player {

// one entry of a folder, as the caller's list gives it
struct TreeEntry {
    // what the next call of the lister or the playlist reader takes (the SD path, the media id)
    memory::String id;
    // the name the order goes by
    memory::String name;
    bool dir = false;
    bool playlist = false;
};

struct TreeLimits {
    size_t items = 1000;  // the queue is cut here
    size_t folders = 1000; // folders listed, the first included
    size_t depth = 8;      // subfolders below the first
};

// the entries of the folder `id` the walk takes: subfolders, playable files and playlists; false
// when the folder cannot be read (the first: the walk fails, a subfolder: it is left out)
using TreeLister = std::function<bool(const memory::String &id, memory::Vector<TreeEntry> &out)>;
// the entries of the playlist `id`; false when it cannot be read (the folder's files are taken)
using TreeReader = std::function<bool(const memory::String &id, memory::Vector<Item> &out)>;
// a playable file as an item of the queue
using TreeItem = std::function<Item(const TreeEntry &entry)>;

// The items under the folder `id`; empty when it cannot be read or holds nothing to play.
memory::Vector<Item> collect_tree(const memory::String &id, const TreeLister &list, const TreeReader &read,
                                  const TreeItem &item, const TreeLimits &limits = {});

} // namespace hc::player
