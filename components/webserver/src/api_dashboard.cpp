// The dashboard (docs/architecture.md section 5), login required after the setup:
//
//   GET  /api/dashboard                 the stored dashboard as JSON; 404 without one
//   POST /api/dashboard?revision=N[&check=1]
//                                       body: the dashboard as JSON, based on revision N (0 when
//                                       there is none; new tiles without "id"); 409 when the
//                                       stored one is newer. check=1 only validates. Answer:
//                                       {"checked", "revision", "dashboard"}, the dashboard with
//                                       the ids and the "cells" of every tile (the editor's
//                                       preview); 400: {"error", "path"}, the tile's index in each
//                                       level of tile lists
//   GET  /api/dashboard/editor          what the web editor needs of the device: {"screen": {"name",
//                                       "width", "height", "gap"} in landscape, "icons": [...] the
//                                       names a tile may use besides none}
//   GET  /api/dashboard.yaml            the dashboard as one YAML file, for the backup
//   POST /api/dashboard/import[?check=1]  multipart/form-data with the files of a dashboard
//                                       folder (the filename of every part is its path inside
//                                       the folder, hass.yaml is the main file); check=1 only
//                                       validates. hass.yaml may carry settings sections of the
//                                       backup file (hass:, camera:, ...), applied with it.
//                                       Answer: {"checked", "revision", "pages", "tiles",
//                                       "settings", "notes", "ignored"}
//
// Only in the main firmware: the recovery app has no dashboard and does not register these.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hc/formats/multipart.hpp"
#include "hc/memory/large.hpp"
#include "hc/webserver/webserver.hpp"

#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

const DashboardAccess *g_dashboard = nullptr;

// The dashboard of a large house is a few dozen KB; an import carries all its files.
constexpr size_t kMaxJsonBody = 256 * 1024;
constexpr size_t kMaxUploadBody = 512 * 1024;

const char *status_line(DashboardResult result)
{
    switch (result) {
    case DashboardResult::Conflict:
        return "409 Conflict";
    case DashboardResult::Invalid:
        return "400 Bad Request";
    default:
        return "500 Internal Server Error";
    }
}

esp_err_t send_text(httpd_req_t *req, char *text, size_t length, const char *type, const char *filename)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (filename != nullptr) {
        char disposition[80];
        std::snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
        httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    }
    const esp_err_t err = httpd_resp_send(req, text, static_cast<ssize_t>(length));
    g_dashboard->release(text);
    return err;
}

esp_err_t send_info(httpd_req_t *req, const DashboardInfo &info, bool checked)
{
    char buffer[640];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .boolean("checked", checked)
        .number("revision", info.revision)
        .number("pages", info.pages)
        .number("tiles", info.tiles)
        .string("settings", info.settings)
        .string("notes", info.notes)
        .string("ignored", info.other_keys)
        .end_object();
    return send_json(req, json);
}

esp_err_t get_dashboard(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    size_t length = 0;
    char error[160] = {};
    char *text = g_dashboard->export_json(length, error, sizeof(error));
    if (text == nullptr) {
        return send_error(req, "404 Not Found", error[0] != 0 ? error : "there is no dashboard");
    }
    return send_text(req, text, length, "application/json", nullptr);
}

esp_err_t get_dashboard_yaml(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    size_t length = 0;
    char *text = g_dashboard->export_yaml(length);
    if (text == nullptr) {
        return send_error(req, "404 Not Found", "there is no dashboard");
    }
    return send_text(req, text, length, "text/yaml; charset=utf-8", "hassctrl-dashboard.yaml");
}

// {"error": message, "path": [...]}: the path names the tile, also one that has no id yet
esp_err_t send_problem(httpd_req_t *req, const char *status, const char *message, const DashboardSaved &saved)
{
    char buffer[256];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().string("error", message).begin_array("path");
    for (uint8_t i = 0; i < saved.path_length; i++) {
        json.number(nullptr, saved.path[i]);
    }
    json.end_array().end_object();
    httpd_resp_set_status(req, status);
    return send_json(req, json);
}

esp_err_t post_dashboard(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char revision_text[16];
    char *end = nullptr;
    if (!query_value(req, "revision", revision_text, sizeof(revision_text))) {
        return send_error(req, "400 Bad Request", "revision required (the revision the change is based on)");
    }
    const unsigned long expected = std::strtoul(revision_text, &end, 10);
    if (end == revision_text || *end != 0) {
        return send_error(req, "400 Bad Request", "revision is a number");
    }
    char check[8] = {};
    const bool check_only = query_value(req, "check", check, sizeof(check)) && std::strcmp(check, "1") == 0;
    size_t length = 0;
    char *body = read_large_body(req, kMaxJsonBody, length);
    if (body == nullptr) {
        return send_error(req, "400 Bad Request", "the request is empty or too large");
    }
    DashboardSaved saved{};
    char error[160] = {};
    const DashboardResult result =
        g_dashboard->save_json(body, length, static_cast<uint32_t>(expected), check_only, saved, error, sizeof(error));
    memory::release_large(body);
    if (result != DashboardResult::Ok) {
        return send_problem(req, status_line(result), error, saved);
    }
    // the dashboard goes out as it is, between the head and the closing brace
    char head[64];
    std::snprintf(head, sizeof(head), "{\"checked\":%s,\"revision\":%u,\"dashboard\":", check_only ? "true" : "false",
                  static_cast<unsigned>(saved.revision));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, saved.json, static_cast<ssize_t>(saved.json_length));
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "}", 1);
    }
    g_dashboard->release(saved.json);
    return err == ESP_OK ? httpd_resp_send_chunk(req, nullptr, 0) : err;
}

esp_err_t get_editor(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    const char *name = "";
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t gap = 0;
    g_dashboard->screen(name, width, height, gap);
    JsonWriter measure(nullptr, 0);
    const auto write = [&](JsonWriter &json) {
        json.begin_object()
            .begin_object("screen")
            .string("name", name)
            .number("width", width)
            .number("height", height)
            .number("gap", gap)
            .end_object()
            .begin_array("icons");
        for (size_t i = 0; const char *name = g_dashboard->icon_name(i); i++) {
            json.string(nullptr, name);
        }
        json.end_array().end_object();
    };
    write(measure);
    auto *buffer = static_cast<char *>(memory::allocate_large(measure.needed() + 1));
    JsonWriter json(buffer, measure.needed() + 1);
    write(json);
    const esp_err_t err = send_json(req, json);
    memory::release_large(buffer);
    return err;
}

esp_err_t post_import(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char check[8] = {};
    const bool check_only = query_value(req, "check", check, sizeof(check)) && std::strcmp(check, "1") == 0;

    char content_type[160] = {};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "Content-Type: multipart/form-data required");
    }
    const std::string_view boundary = formats::multipart_boundary(content_type);
    size_t length = 0;
    char *body = read_large_body(req, kMaxUploadBody, length);
    if (body == nullptr) {
        return send_error(req, "400 Bad Request", "the request is empty or larger than 512 KB");
    }
    auto parts = formats::parse_multipart(std::string_view(body, length), boundary);
    if (!parts) {
        memory::release_large(body);
        return send_error(req, "400 Bad Request", parts.error().c_str());
    }
    memory::Vector<DashboardFile> files;
    for (const auto &part : *parts) {
        if (!part.filename.empty()) {
            files.push_back({part.filename.data(), part.filename.size(), part.data.data(), part.data.size()});
        }
    }
    DashboardInfo info{};
    char error[200] = {};
    const DashboardResult result =
        files.empty() ? DashboardResult::Invalid
                      : g_dashboard->import_yaml(files.data(), files.size(), check_only, info, error, sizeof(error));
    if (files.empty()) {
        std::snprintf(error, sizeof(error), "no files in the request");
    }
    parts->clear();
    files.clear();
    memory::release_large(body);
    if (result != DashboardResult::Ok) {
        return send_error(req, status_line(result), error);
    }
    return send_info(req, info, check_only);
}

} // namespace

void register_dashboard_routes(httpd_handle_t server, const DashboardAccess *dashboard)
{
    if (dashboard == nullptr) {
        return;
    }
    g_dashboard = dashboard;
    const httpd_uri_t routes[] = {
        {.uri = "/api/dashboard", .method = HTTP_GET, .handler = get_dashboard, .user_ctx = nullptr},
        {.uri = "/api/dashboard", .method = HTTP_POST, .handler = post_dashboard, .user_ctx = nullptr},
        {.uri = "/api/dashboard.yaml", .method = HTTP_GET, .handler = get_dashboard_yaml, .user_ctx = nullptr},
        {.uri = "/api/dashboard/import", .method = HTTP_POST, .handler = post_import, .user_ctx = nullptr},
        {.uri = "/api/dashboard/editor", .method = HTTP_GET, .handler = get_editor, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
