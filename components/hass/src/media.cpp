// The media worker (docs/architecture.md section 4): the camera images of the picture tiles, one
// at a time, in a task of its own, so a JPEG that takes a second to arrive and decode never holds
// up the values of the page. The task is created with the first request: a dashboard without a
// camera does not pay for its stack.
//
// An image comes from Home Assistant's camera proxy, or from an MJPEG stream (a tile's `stream`):
// a still image is the first frame of the stream, and one stream at a time can play, frame after
// frame, until it is stopped. A frame is only decoded once the UI took the one before; the frames
// in between are read and dropped, so the stream never falls behind.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/core/image.hpp"
#include "hc/hass/hass.hpp"
#include "internal.hpp"
#include "jpeg_decoder.h"

namespace hc::hass {

namespace {

constexpr const char *kTag = "media";
// a request waits at most this long for the camera; HA asks the camera itself
constexpr int kTimeoutMs = 10000;
// the largest JPEG taken: HA scales to the size asked for when it can, a camera that cannot be
// scaled sends its full snapshot (a 1080p one is a few hundred KB)
constexpr size_t kMaxJpeg = 512 * 1024;
// the requests that wait: one per entity, the newest wins
constexpr size_t kMaxQueued = 4;
// The task's stack: esp_http_client, and with https the TLS handshake (like the WebSocket
// client's task). The decoder's work buffer is allocated per image.
constexpr uint32_t kStackHttp = 6144;
constexpr uint32_t kStackHttps = 8192;
// the read buffer of a stream and the longest header line of a part
constexpr size_t kReadBuffer = 4096;
constexpr size_t kMaxLine = 256;
// a stream's frame waits this long (in steps) for the UI to take the one before
constexpr int kTakeWaitStepMs = 10;
constexpr int kTakeWaitSteps = 10;
// the statistics of a stream that plays are logged this often
constexpr int64_t kStatsUs = 5 * 1000 * 1000;

struct Request {
    memory::String entity_id;
    memory::String stream; // empty: the camera proxy of Home Assistant
    core::EntityIndex entity = core::kNone;
    uint16_t width = 0;
    uint16_t height = 0;
    bool swap_bytes = false;
    uint32_t generation = 0;
};

std::mutex g_mutex;
std::deque<Request> g_queue;
Request g_play;           // under g_mutex; the stream that is wanted, generation 0: none
TaskHandle_t g_task = nullptr;
// the generation of the stream that is wanted, read by the loop between two frames
std::atomic<uint32_t> g_play_generation{0};
// the UI took the last frame of the stream (take_event), the next one may be decoded
std::atomic<bool> g_frame_taken{true};
// how long the UI needed to take the frames (measurement): delivered at, sum and count
std::atomic<int64_t> g_delivered_us{0};
std::atomic<int64_t> g_take_us{0};
std::atomic<uint32_t> g_takes{0};
// the scale of the stream's frames: -1 the smallest that covers the box, else 0..3 (1:1..1:8)
std::atomic<int> g_stream_shift{-1};

// the task's own buffers for the settings and the header, not on its stack: the token is 512 bytes
char g_url[kUrlSize];
char g_token[kTokenSize];
char g_authorization[kTokenSize + 8];

void *psram(size_t size)
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

// ------------------------------------------------------------------------------- HTTP

struct Connection {
    esp_http_client_handle_t client = nullptr;

    Connection() = default;
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    ~Connection()
    {
        if (client != nullptr) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
    }
};

// Opens a GET of `url` and reads its headers; false with `error` set unless the answer is 200.
// `authorize`: with the token of Home Assistant.
bool open(Connection &c, const char *url, bool authorize, int64_t &length, char *error, size_t error_size)
{
    esp_http_client_config_t config{};
    config.url = url;
    config.timeout_ms = kTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    // the token goes into a header longer than the default send buffer
    config.buffer_size_tx = 1024;
    config.user_agent = "hassctrl";
    c.client = esp_http_client_init(&config);
    if (c.client == nullptr) {
        std::snprintf(error, error_size, "no memory for the request");
        return false;
    }
    if (authorize) {
        std::snprintf(g_authorization, sizeof(g_authorization), "Bearer %s", g_token);
        esp_http_client_set_header(c.client, "Authorization", g_authorization);
    }
    if (esp_http_client_open(c.client, 0) != ESP_OK) {
        std::snprintf(error, error_size, authorize ? "Home Assistant cannot be reached" : "the stream cannot be reached");
        return false;
    }
    length = esp_http_client_fetch_headers(c.client);
    const int status = esp_http_client_get_status_code(c.client);
    if (status != 200) {
        std::snprintf(error, error_size, status == 401 ? "the token was rejected" : "HTTP %d", status);
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

    // The length of the next part's JPEG. The boundary line is not compared: the parts of the
    // streams of go2rtc, motion and ffmpeg all start with "--" and carry their length.
    bool next_part(size_t &length, char *error, size_t error_size)
    {
        char text[kMaxLine];
        bool boundary = false;
        bool have_length = false;
        for (int lines = 0; lines < 16; lines++) {
            if (!line(text, sizeof(text))) {
                std::snprintf(error, error_size, "the stream ended");
                return false;
            }
            if (!boundary) {
                boundary = text[0] == '-' && text[1] == '-';
                continue; // the empty line after the last JPEG
            }
            if (text[0] == 0) {
                if (!have_length) {
                    std::snprintf(error, error_size, "the stream sends no Content-Length per frame");
                    return false;
                }
                return true;
            }
            have_length = have_length || core::mjpeg_content_length(text, length);
        }
        std::snprintf(error, error_size, "not an MJPEG stream");
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
};

// The JPEG into a frame. A still image is cut to the request's size (the tile keeps a small frame);
// a stream's frame (`live`) is the decoder's output as it is: the UI centers it and the screen cuts
// off what sticks out, which saves a copy of the whole frame through PSRAM (166 ms at 480x320).
bool decode(const Request &request, const uint8_t *jpeg, size_t size, bool live, int shift_wanted, core::Image &image,
            DecodeTimes &times, char *error, size_t error_size)
{
    esp_jpeg_image_cfg_t config{};
    config.indata = const_cast<uint8_t *>(jpeg);
    config.indata_size = static_cast<uint32_t>(size);
    config.out_format = JPEG_IMAGE_FORMAT_RGB565;
    esp_jpeg_image_output_t info{};
    if (esp_jpeg_get_image_info(&config, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        std::snprintf(error, error_size, "not a JPEG the decoder knows (a progressive one?)");
        return false;
    }
    const uint8_t shift = shift_wanted >= 0 ? static_cast<uint8_t>(std::min(shift_wanted, 3))
                                            : core::cover_shift(info.width, info.height, request.width, request.height);
    times.source_width = info.width;
    times.source_height = info.height;
    times.shift = shift;
    const uint32_t width = core::scaled_size(info.width, shift);
    const uint32_t height = core::scaled_size(info.height, shift);
    const size_t bytes = static_cast<size_t>(width) * height * 2;
    core::Pixels decoded = core::allocate_pixels(static_cast<size_t>(width) * height);
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
    const int64_t started = esp_timer_get_time();
    if (esp_jpeg_decode(&config, &info) != ESP_OK) {
        std::snprintf(error, error_size, "the JPEG could not be decoded");
        return false;
    }
    const int64_t decoded_at = esp_timer_get_time();
    times.decode_us = decoded_at - started;
    if (live) {
        image.width = info.width;
        image.height = info.height;
        image.pixels = std::move(decoded);
        return true;
    }
    core::Pixels pixels = core::allocate_pixels(static_cast<size_t>(request.width) * request.height);
    if (!pixels) {
        std::snprintf(error, error_size, "no memory for the image");
        return false;
    }
    core::cover_copy(decoded.get(), info.width, info.height, pixels.get(), request.width, request.height);
    times.copy_us = esp_timer_get_time() - decoded_at;
    image.width = request.width;
    image.height = request.height;
    image.pixels = std::move(pixels);
    return true;
}

// ------------------------------------------------------------------------------- requests

void credentials()
{
    detail::credentials(g_url, sizeof(g_url), g_token, sizeof(g_token));
}

// one still image: of the camera proxy, or the first frame of the stream
void snapshot(const Request &request)
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
        if (!request.stream.empty()) {
            size_t part = 0;
            if (open(c, request.stream.c_str(), false, length, error, sizeof(error))) {
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
        } else {
            credentials();
            if (g_url[0] == 0 || g_token[0] == 0) {
                std::snprintf(error, sizeof(error), "Home Assistant is not set up");
            } else {
                char url[kUrlSize + 160];
                std::snprintf(url, sizeof(url), "%s/api/camera_proxy/%s?width=%u&height=%u", g_url,
                              request.entity_id.c_str(), request.width, request.height);
                fetched = open(c, url, true, length, error, sizeof(error)) && read_body(c, length, jpeg, error, sizeof(error));
            }
        }
    }
    if (fetched && decode(request, jpeg.data, jpeg.size, false, -1, image, times, error, sizeof(error))) {
        ESP_LOGI(kTag, "%s%s: %u bytes, %ux%u 1:%u into %ux%u, %lld ms (decode %lld, copy %lld), %u bytes of stack left",
                 request.entity_id.c_str(), request.stream.empty() ? "" : " (stream)", static_cast<unsigned>(jpeg.size),
                 times.source_width, times.source_height, 1u << times.shift, request.width, request.height,
                 static_cast<long long>((esp_timer_get_time() - started) / 1000),
                 static_cast<long long>(times.decode_us / 1000), static_cast<long long>(times.copy_us / 1000),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }
    if (error[0] != 0) {
        ESP_LOGW(kTag, "%s: %s", request.entity_id.c_str(), error);
        image.error = error;
    }
    detail::deliver_event(std::move(image));
}

void stream_failed(const Request &request, const char *error)
{
    ESP_LOGW(kTag, "stream of %s: %s", request.entity_id.c_str(), error);
    core::Image image;
    image.generation = request.generation;
    image.entity = request.entity;
    image.live = true;
    image.error = error;
    detail::deliver_event(std::move(image));
}

// Plays the stream until another one is wanted or it is stopped.
void play(const Request &request)
{
    char error[96] = {};
    Connection c;
    int64_t length = 0;
    ESP_LOGI(kTag, "stream of %s into %ux%u: %s", request.entity_id.c_str(), request.width, request.height,
             request.stream.c_str());
    if (!open(c, request.stream.c_str(), false, length, error, sizeof(error))) {
        stream_failed(request, error);
        return;
    }
    StreamReader reader_data(c);
    StreamReader *reader = &reader_data;
    Buffer jpeg;
    g_frame_taken = true;

    // the statistics of the last kStatsUs
    int64_t window_start = esp_timer_get_time();
    unsigned received = 0;
    unsigned shown = 0;
    size_t bytes = 0;
    int64_t decode_us = 0;
    int64_t copy_us = 0;
    int64_t wait_us = 0;
    DecodeTimes times;

    while (g_play_generation == request.generation) {
        size_t part = 0;
        const int64_t wait_start = esp_timer_get_time();
        if (!reader->next_part(part, error, sizeof(error))) {
            stream_failed(request, error);
            return;
        }
        if (part > kMaxJpeg) {
            std::snprintf(error, sizeof(error), "a frame of %u KB is too large", static_cast<unsigned>(part / 1024));
            stream_failed(request, error);
            return;
        }
        // go2rtc sends frames in bursts: the next one is often there before the UI took the last
        // (it takes them within 10 to 15 ms); it is waited for briefly, and only a UI that is
        // really behind makes this frame be read past
        for (int i = 0; i < kTakeWaitSteps && !g_frame_taken; i++) {
            vTaskDelay(pdMS_TO_TICKS(kTakeWaitStepMs));
        }
        const bool wanted = g_frame_taken;
        if (wanted && !jpeg.reserve(part)) {
            stream_failed(request, "no memory for a frame");
            return;
        }
        if (!reader->bytes(wanted ? jpeg.data : nullptr, part)) {
            stream_failed(request, "the stream broke off");
            return;
        }
        wait_us += esp_timer_get_time() - wait_start;
        received++;
        bytes += part;
        if (wanted && g_play_generation == request.generation) {
            core::Image image;
            image.generation = request.generation;
            image.entity = request.entity;
            image.live = true;
            if (!decode(request, jpeg.data, part, true, g_stream_shift, image, times, error, sizeof(error))) {
                stream_failed(request, error);
                return;
            }
            decode_us += times.decode_us;
            copy_us += times.copy_us;
            shown++;
            g_frame_taken = false;
            g_delivered_us = esp_timer_get_time();
            detail::deliver_event(std::move(image));
        }
        const int64_t now = esp_timer_get_time();
        if (now - window_start >= kStatsUs) {
            const double seconds = (now - window_start) / 1e6;
            ESP_LOGI(kTag,
                     "stream %ux%u 1:%u: %.1f fps in, %.1f decoded, %u KB/frame, decode %lld ms, copy %lld ms, "
                     "waiting for data %.0f %%, UI took a frame after %lld ms, free internal %u KB, PSRAM %u KB, stack left %u",
                     times.source_width, times.source_height, 1u << times.shift, received / seconds, shown / seconds,
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

void task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            Request request;
            bool streaming = false;
            {
                std::lock_guard lock(g_mutex);
                if (g_play.generation != 0 && g_play.generation == g_play_generation) {
                    request = g_play;
                    streaming = true;
                } else if (!g_queue.empty()) {
                    request = std::move(g_queue.front());
                    g_queue.pop_front();
                } else {
                    break;
                }
            }
            if (streaming) {
                play(request);
                std::lock_guard lock(g_mutex);
                // a stream that ended by itself is not started again until it is asked for
                if (g_play.generation == request.generation) {
                    g_play = {};
                    g_play_generation = 0;
                }
            } else {
                snapshot(request);
            }
        }
    }
}

// starts the task with the first request; false when there is no memory for it
bool start_task()
{
    if (g_task != nullptr) {
        return true;
    }
    char url[kUrlSize];
    char token[8];
    detail::credentials(url, sizeof(url), token, sizeof(token));
    const uint32_t stack = std::strncmp(url, "https:", 6) == 0 ? kStackHttps : kStackHttp;
    if (xTaskCreate(task, "media", stack, nullptr, 3, &g_task) != pdPASS) {
        g_task = nullptr;
        ESP_LOGE(kTag, "no memory for the task (%u bytes of stack)", static_cast<unsigned>(stack));
        return false;
    }
    ESP_LOGI(kTag, "started (%u bytes of stack)", static_cast<unsigned>(stack));
    return true;
}

void no_task(core::EntityIndex entity, uint32_t generation, bool live)
{
    core::Image image;
    image.generation = generation;
    image.entity = entity;
    image.live = live;
    image.error = "no memory for the camera task";
    detail::deliver_event(std::move(image));
}

} // namespace

namespace detail {

void frame_taken()
{
    if (!g_frame_taken && g_delivered_us != 0) {
        g_take_us += esp_timer_get_time() - g_delivered_us;
        g_takes++;
    }
    g_frame_taken = true;
}

} // namespace detail

void fetch_image(std::string_view entity_id, std::string_view stream, core::EntityIndex entity, uint16_t width,
                 uint16_t height, bool swap_bytes, uint32_t generation)
{
    if (width == 0 || height == 0) {
        return;
    }
    Request request{.entity_id = memory::String(entity_id), .stream = memory::String(stream), .entity = entity,
                    .width = width, .height = height, .swap_bytes = swap_bytes, .generation = generation};
    {
        std::lock_guard lock(g_mutex);
        // one request per source waits: the newest one (tiles of one camera with different
        // streams are different sources)
        auto same = std::find_if(g_queue.begin(), g_queue.end(), [&](const Request &r) {
            return r.entity == request.entity && r.stream == request.stream;
        });
        if (same != g_queue.end()) {
            *same = std::move(request);
        } else {
            if (g_queue.size() >= kMaxQueued) {
                g_queue.pop_front();
            }
            g_queue.push_back(std::move(request));
        }
        if (!start_task()) {
            g_queue.clear();
            no_task(entity, generation, false);
            return;
        }
    }
    xTaskNotifyGive(g_task);
}

void play_stream(std::string_view entity_id, std::string_view stream, core::EntityIndex entity, uint16_t width,
                 uint16_t height, bool swap_bytes, uint32_t generation)
{
    {
        std::lock_guard lock(g_mutex);
        g_play = {.entity_id = memory::String(entity_id), .stream = memory::String(stream), .entity = entity,
                  .width = width, .height = height, .swap_bytes = swap_bytes, .generation = generation};
        g_play_generation = generation;
        if (!start_task()) {
            g_play = {};
            g_play_generation = 0;
            no_task(entity, generation, true);
            return;
        }
    }
    xTaskNotifyGive(g_task);
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
