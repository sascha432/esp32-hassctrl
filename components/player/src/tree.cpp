#include "hc/player/tree.hpp"

#include <algorithm>

#include "hc/core/natural.hpp"

namespace hc::player {

namespace {

struct Walk {
    const TreeLister &list;
    const TreeReader &read;
    const TreeItem &item;
    const TreeLimits &limits;
    memory::Vector<Item> items;
    size_t folders = 0;

    bool full() const
    {
        return items.size() >= limits.items;
    }

    // false when the folder cannot be read
    bool folder(const memory::String &id, size_t depth)
    {
        if (full() || folders >= limits.folders) {
            return true;
        }
        folders++;
        memory::Vector<TreeEntry> entries;
        if (!list(id, entries)) {
            return false;
        }
        std::stable_sort(entries.begin(), entries.end(), [](const TreeEntry &a, const TreeEntry &b) {
            return core::natural_less(a.name, b.name);
        });
        bool from_playlist = false;
        for (const TreeEntry &entry : entries) {
            if (!entry.dir && entry.playlist) {
                memory::Vector<Item> listed;
                if (read(entry.id, listed) && !listed.empty()) {
                    for (Item &one : listed) {
                        if (full()) {
                            break;
                        }
                        items.push_back(std::move(one));
                    }
                    from_playlist = true;
                }
                break;
            }
        }
        if (!from_playlist) {
            for (const TreeEntry &entry : entries) {
                if (full()) {
                    break;
                }
                if (!entry.dir && !entry.playlist) {
                    items.push_back(item(entry));
                }
            }
        }
        if (depth >= limits.depth) {
            return true;
        }
        // only the subfolders' ids stay while they are walked: one level's entries at a time
        memory::Vector<memory::String> dirs;
        for (TreeEntry &entry : entries) {
            if (entry.dir) {
                dirs.push_back(std::move(entry.id));
            }
        }
        memory::Vector<TreeEntry>().swap(entries);
        for (const memory::String &dir : dirs) {
            folder(dir, depth + 1);
        }
        return true;
    }
};

} // namespace

memory::Vector<Item> collect_tree(const memory::String &id, const TreeLister &list, const TreeReader &read,
                                  const TreeItem &item, const TreeLimits &limits)
{
    Walk walk{list, read, item, limits, {}, 0};
    if (!walk.folder(id, 0)) {
        return {};
    }
    return std::move(walk.items);
}

} // namespace hc::player
