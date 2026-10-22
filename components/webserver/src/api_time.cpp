// The clock (docs/ui/web/web-app.md section 4), login required after the setup:
//
//   GET  /api/time        {"synced", "now", "local", "zone", "fixed_zone", "hass_zone",
//                         "last_sync", "servers", "defaults"}: now and last_sync in Unix time (0:
//                         never), local as "2026-10-21 14:03:07", zone the one in use
//   POST /api/time        any of fixed_zone (an IANA name, empty: Home Assistant's), server1,
//                         server2 (empty: none, both empty: the defaults); answers like GET, 400
//                         with the problem and nothing changed
//   GET  /api/time/zones  the IANA names the firmware knows, sorted: ["Africa/Abidjan", ...]
//
// Only in the main firmware: the recovery app keeps no time and does not register these routes.

#include <cstdio>
#include <cstring>
#include <ctime>

#include "hc/core/timezone.hpp"
#include "hc/platform/time_sync.hpp"

#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

esp_err_t send_settings(httpd_req_t *req)
{
    const std::time_t now = std::time(nullptr);
    const bool synced = platform::time_synced();
    char local[24] = {};
    if (synced) {
        std::tm tm{};
        localtime_r(&now, &tm);
        std::strftime(local, sizeof(local), "%Y-%m-%d %H:%M:%S", &tm);
    }
    char zone[core::kMaxZoneLength] = {};
    char fixed[core::kMaxZoneLength] = {};
    char hass[core::kMaxZoneLength] = {};
    platform::time_zone(zone, sizeof(zone));
    platform::fixed_time_zone(fixed, sizeof(fixed));
    platform::hass_time_zone(hass, sizeof(hass));
    const platform::TimeServers servers = platform::time_servers();

    char buffer[640];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .boolean("synced", synced)
        .number("now", static_cast<int64_t>(synced ? now : 0))
        .string("local", local)
        .string("zone", zone)
        .string("fixed_zone", fixed)
        .string("hass_zone", hass)
        .number("last_sync", static_cast<int64_t>(platform::last_time_sync()))
        .begin_array("servers");
    for (const char *name : servers.names) {
        json.string(nullptr, name);
    }
    json.end_array().begin_array("defaults");
    for (const char *name : platform::kDefaultTimeServers) {
        json.string(nullptr, name);
    }
    json.end_array().end_object();
    return send_json(req, json);
}

esp_err_t get_time(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_settings(req);
}

esp_err_t post_time(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[320];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the request is too large");
    }
    // all checked before anything changes
    char zone[core::kMaxZoneLength] = {};
    const bool has_zone = form_has(body, "fixed_zone");
    if (has_zone && (!form_value(body, "fixed_zone", zone, sizeof(zone)) ||
                     (zone[0] != 0 && core::time_zone_rule(zone) == nullptr))) {
        return send_error(req, "400 Bad Request", "fixed_zone: not a time zone this firmware knows");
    }
    platform::TimeServers servers = platform::time_servers();
    bool has_servers = false;
    const char *keys[platform::kTimeServers] = {"server1", "server2"};
    for (size_t i = 0; i < platform::kTimeServers; ++i) {
        if (!form_has(body, keys[i])) {
            continue;
        }
        has_servers = true;
        if (!form_value(body, keys[i], servers.names[i], sizeof(servers.names[i]))) {
            char message[48];
            std::snprintf(message, sizeof(message), "%s: at most %u characters", keys[i],
                          static_cast<unsigned>(platform::kTimeServerSize - 1));
            return send_error(req, "400 Bad Request", message);
        }
    }
    char error[96] = {};
    if (has_servers && !platform::check_time_servers(servers, error, sizeof(error))) {
        return send_error(req, "400 Bad Request", error);
    }
    if (has_zone) {
        platform::set_fixed_time_zone(zone);
    }
    if (has_servers && !platform::set_time_servers(servers, error, sizeof(error))) {
        return send_error(req, "500 Internal Server Error", error);
    }
    return send_settings(req);
}

// about 13 KB: sent in pieces (the names need no escaping)
esp_err_t get_zones(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char chunk[1024];
    size_t length = 0;
    chunk[length++] = '[';
    const size_t count = core::time_zone_count();
    for (size_t i = 0; i < count; ++i) {
        const std::string_view name = core::time_zone_name(i);
        if (length + name.size() + 4 > sizeof(chunk)) {
            if (httpd_resp_send_chunk(req, chunk, static_cast<ssize_t>(length)) != ESP_OK) {
                return ESP_FAIL;
            }
            length = 0;
        }
        if (i > 0) {
            chunk[length++] = ',';
        }
        chunk[length++] = '"';
        std::memcpy(chunk + length, name.data(), name.size());
        length += name.size();
        chunk[length++] = '"';
    }
    chunk[length++] = ']';
    if (httpd_resp_send_chunk(req, chunk, static_cast<ssize_t>(length)) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

} // namespace

void register_time_routes(httpd_handle_t server, bool time)
{
    if (!time) {
        return;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/api/time", .method = HTTP_GET, .handler = get_time, .user_ctx = nullptr},
        {.uri = "/api/time", .method = HTTP_POST, .handler = post_time, .user_ctx = nullptr},
        {.uri = "/api/time/zones", .method = HTTP_GET, .handler = get_zones, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
