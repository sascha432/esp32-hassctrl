// The sounds of the hassctrl_say event (docs/architecture.md section 9.6), in a task of its own:
// the speech of a message is asked of Home Assistant (POST /api/tts_get_url), then the MP3 is
// downloaded into PSRAM and handed to the player. The task exists only while a sound is on its
// way, so its stack (TLS with https) takes internal RAM only then. A newer sound stops a download
// between two reads and takes its place.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "internal.hpp"

namespace hc::hass::detail {

namespace {

constexpr const char *kTag = "say";
// a TTS clip is tens of KB; a longer sound from a URL up to this
constexpr size_t kMaxSound = 1024 * 1024;
// the answer of tts_get_url is a few hundred bytes
constexpr size_t kMaxAnswer = 2048;
// a cloud engine speaks before tts_get_url answers
constexpr int kTimeoutMs = 15000;
constexpr int kMaxRedirects = 3;
// esp_http_client, and with https the TLS handshake (as media.cpp's tasks)
constexpr uint32_t kStackHttp = 6144;
constexpr uint32_t kStackHttps = 8192;

std::mutex g_mutex;
// the sound that waits for the task, under g_mutex
std::optional<hass_protocol::Say> g_next;
// under g_mutex: the task runs, with this much stack
uint32_t g_task_stack = 0;
// counts the requests: a download of an older one stops
std::atomic<uint32_t> g_wanted{0};

// the task's buffers for the settings and the header, not on its stack: the token is 512 bytes
struct Context {
    char url[kUrlSize];
    char token[kTokenSize];
    char authorization[kTokenSize + 8];
    // where a redirect leads
    char location[512];
};

void *psram(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

bool is_https(std::string_view url)
{
    return url.substr(0, 6) == "https:";
}

// with https on the way the task needs the larger stack; an address of another server may
// redirect to https
uint32_t stack_for(const hass_protocol::Say &say)
{
    char url[kUrlSize];
    char token[8];
    credentials(url, sizeof(url), token, sizeof(token));
    const bool other_server = !say.url.empty() && say.url[0] != '/';
    return is_https(url) || other_server ? kStackHttps : kStackHttp;
}

struct Client {
    esp_http_client_handle_t handle = nullptr;
    // of the answer; 0: none came
    int status = 0;

    Client() = default;
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    ~Client()
    {
        close();
    }
    void close()
    {
        if (handle != nullptr) {
            esp_http_client_close(handle);
            esp_http_client_cleanup(handle);
            handle = nullptr;
        }
        status = 0;
    }
};

// Sends the request and reads the headers of the answer; false with `error` set when none came.
bool request(Context &ctx, Client &c, const char *url, bool authorize, const char *body, int64_t &length,
             char *error, size_t error_size)
{
    esp_http_client_config_t config{};
    config.url = url;
    config.timeout_ms = kTimeoutMs;
    config.crt_bundle_attach = tls_attach;
    // the token goes into a header longer than the default send buffer
    config.buffer_size_tx = 1024;
    config.user_agent = "hassctrl";
    config.method = body != nullptr ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    c.handle = esp_http_client_init(&config);
    if (c.handle == nullptr) {
        std::snprintf(error, error_size, "no memory for the request");
        return false;
    }
    if (authorize) {
        std::snprintf(ctx.authorization, sizeof(ctx.authorization), "Bearer %s", ctx.token);
        esp_http_client_set_header(c.handle, "Authorization", ctx.authorization);
    }
    const int body_length = body != nullptr ? static_cast<int>(std::strlen(body)) : 0;
    if (body != nullptr) {
        esp_http_client_set_header(c.handle, "Content-Type", "application/json");
    }
    if (esp_http_client_open(c.handle, body_length) != ESP_OK) {
        std::snprintf(error, error_size, "cannot connect");
        return false;
    }
    if (body_length > 0 && esp_http_client_write(c.handle, body, body_length) != body_length) {
        std::snprintf(error, error_size, "the request could not be sent");
        return false;
    }
    length = esp_http_client_fetch_headers(c.handle);
    if (length < 0) {
        std::snprintf(error, error_size, "no answer within %d s", kTimeoutMs / 1000);
        return false;
    }
    c.status = esp_http_client_get_status_code(c.handle);
    return true;
}

// Opens `url` (a POST of `body` when there is one) and reads the headers; false with `error` set
// unless the answer is 200. A GET follows a few redirects (a sound on a web server), without the
// token. Every step gets a client of its own: esp_http_client keeps its transport when the
// address changes, and sent plain HTTP to the https of a redirect (HTTP 400).
bool open(Context &ctx, Client &c, const char *url, bool authorize, const char *body, int64_t &length, char *error,
          size_t error_size)
{
    for (int redirects = 0;; redirects++) {
        c.close();
        if (!request(ctx, c, url, authorize, body, length, error, error_size)) {
            return false;
        }
        const bool redirect = c.status == 301 || c.status == 302 || c.status == 303 || c.status == 307 ||
                              c.status == 308;
        if (!redirect || body != nullptr || redirects == kMaxRedirects ||
            esp_http_client_set_redirection(c.handle) != ESP_OK ||
            esp_http_client_get_url(c.handle, ctx.location, sizeof(ctx.location)) != ESP_OK) {
            break;
        }
        ESP_LOGI(kTag, "HTTP %d, on to %s", c.status, ctx.location);
        url = ctx.location;
        authorize = false;
    }
    if (c.status != 200) {
        std::snprintf(error, error_size, c.status == 401 ? "the token was rejected" : "HTTP %d", c.status);
        return false;
    }
    return true;
}

// The body into a PSRAM buffer of at most `max` bytes and a terminator; false with `error` set,
// also when a newer request came (`generation`).
bool read_body(Client &c, int64_t length, size_t max, uint32_t generation, uint8_t *&data, size_t &size, char *error,
               size_t error_size)
{
    data = nullptr;
    size = 0;
    if (length > static_cast<int64_t>(max)) {
        std::snprintf(error, error_size, "too large (%lld KB)", static_cast<long long>(length / 1024));
        return false;
    }
    // without a length the buffer grows as the data arrives
    size_t capacity = 0;
    size_t wanted = length > 0 ? static_cast<size_t>(length) : 16 * 1024;
    for (;;) {
        if (length > 0 && size == static_cast<size_t>(length)) {
            data[size] = 0;
            return true;
        }
        if (size == capacity) {
            if (capacity >= max) {
                std::snprintf(error, error_size, "too large (more than %u KB)", static_cast<unsigned>(max / 1024));
                break;
            }
            wanted = capacity == 0 ? wanted : std::min(capacity * 2, max);
            auto *grown = static_cast<uint8_t *>(heap_caps_realloc(data, wanted + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (grown == nullptr) {
                std::snprintf(error, error_size, "no memory for %u KB", static_cast<unsigned>(wanted / 1024));
                break;
            }
            data = grown;
            capacity = wanted;
        }
        const int read = esp_http_client_read(c.handle, reinterpret_cast<char *>(data + size),
                                              static_cast<int>(capacity - size));
        if (read < 0 || (read == 0 && length > 0)) {
            std::snprintf(error, error_size, "the download broke off");
            break;
        }
        if (g_wanted.load() != generation) {
            std::snprintf(error, error_size, "replaced by a newer sound");
            break;
        }
        if (read == 0) {
            data[size] = 0;
            return true;
        }
        size += static_cast<size_t>(read);
    }
    heap_caps_free(data);
    data = nullptr;
    size = 0;
    return false;
}

// The path of the speech of `say` (tts_get_url); false with `error` set.
bool speech_path(Context &ctx, const hass_protocol::Say &say, memory::String &path, char *error, size_t error_size)
{
    char url[kUrlSize + 24];
    std::snprintf(url, sizeof(url), "%s/api/tts_get_url", ctx.url);
    const memory::String body = hass_protocol::tts_url_request(say);
    Client c;
    int64_t length = 0;
    uint8_t *data = nullptr;
    size_t size = 0;
    if (!open(ctx, c, url, true, body.c_str(), length, error, error_size)) {
        // HA says what is wrong in the body of a 400 ("Must specify platform and message")
        char unused[8];
        if (c.status >= 400 && read_body(c, length, kMaxAnswer, g_wanted.load(), data, size, unused, sizeof(unused))) {
            const size_t used = std::strlen(error);
            std::snprintf(error + used, error_size - used, ": %.80s", reinterpret_cast<const char *>(data));
            heap_caps_free(data);
        }
        return false;
    }
    if (!read_body(c, length, kMaxAnswer, g_wanted.load(), data, size, error, error_size)) {
        return false;
    }
    cJSON *answer = cJSON_ParseWithLength(reinterpret_cast<const char *>(data), size);
    const bool ok = hass_protocol::decode_tts_url(answer, path);
    cJSON_Delete(answer);
    heap_caps_free(data);
    if (!ok) {
        std::snprintf(error, error_size, "Home Assistant answered without the address of the speech");
    }
    return ok;
}

void play(Context &ctx, const hass_protocol::Say &say, uint32_t generation)
{
    credentials(ctx.url, sizeof(ctx.url), ctx.token, sizeof(ctx.token));
    const char *what = !say.message.empty() ? say.message.c_str() : say.url.c_str();
    char error[128] = {};
    const int64_t started = esp_timer_get_time();
    memory::String path = say.url;
    if (!say.message.empty()) {
        if (ctx.url[0] == 0) {
            ESP_LOGW(kTag, "\"%s\": Home Assistant is not set up", what);
            return;
        }
        if (!speech_path(ctx, say, path, error, sizeof(error))) {
            ESP_LOGW(kTag, "\"%s\": speech: %s", what, error);
            return;
        }
    }
    memory::String url;
    bool authorize = false;
    if (!hass_protocol::sound_url(ctx.url, path, url, authorize) || (authorize && ctx.url[0] == 0)) {
        ESP_LOGW(kTag, "%s: not an address of a sound", path.c_str());
        return;
    }
    Client c;
    int64_t length = 0;
    uint8_t *data = nullptr;
    size_t size = 0;
    if (!open(ctx, c, url.c_str(), authorize, nullptr, length, error, sizeof(error)) ||
        !read_body(c, length, kMaxSound, generation, data, size, error, sizeof(error))) {
        ESP_LOGW(kTag, "%s: %s", url.c_str(), error);
        return;
    }
    // what the decoder does not know is said here rather than as a decoder error
    if (size < 4 || std::memcmp(data, "RIFF", 4) == 0) {
        ESP_LOGW(kTag, "%s: %s", url.c_str(), size < 4 ? "empty" : "a WAV file, only MP3 plays");
        heap_caps_free(data);
        return;
    }
    ESP_LOGI(kTag, "%s: %u KB in %lld ms", url.c_str(), static_cast<unsigned>((size + 1023) / 1024),
             static_cast<long long>((esp_timer_get_time() - started) / 1000));
    // the player takes the buffer
    sound_player()(data, size);
}

void task(void *arg)
{
    auto *ctx = static_cast<Context *>(arg);
    for (;;) {
        hass_protocol::Say say;
        uint32_t generation;
        {
            std::lock_guard lock(g_mutex);
            // the next one needs https and this task has the smaller stack: one of its own
            if (!g_next || stack_for(*g_next) > g_task_stack) {
                g_task_stack = 0;
                break;
            }
            say = std::move(*g_next);
            g_next.reset();
            generation = g_wanted.load();
        }
        play(*ctx, say, generation);
    }
    std::free(ctx);
    // a sound that came while the task ended
    start_say({});
    vTaskDelete(nullptr);
}

} // namespace

void start_say(hass_protocol::Say say)
{
    if (sound_player() == nullptr) {
        ESP_LOGW(kTag, "no player for sounds");
        return;
    }
    std::lock_guard lock(g_mutex);
    // empty: no new sound, only a task for the one that waits (the end of task())
    if (!say.message.empty() || !say.url.empty()) {
        g_next = std::move(say);
        g_wanted++;
    }
    if (!g_next || g_task_stack != 0) {
        return;
    }
    const uint32_t stack = stack_for(*g_next);
    auto *ctx = static_cast<Context *>(psram(sizeof(Context)));
    if (ctx == nullptr || xTaskCreate(task, "say", stack, ctx, 3, nullptr) != pdPASS) {
        std::free(ctx);
        g_next.reset();
        ESP_LOGE(kTag, "no memory for a task (%u bytes of stack)", static_cast<unsigned>(stack));
        return;
    }
    g_task_stack = stack;
}

} // namespace hc::hass::detail
