#pragma once

// What the start of an MP3 says about it (docs/architecture.md section 9.6): the ID3v2 tag
// (versions 2.2 to 2.4) with the title, artist, album, track and cover, and the Xing/Info or VBRI
// header of its first frame with the number of frames, which gives the length of a file with a
// variable bitrate and, with its table, where a time is in it. Plain C++, host tested.
//
// The player feeds the file's bytes from its start, in pieces as it reads them; the reader keeps
// the tag (up to kMaxTag) and the first 2 KB after it, then is done. A file without a tag is done
// after its first frame.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "hc/memory/large.hpp"

namespace hc::formats {

// The picture of a tag, as it is in the file (JPEG or PNG).
struct Cover {
    std::shared_ptr<uint8_t> data; // malloc'ed
    size_t size = 0;
    memory::String mime; // "image/jpeg", "image/png"
};

struct Mp3Tags {
    memory::String title;
    memory::String artist;
    memory::String album;
    memory::String track; // "3" or "3/12"
    Cover cover;
};

class Mp3Reader {
public:
    // a tag larger than this is skipped (its size still known), its texts not read
    static constexpr uint32_t kMaxTag = 1024 * 1024;

    Mp3Reader() = default;
    ~Mp3Reader();
    Mp3Reader(const Mp3Reader &) = delete;
    Mp3Reader &operator=(const Mp3Reader &) = delete;

    // The next `size` bytes of the file.
    void feed(const uint8_t *data, size_t size);
    // Nothing more to learn: the tag and the first frame are past (or the file ended, end()).
    bool done() const
    {
        return state_ == State::Done;
    }
    // The file ended (or no more is fed): what was read so far is all there is.
    void end();

    // The bytes of the ID3v2 tag at the start (0 without one), known after its first 10 bytes;
    // the audio starts there.
    bool tag_size_known() const
    {
        return state_ != State::Header;
    }
    uint32_t tag_size() const
    {
        return tag_size_;
    }
    const Mp3Tags &tags() const
    {
        return tags_;
    }

    // From the first frame after the tag (once done()): its sample rate, and with a Xing/Info or
    // VBRI header the length; 0 when unknown.
    uint32_t sample_rate() const
    {
        return sample_rate_;
    }
    uint32_t duration_ms() const;
    // The audio's bytes after the tag from the header, 0 when unknown.
    uint32_t audio_bytes() const
    {
        return bytes_;
    }
    // Where `ms` is, counted from the end of the tag: from the header's table, false without one
    // (the bitrate then gives it).
    bool offset_of(uint32_t ms, uint32_t audio_size, uint32_t &offset) const;

private:
    enum class State : uint8_t { Header, Tag, Frame, Done };

    bool append(const uint8_t *data, size_t size, size_t limit);
    void parse_tag();
    void parse_frame();
    void release();

    State state_ = State::Header;
    uint8_t header_[10] = {};
    size_t header_count_ = 0;
    // the tag without its header, then the bytes after it
    uint8_t *buffer_ = nullptr;
    size_t buffer_size_ = 0;
    size_t buffer_capacity_ = 0;
    // bytes of the tag still to skip without keeping them (a tag larger than kMaxTag)
    uint32_t skip_ = 0;
    uint32_t tag_size_ = 0;
    Mp3Tags tags_;
    uint32_t sample_rate_ = 0;
    uint32_t samples_per_frame_ = 0;
    uint32_t frames_ = 0;
    uint32_t bytes_ = 0;
    bool has_toc_ = false;
    uint8_t toc_[100] = {};
};

} // namespace hc::formats
