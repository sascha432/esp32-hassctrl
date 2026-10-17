// Login and session (docs/ui/web/web-app.md section 3):
//
//   GET  /api/session      {"setup": no login exists yet, "logged_in", "user"}
//   POST /api/setup/user   username, password            only in setup mode: creates the login
//   POST /api/login        username, password, remember  sets the session cookie
//   POST /api/logout                                     clears the cookie
//   POST /api/logout-all                                 ends every session ("log out everywhere")
//   POST /api/password     current, password             a new password for the user logged in

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
    bool password_too_long; // the password did not fit: more than kMaxPassword characters
};

bool read_credentials(httpd_req_t *req, Credentials &out)
{
    char body[256];
    char remember[8] = {};
    out = {};
    if (!read_body(req, body, sizeof(body))) {
        return false;
    }
    const bool have_username = form_value(body, "username", out.username, sizeof(out.username));
    const bool have_password = form_value(body, "password", out.password, sizeof(out.password));
    if (!have_username || !have_password) {
        out.password_too_long = !have_password && form_has(body, "password");
        std::memset(out.password, 0, sizeof(out.password));
        std::memset(body, 0, sizeof(body));
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
        return send_error(req, "400 Bad Request",
                          credentials.password_too_long ? "password: 8 to 64 characters" : "username and password required");
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

// The user logged in sets a new password with the current one. set_user ends every session, this
// browser gets a new one (not remembered: the old cookie's lifetime is not known here).
esp_err_t post_password(httpd_req_t *req)
{
    char token[auth::kTokenSize];
    char user[auth::kUsernameSize] = {};
    size_t size = sizeof(token);
    if (!auth::has_user() || httpd_req_get_cookie_val(req, kSessionCookie, token, &size) != ESP_OK ||
        !auth::check_token(token, user)) {
        return send_error(req, "401 Unauthorized", "login required");
    }
    // both passwords form encoded: up to 3 x 64 characters each
    char body[512];
    char current[auth::kMaxPassword + 1] = {};
    char password[auth::kMaxPassword + 1] = {};
    const bool body_read = read_body(req, body, sizeof(body));
    const bool have_current = body_read && form_value(body, "current", current, sizeof(current));
    const bool have_password = body_read && form_value(body, "password", password, sizeof(password));
    // a new password that does not fit its field is too long, not missing
    const bool too_long = body_read && !have_password && form_has(body, "password");
    std::memset(body, 0, sizeof(body));
    const size_t length = std::strlen(password);
    const char *problem = too_long || (have_password && (length < auth::kMinPassword || length > auth::kMaxPassword))
                              ? "password: 8 to 64 characters"
                          : !have_current || !have_password ? "current and new password required"
                                                            : nullptr;
    if (problem == nullptr && !auth::check_password(user, current)) {
        vTaskDelay(pdMS_TO_TICKS(kFailedLoginDelayMs));
        problem = "the current password is wrong";
    }
    std::memset(current, 0, sizeof(current));
    if (problem != nullptr) {
        std::memset(password, 0, sizeof(password));
        // not 401: the session is fine, the web UI would go to its login page
        return send_error(req, "400 Bad Request", problem);
    }
    const esp_err_t stored = auth::set_user(user, password);
    std::memset(password, 0, sizeof(password));
    if (stored != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "the password could not be stored");
    }
    const bool logged_in = set_session_cookie(req, user, false);
    char buffer[96];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("logged_in", logged_in).string("user", logged_in ? user : "").end_object();
    return send_json(req, json);
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
        {.uri = "/api/password", .method = HTTP_POST, .handler = post_password, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
