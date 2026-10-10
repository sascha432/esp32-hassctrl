// The backup of the settings and the dashboard in one YAML file (docs/TODO.md, "Settings export
// and import"), login required:
//
//   GET  /api/backup.yaml[?parts=settings|dashboard]
//                                       the backup file, both parts without `parts`; never a
//                                       password or a token
//   POST /api/backup/import[?check=1]   body: the backup file (a password or a token added by
//                                       hand is used); check=1 only checks. Answer:
//                                       {"checked", "imported", "sections": [...], "problems": [...],
//                                       "ignored": [...], "notes": [...]}, 400 when a problem refused it
//
// Only in the main firmware: the recovery app has no backup and does not register these.

#include <cstdio>
#include <cstring>

#include "hc/memory/large.hpp"
#include "hc/webserver/webserver.hpp"

#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

const BackupAccess *g_backup = nullptr;
const char *g_device_name = "";

// settings and dashboard of a large house: a few dozen KB
constexpr size_t kMaxBackup = 256 * 1024;

esp_err_t get_backup(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char parts[16] = {};
    const bool chosen = query_value(req, "parts", parts, sizeof(parts));
    const bool settings = !chosen || std::strcmp(parts, "settings") == 0;
    const bool dashboard = !chosen || std::strcmp(parts, "dashboard") == 0;
    if (!settings && !dashboard) {
        return send_error(req, "400 Bad Request", "parts: settings or dashboard");
    }
    const memory::String text = g_backup->export_file(settings, dashboard);

    char disposition[96];
    std::snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s-%s.yaml\"", g_device_name,
                  settings && dashboard ? "backup" : settings ? "settings" : "dashboard");
    httpd_resp_set_type(req, "application/yaml; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    return httpd_resp_send(req, text.data(), static_cast<ssize_t>(text.size()));
}

void lines(JsonWriter &json, const char *key, const memory::Vector<memory::String> &values)
{
    json.begin_array(key);
    for (const memory::String &value : values) {
        json.string(nullptr, value.c_str(), value.size());
    }
    json.end_array();
}

esp_err_t post_import(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char check[8] = {};
    const bool check_only = query_value(req, "check", check, sizeof(check)) && std::strcmp(check, "1") == 0;
    size_t length = 0;
    char *body = read_large_body(req, kMaxBackup, length);
    if (body == nullptr) {
        return send_error(req, "400 Bad Request", "the request is empty or larger than 256 KB");
    }
    const BackupReport report = g_backup->import_file(std::string_view(body, length), check_only);
    memory::release_large(body);

    const bool refused = !report.problems.empty();
    const auto write = [&](JsonWriter &json) {
        json.begin_object().boolean("checked", check_only).boolean("imported", !check_only && !refused);
        lines(json, "sections", report.sections);
        lines(json, "problems", report.problems);
        lines(json, "ignored", report.ignored);
        lines(json, "notes", report.notes);
        json.end_object();
    };
    JsonWriter measure(nullptr, 0);
    write(measure);
    auto *buffer = static_cast<char *>(memory::allocate_large(measure.needed() + 1));
    JsonWriter json(buffer, measure.needed() + 1);
    write(json);
    if (refused) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    const esp_err_t err = send_json(req, json);
    memory::release_large(buffer);
    return err;
}

} // namespace

void register_backup_routes(httpd_handle_t server, const BackupAccess *backup, const char *device_name)
{
    if (backup == nullptr) {
        return;
    }
    g_backup = backup;
    g_device_name = device_name;
    const httpd_uri_t routes[] = {
        {.uri = "/api/backup.yaml", .method = HTTP_GET, .handler = get_backup, .user_ctx = nullptr},
        {.uri = "/api/backup/import", .method = HTTP_POST, .handler = post_import, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
