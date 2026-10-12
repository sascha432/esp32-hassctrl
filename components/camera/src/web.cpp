// The camera's web routes (hc/camera/camera.hpp): /api/camera.jpg and /api/camera.mjpg.
//
// A JPEG of the newest frame from the P4's JPEG encoder, made at most once per frame (and size and
// quality) and shared by all readers; at half size the frame is shrunk by the CPU first. An MJPEG
// stream runs in its own task (httpd_req_async_handler_begin), so the web server's single task
// stays free, and sends at most the stream's frame rate. Both routes answer 404 while the stream
// is off and ask for the stream's login (HTTP Basic auth).

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "camera_private.hpp"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/core/image.hpp"
#include "mbedtls/base64.h"

namespace hc::camera {
namespace {

constexpr const char *kTag = "camera";
constexpr int kMaxStreams = 2;
constexpr int kSettleFrames = 15; // half a second at 30 fps

// "Basic " and base64 of "username:password"
std::mutex g_auth_mutex;
char g_authorization[6 + 4 * ((2 * kLoginSize + 2) / 3) + 1] = {};

// The JPEG of the newest frame, shared by the web readers.
struct Jpeg {
    uint32_t generation = 0;
    bool half = false;
    uint8_t quality = 0;
    size_t size = 0;
    std::shared_ptr<uint8_t> data;
};

std::mutex g_jpeg_mutex;
jpeg_encoder_handle_t g_encoder = nullptr;
uint8_t *g_jpeg_work = nullptr; // the encoder's output, from jpeg_alloc_encoder_mem
size_t g_jpeg_work_size = 0;
uint16_t *g_half = nullptr;     // the frame at half size, the encoder's input
size_t g_half_size = 0;
Jpeg g_jpeg;

struct EncodeContext {
    Jpeg *out;
};

void encode(const uint16_t *pixels, uint16_t width, uint16_t height, void *context)
{
    auto &ctx = *static_cast<EncodeContext *>(context);
    if (ctx.out->half) {
        core::shrink(pixels, width, height, width, 1, g_half);
        pixels = g_half;
        width = static_cast<uint16_t>(core::scaled_size(width, 1));
        height = static_cast<uint16_t>(core::scaled_size(height, 1));
    }
    const jpeg_encode_cfg_t config{.height = height,
                                   .width = width,
                                   .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
                                   .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
                                   .image_quality = ctx.out->quality,
                                   .pixel_reverse = false};
    uint32_t size = 0;
    const esp_err_t err = jpeg_encoder_process(g_encoder, &config, reinterpret_cast<const uint8_t *>(pixels),
                                               uint32_t{width} * height * 2, g_jpeg_work, g_jpeg_work_size, &size);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "JPEG: %s", esp_err_to_name(err));
        return;
    }
    auto *copy = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (copy == nullptr) {
        return;
    }
    std::memcpy(copy, g_jpeg_work, size);
    ctx.out->size = size;
    ctx.out->data = std::shared_ptr<uint8_t>(copy, [](uint8_t *p) { heap_caps_free(p); });
}

bool prepare_encoder()
{
    if (g_encoder != nullptr) {
        return true;
    }
    const jpeg_encode_engine_cfg_t engine{.intr_priority = 0, .timeout_ms = 200, .flags = {}};
    if (jpeg_new_encoder_engine(&engine, &g_encoder) != ESP_OK) {
        return false;
    }
    const size_t pixels = size_t{width()} * height();
    const jpeg_encode_memory_alloc_cfg_t output{.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
    g_jpeg_work = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(pixels, &output, &g_jpeg_work_size));
    const jpeg_encode_memory_alloc_cfg_t input{.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
    g_half = static_cast<uint16_t *>(jpeg_alloc_encoder_mem(pixels / 4 * sizeof(uint16_t) + 64, &input, &g_half_size));
    return g_jpeg_work != nullptr && g_half != nullptr;
}

// A JPEG newer than `generation` in the stream's size and quality, waiting up to `timeout_ms`.
bool take_jpeg(uint32_t &generation, uint32_t timeout_ms, const Stream &settings, Jpeg &out)
{
    std::lock_guard<std::mutex> lock(g_jpeg_mutex);
    if (!prepare_encoder()) {
        return false;
    }
    // the JPEG kept is only good while it is of the newest frame: a snapshot (generation 0) after
    // the capture was off got the picture of minutes before
    const bool same = g_jpeg.data != nullptr && g_jpeg.half == settings.half && g_jpeg.quality == settings.quality &&
                      g_jpeg.generation == detail::newest_generation();
    if (!same || g_jpeg.generation <= generation) {
        uint32_t frame = generation;
        Jpeg fresh;
        fresh.half = settings.half;
        fresh.quality = settings.quality;
        EncodeContext context{&fresh};
        if (!use_frame(frame, timeout_ms, encode, &context) || fresh.data == nullptr) {
            return false;
        }
        fresh.generation = frame;
        g_jpeg = fresh;
    }
    out = g_jpeg;
    generation = g_jpeg.generation;
    return true;
}

// false with the answer sent: 404 while the stream is off, 401 without its login
bool allowed(httpd_req_t *req)
{
    if (!stream().enabled) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return false;
    }
    char value[sizeof(g_authorization)] = {};
    if (httpd_req_get_hdr_value_str(req, "Authorization", value, sizeof(value)) == ESP_OK) {
        std::lock_guard<std::mutex> lock(g_auth_mutex);
        if (g_authorization[0] != 0 && std::strcmp(value, g_authorization) == 0) {
            return true;
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"camera\"");
    httpd_resp_sendstr(req, "login required\n");
    return false;
}

esp_err_t get_snapshot(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    // A capture that starts for this picture gives a few frames first: the first one comes before
    // the exposure and the white balance have settled (dark and green).
    const bool started = detail::newest_generation() == 0;
    acquire();
    uint32_t generation = 0;
    for (int i = 0; started && i < kSettleFrames; i++) {
        use_frame(generation, 1000, [](const uint16_t *, uint16_t, uint16_t, void *) {}, nullptr);
    }
    Jpeg jpeg;
    const bool ok = take_jpeg(generation, 3000, stream(), jpeg);
    release();
    if (!ok) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "no frame\n");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char *>(jpeg.data.get()), jpeg.size);
}

std::atomic<int> g_streams{0};

void stream_task(void *arg)
{
    auto *req = static_cast<httpd_req_t *>(arg);
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    acquire();
    uint32_t generation = 0;
    uint32_t frames = 0;
    int64_t next = 0;
    for (;;) {
        const Stream settings = stream();
        if (!settings.enabled) {
            break; // turned off while it played
        }
        const int64_t wait = next - esp_timer_get_time();
        if (wait > 0) {
            vTaskDelay(pdMS_TO_TICKS(wait / 1000) + 1);
        }
        next = esp_timer_get_time() + 1000000 / settings.max_fps;
        Jpeg jpeg;
        if (!take_jpeg(generation, 3000, settings, jpeg)) {
            ESP_LOGW(kTag, "stream: no frame");
            break;
        }
        char header[96];
        const int length = std::snprintf(header, sizeof(header),
                                         "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                                         static_cast<unsigned>(jpeg.size));
        if (httpd_resp_send_chunk(req, header, length) != ESP_OK ||
            httpd_resp_send_chunk(req, reinterpret_cast<const char *>(jpeg.data.get()), jpeg.size) != ESP_OK ||
            httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
            break;
        }
        frames++;
    }
    release();
    ESP_LOGI(kTag, "stream ended after %u frames", static_cast<unsigned>(frames));
    httpd_req_async_handler_complete(req);
    g_streams--;
    vTaskDelete(nullptr);
}

esp_err_t get_stream(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    if (g_streams.fetch_add(1) >= kMaxStreams) {
        g_streams--;
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "too many streams\n");
    }
    httpd_req_t *copy = nullptr;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        g_streams--;
        return ESP_FAIL;
    }
    if (xTaskCreate(stream_task, "cam_stream", 4096, copy, 4, nullptr) != pdPASS) {
        httpd_req_async_handler_complete(copy);
        g_streams--;
        return ESP_FAIL;
    }
    return ESP_OK;
}

} // namespace

namespace detail {

void login_changed(const Stream &stream)
{
    std::lock_guard<std::mutex> lock(g_auth_mutex);
    g_authorization[0] = 0;
    if (stream.username[0] == 0 || stream.password[0] == 0) {
        return;
    }
    char login[2 * kLoginSize + 1];
    const int length = std::snprintf(login, sizeof(login), "%s:%s", stream.username, stream.password);
    std::memcpy(g_authorization, "Basic ", 6);
    size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(g_authorization) + 6, sizeof(g_authorization) - 6,
                              &written, reinterpret_cast<const unsigned char *>(login), length) != 0) {
        g_authorization[0] = 0;
    }
}

} // namespace detail

void register_routes(void *server)
{
    if (!available()) {
        return;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/api/camera.jpg", .method = HTTP_GET, .handler = get_snapshot, .user_ctx = nullptr},
        {.uri = "/api/camera.mjpg", .method = HTTP_GET, .handler = get_stream, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        if (httpd_register_uri_handler(static_cast<httpd_handle_t>(server), &route) != ESP_OK) {
            ESP_LOGW(kTag, "route %s not registered", route.uri);
        }
    }
}

} // namespace hc::camera
