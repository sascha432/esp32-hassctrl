// The log (docs/ui/web/web-app.md section 4, hc/logbuf/logbuf.hpp), login required after the setup:
//
//   GET  /api/log        {"debug", "capacity"}
//   POST /api/log        debug=1|0: info messages are kept as well (stored)
//   GET  /api/log.txt    the log as text, oldest line first: `<unix time or 0> <esp_log line>`

#include <cstring>

#include "hc/logbuf/logbuf.hpp"
#include "hc/memory/large.hpp"

#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

esp_err_t send_settings(httpd_req_t *req)
{
    char buffer[64];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .boolean("debug", logbuf::debug())
        .number("capacity", static_cast<int64_t>(logbuf::capacity()))
        .end_object();
    return send_json(req, json);
}

esp_err_t get_log(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_settings(req);
}

esp_err_t post_log(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[32];
    char value[4];
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "debug", value, sizeof(value)) ||
        (std::strcmp(value, "1") != 0 && std::strcmp(value, "0") != 0)) {
        return send_error(req, "400 Bad Request", "debug: 1 or 0");
    }
    logbuf::set_debug(value[0] == '1');
    return send_settings(req);
}

esp_err_t get_log_text(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    const size_t capacity = logbuf::capacity();
    char *text = capacity > 0 ? static_cast<char *>(memory::allocate_large(capacity)) : nullptr;
    const size_t length = text != nullptr ? logbuf::copy(text, capacity) : 0;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, text, static_cast<ssize_t>(length));
    memory::release_large(text);
    return err;
}

} // namespace

void register_log_routes(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/log", .method = HTTP_GET, .handler = get_log, .user_ctx = nullptr},
        {.uri = "/api/log", .method = HTTP_POST, .handler = post_log, .user_ctx = nullptr},
        {.uri = "/api/log.txt", .method = HTTP_GET, .handler = get_log_text, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
