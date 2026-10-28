#pragma once

// The order of the player's queue (docs/architecture.md section 9.6): the items in their order or
// shuffled, which one plays, what comes next with repeat off, all or one. Plain C++, host tested.

#include <cstddef>
#include <cstdint>

#include "hc/memory/large.hpp"

namespace hc::player {

// where an item comes from
enum class Origin : uint8_t {
    File,    // a file on the SD card, its path relative to the card
    Media,   // Home Assistant's media library, a media-source:// id
    Address, // an http(s) address (a playlist's line)
};

struct Item {
    Origin origin = Origin::File;
    memory::String location;
    // what the list showed (a file name, a playlist's #EXTINF); the tags replace it
    memory::String title;
};

enum class Repeat : uint8_t { Off, All, One };

class Queue {
public:
    // The items, `start` the one that plays first; shuffled from there with `shuffle` (the others
    // in a random order after it).
    void set(memory::Vector<Item> items, size_t start, bool shuffle, uint32_t seed);
    void clear();

    bool empty() const
    {
        return items_.empty();
    }
    size_t size() const
    {
        return items_.size();
    }
    // the place of the current item in the play order, 0-based
    size_t position() const
    {
        return at_;
    }
    // null when empty
    const Item *current() const;

    // After the current item ended by itself: one stays with Repeat::One, the last goes back to
    // the first with Repeat::All; false at the end with Repeat::Off.
    bool advance(Repeat repeat);
    // The next button: like advance(), but Repeat::One goes on to the next item too.
    bool next(Repeat repeat);
    // The one before; false at the first (Repeat::All: the last).
    bool previous(Repeat repeat);

    // Shuffles the items after the current one, or puts them back in order, the current one staying.
    void set_shuffle(bool shuffle, uint32_t seed);

    // Up to `max` of the items after the current one in the play order, the way next() goes: with
    // repeat all or one on from the first after the last, up to the one before the current one.
    size_t upcoming(const Item **out, size_t max, Repeat repeat) const;

private:
    memory::Vector<Item> items_;
    // the play order: indices into items_
    memory::Vector<uint32_t> order_;
    size_t at_ = 0;
};

} // namespace hc::player
