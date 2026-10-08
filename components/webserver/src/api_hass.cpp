// The Home Assistant connection (docs/ui/web.md section 6), login required after the setup:
//
//   GET  /api/hass             state, address, whether a token is stored, version, last error
//   POST /api/hass             url, token: stores them and connects; an empty token keeps the
//                              stored one, an empty url removes address and token
//   POST /api/hass/reconnect   connects at once (also after a rejected token)
//   GET  /api/hass/discover    Home Assistant instances on the network (mDNS, about 2 s)
//
// The token is write-only: no answer contains it. Only in the main firmware: the recovery app
// has no Home Assistant connection and does not register these routes.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hc/webserver/webserver.hpp"

#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

const HassAccess *g_hass = nullptr;

// a token of up to 512 characters and an address of 128, form encoded
constexpr size_t kBodySize = 2048;

esp_err_t send_status(httpd_req_t *req)
{
    HassStatus status{};
    g_hass->status(status);
    char buffer[512];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .string("state", status.state)
        .string("url", status.url)
        .boolean("token_set", status.token_set)
        .string("version", status.version)
        .string("error", status.error)
        .number("retry_in_s", status.retry_in_s)
        .number("connected_s", status.connected_s)
        .end_object();
    return send_json(req, json);
}

esp_err_t get_hass(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_status(req);
}

esp_err_t post_hass(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char *body = static_cast<char *>(std::malloc(kBodySize));
    char *token = static_cast<char *>(std::malloc(kBodySize));
    if (body == nullptr || token == nullptr) {
        std::free(body);
        std::free(token);
        return send_error(req, "503 Service Unavailable", "no memory");
    }
    char url[160] = {};
    char error[96] = {};
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!read_body(req, body, kBodySize)) {
        std::snprintf(error, sizeof(error), "the request is too large");
    } else if (!form_value(body, "url", url, sizeof(url))) {
        std::snprintf(error, sizeof(error), "url required (empty removes the connection)");
    } else {
        if (!form_value(body, "token", token, kBodySize)) {
            token[0] = 0;
        }
        err = g_hass->set(url, token, error, sizeof(error));
    }
    // the token does not stay in RAM longer than needed
    std::memset(body, 0, kBodySize);
    std::memset(token, 0, kBodySize);
    std::free(body);
    std::free(token);
    if (err != ESP_OK) {
        return send_error(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error", error);
    }
    return send_status(req);
}

esp_err_t get_discover(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    constexpr size_t kMax = 6;
    HassFound found[kMax] = {};
    const size_t count = g_hass->discover(found, kMax);
    char buffer[1536];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().begin_array("found");
    for (size_t i = 0; i < count; i++) {
        json.begin_object()
            .string("name", found[i].name)
            .string("url", found[i].url)
            .string("version", found[i].version)
            .end_object();
    }
    json.end_array().end_object();
    return send_json(req, json);
}

esp_err_t post_reconnect(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    g_hass->reconnect();
    return send_status(req);
}

} // namespace

void register_hass_routes(httpd_handle_t server, const HassAccess *hass)
{
    if (hass == nullptr) {
        return;
    }
    g_hass = hass;
    const httpd_uri_t routes[] = {
        {.uri = "/api/hass", .method = HTTP_GET, .handler = get_hass, .user_ctx = nullptr},
        {.uri = "/api/hass", .method = HTTP_POST, .handler = post_hass, .user_ctx = nullptr},
        {.uri = "/api/hass/reconnect", .method = HTTP_POST, .handler = post_reconnect, .user_ctx = nullptr},
        {.uri = "/api/hass/discover", .method = HTTP_GET, .handler = get_discover, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
