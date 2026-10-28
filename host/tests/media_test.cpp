// components/core/src/byte_ring.cpp: the bytes between a download and the player.
// components/formats/src/mp3_tags.cpp: the tags and the Xing header at the start of an MP3.
// components/formats/src/m3u.cpp: the player's playlists.
// components/player/src/queue.cpp: the order of the player's queue.
// components/player/src/tree.cpp: a folder with its subfolders (the play button of a folder).
// components/core/src/natural.cpp: the order of names, numbers by value.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "hc/core/byte_ring.hpp"
#include "hc/core/natural.hpp"
#include "hc/formats/m3u.hpp"
#include "hc/formats/mp3_tags.hpp"
#include "hc/player/queue.hpp"
#include "hc/player/tree.hpp"

using namespace hc;

namespace {

using Bytes = std::vector<uint8_t>;

void put(Bytes &out, const std::string &text)
{
    out.insert(out.end(), text.begin(), text.end());
}

void put32(Bytes &out, uint32_t value, bool synchsafe)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(synchsafe ? static_cast<uint8_t>(value >> (shift / 8 * 7) & 0x7f)
                                : static_cast<uint8_t>(value >> shift));
    }
}

// one frame of a v2.3 or v2.4 tag
void frame(Bytes &out, const char *id, const Bytes &body, int version)
{
    put(out, id);
    put32(out, static_cast<uint32_t>(body.size()), version == 4);
    out.push_back(0);
    out.push_back(0);
    out.insert(out.end(), body.begin(), body.end());
}

Bytes utf16(const std::u16string &text)
{
    Bytes out{1, 0xff, 0xfe};
    for (char16_t c : text) {
        out.push_back(static_cast<uint8_t>(c & 0xff));
        out.push_back(static_cast<uint8_t>(c >> 8));
    }
    out.push_back(0);
    out.push_back(0);
    return out;
}

Bytes tag(int version, const Bytes &frames, size_t padding)
{
    Bytes out;
    put(out, "ID3");
    out.push_back(static_cast<uint8_t>(version));
    out.push_back(0);
    out.push_back(0);
    put32(out, static_cast<uint32_t>(frames.size() + padding), true);
    out.insert(out.end(), frames.begin(), frames.end());
    out.insert(out.end(), padding, 0);
    return out;
}

// an MPEG 1 layer III frame at 44.1 kHz, stereo, with an Info header of `frames` frames
Bytes first_frame(uint32_t frames, bool toc)
{
    Bytes out{0xff, 0xfb, 0x90, 0x04};
    out.insert(out.end(), 32, 0);
    put(out, toc ? "Xing" : "Info");
    put32(out, toc ? 7 : 3, false);
    put32(out, frames, false);
    put32(out, 1000000, false);
    if (toc) {
        for (int i = 0; i < 100; i++) {
            // the first half of the time in the first quarter of the bytes
            out.push_back(static_cast<uint8_t>(i < 50 ? i * 64 / 50 : 64 + (i - 50) * 192 / 50));
        }
    }
    out.resize(1044, 0);
    return out;
}

void feed_in_pieces(formats::Mp3Reader &reader, const Bytes &file, size_t piece)
{
    for (size_t at = 0; at < file.size(); at += piece) {
        reader.feed(file.data() + at, std::min(piece, file.size() - at));
    }
}

} // namespace

TEST(byte_ring_write_read_wrap)
{
    core::ByteRing ring(8);
    REQUIRE(ring.valid());
    const uint8_t in[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK(ring.write(in, 6) == 6);
    uint8_t out[16] = {};
    CHECK(ring.read(out, 4) == 4);
    CHECK(out[0] == 1 && out[3] == 4);
    CHECK(ring.position() == 4);
    // 2 left, 6 fit; the write wraps around the end
    CHECK(ring.write(in + 6, 4) == 4);
    CHECK(ring.available() == 6);
    CHECK(ring.space() == 2);
    CHECK(ring.read(out, 16) == 6);
    CHECK(out[0] == 5 && out[1] == 6 && out[2] == 7 && out[5] == 10);
    CHECK(!ring.ended());
    ring.finish(false);
    CHECK(ring.ended());
    CHECK(!ring.failed());
}

TEST(byte_ring_seek)
{
    core::ByteRing ring(16);
    const uint8_t in[] = {1, 2, 3, 4};
    ring.write(in, 4);
    ring.finish(false);
    uint32_t offset = 0;
    CHECK(!ring.take_seek(offset));
    ring.seek(1000);
    // nothing is read while the seek is on its way, and it is not the end
    CHECK(ring.seeking());
    CHECK(ring.available() == 0);
    CHECK(!ring.ended());
    REQUIRE(ring.take_seek(offset));
    CHECK(offset == 1000);
    CHECK(ring.seeking());
    ring.restart(offset);
    CHECK(!ring.seeking());
    CHECK(!ring.ended());
    CHECK(ring.position() == 1000);
    const uint8_t again[] = {9};
    ring.write(again, 1);
    uint8_t out = 0;
    CHECK(ring.read(&out, 1) == 1);
    CHECK(out == 9);
    CHECK(ring.position() == 1001);
    ring.cancel();
    CHECK(ring.cancelled());
}

TEST(mp3_tags_v23_utf16_and_cover)
{
    Bytes frames;
    frame(frames, "TIT2", utf16(u"Departure Song"), 3);
    frame(frames, "TPE1", utf16(u"The Ocean \U0001F30A"), 3);
    frame(frames, "TALB", Bytes{0, 'T', 'r', 0xe4, 'n', 'e', 'n'}, 3);
    frame(frames, "TRCK", Bytes{3, '1', '/', '9', 0}, 3);
    // a back cover, then the front cover (PNG, said to be a JPEG)
    Bytes back{0};
    put(back, "image/jpeg");
    back.push_back(0);
    back.push_back(4);
    back.push_back(0);
    back.insert(back.end(), {0xff, 0xd8, 0xff, 0xd9});
    frame(frames, "APIC", back, 3);
    Bytes front{1};
    put(front, "image/jpeg");
    front.push_back(0);
    front.push_back(3);
    front.insert(front.end(), {0xff, 0xfe, 'c', 0, 0, 0}); // description "c" in UTF-16
    front.insert(front.end(), {0x89, 'P', 'N', 'G', 1, 2, 3});
    frame(frames, "APIC", front, 3);

    Bytes file = tag(3, frames, 100);
    const Bytes audio = first_frame(1000, false);
    file.insert(file.end(), audio.begin(), audio.end());
    file.insert(file.end(), 3000, 0x55);

    formats::Mp3Reader reader;
    feed_in_pieces(reader, file, 7);
    REQUIRE(reader.done());
    CHECK(reader.tag_size() == 10 + frames.size() + 100);
    const formats::Mp3Tags &tags = reader.tags();
    CHECK_STR(tags.title.c_str(), "Departure Song");
    CHECK_STR(tags.artist.c_str(), "The Ocean \xf0\x9f\x8c\x8a");
    CHECK_STR(tags.album.c_str(), "Tr\xc3\xa4nen");
    CHECK_STR(tags.track.c_str(), "1/9");
    REQUIRE(tags.cover.data != nullptr);
    CHECK(tags.cover.size == 7);
    CHECK_STR(tags.cover.mime.c_str(), "image/png");
    CHECK(tags.cover.data.get()[0] == 0x89);
    CHECK(reader.sample_rate() == 44100);
    // 1000 frames of 1152 samples
    CHECK(reader.duration_ms() == 26122);
    uint32_t offset = 0;
    CHECK(!reader.offset_of(1000, 50000, offset));
}

TEST(mp3_tags_v24_and_toc)
{
    Bytes frames;
    frame(frames, "TIT2", Bytes{3, 'O', 'n', 'e', 0, 'T', 'w', 'o'}, 4);
    // 200 bytes: its size is synchsafe in v2.4 (0x01 0x48), 0x00c8 in plain bytes
    Bytes long_title{3};
    long_title.insert(long_title.end(), 199, 'x');
    frame(frames, "TALB", long_title, 4);
    Bytes file = tag(4, frames, 0);
    const Bytes audio = first_frame(2000, true);
    file.insert(file.end(), audio.begin(), audio.end());
    file.insert(file.end(), 3000, 0x55);

    formats::Mp3Reader reader;
    feed_in_pieces(reader, file, 1000);
    REQUIRE(reader.done());
    CHECK_STR(reader.tags().title.c_str(), "One / Two");
    CHECK(reader.tags().album.size() == 199);
    CHECK(reader.audio_bytes() == 1000000);
    const uint32_t duration = reader.duration_ms();
    CHECK(duration == 52244);
    uint32_t offset = 0;
    REQUIRE(reader.offset_of(duration / 2, 0, offset));
    // half the time is a quarter of the bytes
    CHECK(offset > 245000 && offset < 255000);
}

TEST(mp3_tags_without_tag)
{
    const Bytes file = first_frame(500, false);
    formats::Mp3Reader reader;
    reader.feed(file.data(), 10);
    CHECK(reader.tag_size_known());
    CHECK(reader.tag_size() == 0);
    CHECK(!reader.done());
    reader.feed(file.data() + 10, file.size() - 10);
    reader.end();
    CHECK(reader.done());
    CHECK(reader.duration_ms() == 13061);
    CHECK(reader.tags().title.empty());
}

TEST(m3u_playlist)
{
    const char *text = "\xef\xbb\xbf#EXTM3U\r\n"
                       "#EXTINF:241,The Ocean - Departure Song\r\n"
                       "01 - Departure Song.mp3\r\n"
                       "\r\n"
                       "..\\Solaris\\02 - Sol Grave.mp3\n"
                       "/sounds/alarms/Carbon.mp3\n"
                       "./sub/../03.mp3\n"
                       "../../../outside.mp3\n"
                       "#EXTINF:-1,Radio\n"
                       "http://radio.example.com/stream.mp3\n";
    const auto list = formats::parse_m3u(text, "music/Pelagial");
    REQUIRE(list.size() == 5);
    CHECK_STR(list[0].location.c_str(), "music/Pelagial/01 - Departure Song.mp3");
    CHECK_STR(list[0].title.c_str(), "The Ocean - Departure Song");
    CHECK_STR(list[1].location.c_str(), "music/Solaris/02 - Sol Grave.mp3");
    CHECK(list[1].title.empty());
    CHECK_STR(list[2].location.c_str(), "sounds/alarms/Carbon.mp3");
    CHECK_STR(list[3].location.c_str(), "music/Pelagial/03.mp3");
    CHECK(list[4].address);
    CHECK_STR(list[4].title.c_str(), "Radio");
    CHECK(formats::parse_m3u("a.mp3", "").front().location == "a.mp3");
}

namespace {

memory::Vector<player::Item> items(int count)
{
    memory::Vector<player::Item> out;
    for (int i = 0; i < count; i++) {
        player::Item item;
        item.location = memory::String(1, static_cast<char>('a' + i));
        out.push_back(item);
    }
    return out;
}

} // namespace

TEST(player_queue_order_and_repeat)
{
    player::Queue queue;
    CHECK(queue.current() == nullptr);
    CHECK(!queue.next(player::Repeat::All));
    queue.set(items(3), 1, false, 1);
    CHECK_STR(queue.current()->location.c_str(), "b");
    CHECK(queue.advance(player::Repeat::One));
    CHECK_STR(queue.current()->location.c_str(), "b");
    CHECK(queue.advance(player::Repeat::Off));
    CHECK_STR(queue.current()->location.c_str(), "c");
    CHECK(!queue.advance(player::Repeat::Off));
    CHECK(queue.advance(player::Repeat::All));
    CHECK_STR(queue.current()->location.c_str(), "a");
    CHECK(!queue.previous(player::Repeat::Off));
    CHECK(queue.previous(player::Repeat::All));
    CHECK_STR(queue.current()->location.c_str(), "c");
}

TEST(player_queue_shuffle)
{
    player::Queue queue;
    queue.set(items(10), 4, true, 12345);
    // the chosen one first, then every other one once
    CHECK(queue.position() == 0);
    CHECK_STR(queue.current()->location.c_str(), "e");
    std::string seen = queue.current()->location.c_str();
    while (queue.next(player::Repeat::Off)) {
        seen += queue.current()->location.c_str();
    }
    CHECK(seen.size() == 10);
    std::string sorted = seen;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == "abcdefghij");
    CHECK(seen != "eabcdfghij");
    // back in order: the one that plays stays
    const std::string playing = queue.current()->location.c_str();
    queue.set_shuffle(false, 0);
    CHECK(queue.current()->location.c_str() == playing);
    CHECK(queue.position() == static_cast<size_t>(playing[0] - 'a'));
}

TEST(player_queue_upcoming)
{
    const auto upcoming = [](const player::Queue &queue, size_t max, player::Repeat repeat) {
        const player::Item *out[8];
        std::string names;
        const size_t count = queue.upcoming(out, max, repeat);
        for (size_t i = 0; i < count; i++) {
            names += out[i]->location.c_str();
        }
        return names;
    };
    player::Queue queue;
    CHECK(upcoming(queue, 8, player::Repeat::All).empty());
    queue.set(items(4), 1, false, 1);
    CHECK(upcoming(queue, 8, player::Repeat::Off) == "cd");
    // on from the first, up to the one before the current one
    CHECK(upcoming(queue, 8, player::Repeat::All) == "cda");
    CHECK(upcoming(queue, 8, player::Repeat::One) == "cda");
    CHECK(upcoming(queue, 2, player::Repeat::All) == "cd");
    queue.next(player::Repeat::Off);
    queue.next(player::Repeat::Off);
    CHECK(upcoming(queue, 8, player::Repeat::Off).empty());
    // shuffled: the play order
    queue.set(items(6), 2, true, 777);
    std::string order;
    while (queue.next(player::Repeat::Off)) {
        order += queue.current()->location.c_str();
    }
    queue.set(items(6), 2, true, 777);
    CHECK(upcoming(queue, 8, player::Repeat::Off) == order);
}

TEST(natural_order)
{
    using core::natural_less;
    CHECK(natural_less("2 Flood", "10 High water"));
    CHECK(!natural_less("10 High water", "2 Flood"));
    CHECK(natural_less("track9", "Track10"));
    CHECK(natural_less("abc", "ABCD"));
    CHECK(!natural_less("ABC", "abc") && !natural_less("abc", "ABC"));
    CHECK(natural_less("a1b2", "a1b10"));
    CHECK(natural_less("1", "01") && !natural_less("01", "1"));
    CHECK(natural_less("disc 1/x", "disc 2/a"));
    CHECK(natural_less("12345678901234567890", "12345678901234567891"));
    CHECK(natural_less("Album", "Album 2"));
    std::vector<std::string> names = {"10.mp3", "b.mp3", "2.mp3", "A.mp3", "1.mp3"};
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) { return natural_less(a, b); });
    CHECK(names == (std::vector<std::string>{"1.mp3", "2.mp3", "10.mp3", "A.mp3", "b.mp3"}));
}

namespace {

// a made-up card: a path and whether it is a folder
struct FakeTree {
    std::vector<std::pair<std::string, bool>> entries;
    std::vector<std::string> listed;

    memory::Vector<player::Item> collect(const char *root, const player::TreeLimits &limits = {})
    {
        return player::collect_tree(
            root,
            [this](const memory::String &id, memory::Vector<player::TreeEntry> &out) {
                listed.push_back(id.c_str());
                const std::string folder = id.c_str();
                bool found = folder.empty();
                for (const auto &[path, dir] : entries) {
                    found = found || (path == folder && dir);
                    const size_t slash = path.rfind('/');
                    const std::string parent = slash == std::string::npos ? "" : path.substr(0, slash);
                    if (parent != folder) {
                        continue;
                    }
                    player::TreeEntry entry;
                    entry.id = path.c_str();
                    entry.name = path.substr(slash == std::string::npos ? 0 : slash + 1).c_str();
                    entry.dir = dir;
                    entry.playlist = path.ends_with(".m3u");
                    out.push_back(std::move(entry));
                }
                return found;
            },
            [](const memory::String &id, memory::Vector<player::Item> &out) {
                // "broken.m3u" cannot be read; the others list two addresses
                if (std::string(id.c_str()).ends_with("broken.m3u")) {
                    return false;
                }
                out.push_back({player::Origin::Address, (std::string(id.c_str()) + "#1").c_str(), ""});
                out.push_back({player::Origin::Address, (std::string(id.c_str()) + "#2").c_str(), ""});
                return true;
            },
            [](const player::TreeEntry &entry) { return player::Item{player::Origin::File, entry.id, entry.name}; },
            limits);
    }
};

std::string locations(const memory::Vector<player::Item> &items)
{
    std::string out;
    for (const auto &item : items) {
        out += (out.empty() ? "" : " ") + std::string(item.location.c_str());
    }
    return out;
}

} // namespace

TEST(player_tree_order)
{
    FakeTree tree;
    tree.entries = {
        {"music", true},
        {"music/b.mp3", false},
        {"music/Tides", true},
        {"music/Tides/10 c.mp3", false},
        {"music/Tides/2 b.mp3", false},
        {"music/Tides/1 a.mp3", false},
        {"music/Live", true},
        {"music/Live/x.mp3", false},
        {"music/Live/z.m3u", false},
        {"music/Live/a.m3u", false},
        {"music/Live/Bonus", true},
        {"music/Live/Bonus/y.mp3", false},
        {"music/Broken", true},
        {"music/Broken/broken.m3u", false},
        {"music/Broken/w.mp3", false},
        {"music/A.mp3", false},
    };
    // the folder's files first, then the subfolders in natural order; a playlist (the first by
    // name) in place of the files, its subfolders after it; a playlist that cannot be read: the files
    CHECK_STR(locations(tree.collect("music")).c_str(),
              "music/A.mp3 music/b.mp3 music/Broken/w.mp3 music/Live/a.m3u#1 music/Live/a.m3u#2 "
              "music/Live/Bonus/y.mp3 music/Tides/1 a.mp3 music/Tides/2 b.mp3 music/Tides/10 c.mp3");
    CHECK(tree.collect("nowhere").empty());
    // the limits: items, depth below the first folder, folders listed
    player::TreeLimits limits;
    limits.items = 4;
    CHECK_STR(locations(tree.collect("music", limits)).c_str(), "music/A.mp3 music/b.mp3 music/Broken/w.mp3 music/Live/a.m3u#1");
    limits = {};
    limits.depth = 0;
    CHECK_STR(locations(tree.collect("music", limits)).c_str(), "music/A.mp3 music/b.mp3");
    limits = {};
    limits.folders = 2;
    tree.listed.clear();
    CHECK_STR(locations(tree.collect("music", limits)).c_str(), "music/A.mp3 music/b.mp3 music/Broken/w.mp3");
    CHECK(tree.listed.size() == 2);
}
