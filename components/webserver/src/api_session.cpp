// Login and session (docs/ui/web.md section 5):
//
//   GET  /api/session      {"setup": no login exists yet, "logged_in", "user"}
//   POST /api/setup/user   username, password            only in setup mode: creates the login
//   POST /api/login        username, password, remember  sets the session cookie
//   POST /api/logout                                     clears the cookie
//   POST /api/logout-all                                 ends every session ("log out everywhere")

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "auth.hpp"
#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

// a failed login waits this long, which slows down guessing
constexpr uint32_t kFailedLoginDelayMs = 1000;

struct Credentials {
    char username[auth::kUsernameSize];
    char password[auth::kMaxPassword + 1];
    bool remember;
};

bool read_credentials(httpd_req_t *req, Credentials &out)
{
    char body[256];
    char remember[8] = {};
    out = {};
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "username", out.username, sizeof(out.username)) ||
        !form_value(body, "password", out.password, sizeof(out.password))) {
        return false;
    }
    form_value(body, "remember", remember, sizeof(remember));
    out.remember = std::strcmp(remember, "1") == 0 || std::strcmp(remember, "on") == 0;
    std::memset(body, 0, sizeof(body));
    return true;
}

esp_err_t get_session(httpd_req_t *req)
{
    char token[auth::kTokenSize];
    char user[auth::kUsernameSize] = {};
    size_t size = sizeof(token);
    const bool logged_in = auth::has_user() && httpd_req_get_cookie_val(req, kSessionCookie, token, &size) == ESP_OK &&
                           auth::check_token(token, user);
    char buffer[128];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .boolean("setup", !auth::has_user())
        .boolean("logged_in", logged_in)
        .string("user", logged_in ? user : "")
        .end_object();
    return send_json(req, json);
}

esp_err_t post_setup_user(httpd_req_t *req)
{
    if (auth::has_user()) {
        return send_error(req, "403 Forbidden", "the login exists already");
    }
    Credentials credentials;
    if (!read_credentials(req, credentials)) {
        return send_error(req, "400 Bad Request", "username and password required");
    }
    if (!auth::valid_username(credentials.username)) {
        return send_error(req, "400 Bad Request", "username: 1 to 32 letters, digits, '.', '_' or '-'");
    }
    const size_t length = std::strlen(credentials.password);
    if (length < auth::kMinPassword || length > auth::kMaxPassword) {
        return send_error(req, "400 Bad Request", "password: 8 to 64 characters");
    }
    if (auth::set_user(credentials.username, credentials.password) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "the login could not be stored");
    }
    std::memset(credentials.password, 0, sizeof(credentials.password));
    // the new cookie is in this answer only, so the session is reported directly
    const bool logged_in = set_session_cookie(req, credentials.username, credentials.remember);
    char buffer[128];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .boolean("setup", false)
        .boolean("logged_in", logged_in)
        .string("user", logged_in ? credentials.username : "")
        .end_object();
    return send_json(req, json);
}

esp_err_t post_login(httpd_req_t *req)
{
    Credentials credentials;
    if (!read_credentials(req, credentials)) {
        return send_error(req, "400 Bad Request", "username and password required");
    }
    const bool ok = auth::check_password(credentials.username, credentials.password);
    std::memset(credentials.password, 0, sizeof(credentials.password));
    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(kFailedLoginDelayMs));
        return send_error(req, "401 Unauthorized", "wrong username or password");
    }
    if (!set_session_cookie(req, credentials.username, credentials.remember)) {
        return send_error(req, "500 Internal Server Error", "no session");
    }
    char buffer[96];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("logged_in", true).string("user", credentials.username).end_object();
    return send_json(req, json);
}

esp_err_t post_logout(httpd_req_t *req)
{
    clear_session_cookie(req);
    char buffer[32];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("logged_in", false).end_object();
    return send_json(req, json);
}

esp_err_t post_logout_all(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    if (auth::has_user() && auth::revoke_all() != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not end the sessions");
    }
    return post_logout(req);
}

} // namespace

void register_session_routes(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/session", .method = HTTP_GET, .handler = get_session, .user_ctx = nullptr},
        {.uri = "/api/setup/user", .method = HTTP_POST, .handler = post_setup_user, .user_ctx = nullptr},
        {.uri = "/api/login", .method = HTTP_POST, .handler = post_login, .user_ctx = nullptr},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = post_logout, .user_ctx = nullptr},
        {.uri = "/api/logout-all", .method = HTTP_POST, .handler = post_logout_all, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
