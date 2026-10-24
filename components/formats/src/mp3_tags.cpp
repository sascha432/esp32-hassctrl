#include "hc/formats/mp3_tags.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace hc::formats {

namespace {

// the bytes after the tag that hold the first frame's header and its Xing or VBRI header
constexpr size_t kFrameBytes = 2048;

uint32_t synchsafe(const uint8_t *p)
{
    return (uint32_t{p[0]} & 0x7f) << 21 | (uint32_t{p[1]} & 0x7f) << 14 | (uint32_t{p[2]} & 0x7f) << 7 |
           (uint32_t{p[3]} & 0x7f);
}

uint32_t big_endian(const uint8_t *p, size_t bytes)
{
    uint32_t value = 0;
    for (size_t i = 0; i < bytes; i++) {
        value = value << 8 | p[i];
    }
    return value;
}

// Undoes the unsynchronisation (0xff 0x00 -> 0xff) in place; the new size.
size_t resync(uint8_t *data, size_t size)
{
    size_t out = 0;
    for (size_t i = 0; i < size; i++) {
        data[out++] = data[i];
        if (data[i] == 0xff && i + 1 < size && data[i + 1] == 0x00) {
            i++;
        }
    }
    return out;
}

void append_utf8(memory::String &out, uint32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xc0 | c >> 6);
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xe0 | c >> 12);
        out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | c >> 18);
        out += static_cast<char>(0x80 | (c >> 12 & 0x3f));
        out += static_cast<char>(0x80 | (c >> 6 & 0x3f));
        out += static_cast<char>(0x80 | (c & 0x3f));
    }
}

// The length of a text in `encoding` up to its terminator (one zero byte, two for UTF-16).
size_t text_length(uint8_t encoding, const uint8_t *data, size_t size)
{
    if (encoding == 1 || encoding == 2) {
        size_t i = 0;
        while (i + 1 < size && (data[i] != 0 || data[i + 1] != 0)) {
            i += 2;
        }
        return std::min(i, size);
    }
    const auto *end = static_cast<const uint8_t *>(std::memchr(data, 0, size));
    return end != nullptr ? static_cast<size_t>(end - data) : size;
}

// A text of `encoding` (0 ISO-8859-1, 1 UTF-16 with a byte order mark, 2 UTF-16BE, 3 UTF-8) as
// UTF-8, up to its terminator; several values (v2.4) are joined with " / ".
memory::String decode_text(uint8_t encoding, const uint8_t *data, size_t size)
{
    memory::String out;
    if (encoding == 0) {
        for (size_t i = 0; i < size; i++) {
            if (data[i] == 0) {
                if (i + 1 < size && data[i + 1] != 0) {
                    out += " / ";
                    continue;
                }
                break;
            }
            append_utf8(out, data[i]);
        }
        return out;
    }
    if (encoding == 3) {
        for (size_t i = 0; i < size; i++) {
            if (data[i] == 0) {
                if (i + 1 < size && data[i + 1] != 0) {
                    out += " / ";
                    continue;
                }
                break;
            }
            out += static_cast<char>(data[i]);
        }
        return out;
    }
    bool big = encoding == 2;
    size_t i = 0;
    uint32_t high = 0;
    while (i + 1 < size) {
        // a byte order mark, at the start and at the start of a further value
        if ((data[i] == 0xff && data[i + 1] == 0xfe) || (data[i] == 0xfe && data[i + 1] == 0xff)) {
            big = data[i] == 0xfe;
            i += 2;
            continue;
        }
        const uint32_t unit = big ? uint32_t{data[i]} << 8 | data[i + 1] : uint32_t{data[i + 1]} << 8 | data[i];
        i += 2;
        if (unit == 0) {
            if (i + 1 < size && (data[i] != 0 || data[i + 1] != 0)) {
                out += " / ";
                continue;
            }
            break;
        }
        if (unit >= 0xd800 && unit < 0xdc00) {
            high = unit;
            continue;
        }
        if (unit >= 0xdc00 && unit < 0xe000) {
            if (high != 0) {
                append_utf8(out, 0x10000 + ((high - 0xd800) << 10) + (unit - 0xdc00));
            }
            high = 0;
            continue;
        }
        high = 0;
        append_utf8(out, unit);
    }
    return out;
}

// the cover of an APIC (v2.3, v2.4) or PIC (v2.2) frame; `front`: picture type 3
bool decode_picture(const uint8_t *data, size_t size, bool v22, Cover &out, bool &front)
{
    if (size < 4) {
        return false;
    }
    const uint8_t encoding = data[0];
    size_t at = 1;
    if (v22) {
        // a three letter format instead of a MIME type
        out.mime = std::memcmp(data + 1, "PNG", 3) == 0 || std::memcmp(data + 1, "png", 3) == 0 ? "image/png"
                                                                                              : "image/jpeg";
        at = 4;
    } else {
        const size_t length = text_length(0, data + at, size - at);
        out.mime.assign(reinterpret_cast<const char *>(data + at), length);
        at += length + 1;
    }
    if (at >= size) {
        return false;
    }
    front = data[at] == 3;
    at++;
    const size_t description = text_length(encoding, data + at, size - at);
    at += description + (encoding == 1 || encoding == 2 ? 2 : 1);
    if (at >= size) {
        return false;
    }
    // what the bytes are, whatever the tag says
    const uint8_t *picture = data + at;
    const size_t picture_size = size - at;
    if (picture_size >= 4 && std::memcmp(picture, "\x89PNG", 4) == 0) {
        out.mime = "image/png";
    } else if (picture_size >= 2 && picture[0] == 0xff && picture[1] == 0xd8) {
        out.mime = "image/jpeg";
    }
    auto *copy = static_cast<uint8_t *>(std::malloc(picture_size));
    if (copy == nullptr) {
        return false;
    }
    std::memcpy(copy, picture, picture_size);
    out.data = std::shared_ptr<uint8_t>(copy, [](uint8_t *p) { std::free(p); });
    out.size = picture_size;
    return true;
}

} // namespace

Mp3Reader::~Mp3Reader()
{
    std::free(buffer_);
}

bool Mp3Reader::append(const uint8_t *data, size_t size, size_t limit)
{
    const size_t take = std::min(size, limit - buffer_size_);
    if (buffer_size_ + take > buffer_capacity_) {
        auto *larger = static_cast<uint8_t *>(std::realloc(buffer_, limit));
        if (larger == nullptr) {
            return false;
        }
        buffer_ = larger;
        buffer_capacity_ = limit;
    }
    std::memcpy(buffer_ + buffer_size_, data, take);
    buffer_size_ += take;
    return true;
}

void Mp3Reader::feed(const uint8_t *data, size_t size)
{
    while (size > 0 && state_ != State::Done) {
        switch (state_) {
        case State::Header: {
            const size_t take = std::min(size, sizeof(header_) - header_count_);
            std::memcpy(header_ + header_count_, data, take);
            header_count_ += take;
            data += take;
            size -= take;
            if (header_count_ < sizeof(header_)) {
                break;
            }
            if (std::memcmp(header_, "ID3", 3) != 0 || header_[3] < 2 || header_[3] > 4) {
                // no tag: these bytes are the first frame's
                state_ = State::Frame;
                if (!append(header_, sizeof(header_), kFrameBytes)) {
                    state_ = State::Done;
                }
                break;
            }
            const uint32_t body = synchsafe(header_ + 6);
            // a footer (v2.4) follows the frames
            const uint32_t footer = header_[3] == 4 && (header_[5] & 0x10) != 0 ? 10 : 0;
            tag_size_ = 10 + body + footer;
            state_ = State::Tag;
            if (body + footer > kMaxTag) {
                skip_ = body + footer;
            }
            if (body + footer == 0) {
                state_ = State::Frame;
            }
            break;
        }
        case State::Tag: {
            const size_t rest = tag_size_ - 10 - (skip_ > 0 ? 0 : buffer_size_);
            if (skip_ > 0) {
                const size_t take = std::min<size_t>(size, skip_);
                skip_ -= static_cast<uint32_t>(take);
                data += take;
                size -= take;
                if (skip_ == 0) {
                    state_ = State::Frame;
                }
                break;
            }
            const size_t take = std::min(size, rest);
            if (!append(data, take, tag_size_ - 10)) {
                // no memory for the tag: its texts are lost, the rest goes on
                skip_ = static_cast<uint32_t>(rest - take);
                buffer_size_ = 0;
                if (skip_ == 0) {
                    state_ = State::Frame;
                }
            }
            data += take;
            size -= take;
            if (skip_ == 0 && buffer_size_ == tag_size_ - 10) {
                parse_tag();
                buffer_size_ = 0;
                state_ = State::Frame;
            }
            break;
        }
        case State::Frame: {
            const size_t take = std::min(size, kFrameBytes - buffer_size_);
            if (!append(data, take, kFrameBytes)) {
                state_ = State::Done;
                break;
            }
            data += take;
            size -= take;
            if (buffer_size_ == kFrameBytes) {
                parse_frame();
                release();
            }
            break;
        }
        case State::Done:
            break;
        }
    }
}

void Mp3Reader::end()
{
    if (state_ == State::Tag && skip_ == 0 && buffer_size_ > 0) {
        // a cut file: what came of the tag
        parse_tag();
    } else if (state_ == State::Frame) {
        parse_frame();
    }
    state_ = State::Done;
    release();
}

void Mp3Reader::release()
{
    std::free(buffer_);
    buffer_ = nullptr;
    buffer_size_ = buffer_capacity_ = 0;
}

void Mp3Reader::parse_tag()
{
    const uint8_t version = header_[3];
    const uint8_t flags = header_[5];
    size_t size = buffer_size_;
    if (version == 4 && (flags & 0x10) != 0) {
        size = size >= 10 ? size - 10 : 0;
    }
    // v2.2 and v2.3 unsynchronise the whole tag, v2.4 each frame
    if (version < 4 && (flags & 0x80) != 0) {
        size = resync(buffer_, size);
    }
    size_t at = 0;
    if (version >= 3 && (flags & 0x40) != 0 && size >= 4) {
        // the extended header: v2.3 counts its size without these 4 bytes, v2.4 with them
        at = version == 3 ? 4 + big_endian(buffer_, 4) : synchsafe(buffer_);
    }
    const bool v22 = version == 2;
    const size_t header = v22 ? 6 : 10;
    bool have_front = false;
    while (at + header <= size) {
        const uint8_t *frame = buffer_ + at;
        if (frame[0] == 0) {
            break; // padding
        }
        const uint32_t frame_size = v22             ? big_endian(frame + 3, 3)
                                    : version == 4 ? synchsafe(frame + 4)
                                                   : big_endian(frame + 4, 4);
        if (frame_size > size - at - header) {
            break;
        }
        uint8_t *body = buffer_ + at + header;
        size_t body_size = frame_size;
        at += header + frame_size;
        if (!v22) {
            const uint8_t format = frame[9];
            // compressed or encrypted (v2.3 0xc0, v2.4 0x0c): not read
            if ((version == 3 && (format & 0xc0) != 0) || (version == 4 && (format & 0x0c) != 0)) {
                continue;
            }
            if (version == 4 && (format & 0x01) != 0 && body_size >= 4) {
                // the data length indicator
                body += 4;
                body_size -= 4;
            }
            if (version == 4 && (format & 0x02) != 0) {
                body_size = resync(body, body_size);
            }
        }
        const char *id = reinterpret_cast<const char *>(frame);
        auto is = [&](const char *v2, const char *v3) {
            return v22 ? std::memcmp(id, v2, 3) == 0 : std::memcmp(id, v3, 4) == 0;
        };
        if (body_size == 0) {
            continue;
        }
        if (is("TT2", "TIT2")) {
            tags_.title = decode_text(body[0], body + 1, body_size - 1);
        } else if (is("TP1", "TPE1")) {
            tags_.artist = decode_text(body[0], body + 1, body_size - 1);
        } else if (is("TAL", "TALB")) {
            tags_.album = decode_text(body[0], body + 1, body_size - 1);
        } else if (is("TRK", "TRCK")) {
            tags_.track = decode_text(body[0], body + 1, body_size - 1);
        } else if (is("PIC", "APIC") && !have_front) {
            Cover cover;
            bool front = false;
            if (decode_picture(body, body_size, v22, cover, front) && (front || !tags_.cover.data)) {
                tags_.cover = std::move(cover);
                have_front = front;
            }
        }
    }
}

void Mp3Reader::parse_frame()
{
    state_ = State::Done;
    static constexpr uint32_t kRates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    for (size_t i = 0; i + 4 <= buffer_size_; i++) {
        const uint8_t *h = buffer_ + i;
        if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0) {
            continue;
        }
        const uint8_t version = h[1] >> 3 & 3; // 3: MPEG 1, 2: MPEG 2, 0: MPEG 2.5
        const uint8_t layer = h[1] >> 1 & 3;   // 1: layer III
        const uint8_t rate = h[2] >> 2 & 3;
        if (version == 1 || layer != 1 || rate == 3 || (h[2] >> 4) == 0xf) {
            continue;
        }
        const bool mpeg1 = version == 3;
        sample_rate_ = kRates[mpeg1 ? 0 : version == 2 ? 1 : 2][rate];
        samples_per_frame_ = mpeg1 ? 1152 : 576;
        const bool mono = (h[3] >> 6) == 3;
        const size_t side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        const uint8_t *xing = h + 4 + side;
        if (static_cast<size_t>(xing - buffer_) + 120 <= buffer_size_ &&
            (std::memcmp(xing, "Xing", 4) == 0 || std::memcmp(xing, "Info", 4) == 0)) {
            const uint32_t flags = big_endian(xing + 4, 4);
            const uint8_t *p = xing + 8;
            if ((flags & 1) != 0) {
                frames_ = big_endian(p, 4);
                p += 4;
            }
            if ((flags & 2) != 0) {
                bytes_ = big_endian(p, 4);
                p += 4;
            }
            if ((flags & 4) != 0) {
                std::memcpy(toc_, p, sizeof(toc_));
                has_toc_ = true;
            }
            return;
        }
        const uint8_t *vbri = h + 36;
        if (static_cast<size_t>(vbri - buffer_) + 18 <= buffer_size_ && std::memcmp(vbri, "VBRI", 4) == 0) {
            bytes_ = big_endian(vbri + 10, 4);
            frames_ = big_endian(vbri + 14, 4);
        }
        return;
    }
}

uint32_t Mp3Reader::duration_ms() const
{
    if (frames_ == 0 || sample_rate_ == 0) {
        return 0;
    }
    return static_cast<uint32_t>(uint64_t{frames_} * samples_per_frame_ * 1000 / sample_rate_);
}

bool Mp3Reader::offset_of(uint32_t ms, uint32_t audio_size, uint32_t &offset) const
{
    const uint32_t duration = duration_ms();
    if (!has_toc_ || duration == 0) {
        return false;
    }
    const uint32_t bytes = bytes_ != 0 ? bytes_ : audio_size;
    // the table: 100 points of the time, each the offset in 1/256 of the bytes; between two of
    // them on a straight line
    const float percent = std::min(99.999f, 100.0f * static_cast<float>(ms) / static_cast<float>(duration));
    const int index = static_cast<int>(percent);
    const float a = toc_[index];
    const float b = index < 99 ? toc_[index + 1] : 256.0f;
    const float at = a + (b - a) * (percent - static_cast<float>(index));
    offset = static_cast<uint32_t>(at / 256.0f * static_cast<float>(bytes));
    return true;
}

} // namespace hc::formats
