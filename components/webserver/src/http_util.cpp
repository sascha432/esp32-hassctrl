#include "http_util.hpp"

#include <cstdio>
#include <cstring>

#include "hc/memory/large.hpp"

#include "auth.hpp"

namespace hc::webserver::detail {

namespace {

int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// decodes `length` characters of form encoding ('+' is a space, %XX a byte)
bool decode(const char *in, size_t length, char *out, size_t size)
{
    size_t n = 0;
    for (size_t i = 0; i < length; i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < length && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
            c = static_cast<char>(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
            i += 2;
        }
        if (n + 1 >= size) {
            return false;
        }
        out[n++] = c;
    }
    out[n] = 0;
    return true;
}

// the token of the session cookie, false without one
bool session_token(httpd_req_t *req, char (&token)[auth::kTokenSize])
{
    size_t size = sizeof(token);
    return httpd_req_get_cookie_val(req, kSessionCookie, token, &size) == ESP_OK;
}

} // namespace

bool read_body(httpd_req_t *req, char *out, size_t size)
{
    if (req->content_len >= size) {
        return false;
    }
    // A form arrives at once. The server has one task: a client that announces a body and stalls
    // would block every other request, so one receive timeout (5 s) ends the request.
    size_t received = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, out + received, req->content_len - received);
        if (n <= 0) {
            return false;
        }
        received += static_cast<size_t>(n);
    }
    out[received] = 0;
    return true;
}

char *read_large_body(httpd_req_t *req, size_t limit, size_t &length)
{
    if (req->content_len == 0 || req->content_len > limit) {
        return nullptr;
    }
    auto *body = static_cast<char *>(memory::allocate_large(req->content_len + 1));
    size_t received = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            memory::release_large(body);
            return nullptr;
        }
        received += static_cast<size_t>(n);
    }
    body[received] = 0;
    length = received;
    return body;
}

bool query_value(httpd_req_t *req, const char *key, char *out, size_t size)
{
    char query[64];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, out, size) == ESP_OK;
}

bool form_value(const char *body, const char *key, char *out, size_t size)
{
    const size_t key_length = std::strlen(key);
    const char *pair = body;
    while (pair != nullptr && *pair != 0) {
        const char *end = std::strchr(pair, '&');
        const size_t pair_length = end != nullptr ? static_cast<size_t>(end - pair) : std::strlen(pair);
        if (pair_length > key_length && std::strncmp(pair, key, key_length) == 0 && pair[key_length] == '=') {
            return decode(pair + key_length + 1, pair_length - key_length - 1, out, size);
        }
        pair = end != nullptr ? end + 1 : nullptr;
    }
    return false;
}

bool form_has(const char *body, const char *key)
{
    const size_t key_length = std::strlen(key);
    for (const char *pair = body; pair != nullptr && *pair != 0;) {
        if (std::strncmp(pair, key, key_length) == 0 && pair[key_length] == '=') {
            return true;
        }
        const char *end = std::strchr(pair, '&');
        pair = end != nullptr ? end + 1 : nullptr;
    }
    return false;
}

esp_err_t send_json(httpd_req_t *req, const JsonWriter &json)
{
    if (!json.ok()) {
        return send_error(req, "500 Internal Server Error", "answer too long");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.length()));
}

esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    char buffer[160];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().string("error", message).end_object();
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.length()));
}

bool require_login(httpd_req_t *req)
{
    if (!auth::has_user()) {
        return true; // setup mode: open until the first login exists (docs/ui/web/web-app.md section 1)
    }
    char token[auth::kTokenSize];
    char user[auth::kUsernameSize];
    if (session_token(req, token) && auth::check_token(token, user)) {
        return true;
    }
    send_error(req, "401 Unauthorized", "login required");
    return false;
}

bool set_session_cookie(httpd_req_t *req, const char *username, bool remember)
{
    char token[auth::kTokenSize];
    if (!auth::make_token(username, remember ? auth::kRememberSeconds : auth::kSessionSeconds, token)) {
        return false;
    }
    // the header value has to live until the response is sent
    static char cookie[auth::kTokenSize + 96];
    std::snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; HttpOnly; SameSite=Strict%s", kSessionCookie, token,
                  remember ? "; Max-Age=7776000" : "");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return true;
}

void clear_session_cookie(httpd_req_t *req)
{
    static char cookie[64];
    std::snprintf(cookie, sizeof(cookie), "%s=; Path=/; Max-Age=0", kSessionCookie);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

} // namespace hc::webserver::detail
