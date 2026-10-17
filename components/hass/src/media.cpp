// The media worker (docs/architecture.md section 4): the camera images of the picture tiles, one
// at a time, in a task of its own, so a JPEG that takes a second to arrive and decode never holds
// up the values of the page. The task is created with the first request: a dashboard without a
// camera does not pay for its stack.
//
// An image comes from Home Assistant's camera proxy, or from an MJPEG stream (a tile's `stream`):
// a still image is the first frame of the stream, and one stream at a time can play, frame after
// frame, until it is stopped. A frame is only decoded once the UI took the one before; the frames
// in between are read and dropped. A stream that sends more than the device reads falls behind
// (the sender queues the rest): it is opened again (play). On the P4 up to kMaxWorkers
// tasks fetch at the same time, as the browser does: Home Assistant needs seconds for some
// cameras. They decode one at a time (one JPEG engine and PPA).

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hc/core/image.hpp"
#include "hc/hass/hass.hpp"
#include "internal.hpp"
#include "image_hw.hpp"
#include "new_jpeg.hpp"
#include "jpeg_decoder.h"
#include "lwip/sockets.h"

namespace hc::hass {

namespace {

constexpr const char *kTag = "media";
// a request waits at most this long for the camera; HA asks the camera itself
constexpr int kTimeoutMs = 10000;
// a stream played full screen: go2rtc answers once its first frame is there, which took longer
// than 10 s when it first had to start its source ("HTTP -1", 2026-10-11); the view shows a spinner
constexpr int kStreamTimeoutMs = 30000;
// A stream's server that does not take the connection within this is not reachable (a server that
// restarts answers nothing at all: the connect waited 18 s). The UI tries every 10 s
// (docs/ui/lvgl/lvgl-camera.md section 2); the answer and the data still get kStreamTimeoutMs.
constexpr int kStreamConnectMs = 5000;
// the largest JPEG taken: HA scales to the size asked for when it can, a camera that cannot be
// scaled sends its full snapshot (a 1080p one is a few hundred KB)
constexpr size_t kMaxJpeg = 512 * 1024;
// the requests that wait: one per tile (a tile asks again once its image came), the newest of a
// source wins
constexpr size_t kMaxQueued = 8;
// the tasks that fetch at the same time: three on the P4; the internal RAM of the ESP32 and the S3
// has room for one HTTP client (on the S3 three left 19 bytes, a socket could not be created)
#if CONFIG_IDF_TARGET_ESP32P4
constexpr int kMaxWorkers = 3;
#else
constexpr int kMaxWorkers = 1;
#endif
// The task's stack: esp_http_client, and with https the TLS handshake (like the WebSocket
// client's task). The decoder's work buffer is allocated per image.
constexpr uint32_t kStackHttp = 6144;
constexpr uint32_t kStackHttps = 8192;
// the read buffer of a stream and the longest header line of a part
constexpr size_t kReadBuffer = 4096;
constexpr size_t kMaxLine = 256;
// a stream's frame waits this long for the UI to take the one before
constexpr int kTakeWaitMs = 100;
// the statistics of a stream that plays are logged this often
constexpr int64_t kStatsUs = 5 * 1000 * 1000;
// A frame read with this much of the stream already waiting behind it is not the newest: the
// stream is behind (more than the next part's header and the line ending of a frame).
constexpr size_t kBehindBytes = 512;
// A stream that stayed behind this long is opened again: the sender's queue (go2rtc: 64 frames,
// 10 s on the P4 and 40 s on the WT32 for 37 KB at 10 fps, 2026-10-11) is dropped with the
// connection, so the picture does not drift further behind. The stream has no timestamp to measure
// the delay exactly; a lower bit rate at the source is the fix. Skipping frames to catch up did
// not help: the boards read go2rtc's doorspy (370 KB/s) slower than it sends even without
// decoding (the S3 9.5 of 10 fps, the WT32 reads about 140 KB/s).
constexpr int64_t kBehindUs = 10 * 1000 * 1000;
// A frame that cannot be decoded is skipped (go2rtc's doorspy sent one in a minute to the WT32,
// 2026-10-11); this many in a row end the stream.
constexpr int kMaxBrokenFrames = 3;
// the second connection is opened before the first is closed (go2rtc stops a source without
// consumers and starting it again takes seconds) while this much internal RAM is free
constexpr size_t kReconnectInternal = 16 * 1024;
// TJpgDec's work area: esp_jpeg's own 3100 bytes hold the usual tables only, a snapshot of Home
// Assistant (camera.balconycam) needed more (JDR_MEM1); with all four quantization and Huffman
// tables at their largest it takes about 6.2 KB
constexpr size_t kDecoderWork = 8192;

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

std::mutex g_mutex;
std::deque<Request> g_queue;
Request g_play;           // under g_mutex; the stream that is wanted, generation 0: none
int g_workers = 0;        // under g_mutex: the tasks started
int g_busy = 0;           // under g_mutex: the tasks that fetch or play
bool g_streaming = false; // under g_mutex: a task plays the stream
// given for every request; the tasks wait on it
SemaphoreHandle_t g_work = nullptr;
// one decoder at a time
std::mutex g_decode_mutex;
// the generation of the stream that is wanted, read by the loop between two frames
std::atomic<uint32_t> g_play_generation{0};
// the UI took the last frame of the stream (take_event), the next one may be decoded
std::atomic<bool> g_frame_taken{true};
// given when the UI takes a frame: the stream waits on it (the tick of 10 ms is too coarse to poll)
SemaphoreHandle_t g_taken_signal = nullptr;
// how long the UI needed to take the frames (measurement): delivered at, sum and count
std::atomic<int64_t> g_delivered_us{0};
std::atomic<int64_t> g_take_us{0};
std::atomic<uint32_t> g_takes{0};
// the scale of the stream's frames: -1 the smallest that covers the box, else 0..3 (1:1..1:8)
std::atomic<int> g_stream_shift{-1};

// a task's own buffers for the settings, the header and the address of a request, not on its
// stack: the token is 512 bytes
struct Context {
    char url[kUrlSize];
    char token[kTokenSize];
    char authorization[kTokenSize + 8];
    char request_url[kUrlSize + 160];
};

void *psram(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

// the decoder's work area for one image (it reads its tables for every pixel)
struct DecoderWork {
    uint8_t *data = static_cast<uint8_t *>(detail::hot_alloc(kDecoderWork, 4));

    DecoderWork() = default;
    DecoderWork(const DecoderWork &) = delete;
    DecoderWork &operator=(const DecoderWork &) = delete;
    ~DecoderWork()
    {
        std::free(data);
    }
};

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

// ------------------------------------------------------------------------------- HTTP

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

// The login in a stream's address (http://user:password@host/...): none, the user alone (an
// imported dashboard: the export leaves the password out), or both
enum class UrlLogin : uint8_t { None, UserOnly, Both };

UrlLogin url_login(std::string_view url)
{
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string_view::npos ? 0 : scheme + 3;
    const size_t end = std::min(url.find('/', start), url.size());
    const size_t at = url.rfind('@', end);
    if (at == std::string_view::npos || at < start) {
        return UrlLogin::None;
    }
    const size_t colon = url.find(':', start);
    return colon < at && colon + 1 < at ? UrlLogin::Both : UrlLogin::UserOnly;
}

// Opens a GET of `url` and reads its headers; false with `error` set unless the answer is 200.
// `authorize`: with the token of Home Assistant. `connect_ms` (0: `timeout_ms`): the connection
// itself; the answer then gets `timeout_ms`. `announce`: the stream whose UI is told once the
// connection is up and the request sent (go2rtc answers only with its first frame).
bool open(Context &ctx, Connection &c, const char *url, bool authorize, int timeout_ms, int64_t &length,
          char *error, size_t error_size, int connect_ms = 0, const Request *announce = nullptr)
{
    esp_http_client_config_t config{};
    config.url = url;
    config.timeout_ms = connect_ms > 0 ? connect_ms : timeout_ms;
    config.crt_bundle_attach = detail::tls_attach;
    // the token goes into a header longer than the default send buffer
    config.buffer_size_tx = 1024;
    config.user_agent = "hassctrl";
    // esp_http_client takes user and password from the address, but sends them only with Basic
    // auth set: open() does not answer a 401 the way perform() does
    const UrlLogin login = authorize ? UrlLogin::None : url_login(url);
    if (login != UrlLogin::None) {
        config.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    c.client = esp_http_client_init(&config);
    if (c.client == nullptr) {
        std::snprintf(error, error_size, "no memory for the request");
        return false;
    }
    if (authorize) {
        std::snprintf(ctx.authorization, sizeof(ctx.authorization), "Bearer %s", ctx.token);
        esp_http_client_set_header(c.client, "Authorization", ctx.authorization);
    }
    if (esp_http_client_open(c.client, 0) != ESP_OK) {
        std::snprintf(error, error_size, authorize ? "Home Assistant cannot be reached" : "the stream cannot be reached");
        c.problem = core::StreamProblem::Unreachable;
        return false;
    }
    esp_http_client_set_timeout_ms(c.client, timeout_ms);
    if (announce != nullptr) {
        stream_connected(*announce);
    }
    length = esp_http_client_fetch_headers(c.client);
    if (length < 0) {
        // else the status of a response that never came reads "HTTP -1"
        if (length == -ESP_ERR_HTTP_EAGAIN) {
            std::snprintf(error, error_size, "no answer within %d s", timeout_ms / 1000);
        } else {
            std::snprintf(error, error_size, "no answer (the connection broke off or timed out)");
        }
        c.problem = core::StreamProblem::NoAnswer;
        return false;
    }
    const int status = esp_http_client_get_status_code(c.client);
    if (status == 401 && !authorize) {
        // the screen shows these words: a stream of its own wants a login
        std::snprintf(error, error_size, "%s", login == UrlLogin::None       ? "login needed"
                                               : login == UrlLogin::UserOnly ? "password missing"
                                                                             : "login rejected");
        c.problem = core::StreamProblem::Login;
        c.status = status;
        return false;
    }
    if (status != 200) {
        std::snprintf(error, error_size, status == 401 ? "the token was rejected" : "HTTP %d", status);
        c.problem = core::StreamProblem::Http;
        c.status = status;
        return false;
    }
    return true;
}

// The body of a response into `out` (a still image).
bool read_body(Connection &c, int64_t length, Buffer &out, char *error, size_t error_size)
{
    if (length > static_cast<int64_t>(kMaxJpeg)) {
        std::snprintf(error, error_size, "the image is too large (%lld KB)", static_cast<long long>(length / 1024));
        return false;
    }
    // without a length the buffer grows as the data arrives
    if (!out.reserve(length > 0 ? static_cast<size_t>(length) : 64 * 1024)) {
        std::snprintf(error, error_size, "no memory for the image");
        return false;
    }
    for (;;) {
        if (out.size == out.capacity) {
            if (out.capacity >= kMaxJpeg || !out.reserve(std::min(out.capacity * 2, kMaxJpeg))) {
                std::snprintf(error, error_size, "the image is too large");
                return false;
            }
        }
        const int read = esp_http_client_read(c.client, reinterpret_cast<char *>(out.data + out.size),
                                              static_cast<int>(out.capacity - out.size));
        if (read < 0) {
            std::snprintf(error, error_size, "the image broke off");
            return false;
        }
        if (read == 0) {
            break;
        }
        out.size += static_cast<size_t>(read);
    }
    if (out.size == 0) {
        std::snprintf(error, error_size, "the camera sent no image");
        return false;
    }
    return true;
}

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

    bool fill()
    {
        pos = 0;
        if (buffer == nullptr) {
            return false;
        }
        const int read = esp_http_client_read(c.client, reinterpret_cast<char *>(buffer), static_cast<int>(kReadBuffer));
        len = read > 0 ? static_cast<size_t>(read) : 0;
        if (read == -ESP_ERR_HTTP_EAGAIN) {
            problem = core::StreamProblem::NoAnswer;
        } else if (read == 0) {
            problem = core::StreamProblem::Ended;
        } else if (read < 0) {
            problem = core::StreamProblem::BrokeOff;
        }
        return len > 0;
    }

    // a line without its \r\n
    bool line(char *out, size_t size)
    {
        size_t n = 0;
        for (;;) {
            if (pos == len && !fill()) {
                return false;
            }
            const char ch = static_cast<char>(buffer[pos++]);
            if (ch == '\n') {
                if (n > 0 && out[n - 1] == '\r') {
                    n--;
                }
                out[n] = 0;
                return true;
            }
            if (n + 1 < size) {
                out[n++] = ch;
            } else if (n + 1 >= kMaxLine) {
                return false; // not the header of a part
            }
        }
    }

    // `size` bytes into `out`, or skipped when `out` is null
    bool bytes(uint8_t *out, size_t size)
    {
        while (size > 0) {
            if (pos == len && !fill()) {
                return false;
            }
            const size_t n = std::min(size, len - pos);
            if (out != nullptr) {
                std::memcpy(out, buffer + pos, n);
                out += n;
            }
            pos += n;
            size -= n;
        }
        return true;
    }

    // The bytes that arrived and are not read yet: in the buffer and in the socket (TLS and chunked
    // encoding count their own bytes, enough to tell a waiting frame).
    size_t waiting() const
    {
        size_t n = len - pos;
        const int fd = esp_http_client_get_socket(c.client);
        if (fd >= 0) {
            uint8_t peek[kBehindBytes];
            const int queued = recv(fd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT);
            n += queued > 0 ? static_cast<size_t>(queued) : 0;
        }
        return n;
    }

    // after a new connection: what was buffered belongs to the old one
    void clear()
    {
        pos = len = 0;
    }

    // The length of the next part's JPEG. The boundary line is not compared: the parts of the
    // streams of go2rtc, motion and ffmpeg all start with "--" and carry their length.
    bool next_part(size_t &length, char *error, size_t error_size)
    {
        char text[kMaxLine];
        bool boundary = false;
        bool have_length = false;
        for (int lines = 0; lines < 16; lines++) {
            if (!line(text, sizeof(text))) {
                if (problem == core::StreamProblem::NoAnswer) {
                    std::snprintf(error, error_size, "no data within %d s", kStreamTimeoutMs / 1000);
                } else {
                    std::snprintf(error, error_size, "the stream ended");
                    problem = problem == core::StreamProblem::None ? core::StreamProblem::Ended : problem;
                }
                return false;
            }
            if (!boundary) {
                boundary = text[0] == '-' && text[1] == '-';
                continue; // the empty line after the last JPEG
            }
            if (text[0] == 0) {
                if (!have_length) {
                    std::snprintf(error, error_size, "the stream sends no Content-Length per frame");
                    problem = core::StreamProblem::NotMjpeg;
                    return false;
                }
                return true;
            }
            have_length = have_length || core::mjpeg_content_length(text, length);
        }
        std::snprintf(error, error_size, "not an MJPEG stream");
        problem = core::StreamProblem::NotMjpeg;
        return false;
    }
};

// ------------------------------------------------------------------------------- JPEG

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
const char *decoder_name(const DecodeTimes &times)
{
    return times.scaled ? "hw+ppa" : times.hardware ? "hw" : times.simd ? "simd" : "sw";
}

// The decoder's scale for a picture: a stream's frame shows most of the picture, a still image
// covers the request's box (core::stream_shift, core::cover_shift); `shift_wanted` >= 0 overrides
// it (the console's `hass streamscale`).
uint8_t scale_of(const Request &request, uint32_t width, uint32_t height, bool live, int shift_wanted)
{
    if (shift_wanted >= 0) {
        return static_cast<uint8_t>(std::min(shift_wanted, 3));
    }
    return live ? core::stream_shift(width, height, request.width, request.height)
                : core::cover_shift(width, height, request.width, request.height);
}

// The hardware decoder (ESP32-P4, image_hw): the picture at full size, scaled by the PPA to cover
// the request's size, so LVGL shows it as it is (its zoom took 100 ms a frame). With a scale from
// the console, or without a PPA, it is made smaller by core::shrink when the scale asks for it.
// False when there is no decoder or it cannot take the JPEG.
bool decode_hardware(const Request &request, const uint8_t *jpeg, size_t size, bool live, int shift_wanted,
                     core::Pixels &decoded, uint32_t &width, uint32_t &height, DecodeTimes &times)
{
    char error[64];
    image_hw::Frame frame;
    const int64_t started = esp_timer_get_time();
    if (!image_hw::decode(jpeg, size, request.swap_bytes, frame, error, sizeof(error))) {
        ESP_LOGD(kTag, "%s: %s, decoded in software", request.entity_id.c_str(), error);
        return false;
    }
    const int64_t decoded_at = esp_timer_get_time();
    times.decode_us = decoded_at - started;
    times.source_width = static_cast<uint16_t>(frame.picture_width);
    times.source_height = static_cast<uint16_t>(frame.picture_height);
    times.hardware = true;
    const bool fits = frame.width == request.width && frame.height == request.height &&
                      frame.picture_width == frame.width && frame.picture_height == frame.height;
    if (shift_wanted < 0 && !fits) {
        if (image_hw::cover(frame, request.width, request.height, live || request.whole, decoded, width, height, error,
                            sizeof(error))) {
            times.scaled = true;
            times.copy_us = esp_timer_get_time() - decoded_at;
            return true;
        }
        ESP_LOGD(kTag, "%s: %s", request.entity_id.c_str(), error);
    }
    const uint8_t shift = scale_of(request, frame.picture_width, frame.picture_height, live, shift_wanted);
    times.shift = shift;
    if (shift == 0) {
        // whole blocks: their edge beyond the picture repeats it
        decoded = std::move(frame.pixels);
        width = frame.width;
        height = frame.height;
        return true;
    }
    width = core::scaled_size(frame.picture_width, shift);
    height = core::scaled_size(frame.picture_height, shift);
    decoded = core::allocate_pixels(static_cast<size_t>(width) * height);
    if (!decoded) {
        return false;
    }
    core::shrink(frame.pixels.get(), frame.picture_width, frame.picture_height, frame.width, shift, decoded.get());
    times.copy_us = esp_timer_get_time() - decoded_at;
    return true;
}

// esp_new_jpeg (ESP32-S3, new_jpeg): a quarter of TJpgDec's time, scaled while it decodes to the
// smallest size that covers the request's (sides in multiples of 8), so a stream's frame needs no
// zoom of LVGL. Not with a scale from the console. False when it is not built in or cannot take
// the JPEG.
bool decode_simd(const Request &request, const uint8_t *jpeg, size_t size, int shift_wanted, core::Pixels &decoded,
                 uint32_t &width, uint32_t &height, DecodeTimes &times)
{
    if (shift_wanted >= 0) {
        return false;
    }
    char error[64];
    uint32_t picture_width = 0;
    uint32_t picture_height = 0;
    const int64_t started = esp_timer_get_time();
    if (!new_jpeg::decode(jpeg, size, request.swap_bytes, request.width, request.height, decoded, width, height,
                          picture_width, picture_height, error, sizeof(error))) {
        ESP_LOGD(kTag, "%s: %s", request.entity_id.c_str(), error);
        return false;
    }
    times.decode_us = esp_timer_get_time() - started;
    times.source_width = static_cast<uint16_t>(picture_width);
    times.source_height = static_cast<uint16_t>(picture_height);
    times.simd = true;
    return true;
}

// The software decoder (esp_jpeg, TJpgDec), which scales while it decodes.
bool decode_software(const Request &request, const uint8_t *jpeg, size_t size, bool live, int shift_wanted,
                     core::Pixels &decoded, uint32_t &width, uint32_t &height, DecodeTimes &times, char *error,
                     size_t error_size)
{
    const DecoderWork work_area;
    uint8_t *work = work_area.data;
    if (work == nullptr) {
        std::snprintf(error, error_size, "no memory for the decoder");
        return false;
    }
    esp_jpeg_image_cfg_t config{};
    config.indata = const_cast<uint8_t *>(jpeg);
    config.indata_size = static_cast<uint32_t>(size);
    config.out_format = JPEG_IMAGE_FORMAT_RGB565;
    config.advanced.working_buffer = work;
    config.advanced.working_buffer_size = kDecoderWork;
    esp_jpeg_image_output_t info{};
    if (esp_jpeg_get_image_info(&config, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        std::snprintf(error, error_size, "not a JPEG the decoder knows (a progressive one?)");
        return false;
    }
    const uint8_t shift = scale_of(request, info.width, info.height, live, shift_wanted);
    times.source_width = info.width;
    times.source_height = info.height;
    times.shift = shift;
    times.hardware = false;
    const uint32_t out_width = core::scaled_size(info.width, shift);
    const uint32_t out_height = core::scaled_size(info.height, shift);
    const size_t bytes = static_cast<size_t>(out_width) * out_height * 2;
    decoded = core::allocate_pixels(static_cast<size_t>(out_width) * out_height);
    if (!decoded) {
        std::snprintf(error, error_size, "no memory for the decoded image (%u KB)", static_cast<unsigned>(bytes / 1024));
        return false;
    }
    config = {};
    config.indata = const_cast<uint8_t *>(jpeg);
    config.indata_size = static_cast<uint32_t>(size);
    config.outbuf = reinterpret_cast<uint8_t *>(decoded.get());
    config.outbuf_size = static_cast<uint32_t>(bytes);
    config.out_format = JPEG_IMAGE_FORMAT_RGB565;
    config.out_scale = static_cast<esp_jpeg_image_scale_t>(shift);
    config.flags.swap_color_bytes = request.swap_bytes ? 1 : 0;
    config.advanced.working_buffer = work;
    config.advanced.working_buffer_size = kDecoderWork;
    const int64_t started = esp_timer_get_time();
    if (esp_jpeg_decode(&config, &info) != ESP_OK) {
        std::snprintf(error, error_size, "the JPEG could not be decoded");
        return false;
    }
    times.decode_us = esp_timer_get_time() - started;
    width = info.width;
    height = info.height;
    return true;
}

// The JPEG into a frame, in hardware where the chip has a decoder, else in software. A still image
// is cut to the request's size (the tile keeps a small frame); a stream's frame (`live`) and a
// whole picture are the decoder's output as it is: the UI scales it to cover its box and the
// screen cuts off what sticks out, which saves a copy of the whole frame through PSRAM (166 ms at
// 480x320 on the ESP32).
bool decode(const Request &request, const uint8_t *jpeg, size_t size, bool live, int shift_wanted, core::Image &image,
            DecodeTimes &times, char *error, size_t error_size)
{
    times = {};
    core::Pixels decoded;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!decode_hardware(request, jpeg, size, live, shift_wanted, decoded, width, height, times) &&
        !decode_simd(request, jpeg, size, shift_wanted, decoded, width, height, times) &&
        !decode_software(request, jpeg, size, live, shift_wanted, decoded, width, height, times, error, error_size)) {
        return false;
    }
    image.source_width = times.source_width;
    image.source_height = times.source_height;
    if (live || request.whole || times.scaled) {
        // a stream's frame or a whole picture as it is; the PPA's needs no cut either
        image.width = static_cast<uint16_t>(width);
        image.height = static_cast<uint16_t>(height);
        image.pixels = std::move(decoded);
        return true;
    }
    const int64_t copy_started = esp_timer_get_time();
    core::Pixels pixels = core::allocate_pixels(static_cast<size_t>(request.width) * request.height);
    if (!pixels) {
        std::snprintf(error, error_size, "no memory for the image");
        return false;
    }
    core::cover_copy(decoded.get(), width, height, pixels.get(), request.width, request.height);
    times.copy_us += esp_timer_get_time() - copy_started;
    image.width = request.width;
    image.height = request.height;
    image.pixels = std::move(pixels);
    return true;
}

// ------------------------------------------------------------------------------- requests

void credentials(Context &ctx)
{
    detail::credentials(ctx.url, sizeof(ctx.url), ctx.token, sizeof(ctx.token));
}

// a stream of its own (http://), not one of Home Assistant
bool own_stream(const Request &request)
{
    return !request.stream.empty() && request.stream != core::kHassStream;
}

// ctx.request_url: `path` of Home Assistant for the request's camera; false with `error` set when
// Home Assistant is not set up
bool hass_url(Context &ctx, const Request &request, const char *path, char *error, size_t error_size)
{
    credentials(ctx);
    if (ctx.url[0] == 0 || ctx.token[0] == 0) {
        std::snprintf(error, error_size, "Home Assistant is not set up");
        return false;
    }
    std::snprintf(ctx.request_url, sizeof(ctx.request_url), "%s%s%s", ctx.url, path, request.entity_id.c_str());
    return true;
}

// one still image: of the camera proxy, or the first frame of the stream (Home Assistant's own
// stream is not asked for a still image: the camera proxy scales it to the tile)
void snapshot(Context &ctx, const Request &request)
{
    core::Image image;
    image.generation = request.generation;
    image.entity = request.entity;
    char error[96] = {};
    const int64_t started = esp_timer_get_time();
    Buffer jpeg;
    DecodeTimes times;
    bool fetched = false;
    {
        Connection c;
        int64_t length = 0;
        if (own_stream(request)) {
            size_t part = 0;
            if (open(ctx, c, request.stream.c_str(), false, kTimeoutMs, length, error, sizeof(error))) {
                StreamReader reader(c);
                if (reader.next_part(part, error, sizeof(error))) {
                    if (part > kMaxJpeg || !jpeg.reserve(part)) {
                        std::snprintf(error, sizeof(error), "the frame is too large (%u KB)", static_cast<unsigned>(part / 1024));
                    } else if (!reader.bytes(jpeg.data, part)) {
                        std::snprintf(error, sizeof(error), "the stream broke off");
                    } else {
                        jpeg.size = part;
                        fetched = true;
                    }
                }
            }
        } else if (hass_url(ctx, request, "/api/camera_proxy/", error, sizeof(error))) {
            const size_t used = std::strlen(ctx.request_url);
            std::snprintf(ctx.request_url + used, sizeof(ctx.request_url) - used, "?width=%u&height=%u", request.width,
                          request.height);
            fetched = open(ctx, c, ctx.request_url, true, kTimeoutMs, length, error, sizeof(error)) &&
                      read_body(c, length, jpeg, error, sizeof(error));
        }
    }
    bool decoded = false;
    if (fetched) {
        std::lock_guard lock(g_decode_mutex);
        decoded = decode(request, jpeg.data, jpeg.size, false, -1, image, times, error, sizeof(error));
    }
    if (decoded) {
        ESP_LOGI(kTag, "%s%s: %u bytes, %ux%u 1:%u into %ux%u, %lld ms (decode %lld %s, copy %lld), %u bytes of stack left",
                 request.entity_id.c_str(), own_stream(request) ? " (stream)" : "", static_cast<unsigned>(jpeg.size),
                 times.source_width, times.source_height, 1u << times.shift, request.width, request.height,
                 static_cast<long long>((esp_timer_get_time() - started) / 1000),
                 static_cast<long long>(times.decode_us / 1000), decoder_name(times),
                 static_cast<long long>(times.copy_us / 1000), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }
    if (error[0] != 0) {
        ESP_LOGW(kTag, "%s: %s", request.entity_id.c_str(), error);
        image.error = error;
    }
    detail::deliver_event(std::move(image));
}

void stream_failed(const Request &request, const char *error, core::StreamProblem problem, int status = 0)
{
    ESP_LOGW(kTag, "stream of %s: %s", request.entity_id.c_str(), error);
    core::Image image;
    image.generation = request.generation;
    image.entity = request.entity;
    image.live = true;
    image.error = error;
    image.problem = problem;
    image.http_status = status;
    detail::deliver_event(std::move(image));
}

// The stream is connected and its request sent: the UI shows that it waits for the first frame now
// (not live: it is no frame the stream waits to be taken)
void stream_connected(const Request &request)
{
    core::Image image;
    image.generation = request.generation;
    image.entity = request.entity;
    image.opened = true;
    detail::deliver_event(std::move(image));
}

// a frame of the stream, with the decoder to itself
bool decode_frame(const Request &request, const uint8_t *jpeg, size_t size, core::Image &image, DecodeTimes &times,
                  char *error, size_t error_size)
{
    std::lock_guard lock(g_decode_mutex);
    return decode(request, jpeg, size, true, g_stream_shift, image, times, error, error_size);
}

// The stream that is wanted now is the one that plays in another size (the full screen view
// zoomed): `request` takes its size and generation, the next frames have them, the connection
// stays.
bool resized(Request &request)
{
    std::lock_guard lock(g_mutex);
    if (g_play.generation == 0 || g_play.generation != g_play_generation || g_play.entity != request.entity ||
        g_play.stream != request.stream || g_play.entity_id != request.entity_id) {
        return false;
    }
    ESP_LOGI(kTag, "stream of %s now into %ux%u", request.entity_id.c_str(), g_play.width, g_play.height);
    request = g_play;
    return true;
}

// The stream opened again, the new connection replaces `c` (the reader keeps it). The old one stays
// open meanwhile when there is RAM for two.
bool reconnect(Context &ctx, Connection &c, StreamReader &reader, const char *url, bool authorize, char *error,
               size_t error_size)
{
    int64_t length = 0;
    Connection fresh;
    reader.clear();
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= kReconnectInternal &&
        open(ctx, fresh, url, authorize, kStreamTimeoutMs, length, error, error_size, kStreamConnectMs)) {
        std::swap(c.client, fresh.client);
        return true;
    }
    fresh.close();
    c.close();
    return open(ctx, c, url, authorize, kStreamTimeoutMs, length, error, error_size, kStreamConnectMs);
}

// Plays the stream until another one is wanted or it is stopped; `request` follows a new size of
// the same stream (resized).
void play(Context &ctx, Request &request)
{
    char error[96] = {};
    Connection c;
    int64_t length = 0;
    // Home Assistant's stream has the camera's full size: it cannot be scaled like a snapshot
    const bool hass = !own_stream(request);
    if (hass && !hass_url(ctx, request, "/api/camera_proxy_stream/", error, sizeof(error))) {
        stream_failed(request, error, core::StreamProblem::Other);
        return;
    }
    const char *url = hass ? ctx.request_url : request.stream.c_str();
    ESP_LOGI(kTag, "stream of %s into %ux%u: %s", request.entity_id.c_str(), request.width, request.height, url);
    if (!open(ctx, c, url, hass, kStreamTimeoutMs, length, error, sizeof(error), kStreamConnectMs, &request)) {
        stream_failed(request, error, c.problem, c.status);
        return;
    }
    StreamReader reader_data(c);
    StreamReader *reader = &reader_data;
    Buffer jpeg;
    g_frame_taken = true;
    // the last frame read with nothing behind it
    int64_t caught_up_at = esp_timer_get_time();
    int broken_frames = 0; // in a row

    // the statistics of the last kStatsUs
    int64_t window_start = esp_timer_get_time();
    unsigned received = 0;
    unsigned shown = 0;
    unsigned reconnects = 0; // since the start
    size_t bytes = 0;
    int64_t decode_us = 0;
    int64_t copy_us = 0;
    int64_t wait_us = 0;
    DecodeTimes times;

    while (g_play_generation == request.generation || resized(request)) {
        size_t part = 0;
        const int64_t wait_start = esp_timer_get_time();
        if (!reader->next_part(part, error, sizeof(error))) {
            stream_failed(request, error, reader->problem);
            return;
        }
        if (part > kMaxJpeg) {
            std::snprintf(error, sizeof(error), "a frame of %u KB is too large", static_cast<unsigned>(part / 1024));
            stream_failed(request, error, core::StreamProblem::NotMjpeg);
            return;
        }
        if (!jpeg.reserve(part)) {
            stream_failed(request, "no memory for a frame", core::StreamProblem::NoMemory);
            return;
        }
        if (!reader->bytes(jpeg.data, part)) {
            stream_failed(request, "the stream broke off", core::StreamProblem::BrokeOff);
            return;
        }
        const int64_t read_at = esp_timer_get_time();
        wait_us += read_at - wait_start;
        received++;
        bytes += part;
        // With the next frame already waiting this one is old. A stream that stays behind reads
        // slower than its source sends, the sender's queue grows: opened again.
        const bool behind = reader->waiting() >= kBehindBytes;
        if (!behind) {
            caught_up_at = read_at;
        } else if (read_at - caught_up_at >= kBehindUs) {
            ESP_LOGI(kTag, "stream of %s behind for %lld s, opened again", request.entity_id.c_str(),
                     static_cast<long long>(kBehindUs / 1000000));
            if (!reconnect(ctx, c, *reader, url, hass, error, sizeof(error))) {
                stream_failed(request, error, c.problem, c.status);
                return;
            }
            reconnects++;
            caught_up_at = esp_timer_get_time();
            continue;
        }
        // The frame is read while the UI draws the last one (the P4 takes 50 ms for 800x480). The
        // frame of the P4's hardware decoder (17 ms) or of esp_new_jpeg on the S3 is decoded then
        // too, before the UI is waited for; TJpgDec's (300 ms on the ESP32) only once the UI took
        // the last, so a frame that
        // is dropped is not decoded for nothing. The UI is waited for briefly (go2rtc also sends
        // frames in bursts); only a UI that is really behind makes a frame be dropped.
        const bool decode_first = times.hardware || times.simd; // the decoder of the last frame
        core::Image image;
        image.generation = request.generation;
        image.entity = request.entity;
        image.live = true;
        bool broken = decode_first && !decode_frame(request, jpeg.data, part, image, times, error, sizeof(error));
        const TickType_t wait_until = xTaskGetTickCount() + pdMS_TO_TICKS(kTakeWaitMs);
        while (!broken && !g_frame_taken) {
            const TickType_t now_ticks = xTaskGetTickCount();
            if (static_cast<int32_t>(wait_until - now_ticks) <= 0) {
                break;
            }
            xSemaphoreTake(g_taken_signal, wait_until - now_ticks);
        }
        const bool wanted = !broken && g_frame_taken;
        if (wanted && g_play_generation == request.generation) {
            broken = !decode_first && !decode_frame(request, jpeg.data, part, image, times, error, sizeof(error));
        }
        if (broken) {
            if (++broken_frames >= kMaxBrokenFrames) {
                stream_failed(request, error, core::StreamProblem::NotMjpeg);
                return;
            }
            ESP_LOGW(kTag, "stream of %s: %s, frame of %u bytes skipped", request.entity_id.c_str(), error,
                     static_cast<unsigned>(part));
        } else if (wanted && g_play_generation == request.generation) {
            broken_frames = 0;
            decode_us += times.decode_us;
            copy_us += times.copy_us;
            shown++;
            g_frame_taken = false;
            xSemaphoreTake(g_taken_signal, 0); // a signal of an earlier frame
            g_delivered_us = esp_timer_get_time();
            detail::deliver_event(std::move(image));
        }
        const int64_t now = esp_timer_get_time();
        if (now - window_start >= kStatsUs) {
            const double seconds = (now - window_start) / 1e6;
            ESP_LOGI(kTag,
                     "stream %ux%u 1:%u %s: %.1f fps in, %.1f decoded, opened again %u times (behind), "
                     "%u KB/frame, decode %lld ms, copy %lld ms, waiting for data %.0f %%, UI took a frame after %lld ms, free internal %u KB, PSRAM %u KB, stack left %u",
                     times.source_width, times.source_height, 1u << times.shift, decoder_name(times),
                     received / seconds, shown / seconds, reconnects,
                     static_cast<unsigned>(received > 0 ? bytes / received / 1024 : 0),
                     static_cast<long long>(shown > 0 ? decode_us / shown / 1000 : 0),
                     static_cast<long long>(shown > 0 ? copy_us / shown / 1000 : 0), wait_us / 1e4 / seconds,
                     static_cast<long long>(g_takes > 0 ? g_take_us / g_takes / 1000 : 0),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            window_start = now;
            received = shown = 0;
            bytes = 0;
            decode_us = copy_us = wait_us = 0;
            g_take_us = 0;
            g_takes = 0;
        }
    }
    ESP_LOGI(kTag, "stream of %s stopped", request.entity_id.c_str());
}

void task(void *arg)
{
    Context &ctx = *static_cast<Context *>(arg);
    for (;;) {
        xSemaphoreTake(g_work, portMAX_DELAY);
        for (;;) {
            Request request;
            bool streaming = false;
            {
                std::lock_guard lock(g_mutex);
                // one task plays the stream; a new one waits until the last one stopped
                if (!g_streaming && g_play.generation != 0 && g_play.generation == g_play_generation) {
                    request = g_play;
                    streaming = g_streaming = true;
                } else if (!g_queue.empty()) {
                    request = std::move(g_queue.front());
                    g_queue.pop_front();
                } else {
                    break;
                }
                g_busy++;
            }
            if (streaming) {
                play(ctx, request);
            } else {
                snapshot(ctx, request);
            }
            std::lock_guard lock(g_mutex);
            g_busy--;
            if (streaming) {
                g_streaming = false;
                // a stream that ended by itself is not started again until it is asked for
                if (g_play.generation == request.generation) {
                    g_play = {};
                    g_play_generation = 0;
                }
            }
        }
    }
}

// Under g_mutex: one more task when every task is busy and work waits; false when there is no task
// and no memory for one.
bool start_worker()
{
    const int waiting = static_cast<int>(g_queue.size()) + (g_play.generation != 0 && !g_streaming ? 1 : 0);
    if (g_workers >= kMaxWorkers || g_workers - g_busy >= waiting) {
        return g_workers > 0;
    }
    if (g_taken_signal == nullptr) {
        g_taken_signal = xSemaphoreCreateBinary();
    }
    if (g_work == nullptr) {
        g_work = xSemaphoreCreateCounting(32, 0);
    }
    auto *ctx = static_cast<Context *>(psram(sizeof(Context)));
    if (g_taken_signal == nullptr || g_work == nullptr || ctx == nullptr) {
        std::free(ctx);
        ESP_LOGE(kTag, "no memory for a task");
        return g_workers > 0;
    }
    char url[kUrlSize];
    char token[8];
    detail::credentials(url, sizeof(url), token, sizeof(token));
    const uint32_t stack = std::strncmp(url, "https:", 6) == 0 ? kStackHttps : kStackHttp;
    // on the second core where there is one: Wi-Fi runs on the first, and with LVGL and a stream's
    // decoding both there the S3's first core was busy for seconds (task watchdog)
    const BaseType_t core = portNUM_PROCESSORS > 1 ? 1 : tskNO_AFFINITY;
    if (xTaskCreatePinnedToCore(task, "media", stack, ctx, 3, nullptr, core) != pdPASS) {
        std::free(ctx);
        ESP_LOGE(kTag, "no memory for a task (%u bytes of stack)", static_cast<unsigned>(stack));
        return g_workers > 0;
    }
    g_workers++;
    ESP_LOGI(kTag, "task %d started (%u bytes of stack)", g_workers, static_cast<unsigned>(stack));
    return true;
}

void no_task(core::EntityIndex entity, uint32_t generation, bool live)
{
    core::Image image;
    image.generation = generation;
    image.entity = entity;
    image.live = live;
    image.error = "no memory for the camera task";
    image.problem = core::StreamProblem::NoMemory;
    detail::deliver_event(std::move(image));
}

} // namespace

namespace detail {

void *hot_alloc(size_t size, size_t align)
{
    // what the others need while a decoder holds its buffers: the S3 had 24 KB free with the media
    // task, the P4 239 KB
    constexpr size_t kInternalReserve = 32 * 1024;
    constexpr uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    align = std::max<size_t>(align, 4);
    // the largest block too: a request that cannot fit is not even tried (the failure hook logs it)
    if (heap_caps_get_free_size(kInternal) >= size + kInternalReserve &&
        heap_caps_get_largest_free_block(kInternal) >= size + align) {
        if (void *data = heap_caps_aligned_calloc(align, 1, size, kInternal)) {
            return data;
        }
    }
    return heap_caps_aligned_calloc(align, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void frame_taken()
{
    if (!g_frame_taken && g_delivered_us != 0) {
        g_take_us += esp_timer_get_time() - g_delivered_us;
        g_takes++;
    }
    g_frame_taken = true;
    if (g_taken_signal != nullptr) {
        xSemaphoreGive(g_taken_signal);
    }
}

} // namespace detail

void fetch_image(std::string_view entity_id, std::string_view stream, core::EntityIndex entity, uint16_t width,
                 uint16_t height, bool whole, bool swap_bytes, uint32_t generation)
{
    if (width == 0 || height == 0) {
        return;
    }
    Request request{.entity_id = memory::String(entity_id), .stream = memory::String(stream), .entity = entity,
                    .width = width, .height = height, .swap_bytes = swap_bytes, .whole = whole,
                    .generation = generation};
    {
        std::lock_guard lock(g_mutex);
        // one request per source waits: the newest one (tiles of one camera with different
        // streams or sizes are different sources)
        auto same = std::find_if(g_queue.begin(), g_queue.end(), [&](const Request &r) {
            return r.entity == request.entity && r.stream == request.stream && r.width == request.width &&
                   r.height == request.height && r.whole == request.whole;
        });
        if (same != g_queue.end()) {
            *same = std::move(request);
        } else {
            if (g_queue.size() >= kMaxQueued) {
                // its tile hears of it, or it would wait for the image
                const Request &dropped = g_queue.front();
                ESP_LOGW(kTag, "%s: dropped, %u requests wait", dropped.entity_id.c_str(),
                         static_cast<unsigned>(g_queue.size()));
                core::Image image;
                image.generation = dropped.generation;
                image.entity = dropped.entity;
                image.error = "too many cameras wait";
                detail::deliver_event(std::move(image));
                g_queue.pop_front();
            }
            g_queue.push_back(std::move(request));
        }
        if (!start_worker()) {
            g_queue.clear();
            no_task(entity, generation, false);
            return;
        }
    }
    xSemaphoreGive(g_work);
}

void play_stream(std::string_view entity_id, std::string_view stream, core::EntityIndex entity, uint16_t width,
                 uint16_t height, bool swap_bytes, uint32_t generation)
{
    {
        std::lock_guard lock(g_mutex);
        g_play = {.entity_id = memory::String(entity_id), .stream = memory::String(stream), .entity = entity,
                  .width = width, .height = height, .swap_bytes = swap_bytes, .whole = true,
                  .generation = generation};
        g_play_generation = generation;
        if (!start_worker()) {
            g_play = {};
            g_play_generation = 0;
            no_task(entity, generation, true);
            return;
        }
    }
    xSemaphoreGive(g_work);
}

void stop_stream()
{
    std::lock_guard lock(g_mutex);
    g_play = {};
    g_play_generation = 0;
}

void set_stream_scale(int shift)
{
    g_stream_shift = shift < 0 ? -1 : std::min(shift, 3);
}

} // namespace hc::hass
