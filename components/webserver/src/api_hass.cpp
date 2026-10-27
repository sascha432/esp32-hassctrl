// The Home Assistant connection (docs/ui/web/web-app.md section 4), login required after the setup:
//
//   GET  /api/hass             state, address, whether a token is stored, version, last error
//   POST /api/hass             url, token: stores them and connects; a new token is tried with
//                              Home Assistant first (rejected: 400, nothing stored); an empty token keeps the
//                              stored one, an empty url removes address and token
//   POST /api/hass/reconnect   connects at once (also after a rejected token)
//   POST /api/hass/certificate fingerprint: trusts the https certificate with this SHA-256 (the
//                              refused_certificate of the status) and connects; empty forgets it
//   GET  /api/hass/discover    Home Assistant instances on the network (mDNS, about 2 s)
//   GET  /api/hass/entities?domain=&q=
//                              the entity picker of the editor: {"more", "rows": [[id, name,
//                              domain, device_class, unit], ...]}, the entities of the domain
//                              (none: all) whose id or name contains q, at most 100; 503 without
//                              a connection (docs/ui/web/web-dashboard-editor.md section 3)
//   GET  /api/hass/entity?id=  {"id", "name", "domain", "state", "icon", "device_class", "unit",
//                              "color_modes", "temp_step"}, or {"id", "missing": true}
//   GET  /api/hass/media?id=   the folder picker of a media tile: {"title", "folders": [[id,
//                              title], ...]}, the subfolders of the media library's folder id
//                              (media-source://..., none: the top) in name order; 503 without
//                              a connection or when Home Assistant refuses it
//
// The token is write-only: no answer contains it. Only in the main firmware: the recovery app
// has no Home Assistant connection and does not register these routes.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hc/memory/large.hpp"
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
    char buffer[768];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .string("state", status.state)
        .string("url", status.url)
        .boolean("token_set", status.token_set)
        .string("version", status.version)
        .string("error", status.error)
        .number("retry_in_s", status.retry_in_s)
        .number("connected_s", status.connected_s)
        .string("certificate", status.certificate)
        .string("refused_certificate", status.refused_certificate)
        .string("motion_helper", status.motion_helper != nullptr ? status.motion_helper : "")
        .string("motion_entity", status.motion_entity)
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

// the decoded value of `key` in the query string, empty when it is not there
void query_text(httpd_req_t *req, const char *key, char *out, size_t size)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK || !form_value(query, key, out, size)) {
        out[0] = 0;
    }
}

// [a-z0-9_] on both sides of one dot (core::valid_entity_id, which the web server does not link)
bool entity_id_ok(const char *id)
{
    const char *dot = std::strchr(id, '.');
    if (dot == nullptr || dot == id || dot[1] == 0 || std::strchr(dot + 1, '.') != nullptr) {
        return false;
    }
    for (const char *p = id; *p != 0; p++) {
        if (p != dot && !((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            return false;
        }
    }
    return true;
}

esp_err_t send_answer(httpd_req_t *req, char *text, size_t length, const char *error)
{
    if (text == nullptr) {
        return send_error(req, "503 Service Unavailable", error);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, text, static_cast<ssize_t>(length));
    memory::release_large(text);
    return err;
}

esp_err_t get_entities(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char domain[33];
    char query[65];
    query_text(req, "domain", domain, sizeof(domain));
    query_text(req, "q", query, sizeof(query));
    for (const char *p = domain; *p != 0; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            return send_error(req, "400 Bad Request", "not a domain");
        }
    }
    char error[96] = {};
    size_t length = 0;
    char *text = g_hass->entities(domain, query, length, error, sizeof(error));
    return send_answer(req, text, length, error);
}

esp_err_t get_entity(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char id[129];
    query_text(req, "id", id, sizeof(id));
    if (!entity_id_ok(id)) {
        return send_error(req, "400 Bad Request", "not an entity id");
    }
    char error[96] = {};
    size_t length = 0;
    char *text = g_hass->entity(id, length, error, sizeof(error));
    return send_answer(req, text, length, error);
}

esp_err_t get_media(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char id[256];
    query_text(req, "id", id, sizeof(id));
    if (id[0] != 0 && std::strncmp(id, "media-source://", 15) != 0) {
        return send_error(req, "400 Bad Request", "not a media-source:// id");
    }
    char error[96] = {};
    size_t length = 0;
    char *text = g_hass->media(id, length, error, sizeof(error));
    return send_answer(req, text, length, error);
}

esp_err_t post_reconnect(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    g_hass->reconnect();
    return send_status(req);
}

esp_err_t post_certificate(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[256];
    char fingerprint[128];
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "fingerprint", fingerprint, sizeof(fingerprint))) {
        return send_error(req, "400 Bad Request", "fingerprint required (empty forgets the certificate)");
    }
    char error[96] = {};
    const esp_err_t err = g_hass->trust(fingerprint, error, sizeof(error));
    if (err != ESP_OK) {
        return send_error(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error", error);
    }
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
        {.uri = "/api/hass/certificate", .method = HTTP_POST, .handler = post_certificate, .user_ctx = nullptr},
        {.uri = "/api/hass/discover", .method = HTTP_GET, .handler = get_discover, .user_ctx = nullptr},
        {.uri = "/api/hass/entities", .method = HTTP_GET, .handler = get_entities, .user_ctx = nullptr},
        {.uri = "/api/hass/entity", .method = HTTP_GET, .handler = get_entity, .user_ctx = nullptr},
        {.uri = "/api/hass/media", .method = HTTP_GET, .handler = get_media, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
