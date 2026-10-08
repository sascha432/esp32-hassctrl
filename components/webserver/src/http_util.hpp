#pragma once

// Helpers of the API handlers. Request bodies are HTML form encoded
// (application/x-www-form-urlencoded), answers are JSON.

#include <cstddef>

#include "esp_http_server.h"
#include "hc/formats/json_writer.hpp"

namespace hc::webserver {
struct HassAccess;
struct DashboardAccess;
}

namespace hc::webserver::detail {

using formats::JsonWriter;

constexpr const char *kSessionCookie = "hc_session";

// Reads the whole body into `out` (terminated). False when it is larger than the buffer.
bool read_body(httpd_req_t *req, char *out, size_t size);

// The decoded value of `key` in a form encoded body; false when the key is missing or the value
// does not fit.
bool form_value(const char *body, const char *key, char *out, size_t size);

esp_err_t send_json(httpd_req_t *req, const JsonWriter &json);
// {"error": message} with an HTTP status such as "401 Unauthorized"
esp_err_t send_error(httpd_req_t *req, const char *status, const char *message);

// Setup mode (no login yet) or a valid session cookie. Sends 401 and returns false otherwise.
bool require_login(httpd_req_t *req);

// a session cookie for the user; `remember` makes it last 90 days
bool set_session_cookie(httpd_req_t *req, const char *username, bool remember);
void clear_session_cookie(httpd_req_t *req);

void register_session_routes(httpd_handle_t server);
void register_wifi_routes(httpd_handle_t server);
void register_update_routes(httpd_handle_t server);
void register_file_routes(httpd_handle_t server);
// nothing without a Home Assistant connection (the recovery app)
void register_hass_routes(httpd_handle_t server, const HassAccess *hass);
// nothing without a dashboard (the recovery app)
void register_dashboard_routes(httpd_handle_t server, const DashboardAccess *dashboard);

} // namespace hc::webserver::detail
