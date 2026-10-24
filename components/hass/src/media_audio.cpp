// The music of the media library (docs/architecture.md section 9.6), downloaded while it plays:
// one task per file writes what arrives into the player's ByteRing. A media-source:// id is
// resolved first (Home Assistant signs the address for 24 hours); the address is kept until a
// connection fails. The download comes in bursts: once the ring is full the connection goes and
// a new one fetches the rest (Range) when half of the ring was played. A connection kept open
// with a full ring holds its whole TCP window in Wi-Fi's receive buffers, internal RAM: 76 KB on
// the P4, the last 10 KB on the JC3248W535 (2026-10-23). A seek fetches the file again from there,
// so does a connection that broke off; the task lives until the player cancels the ring, also
// after the file is complete (a seek back fetches again). Its stack is PSRAM: it touches no flash.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "internal.hpp"

namespace hc::hass {

namespace {

constexpr const char *kTag = "music";
constexpr size_t kReadSize = 4096;
// a connection that gives nothing for this long counts as broken
constexpr int kTimeoutMs = 10000;
constexpr uint32_t kResolveTimeoutMs = 10000;
// a broken connection is tried again this often, this long apart, before the download fails
constexpr int kRetries = 5;
constexpr TickType_t kRetryDelay = pdMS_TO_TICKS(2000);
// the ring is full or the file is complete: the player's seek and cancel are looked at this often
constexpr TickType_t kIdlePoll = pdMS_TO_TICKS(20);
// esp_http_client, and with https the TLS handshake (as say.cpp)
constexpr uint32_t kStackHttp = 6144;
constexpr uint32_t kStackHttps = 8192;
constexpr int kMaxRedirects = 3;

// what the task needs, in PSRAM: the token is 512 bytes, a signed address about 400
struct Job {
    memory::String source;
    std::shared_ptr<core::ByteRing> ring;
    char url[kUrlSize];
    char token[kTokenSize];
    char authorization[kTokenSize + 8];
    char range[32];
    // where a redirect leads
    char location[512];
    // the address of the file once known, and whether it gets the token
    memory::String address;
    bool authorize = false;
    uint8_t buffer[kReadSize];
};

void *psram(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

struct Client {
    esp_http_client_handle_t handle = nullptr;

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
    }
};

// The address of the file: a media-source:// id resolved, a path of Home Assistant, or an
// address as it is. False when there is none.
bool address_of(Job &job, bool &authorize)
{
    memory::String path = job.source;
    if (path.rfind("media-source://", 0) == 0) {
        memory::String mime, error;
        if (!job.address.empty()) {
            // resolved before, still valid: the signature lasts 24 hours
            authorize = job.authorize;
            return true;
        }
        if (!resolve_media(job.source, kResolveTimeoutMs, path, mime, error)) {
            ESP_LOGW(kTag, "%s: %s", job.source.c_str(), error.c_str());
            return false;
        }
    }
    if (!hass_protocol::sound_url(job.url, path, job.address, authorize)) {
        ESP_LOGW(kTag, "not an address: %s", path.c_str());
        job.address.clear();
        return false;
    }
    job.authorize = authorize;
    return true;
}

// Opens `address` from `offset` on; false when it cannot. A server that ignores the range sends
// the whole file: its bytes before `offset` are read past.
bool open_address(Job &job, Client &c, uint32_t offset, bool authorize)
{
    std::snprintf(job.range, sizeof(job.range), "bytes=%lu-", static_cast<unsigned long>(offset));
    memory::String url = job.address;
    for (int redirect = 0; redirect <= kMaxRedirects; redirect++) {
        c.close();
        esp_http_client_config_t config{};
        config.url = url.c_str();
        config.timeout_ms = kTimeoutMs;
        config.crt_bundle_attach = detail::tls_attach;
        config.buffer_size_tx = 1024;
        config.user_agent = "hassctrl";
        c.handle = esp_http_client_init(&config);
        if (c.handle == nullptr) {
            return false;
        }
        if (authorize && redirect == 0) {
            std::snprintf(job.authorization, sizeof(job.authorization), "Bearer %s", job.token);
            esp_http_client_set_header(c.handle, "Authorization", job.authorization);
        }
        if (offset > 0) {
            esp_http_client_set_header(c.handle, "Range", job.range);
        }
        if (esp_http_client_open(c.handle, 0) != ESP_OK) {
            ESP_LOGW(kTag, "cannot connect to %.60s", url.c_str());
            return false;
        }
        const int64_t length = esp_http_client_fetch_headers(c.handle);
        const int status = esp_http_client_get_status_code(c.handle);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            // the client knows the new address (esp_http_client_get_header reads the request's)
            if (esp_http_client_set_redirection(c.handle) != ESP_OK ||
                esp_http_client_get_url(c.handle, job.location, sizeof(job.location)) != ESP_OK) {
                return false;
            }
            url = job.location;
            continue;
        }
        if (status == 206) {
            // the rest of the file from `offset`
            if (length > 0) {
                job.ring->set_size(offset + static_cast<uint32_t>(length));
            }
            return true;
        }
        if (status != 200) {
            ESP_LOGW(kTag, "HTTP %d for %.80s", status, url.c_str());
            return false;
        }
        if (length > 0) {
            job.ring->set_size(static_cast<uint32_t>(length));
        }
        // the whole file: read past what the player has
        for (uint32_t skipped = 0; skipped < offset;) {
            const int read = esp_http_client_read(c.handle, reinterpret_cast<char *>(job.buffer),
                                                  static_cast<int>(std::min<uint32_t>(kReadSize, offset - skipped)));
            if (read <= 0 || job.ring->cancelled()) {
                return false;
            }
            skipped += static_cast<uint32_t>(read);
        }
        return true;
    }
    return false;
}

// Opens the file from `offset` on, its address resolved when it is not known; false when it cannot.
bool connect(Job &job, Client &c, uint32_t offset)
{
    bool authorize = false;
    if (!address_of(job, authorize)) {
        return false;
    }
    // a failure resolves again next time (a signature that ran out, a moved file)
    const bool opened = open_address(job, c, offset, authorize);
    if (!opened) {
        job.address.clear();
    }
    return opened;
}

void task(void *arg)
{
    std::unique_ptr<Job, void (*)(Job *)> job(static_cast<Job *>(arg), [](Job *j) {
        j->~Job();
        heap_caps_free(j);
    });
    core::ByteRing &ring = *job->ring;
    Client c;
    // the file's offset of the next byte written, and the connection is open for it
    uint32_t offset = 0;
    bool open = false;
    bool complete = false;
    // the ring was full: the next burst starts when half of it is free
    bool waiting = false;
    int retries = 0;
    ESP_LOGI(kTag, "%s", job->source.c_str());
    while (!ring.cancelled()) {
        uint32_t seek = 0;
        if (ring.take_seek(seek)) {
            c.close();
            ring.restart(seek);
            offset = seek;
            open = complete = waiting = false;
            retries = 0;
        }
        if (!complete && !waiting && ring.space() < kReadSize) {
            // full: the connection goes, with the TCP window it holds
            c.close();
            open = false;
            waiting = true;
        }
        if (complete || (waiting && ring.space() < ring.capacity() / 2)) {
            vTaskDelay(kIdlePoll);
            continue;
        }
        waiting = false;
        if (!open) {
            open = connect(*job, c, offset);
            if (!open) {
                if (++retries > kRetries) {
                    ESP_LOGW(kTag, "given up after %d tries", kRetries);
                    ring.finish(true);
                    complete = true;
                } else {
                    vTaskDelay(kRetryDelay);
                }
                continue;
            }
        }
        const int read = esp_http_client_read(c.handle, reinterpret_cast<char *>(job->buffer), kReadSize);
        if (read > 0) {
            // a seek that came meanwhile empties the ring: these bytes are from before it
            if (!ring.seeking()) {
                ring.write(job->buffer, static_cast<size_t>(read));
                offset += static_cast<uint32_t>(read);
            }
            retries = 0;
            continue;
        }
        if (read == 0 && esp_http_client_is_complete_data_received(c.handle)) {
            ESP_LOGI(kTag, "complete, %lu bytes", static_cast<unsigned long>(offset));
            c.close();
            open = false;
            ring.finish(false);
            complete = true;
            continue;
        }
        // broken off: again from where it was
        ESP_LOGW(kTag, "the connection broke at %lu bytes (%d)", static_cast<unsigned long>(offset), read);
        c.close();
        open = false;
    }
    ESP_LOGI(kTag, "ended");
    c.close();
    job.reset();
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

bool stream_media(std::string_view source, std::shared_ptr<core::ByteRing> ring)
{
    if (!ring || !ring->valid()) {
        return false;
    }
    void *memory = psram(sizeof(Job));
    if (memory == nullptr) {
        ring->finish(true);
        return false;
    }
    auto *job = new (memory) Job{};
    job->source = memory::String(source);
    job->ring = std::move(ring);
    detail::credentials(job->url, sizeof(job->url), job->token, sizeof(job->token));
    const bool https = std::strncmp(job->url, "https:", 6) == 0 || source.rfind("https:", 0) == 0;
    const uint32_t stack = https ? kStackHttps : kStackHttp;
    if (xTaskCreateWithCaps(task, "music", stack, job, 3, nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(kTag, "no memory for a task (%u bytes of stack)", static_cast<unsigned>(stack));
        job->ring->finish(true);
        job->~Job();
        heap_caps_free(memory);
        return false;
    }
    return true;
}

} // namespace hc::hass
