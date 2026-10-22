#include "hc/webserver/webserver.hpp"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "hc/network/network.hpp"
#include "hc/platform/storage.hpp"
#include "lwip/sockets.h"

#include "auth.hpp"
#include "builtin_page.hpp"
#include "captive_dns.hpp"
#include "http_util.hpp"

namespace hc::webserver {

namespace {

using detail::JsonWriter;

constexpr const char *kTag = "web";
constexpr const char *kWebRoot = "/www";
constexpr size_t kChunkSize = 1024;

const char *g_device_name = "";
const HassAccess *g_hass = nullptr;
const CameraAccess *g_camera = nullptr;
uint32_t g_hotspot_ip = 0; // network byte order
char g_hotspot_host[16] = {};
char g_local_host[40] = {};
// the captive portal address announced by the hotspot's DHCP; esp_netif keeps the pointer
char g_portal_uri[32] = {};

// ----------------------------------------------------------------------------- captive portal

// true when the request arrived on the hotspot's address
bool via_hotspot(httpd_req_t *req)
{
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getsockname(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        return false;
    }
    uint32_t ip = 0;
    if (address.ss_family == AF_INET) {
        ip = reinterpret_cast<sockaddr_in *>(&address)->sin_addr.s_addr;
    }
#if CONFIG_LWIP_IPV6
    else if (address.ss_family == AF_INET6) {
        // the server listens on IPv6 with IPv4 mapped addresses: the last four bytes
        std::memcpy(&ip, reinterpret_cast<sockaddr_in6 *>(&address)->sin6_addr.s6_addr + 12, sizeof(ip));
    }
#endif
    return ip == g_hotspot_ip;
}

// A request on the hotspot for another host (a phone checking for internet access, any page the
// user typed) is sent to the portal. That is what makes phones open the setup page.
bool redirect_to_portal(httpd_req_t *req)
{
    if (!via_hotspot(req)) {
        return false;
    }
    char host[64] = {};
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    if (std::strcmp(host, g_hotspot_host) == 0 || strcasecmp(host, g_local_host) == 0) {
        return false;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", g_portal_uri);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // iOS only detects a captive portal when the answer has content
    httpd_resp_sendstr(req, "Redirect to the setup page");
    return true;
}

// ------------------------------------------------------------------------------- static files

const char *content_type(const char *path)
{
    const char *dot = std::strrchr(path, '.');
    if (dot == nullptr) {
        return "application/octet-stream";
    }
    static constexpr struct {
        const char *extension;
        const char *type;
    } kTypes[] = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css"},     {".js", "text/javascript"},
        {".json", "application/json"},         {".svg", "image/svg+xml"}, {".png", "image/png"},
        {".ico", "image/x-icon"},              {".woff2", "font/woff2"},  {".txt", "text/plain"},
    };
    for (const auto &entry : kTypes) {
        if (strcasecmp(dot, entry.extension) == 0) {
            return entry.type;
        }
    }
    return "application/octet-stream";
}

// True when the request's Accept-Encoding names gzip (a longer header is read as far as it fits)
bool accepts_gzip(httpd_req_t *req)
{
    char value[128];
    const esp_err_t err = httpd_req_get_hdr_value_str(req, "Accept-Encoding", value, sizeof(value));
    return (err == ESP_OK || err == ESP_ERR_HTTPD_RESULT_TRUNC) && strcasestr(value, "gzip") != nullptr;
}

// Sends /fs/www/<path>, preferring <path>.gz; 406 when only the .gz is there and the client does
// not take gzip (the web image keeps no uncompressed copies). False when there is no such file.
bool send_file(httpd_req_t *req, const char *uri)
{
    if (hc::platform::storage_status().fs != hc::platform::FsState::Mounted) {
        return false;
    }
    char path[128];
    const size_t uri_length = std::strcspn(uri, "?#");
    const bool directory = uri_length > 0 && uri[uri_length - 1] == '/';
    const int n = std::snprintf(path, sizeof(path), "%s%s%.*s%s", hc::platform::kFsMountPoint, kWebRoot,
                                static_cast<int>(uri_length), uri, directory ? "index.html" : "");
    if (n <= 0 || n + 3 >= static_cast<int>(sizeof(path)) || std::strstr(path, "..") != nullptr) {
        return false;
    }

    char gz_path[sizeof(path) + 3];
    std::snprintf(gz_path, sizeof(gz_path), "%s.gz", path);
    struct stat info {};
    bool gzip = stat(gz_path, &info) == 0;
    if (gzip && !accepts_gzip(req)) {
        gzip = false;
        if (stat(path, &info) != 0) {
            httpd_resp_set_status(req, "406 Not Acceptable");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "gzip only: send Accept-Encoding: gzip (curl --compressed)\n");
            return true;
        }
    } else if (!gzip && stat(path, &info) != 0) {
        return false;
    }
    FILE *file = std::fopen(gzip ? gz_path : path, "rb");
    if (file == nullptr) {
        return false;
    }

    httpd_resp_set_type(req, content_type(path));
    if (gzip) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char chunk[kChunkSize];
    size_t length = 0;
    bool ok = true;
    while ((length = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, length) != ESP_OK) {
            ok = false;
            break;
        }
    }
    std::fclose(file);
    if (ok) {
        httpd_resp_send_chunk(req, nullptr, 0);
    }
    return true;
}

// ------------------------------------------------------------------------------------ routes

const char *fs_state_name(hc::platform::FsState state)
{
    switch (state) {
    case hc::platform::FsState::Mounted:
        return "mounted";
    case hc::platform::FsState::MountFailed:
        return "damaged";
    case hc::platform::FsState::NoPartition:
        return "none";
    }
    return "?";
}

esp_err_t get_status(httpd_req_t *req)
{
    if (!detail::require_login(req)) {
        return ESP_OK;
    }
    namespace net = hc::network;
    const net::Status wifi = net::status();
    const auto &fs = hc::platform::storage_status();

    char buffer[1024];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .begin_object("device")
        .string("name", g_device_name)
        .string("firmware", esp_app_get_description()->version)
        .number("uptime_s", esp_timer_get_time() / 1000000)
        .number("heap_internal_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024)
        .number("heap_psram_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024)
        .end_object()
        .begin_object("wifi")
        .string("state", net::state_name(wifi.state))
        .string("ssid", wifi.ssid)
        .string("ip", wifi.ip)
        .number("rssi", wifi.rssi)
        .number("channel", wifi.channel)
        .string("error", wifi.error)
        .number("saved", wifi.saved_count)
        .end_object()
        .begin_object("hotspot")
        .boolean("on", wifi.hotspot_on)
        .string("ssid", wifi.hotspot_ssid)
        .number("clients", wifi.hotspot_clients)
        .number("off_in_s", wifi.hotspot_off_in_s)
        .end_object()
        .begin_object("fs")
        .string("state", fs_state_name(fs.fs))
        .number("used_kb", fs.fs_used / 1024)
        .number("total_kb", fs.fs_total / 1024)
        .end_object()
        .boolean("camera", g_camera != nullptr);
    if (g_hass != nullptr) {
        HassStatus hass{};
        g_hass->status(hass);
        json.begin_object("hass")
            .string("state", hass.state)
            .string("url", hass.url)
            .string("version", hass.version)
            .string("error", hass.error)
            .end_object();
    }
    json.end_object();
    return detail::send_json(req, json);
}

// with the path in the log (httpd_resp_send_err's own warning does not name it)
esp_err_t send_not_found(httpd_req_t *req)
{
    ESP_LOGW(kTag, "404 %s %s", http_method_str(static_cast<enum http_method>(req->method)), req->uri);
    httpd_resp_set_status(req, HTTPD_404);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "not found");
}

esp_err_t get_any(httpd_req_t *req)
{
    if (redirect_to_portal(req) || send_file(req, req->uri)) {
        return ESP_OK;
    }
    const size_t length = std::strcspn(req->uri, "?#");
    // "/" without web files, and "/recovery" always: login, update and restart even when the web
    // UI on LittleFS is broken (docs/ui/web/web-recovery.md)
    if ((length == 1 && req->uri[0] == '/') || (length == 9 && std::strncmp(req->uri, "/recovery", 9) == 0)) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, detail::kBuiltinPage);
    }
    return send_not_found(req);
}

// requests no handler takes (other methods) on the hotspot go to the portal as well
esp_err_t not_found(httpd_req_t *req, httpd_err_code_t)
{
    if (redirect_to_portal(req)) {
        return ESP_OK;
    }
    return send_not_found(req);
}

// The hotspot address, and the portal address announced by its DHCP server (option 114, which
// newer phones use to open the portal without probing).
void prepare_portal()
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info{};
    if (ap == nullptr || esp_netif_get_ip_info(ap, &info) != ESP_OK) {
        ESP_LOGW(kTag, "no hotspot interface, no captive portal");
        return;
    }
    g_hotspot_ip = info.ip.addr;
    std::snprintf(g_hotspot_host, sizeof(g_hotspot_host), IPSTR, IP2STR(&info.ip));
    std::snprintf(g_portal_uri, sizeof(g_portal_uri), "http://%s/", g_hotspot_host);

    esp_netif_dhcps_stop(ap);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, g_portal_uri, std::strlen(g_portal_uri));
    esp_netif_dhcps_start(ap);
}

} // namespace

esp_err_t start(const Config &config)
{
    g_device_name = config.device_name;
    g_hass = config.hass;
    g_camera = config.camera;
    ESP_RETURN_ON_ERROR(detail::auth::init(), kTag, "login");
    std::snprintf(g_local_host, sizeof(g_local_host), "%s.local", config.device_name);
    prepare_portal();

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    server_config.max_uri_handlers = 56;
    // browsers keep connections open; with all sessions taken the oldest is closed (LRU). Sessions
    // + 2 of the server + the captive DNS must stay within CONFIG_LWIP_MAX_SOCKETS (16).
    server_config.max_open_sockets = 12;
    server_config.lru_purge_enable = true;
    // the dashboard import parses on this stack
    server_config.stack_size = 8192;
    // A browser sends every cookie it holds for the address, also those of a device that had the
    // address before (seen: more than 1 KB, the default, ended in "431 header fields too long").
    // The buffer grows with the request, so the limit costs nothing for normal requests.
    server_config.max_req_hdr_len = 8192;
    httpd_handle_t server = nullptr;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &server_config), kTag, "start");

    const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = get_status, .user_ctx = nullptr},
        // last: everything else
        {.uri = "/*", .method = HTTP_GET, .handler = get_any, .user_ctx = nullptr},
    };
    // the API first: "/*" takes everything that is left
    detail::register_session_routes(server);
    detail::register_wifi_routes(server);
    detail::register_update_routes(server);
    detail::register_file_routes(server);
    detail::register_log_routes(server);
    detail::register_hass_routes(server, config.hass);
    detail::register_dashboard_routes(server, config.dashboard);
    detail::register_backup_routes(server, config.backup, config.device_name);
    detail::register_camera_routes(server, config.camera);
    detail::register_time_routes(server, config.time);
    if (config.add_routes != nullptr) {
        config.add_routes(server);
    }
    for (const auto &route : routes) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &route), kTag, "route %s", route.uri);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);

    if (g_hotspot_ip != 0) {
        ESP_RETURN_ON_ERROR(detail::start_captive_dns(g_hotspot_ip), kTag, "dns");
    }
    ESP_LOGI(kTag, "HTTP server on port %u, captive portal %s", server_config.server_port,
             g_portal_uri[0] != 0 ? g_portal_uri : "off");
    return ESP_OK;
}

esp_err_t reset_login()
{
    return detail::auth::remove_user();
}

bool import_login(const char *username, const char *password, bool apply, char *error, size_t error_size)
{
    namespace auth = detail::auth;
    const size_t length = strnlen(password, auth::kMaxPassword + 1);
    const char *problem = !auth::valid_username(username)                            ? "user: 1 to 32 letters, digits, '.', '_' or '-'"
                          : length < auth::kMinPassword || length > auth::kMaxPassword ? "password: 8 to 64 characters"
                          : apply && auth::set_user(username, password) != ESP_OK   ? "the login could not be stored"
                                                                                     : nullptr;
    if (problem != nullptr) {
        std::snprintf(error, error_size, "%s", problem);
        return false;
    }
    if (apply) {
        ESP_LOGI(kTag, "web UI login '%s' imported", username);
    }
    return true;
}

} // namespace hc::webserver
