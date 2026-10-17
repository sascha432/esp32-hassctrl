#pragma once

// What the files of the media worker share (media.cpp: the worker and its requests; media_http.cpp:
// HTTP and the MJPEG stream; media_decode.cpp: the JPEG decoders).

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "hc/core/image.hpp"
#include "hc/hass/hass.hpp"

namespace hc::hass::media {

// a stream played full screen: go2rtc answers once its first frame is there, which took longer
// than 10 s when it first had to start its source ("HTTP -1", 2026-10-11); the view shows a spinner
constexpr int kStreamTimeoutMs = 30000;
// the largest JPEG taken: HA scales to the size asked for when it can, a camera that cannot be
// scaled sends its full snapshot (a 1080p one is a few hundred KB)
constexpr size_t kMaxJpeg = 512 * 1024;
// the read buffer of a stream
constexpr size_t kReadBuffer = 4096;
// A frame read with this much of the stream already waiting behind it is not the newest: the
// stream is behind (more than the next part's header and the line ending of a frame).
constexpr size_t kBehindBytes = 512;

struct Request {
    memory::String entity_id;
    memory::String stream; // empty or core::kHassStream: the camera proxy of Home Assistant
    core::EntityIndex entity = core::kNone;
    uint16_t width = 0;
    uint16_t height = 0;
    bool swap_bytes = false;
    // the whole picture, scaled to cover the box but not cut to it (the full screen view moves it
    // around); a stream's frames always are
    bool whole = false;
    uint32_t generation = 0;
};

// a task's own buffers for the settings, the header and the address of a request, not on its
// stack: the token is 512 bytes
struct Context {
    char url[kUrlSize];
    char token[kTokenSize];
    char authorization[kTokenSize + 8];
    char request_url[kUrlSize + 160];
};

inline void *psram(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

struct Buffer {
    uint8_t *data = nullptr;
    size_t size = 0;
    size_t capacity = 0;

    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    ~Buffer()
    {
        std::free(data);
    }
    bool reserve(size_t wanted)
    {
        if (wanted <= capacity) {
            return true;
        }
        auto *grown = static_cast<uint8_t *>(heap_caps_realloc(data, wanted, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (grown == nullptr) {
            return false;
        }
        data = grown;
        capacity = wanted;
        return true;
    }
};

struct Connection {
    esp_http_client_handle_t client = nullptr;
    // why open() failed, for the screen's short text
    core::StreamProblem problem = core::StreamProblem::None;
    int status = 0;

    Connection() = default;
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    ~Connection()
    {
        close();
    }
    void close()
    {
        if (client != nullptr) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
        }
    }
};

// the UI learns that a stream is connected and waits for its answer (defined below)
void stream_connected(const Request &request);

// Opens a GET of `url` and reads its headers; false with `error` set unless the answer is 200.
// `authorize`: with the token of Home Assistant. `connect_ms` (0: `timeout_ms`): the connection
// itself; the answer then gets `timeout_ms`. `announce`: the stream whose UI is told once the
// connection is up and the request sent (go2rtc answers only with its first frame).
bool open(Context &ctx, Connection &c, const char *url, bool authorize, int timeout_ms, int64_t &length,
          char *error, size_t error_size, int connect_ms = 0, const Request *announce = nullptr);

// The body of a response into `out` (a still image).
bool read_body(Connection &c, int64_t length, Buffer &out, char *error, size_t error_size);

// The parts of an MJPEG stream: "--boundary", header lines with Content-Length, an empty line,
// the JPEG.
struct StreamReader {
    Connection &c;
    // PSRAM: neither the task's stack nor the scarce internal RAM
    uint8_t *buffer = static_cast<uint8_t *>(psram(kReadBuffer));
    size_t pos = 0;
    size_t len = 0;
    // why the last read gave nothing: no data in time, the server closed, or the connection failed
    core::StreamProblem problem = core::StreamProblem::None;

    explicit StreamReader(Connection &connection) : c(connection) {}
    StreamReader(const StreamReader &) = delete;
    StreamReader &operator=(const StreamReader &) = delete;
    ~StreamReader()
    {
        std::free(buffer);
    }

    bool fill();
    // a line without its \r\n
    bool line(char *out, size_t size);
    // `size` bytes into `out`, or skipped when `out` is null
    bool bytes(uint8_t *out, size_t size);
    // the bytes that arrived and are not read yet, in the buffer and in the socket
    size_t waiting() const;
    // after a new connection: what was buffered belongs to the old one
    void clear()
    {
        pos = len = 0;
    }
    // the length of the next part's JPEG
    bool next_part(size_t &length, char *error, size_t error_size);
};

struct DecodeTimes {
    int64_t decode_us = 0;
    int64_t copy_us = 0;
    uint16_t source_width = 0;
    uint16_t source_height = 0;
    uint8_t shift = 0;
    bool hardware = false;
    bool scaled = false; // by the PPA, to the request's size
    bool simd = false;   // esp_new_jpeg (ESP32-S3)
};

// the decoders used, for the log
const char *decoder_name(const DecodeTimes &times);

// The JPEG into a frame, in hardware where the chip has a decoder, else in software; `live`: a
// stream's frame, `shift_wanted` >= 0 the console's scale.
bool decode(const Request &request, const uint8_t *jpeg, size_t size, bool live, int shift_wanted, core::Image &image,
            DecodeTimes &times, char *error, size_t error_size);

} // namespace hc::hass::media
