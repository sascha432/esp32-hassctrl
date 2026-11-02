#pragma once

// Helpers of the API handlers. Request bodies are HTML form encoded
// (application/x-www-form-urlencoded), answers are JSON.

#include <cstddef>

#include "esp_http_server.h"
#include "hc/formats/json_writer.hpp"
#include "hc/memory/large.hpp"
#include "hc/webserver/route.hpp"
#include "sdkconfig.h"

#include "auth.hpp"

namespace hc::webserver {
struct HassAccess;
struct DashboardAccess;
struct BackupAccess;
struct CameraAccess;
struct PlayerAccess;
}

namespace hc::webserver::detail {

using formats::JsonWriter;

constexpr const char *kSessionCookie = "hc_session";

// Reads the whole body into `out` (terminated). False when it is larger than the buffer.
bool read_body(httpd_req_t *req, char *out, size_t size);

// The whole request body in a buffer of the large heap (memory::release_large), terminated; null
// when it is empty, larger than `limit` or the client stalls (one receive timeout, 5 s: a form
// arrives at once).
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

// The answer of a GET that the live socket pushes as well (live.cpp): writes the whole state,
// called once to measure and once to write (twice more if the state grew in between).
using StateWriter = void (*)(JsonWriter &json);
// the state's JSON in `buffer`, its length; 0 when it kept growing
size_t build_state(StateWriter write, memory::Vector<char> &buffer);
esp_err_t send_state(httpd_req_t *req, StateWriter write);

// Setup mode (no login yet) or a valid session cookie. Sends 401 and returns false otherwise.
bool require_login(httpd_req_t *req);
// the token of the session cookie, false without one
bool session_token(httpd_req_t *req, char (&token)[auth::kTokenSize]);

// ------------------------------------------------------------------------- sessions (idle.cpp)

// The server's open_fn and close_fn: the time of every session's last byte, sent or received; a
// session silent for 30 s is closed (start_idle_close), the live socket and a long response on a
// task of its own (async.hpp) excepted.
esp_err_t open_session(httpd_handle_t server, int fd);
void close_session(httpd_handle_t server, int fd);
void start_idle_close(httpd_handle_t server);

// ------------------------------------------------------------------- live socket (live.cpp)

// GET /api/live, the WebSocket of the web app: it sends the path of a GET it wants to follow,
// the server pushes that answer whenever it changes (checked every `period_ms`). Topics are added
// by the route files, before or after start_live.
void add_live_topic(const char *path, uint32_t period_ms, StateWriter write);
void register_live_routes(httpd_handle_t server);
// the session closed: its subscription goes (close_session)
void live_forget(int fd);
// closes the live sockets of the session with this token (a logout)
void live_close_token(const char *token);

// a session cookie for the user; `remember` makes it last 90 days
bool set_session_cookie(httpd_req_t *req, const char *username, bool remember);
void clear_session_cookie(httpd_req_t *req);

void register_session_routes(httpd_handle_t server);
void register_wifi_routes(httpd_handle_t server);
void register_update_routes(httpd_handle_t server);
void register_file_routes(httpd_handle_t server);
void register_log_routes(httpd_handle_t server);
#if CONFIG_HC_WEB_FIRMWARE_ROUTES
// nothing without a Home Assistant connection
void register_hass_routes(httpd_handle_t server, const HassAccess *hass);
// nothing without a dashboard
void register_dashboard_routes(httpd_handle_t server, const DashboardAccess *dashboard);
// nothing without a backup
void register_backup_routes(httpd_handle_t server, const BackupAccess *backup, const char *device_name);
// nullptr: no routes
void register_camera_routes(httpd_handle_t server, const CameraAccess *camera);
// nullptr: no routes
void register_player_routes(httpd_handle_t server, const PlayerAccess *player);
// nothing without the clock
void register_time_routes(httpd_handle_t server, bool time);
#endif
#if CONFIG_HC_SDCARD
// the SD card page (api_sd.cpp); the server makes room for kSdRoutes more handlers
constexpr size_t kSdRoutes = 9;
void register_sd_routes(httpd_handle_t server);
#endif

} // namespace hc::webserver::detail
