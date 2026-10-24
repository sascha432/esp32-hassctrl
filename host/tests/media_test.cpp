// components/core/src/byte_ring.cpp: the bytes between a download and the player.
// components/formats/src/mp3_tags.cpp: the tags and the Xing header at the start of an MP3.
// components/formats/src/m3u.cpp: the player's playlists.
// components/player/src/queue.cpp: the order of the player's queue.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "hc/core/byte_ring.hpp"
#include "hc/formats/m3u.hpp"
#include "hc/formats/mp3_tags.hpp"
#include "hc/player/queue.hpp"

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
