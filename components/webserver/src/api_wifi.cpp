// Wi-Fi of the setup and of Settings (docs/ui/web/web-app.md sections 2 and 4), login required after the
// setup:
//
//   GET  /api/wifi          saved networks (with password_needed: imported without its
//                           password, not tried until it has one) and the results of the last scan
//   POST /api/wifi/scan     starts a scan; the results follow in GET /api/wifi
//   POST /api/wifi/add      ssid, password: saves a network (updates the password of a saved one;
//                           an empty password keeps it, or makes one that needs its password open)
//   POST /api/wifi/remove   index (0 = first): forgets a saved network
//   POST /api/wifi/move     index, delta (-1 up, 1 down): changes the order
//   POST /api/wifi/policy   policy=order|strongest
//   POST /api/wifi/reconnect
//   GET  /api/hotspot       on, kept on by the user, name, password, devices, seconds left
//   POST /api/hotspot       on=1|0
//   POST /api/hotspot/password   a new generated password
//
// The hotspot password is shown on the screen as well; it is the one secret the web UI reads,
// so a phone can join.
//
// Passwords are write-only: no answer contains one.

#include <cstdlib>
#include <cstring>

#include "esp_attr.h"
#include "hc/network/network.hpp"

#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

namespace net = hc::network;

esp_err_t get_wifi(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    // scratch of the web server's task, too large for its stack; in PSRAM (EXT_RAM_BSS_ATTR), it is
    // only touched while the page loads
    static EXT_RAM_BSS_ATTR char buffer[2560];
    static EXT_RAM_BSS_ATTR net::SavedNetworkInfo saved[net::kMaxNetworks];
    static EXT_RAM_BSS_ATTR net::ScanEntry scan[20];
    const size_t saved_count = net::saved_networks(saved, net::kMaxNetworks);
    const size_t scan_count = net::last_scan(scan, 20);

    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object();
    json.begin_array("saved");
    for (size_t i = 0; i < saved_count; i++) {
        json.begin_object()
            .string("ssid", saved[i].ssid)
            .boolean("password_needed", saved[i].password_needed)
            .end_object();
    }
    json.end_array();
    json.begin_array("scan");
    for (size_t i = 0; i < scan_count; i++) {
        json.begin_object()
            .string("ssid", scan[i].ssid)
            .number("rssi", scan[i].rssi)
            .number("channel", scan[i].channel)
            .boolean("secure", scan[i].secure)
            .boolean("saved", scan[i].saved)
            .end_object();
    }
    json.end_array();
    json.string("policy", net::status().policy == net::Policy::StrongestSignal ? "strongest" : "order");
    json.end_object();
    return send_json(req, json);
}

esp_err_t send_ok(httpd_req_t *req)
{
    char buffer[16];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("ok", true).end_object();
    return send_json(req, json);
}

// the decimal value of `key` in the form
bool form_number(const char *body, const char *key, long &out)
{
    char text[12];
    if (!form_value(body, key, text, sizeof(text)) || text[0] == 0) {
        return false;
    }
    char *end = nullptr;
    out = std::strtol(text, &end, 10);
    return *end == 0;
}

esp_err_t post_remove(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[64];
    long index = 0;
    if (!read_body(req, body, sizeof(body)) || !form_number(body, "index", index) || index < 0 ||
        index >= static_cast<long>(net::kMaxNetworks) || net::remove_network(static_cast<uint8_t>(index)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "index required");
    }
    return send_ok(req);
}

esp_err_t post_move(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[64];
    long index = 0;
    long delta = 0;
    if (!read_body(req, body, sizeof(body)) || !form_number(body, "index", index) ||
        !form_number(body, "delta", delta) || index < 0 || index >= static_cast<long>(net::kMaxNetworks) ||
        (delta != -1 && delta != 1) ||
        net::move_network(static_cast<uint8_t>(index), static_cast<int8_t>(delta)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "index and delta (-1 or 1) required");
    }
    return send_ok(req);
}

esp_err_t post_policy(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[48];
    char policy[16] = {};
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "policy", policy, sizeof(policy)) ||
        (std::strcmp(policy, "order") != 0 && std::strcmp(policy, "strongest") != 0)) {
        return send_error(req, "400 Bad Request", "policy is order or strongest");
    }
    net::set_policy(policy[0] == 's' ? net::Policy::StrongestSignal : net::Policy::EnteredOrder);
    return send_ok(req);
}

esp_err_t post_reconnect(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    net::reconnect();
    return send_ok(req);
}

esp_err_t get_hotspot(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    const net::Status s = net::status();
    char buffer[192];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .boolean("on", s.hotspot_on)
        .boolean("kept_on", s.hotspot_enabled)
        .string("ssid", s.hotspot_ssid)
        .string("password", s.hotspot_password)
        .number("clients", s.hotspot_clients)
        .number("off_in_s", s.hotspot_off_in_s)
        .end_object();
    return send_json(req, json);
}

esp_err_t post_hotspot(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[32];
    long on = 0;
    if (!read_body(req, body, sizeof(body)) || !form_number(body, "on", on)) {
        return send_error(req, "400 Bad Request", "on=1 or on=0 required");
    }
    net::set_hotspot(on != 0);
    return send_ok(req);
}

esp_err_t post_hotspot_password(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    net::new_hotspot_password();
    return send_ok(req);
}

esp_err_t post_scan(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    if (net::scan() != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "busy, try again");
    }
    char buffer[32];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("scanning", true).end_object();
    return send_json(req, json);
}

esp_err_t post_add(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[256];
    char ssid[net::kSsidSize] = {};
    char password[net::kPasswordSize] = {};
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == 0) {
        return send_error(req, "400 Bad Request", "network name required");
    }
    form_value(body, "password", password, sizeof(password));
    std::memset(body, 0, sizeof(body));
    const esp_err_t err = net::add_network(ssid, password);
    std::memset(password, 0, sizeof(password));
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request", "the network could not be saved");
    }
    char buffer[96];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().string("saved", ssid).end_object();
    return send_json(req, json);
}

} // namespace

void register_wifi_routes(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = get_wifi, .user_ctx = nullptr},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = post_scan, .user_ctx = nullptr},
        {.uri = "/api/wifi/add", .method = HTTP_POST, .handler = post_add, .user_ctx = nullptr},
        {.uri = "/api/wifi/remove", .method = HTTP_POST, .handler = post_remove, .user_ctx = nullptr},
        {.uri = "/api/wifi/move", .method = HTTP_POST, .handler = post_move, .user_ctx = nullptr},
        {.uri = "/api/wifi/policy", .method = HTTP_POST, .handler = post_policy, .user_ctx = nullptr},
        {.uri = "/api/wifi/reconnect", .method = HTTP_POST, .handler = post_reconnect, .user_ctx = nullptr},
        {.uri = "/api/hotspot", .method = HTTP_GET, .handler = get_hotspot, .user_ctx = nullptr},
        {.uri = "/api/hotspot", .method = HTTP_POST, .handler = post_hotspot, .user_ctx = nullptr},
        {.uri = "/api/hotspot/password", .method = HTTP_POST, .handler = post_hotspot_password, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
