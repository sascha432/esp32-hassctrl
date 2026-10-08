// Firmware and file system updates (docs/ui/web.md section 8), login required after the setup:
//
//   GET  /api/update              which app runs, what it can update, the state of an update
//   POST /api/update/firmware     the body is the firmware image (application/octet-stream)
//   POST /api/update/filesystem   the body is a LittleFS image of the "storage" partition
//   POST /api/update/url          target=firmware|filesystem, url=http(s)://...: the device
//                                 downloads the image; the progress follows in GET /api/update
//   POST /api/restart             to=firmware|recovery (optional): restarts, into that app
//   POST /api/factory-reset       confirm=yes: erases every setting (networks, login, ...) and
//                                 restarts; the web files on LittleFS stay
//
// The body of an upload is the raw image, so a tool can push a build without a browser:
//
//   curl -b cookie.txt --data-binary @build/.../hassctrl.bin http://<device>/api/update/firmware
//
// The main firmware on a 4 MB board cannot write its own partition: /api/update/firmware answers
// 409 with "recovery": true, the page restarts the device into the recovery app
// (/api/restart to=recovery) and sends the image there. A firmware update restarts the device
// when it is done.

#include <cstdlib>
#include <cstring>

#include "hc/ota/ota.hpp"
#include "hc/platform/settings.hpp"

#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

constexpr size_t kChunk = 4096;
// a stalled upload blocks the server (one task): at most 3 receive timeouts of 5 s in a row
constexpr int kMaxTimeouts = 3;
constexpr uint32_t kRestartDelayMs = 1000;

esp_err_t get_update(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    const ota::Info info = ota::info();
    const ota::Status status = ota::status();
    char buffer[512];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .string("app", info.recovery_app ? "recovery" : "main")
        .boolean("firmware_direct", info.firmware_direct)
        .boolean("firmware_present", info.firmware_present)
        .boolean("firmware_failed", info.firmware_failed)
        .boolean("confirmed", info.confirmed)
        .string("firmware_version", info.firmware_version)
        .number("firmware_max", info.firmware_max)
        .number("filesystem_max", info.filesystem_max)
        .string("state", ota::state_name(status.state))
        .string("target", ota::target_name(status.target))
        .number("done", status.done)
        .number("total", status.total)
        .string("error", status.error)
        .end_object();
    return send_json(req, json);
}

esp_err_t send_update_error(httpd_req_t *req, esp_err_t err)
{
    const ota::Status status = ota::status();
    const char *message = status.error[0] != 0 ? status.error : esp_err_to_name(err);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "another update is running");
    }
    return send_error(req, "400 Bad Request", message);
}

// the body of the request goes to the image writer
esp_err_t post_image(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    const auto target = static_cast<ota::Target>(reinterpret_cast<intptr_t>(req->user_ctx));
    if (target == ota::Target::Firmware && !ota::info().firmware_direct) {
        char buffer[96];
        JsonWriter json(buffer, sizeof(buffer));
        json.begin_object().string("error", "update from the recovery app").boolean("recovery", true).end_object();
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, json);
    }
    if (req->content_len == 0) {
        return send_error(req, "411 Length Required", "the image is empty");
    }
    esp_err_t err = ota::begin(target, req->content_len);
    if (err != ESP_OK) {
        return send_update_error(req, err);
    }

    auto *chunk = static_cast<char *>(std::malloc(kChunk));
    if (chunk == nullptr) {
        ota::abort();
        return send_error(req, "503 Service Unavailable", "no memory");
    }
    size_t remaining = req->content_len;
    int timeouts = 0;
    while (remaining > 0 && err == ESP_OK) {
        const int n = httpd_req_recv(req, chunk, remaining < kChunk ? remaining : kChunk);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kMaxTimeouts) {
            continue;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        remaining -= static_cast<size_t>(n);
        err = ota::write(chunk, static_cast<size_t>(n));
    }
    std::free(chunk);
    if (err != ESP_OK) {
        ota::abort();
        return send_update_error(req, err);
    }
    err = ota::finish();
    if (err != ESP_OK) {
        return send_update_error(req, err);
    }

    const bool restart = target == ota::Target::Firmware;
    char buffer[64];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("done", true).boolean("restart", restart).end_object();
    send_json(req, json);
    if (restart) {
        ota::restart_later(kRestartDelayMs);
    }
    return ESP_OK;
}

esp_err_t post_url(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[400];
    char target[16] = {};
    char url[256] = {};
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "target", target, sizeof(target)) ||
        !form_value(body, "url", url, sizeof(url))) {
        return send_error(req, "400 Bad Request", "target and url required");
    }
    const bool firmware = std::strcmp(target, "firmware") == 0;
    if (!firmware && std::strcmp(target, "filesystem") != 0) {
        return send_error(req, "400 Bad Request", "target is firmware or filesystem");
    }
    if (firmware && !ota::info().firmware_direct) {
        char buffer[96];
        JsonWriter json(buffer, sizeof(buffer));
        json.begin_object().string("error", "update from the recovery app").boolean("recovery", true).end_object();
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, json);
    }
    const esp_err_t err = ota::start_download(firmware ? ota::Target::Firmware : ota::Target::FileSystem, url);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request", "the URL has to start with http:// or https://");
    }
    if (err != ESP_OK) {
        return send_update_error(req, err);
    }
    httpd_resp_set_status(req, "202 Accepted");
    char buffer[48];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("downloading", true).end_object();
    return send_json(req, json);
}

esp_err_t post_restart(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[64] = {};
    char to[16] = {};
    if (req->content_len > 0) {
        read_body(req, body, sizeof(body));
        form_value(body, "to", to, sizeof(to));
    }
    esp_err_t err = ESP_OK;
    if (std::strcmp(to, "recovery") == 0) {
        err = ota::restart_into_recovery(kRestartDelayMs);
    } else if (std::strcmp(to, "firmware") == 0) {
        err = ota::restart_into_firmware(kRestartDelayMs);
    } else {
        ota::restart_later(kRestartDelayMs);
    }
    if (err != ESP_OK) {
        return send_error(req, "409 Conflict",
                          err == ESP_ERR_NOT_FOUND ? "no complete firmware in flash, upload one first" : esp_err_to_name(err));
    }
    char buffer[32];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("restart", true).end_object();
    return send_json(req, json);
}

esp_err_t post_factory_reset(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[32];
    char confirm[8] = {};
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "confirm", confirm, sizeof(confirm)) ||
        std::strcmp(confirm, "yes") != 0) {
        return send_error(req, "400 Bad Request", "confirm=yes required");
    }
    if (platform::erase_all_settings() != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "the settings could not be erased");
    }
    char buffer[32];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("restart", true).end_object();
    send_json(req, json);
    ota::restart_later(kRestartDelayMs);
    return ESP_OK;
}

} // namespace

void register_update_routes(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/update", .method = HTTP_GET, .handler = get_update, .user_ctx = nullptr},
        {.uri = "/api/update/firmware",
         .method = HTTP_POST,
         .handler = post_image,
         .user_ctx = reinterpret_cast<void *>(static_cast<intptr_t>(ota::Target::Firmware))},
        {.uri = "/api/update/filesystem",
         .method = HTTP_POST,
         .handler = post_image,
         .user_ctx = reinterpret_cast<void *>(static_cast<intptr_t>(ota::Target::FileSystem))},
        {.uri = "/api/update/url", .method = HTTP_POST, .handler = post_url, .user_ctx = nullptr},
        {.uri = "/api/restart", .method = HTTP_POST, .handler = post_restart, .user_ctx = nullptr},
        {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = post_factory_reset, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
