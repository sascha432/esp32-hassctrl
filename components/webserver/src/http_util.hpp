#pragma once

// Helpers of the API handlers. Request bodies are HTML form encoded
// (application/x-www-form-urlencoded), answers are JSON.

#include <cstddef>

#include "esp_http_server.h"
#include "hc/formats/json_writer.hpp"
#include "sdkconfig.h"

namespace hc::webserver {
struct HassAccess;
struct DashboardAccess;
struct BackupAccess;
struct CameraAccess;
}

namespace hc::webserver::detail {

using formats::JsonWriter;

constexpr const char *kSessionCookie = "hc_session";

// Reads the whole body into `out` (terminated). False when it is larger than the buffer.
bool read_body(httpd_req_t *req, char *out, size_t size);

// The whole request body in a buffer of the large heap (memory::release_large), terminated; null
// when it is empty, larger than `limit` or the client stalls (the server has one task, one
// receive timeout ends the request).
char *read_large_body(httpd_req_t *req, size_t limit, size_t &length);

// the value of `key` in the query string, false when it is not there
bool query_value(httpd_req_t *req, const char *key, char *out, size_t size);

// The decoded value of `key` in a form encoded body; false when the key is missing or the value
// does not fit.
bool form_value(const char *body, const char *key, char *out, size_t size);

// true when the form has `key`, also when its value would not fit (form_value fails then): a value
// that is too long is not a missing one
bool form_has(const char *body, const char *key);

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
void register_log_routes(httpd_handle_t server);
// nothing without a Home Assistant connection (the recovery app)
void register_hass_routes(httpd_handle_t server, const HassAccess *hass);
// nothing without a dashboard (the recovery app)
void register_dashboard_routes(httpd_handle_t server, const DashboardAccess *dashboard);
// nothing without a backup (the recovery app)
void register_backup_routes(httpd_handle_t server, const BackupAccess *backup, const char *device_name);
// nullptr: no routes
void register_camera_routes(httpd_handle_t server, const CameraAccess *camera);
// nothing without the clock (the recovery app)
void register_time_routes(httpd_handle_t server, bool time);
#if CONFIG_HC_SDCARD
// the SD card page (api_sd.cpp); the server makes room for kSdRoutes more handlers
constexpr size_t kSdRoutes = 9;
void register_sd_routes(httpd_handle_t server);
#endif

} // namespace hc::webserver::detail
