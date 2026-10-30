// A file from an http:// or https:// URL onto the card (the web UI's SD card page). The download
// follows hc::ota's (components/ota/src/ota.cpp): esp_http_client with the IDF certificate bundle,
// redirects by hand, one job at a time in a task of its own.

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/sdcard/sdcard.hpp"

namespace hc::sdcard {

namespace {

constexpr const char *kTag = "sdcard";
constexpr size_t kUrlSize = 257;
constexpr int kMaxRedirects = 5;
constexpr size_t kBufferSize = 4096;
// TLS needs the larger stack; PSRAM, the JC3248W535 has no 8 KB of internal RAM to spare
constexpr uint32_t kStackSize = 8192;

struct Job {
    char url[kUrlSize];
    char path[kMaxPath];
};

portMUX_TYPE g_status_lock = portMUX_INITIALIZER_UNLOCKED;
FetchStatus g_status{};

void set_progress(size_t done, size_t total)
{
    taskENTER_CRITICAL(&g_status_lock);
    g_status.done = done;
    g_status.total = total;
    taskEXIT_CRITICAL(&g_status_lock);
}

esp_err_t fail(esp_err_t err, const char *format, ...)
{
    char error[sizeof(g_status.error)];
    va_list args;
    va_start(args, format);
    std::vsnprintf(error, sizeof(error), format, args);
    va_end(args);
    ESP_LOGW(kTag, "download: %s", error);
    taskENTER_CRITICAL(&g_status_lock);
    g_status.state = FetchState::Failed;
    std::memcpy(g_status.error, error, sizeof(error));
    taskEXIT_CRITICAL(&g_status_lock);
    return err;
}

esp_err_t download(const Job &job)
{
    esp_http_client_config_t config{};
    config.url = job.url;
    config.timeout_ms = 15000;
    config.buffer_size = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return fail(ESP_ERR_NO_MEM, "no memory for the download");
    }

    int64_t length = 0;
    // redirects are followed by hand: open() and read() do not follow them
    for (int redirects = 0;; redirects++) {
        const esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            return fail(err, "cannot connect (%s)", esp_err_to_name(err));
        }
        length = esp_http_client_fetch_headers(client);
        const int code = esp_http_client_get_status_code(client);
        if (code >= 300 && code < 400 && redirects < kMaxRedirects) {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        if (code != 200) {
            esp_http_client_cleanup(client);
            return fail(ESP_FAIL, "the server answered %d", code);
        }
        break;
    }

    char part[kMaxPath + 5];
    std::snprintf(part, sizeof(part), "%s.part", job.path);
    FILE *file = std::fopen(part, "wb");
    auto *buffer = static_cast<char *>(std::malloc(kBufferSize));
    esp_err_t err = ESP_OK;
    if (file == nullptr) {
        err = fail(ESP_FAIL, "cannot create the file (%s)", std::strerror(errno));
    } else if (buffer == nullptr) {
        err = fail(ESP_ERR_NO_MEM, "no memory for the download");
    }
    const size_t total = length > 0 ? static_cast<size_t>(length) : 0;
    size_t done = 0;
    set_progress(0, total);
    while (err == ESP_OK) {
        const int n = esp_http_client_read(client, buffer, kBufferSize);
        if (n < 0) {
            err = fail(ESP_FAIL, "the download broke off");
        } else if (n == 0) {
            if (!esp_http_client_is_complete_data_received(client)) {
                err = fail(ESP_FAIL, "the download broke off");
            }
            break;
        } else if (std::fwrite(buffer, 1, static_cast<size_t>(n), file) != static_cast<size_t>(n)) {
            err = fail(ESP_FAIL, "writing failed (card full?)");
        } else {
            done += static_cast<size_t>(n);
            set_progress(done, total);
        }
    }
    std::free(buffer);
    esp_http_client_cleanup(client);
    if (file != nullptr && std::fclose(file) != 0 && err == ESP_OK) {
        err = fail(ESP_FAIL, "writing failed (card full?)");
    }
    if (err == ESP_OK) {
        // an older file of the name is replaced
        std::remove(job.path);
        if (std::rename(part, job.path) != 0) {
            err = fail(ESP_FAIL, "cannot rename the file (%s)", std::strerror(errno));
        }
    }
    if (err != ESP_OK) {
        std::remove(part);
        return err;
    }
    ESP_LOGI(kTag, "downloaded %s (%zu bytes)", job.path, done);
    taskENTER_CRITICAL(&g_status_lock);
    g_status.state = FetchState::Done;
    taskEXIT_CRITICAL(&g_status_lock);
    return ESP_OK;
}

void fetch_task(void *arg)
{
    auto *job = static_cast<Job *>(arg);
    download(*job);
    delete job;
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

esp_err_t fetch_start(const char *url, const char *path)
{
    if ((std::strncmp(url, "http://", 7) != 0 && std::strncmp(url, "https://", 8) != 0) ||
        std::strlen(url) >= kUrlSize) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *job = new Job{};
    std::snprintf(job->url, sizeof(job->url), "%s", url);
    // room for ".part"
    if (!make_path(path, job->path, sizeof(job->path) - 5) || std::strcmp(job->path, kMountPoint) == 0) {
        delete job;
        return ESP_ERR_INVALID_ARG;
    }
    if (status().state != State::Mounted) {
        delete job;
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&g_status_lock);
    const bool running = g_status.state == FetchState::Running;
    if (!running) {
        g_status = {FetchState::Running, 0, 0, {}, {}};
        std::memcpy(g_status.path, job->path, sizeof(g_status.path));
    }
    taskEXIT_CRITICAL(&g_status_lock);
    if (running) {
        delete job;
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreateWithCaps(fetch_task, "sd_fetch", kStackSize, job, 5, nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
        delete job;
        taskENTER_CRITICAL(&g_status_lock);
        g_status.state = FetchState::Idle;
        taskEXIT_CRITICAL(&g_status_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

FetchStatus fetch_status()
{
    taskENTER_CRITICAL(&g_status_lock);
    const FetchStatus out = g_status;
    taskEXIT_CRITICAL(&g_status_lock);
    return out;
}

} // namespace hc::sdcard
