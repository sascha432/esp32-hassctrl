// The SD card page of the web UI (docs/ui/web/web-app.md section 4, SD card), CONFIG_HC_SDCARD only,
// login required like every route:
//
//   GET  /api/sd                 the card's state and space, the download from a URL
//   GET  /api/sd/list?dir=       the entries of a directory (name, size, dir)
//   GET  /api/sd/file?path=      a file, streamed
//   POST /api/sd/file?path=      the body is the file (not multipart); <path>.part, then renamed
//   POST /api/sd/delete          path: a file or an empty directory
//   POST /api/sd/mkdir           path
//   POST /api/sd/fetch           url, path: downloads in the background (202), progress in /api/sd
//   POST /api/sd/mount           action=mount|unmount
//   POST /api/sd/format          confirm=yes and password (the login's, again): erases the card
//
// Paths are relative to the card and go through sdcard::make_path (no "..").

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/sdcard/sdcard.hpp"

#include "auth.hpp"
#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

constexpr size_t kChunk = 4096;
// a download reads this much at a time into a buffer the SDMMC DMA takes (cache line aligned)
constexpr size_t kReadChunk = 16384;
constexpr size_t kDmaAlign = 64;
// a path, URL-encoded: up to three characters per byte
constexpr size_t kQuerySize = 3 * sdcard::kMaxPath + 16;
constexpr int kMaxTimeouts = 3;
constexpr uint32_t kFailedPasswordDelayMs = 1000;

// the decoded value of `key` in the query string
bool query_text(httpd_req_t *req, const char *key, char *out, size_t size)
{
    char query[kQuerySize];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && form_value(query, key, out, size);
}

// the path named by `key` of the query (`body` nullptr) or the form body, as "/sd/..."
bool request_path(httpd_req_t *req, const char *body, const char *key, char *out)
{
    char relative[sdcard::kMaxPath];
    const bool found = body != nullptr ? form_value(body, key, relative, sizeof(relative))
                                       : query_text(req, key, relative, sizeof(relative));
    return found && sdcard::make_path(relative, out, sdcard::kMaxPath);
}

bool mounted(httpd_req_t *req)
{
    if (sdcard::status().state == sdcard::State::Mounted) {
        return true;
    }
    send_error(req, "409 Conflict", "no SD card mounted");
    return false;
}

const char *fetch_state_name(sdcard::FetchState state)
{
    switch (state) {
    case sdcard::FetchState::Idle:
        return "idle";
    case sdcard::FetchState::Running:
        return "running";
    case sdcard::FetchState::Done:
        return "done";
    case sdcard::FetchState::Failed:
        return "failed";
    }
    return "?";
}

esp_err_t send_done(httpd_req_t *req, const char *key)
{
    char buffer[32];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean(key, true).end_object();
    return send_json(req, json);
}

esp_err_t get_sd(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    const sdcard::Status s = sdcard::status();
    const sdcard::FetchStatus f = sdcard::fetch_status();
    // the path without the mount point, as the web UI names it
    const size_t prefix = std::strlen(sdcard::kMountPoint) + 1;
    char buffer[512];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .string("state", sdcard::state_name(s.state))
        .number("total_kb", static_cast<int64_t>(s.total_bytes / 1024))
        .number("free_kb", static_cast<int64_t>(s.free_bytes / 1024))
        .string("name", s.name)
        .number("clock_khz", s.clock_khz)
        .begin_object("fetch")
        .string("state", fetch_state_name(f.state))
        .string("path", std::strlen(f.path) > prefix ? f.path + prefix : "")
        .number("done", static_cast<int64_t>(f.done))
        .number("total", static_cast<int64_t>(f.total))
        .string("error", f.error)
        .end_object()
        .end_object();
    return send_json(req, json);
}

// The entries one by one in chunks: a directory can be long, nothing larger than one entry is held.
esp_err_t get_list(httpd_req_t *req)
{
    if (!require_login(req) || !mounted(req)) {
        return ESP_OK;
    }
    // no dir: the root
    char relative[sdcard::kMaxPath] = "";
    query_text(req, "dir", relative, sizeof(relative));
    char path[sdcard::kMaxPath];
    if (!sdcard::make_path(relative, path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "not a path on the card");
    }
    DIR *dir = opendir(path);
    if (dir == nullptr) {
        return send_error(req, "404 Not Found", "no such directory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, "{\"entries\":[", HTTPD_RESP_USE_STRLEN);
    char entry_path[sdcard::kMaxPath + 256];
    char buffer[384];
    bool first = true;
    while (err == ESP_OK) {
        const dirent *entry = readdir(dir);
        if (entry == nullptr) {
            break;
        }
        std::snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);
        struct stat info {};
        stat(entry_path, &info);
        const bool is_dir = S_ISDIR(info.st_mode);
        buffer[0] = ',';
        JsonWriter json(buffer + 1, sizeof(buffer) - 1);
        json.begin_object()
            .string("name", entry->d_name)
            .number("size", is_dir ? 0 : static_cast<int64_t>(info.st_size))
            .boolean("dir", is_dir)
            .end_object();
        if (json.ok()) {
            err = httpd_resp_send_chunk(req, first ? buffer + 1 : buffer, HTTPD_RESP_USE_STRLEN);
            first = false;
        }
    }
    closedir(dir);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, nullptr, 0);
    }
    return ESP_OK;
}

esp_err_t get_file(httpd_req_t *req)
{
    if (!require_login(req) || !mounted(req)) {
        return ESP_OK;
    }
    char path[sdcard::kMaxPath];
    if (!request_path(req, nullptr, "path", path)) {
        return send_error(req, "400 Bad Request", "not a path on the card");
    }
    struct stat info {};
    const int file = stat(path, &info) == 0 && !S_ISDIR(info.st_mode) ? open(path, O_RDONLY) : -1;
    if (file < 0) {
        return send_error(req, "404 Not Found", "no such file");
    }
    // whole clusters' worth straight into a DMA buffer: the SDMMC driver reads them in one
    // transfer, where 4 KB through stdio went sector by sector through its bounce buffer
    auto *chunk = static_cast<char *>(heap_caps_aligned_alloc(kDmaAlign, kReadChunk, MALLOC_CAP_DMA));
    if (chunk == nullptr) {
        close(file);
        return send_error(req, "503 Service Unavailable", "no memory");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    // the web UI's link names the file (the download attribute)
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    bool ok = true;
    ssize_t length = 0;
    while ((length = read(file, chunk, kReadChunk)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, static_cast<size_t>(length)) != ESP_OK) {
            ok = false;
            break;
        }
    }
    heap_caps_free(chunk);
    close(file);
    if (ok) {
        httpd_resp_send_chunk(req, nullptr, 0);
    }
    return ESP_OK;
}

// the body as it comes in, into <path>.part; an older file of the name is replaced at the end
esp_err_t post_file(httpd_req_t *req)
{
    if (!require_login(req) || !mounted(req)) {
        return ESP_OK;
    }
    char path[sdcard::kMaxPath];
    if (!request_path(req, nullptr, "path", path) || std::strcmp(path, sdcard::kMountPoint) == 0 ||
        std::strlen(path) + 5 >= sizeof(path)) {
        return send_error(req, "400 Bad Request", "not a path on the card");
    }
    struct stat info {};
    if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
        return send_error(req, "409 Conflict", "a directory has this name");
    }
    char part[sdcard::kMaxPath];
    std::snprintf(part, sizeof(part), "%s.part", path);
    FILE *file = std::fopen(part, "wb");
    if (file == nullptr) {
        return send_error(req, "404 Not Found", "the directory does not exist");
    }
    auto *chunk = static_cast<char *>(std::malloc(kChunk));
    bool ok = chunk != nullptr;
    const char *problem = ok ? nullptr : "no memory";
    size_t remaining = req->content_len;
    int timeouts = 0;
    while (ok && remaining > 0) {
        const int n = httpd_req_recv(req, chunk, remaining < kChunk ? remaining : kChunk);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < kMaxTimeouts) {
            continue;
        }
        if (n <= 0) {
            ok = false;
            problem = "the upload broke off";
            break;
        }
        timeouts = 0;
        remaining -= static_cast<size_t>(n);
        if (std::fwrite(chunk, 1, static_cast<size_t>(n), file) != static_cast<size_t>(n)) {
            ok = false;
            problem = "writing failed (card full?)";
        }
    }
    std::free(chunk);
    if (std::fclose(file) != 0 && ok) {
        ok = false;
        problem = "writing failed (card full?)";
    }
    if (ok) {
        std::remove(path);
        if (std::rename(part, path) != 0) {
            ok = false;
            problem = "the file could not be renamed";
        }
    }
    if (!ok) {
        std::remove(part);
        return send_error(req, "500 Internal Server Error", problem);
    }
    char buffer[48];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("saved", true).number("size", static_cast<int64_t>(req->content_len)).end_object();
    return send_json(req, json);
}

esp_err_t post_delete(httpd_req_t *req)
{
    if (!require_login(req) || !mounted(req)) {
        return ESP_OK;
    }
    char body[kQuerySize];
    char path[sdcard::kMaxPath];
    if (!read_body(req, body, sizeof(body)) || !request_path(req, body, "path", path) ||
        std::strcmp(path, sdcard::kMountPoint) == 0) {
        return send_error(req, "400 Bad Request", "not a path on the card");
    }
    struct stat info {};
    if (stat(path, &info) != 0) {
        return send_error(req, "404 Not Found", "no such file");
    }
    if (S_ISDIR(info.st_mode) ? rmdir(path) != 0 : unlink(path) != 0) {
        return send_error(req, "409 Conflict", S_ISDIR(info.st_mode) ? "the directory is not empty" : "cannot delete");
    }
    return send_done(req, "deleted");
}

esp_err_t post_mkdir(httpd_req_t *req)
{
    if (!require_login(req) || !mounted(req)) {
        return ESP_OK;
    }
    char body[kQuerySize];
    char path[sdcard::kMaxPath];
    if (!read_body(req, body, sizeof(body)) || !request_path(req, body, "path", path) ||
        std::strcmp(path, sdcard::kMountPoint) == 0) {
        return send_error(req, "400 Bad Request", "not a path on the card");
    }
    if (mkdir(path, 0755) != 0) {
        return send_error(req, "409 Conflict", errno == EEXIST ? "it exists already" : "cannot create it");
    }
    return send_done(req, "created");
}

esp_err_t post_fetch(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[1024];
    char url[257];
    char path[sdcard::kMaxPath];
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "url", url, sizeof(url)) ||
        !form_value(body, "path", path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "url and path required");
    }
    const esp_err_t err = sdcard::fetch_start(url, path);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request", "an http:// or https:// URL and a file name on the card");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "no SD card mounted, or a download runs");
    }
    if (err != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "the download did not start");
    }
    httpd_resp_set_status(req, "202 Accepted");
    return send_done(req, "downloading");
}

esp_err_t post_mount(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[32];
    char action[12] = {};
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "action", action, sizeof(action))) {
        return send_error(req, "400 Bad Request", "action=mount|unmount required");
    }
    if (std::strcmp(action, "unmount") == 0) {
        if (sdcard::fetch_status().state == sdcard::FetchState::Running) {
            return send_error(req, "409 Conflict", "a download runs");
        }
        return sdcard::unmount() == ESP_OK ? get_sd(req) : send_error(req, "500 Internal Server Error", "unmount failed");
    }
    if (std::strcmp(action, "mount") != 0) {
        return send_error(req, "400 Bad Request", "action=mount|unmount required");
    }
    const esp_err_t err = sdcard::mount();
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "no card answers");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, "409 Conflict", "the card is not formatted");
    }
    return err == ESP_OK ? get_sd(req) : send_error(req, "500 Internal Server Error", "mount failed");
}

// The login's password once more (as POST /api/password): a session left open in a browser does
// not erase the card. In setup mode there is no password, confirm=yes alone.
esp_err_t post_format(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[256];
    char confirm[8] = {};
    char password[auth::kMaxPassword + 1] = {};
    const bool body_read = read_body(req, body, sizeof(body));
    const bool confirmed =
        body_read && form_value(body, "confirm", confirm, sizeof(confirm)) && std::strcmp(confirm, "yes") == 0;
    const bool have_password = body_read && form_value(body, "password", password, sizeof(password));
    std::memset(body, 0, sizeof(body));
    if (!confirmed) {
        std::memset(password, 0, sizeof(password));
        return send_error(req, "400 Bad Request", "confirm=yes required");
    }
    if (auth::has_user()) {
        char token[auth::kTokenSize];
        char user[auth::kUsernameSize] = {};
        size_t size = sizeof(token);
        const bool session = httpd_req_get_cookie_val(req, kSessionCookie, token, &size) == ESP_OK &&
                             auth::check_token(token, user);
        const bool right = session && have_password && auth::check_password(user, password);
        std::memset(password, 0, sizeof(password));
        if (!right) {
            vTaskDelay(pdMS_TO_TICKS(kFailedPasswordDelayMs));
            // not 401: the session is fine, the web UI would go to its login page
            return send_error(req, "400 Bad Request", "the password is wrong");
        }
    }
    std::memset(password, 0, sizeof(password));
    if (sdcard::fetch_status().state == sdcard::FetchState::Running) {
        return send_error(req, "409 Conflict", "a download runs");
    }
    const esp_err_t err = sdcard::format();
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(req, "404 Not Found", "no card answers");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "the card could not be formatted");
    }
    return get_sd(req);
}

} // namespace

void register_sd_routes(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/sd", .method = HTTP_GET, .handler = get_sd, .user_ctx = nullptr},
        {.uri = "/api/sd/list", .method = HTTP_GET, .handler = get_list, .user_ctx = nullptr},
        {.uri = "/api/sd/file", .method = HTTP_GET, .handler = get_file, .user_ctx = nullptr},
        {.uri = "/api/sd/file", .method = HTTP_POST, .handler = post_file, .user_ctx = nullptr},
        {.uri = "/api/sd/delete", .method = HTTP_POST, .handler = post_delete, .user_ctx = nullptr},
        {.uri = "/api/sd/mkdir", .method = HTTP_POST, .handler = post_mkdir, .user_ctx = nullptr},
        {.uri = "/api/sd/fetch", .method = HTTP_POST, .handler = post_fetch, .user_ctx = nullptr},
        {.uri = "/api/sd/mount", .method = HTTP_POST, .handler = post_mount, .user_ctx = nullptr},
        {.uri = "/api/sd/format", .method = HTTP_POST, .handler = post_format, .user_ctx = nullptr},
    };
    static_assert(std::size(routes) == kSdRoutes);
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
