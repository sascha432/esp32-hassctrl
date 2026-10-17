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
#include "media.hpp"

namespace hc::hass {

using namespace media;

namespace {

constexpr const char *kTag = "media";
// a request waits at most this long for the camera; HA asks the camera itself
constexpr int kTimeoutMs = 10000;
// A stream's server that does not take the connection within this is not reachable (a server that
// restarts answers nothing at all: the connect waited 18 s). The UI tries every 10 s
// (docs/ui/lvgl/lvgl-camera.md section 2); the answer and the data still get kStreamTimeoutMs.
constexpr int kStreamConnectMs = 5000;
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
// a stream's frame waits this long for the UI to take the one before
constexpr int kTakeWaitMs = 100;
// the statistics of a stream that plays are logged this often
constexpr int64_t kStatsUs = 5 * 1000 * 1000;
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

} // namespace

namespace media {

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

} // namespace media

namespace {

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
