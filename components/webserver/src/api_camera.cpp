// The camera's web stream (docs/ui/web/web-app.md section 4, Camera), login required after the setup:
//
//   GET  /api/camera    {"enabled", "size": "full"|"half", "width", "height", "fps", "quality",
//                        "username", "password_set"}
//   POST /api/camera    any of enabled=1|0, size=full|half, fps=5|10|15|30, quality=30..95,
//                       username (empty: no login, the stream goes off), password (empty or
//                       missing: the stored one stays); answers like GET
//
// The password is never sent back. Only with a camera (Config::camera).

#include <cstdlib>
#include <cstring>

#include "hc/webserver/webserver.hpp"
#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

const CameraAccess *g_camera = nullptr;

esp_err_t send_stream(httpd_req_t *req)
{
    CameraStream s{};
    g_camera->stream(s);
    char buffer[256];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object()
        .boolean("enabled", s.enabled)
        .string("size", s.half ? "half" : "full")
        .number("width", s.width)
        .number("height", s.height)
        .number("fps", s.max_fps)
        .number("quality", s.quality)
        .string("username", s.username)
        .boolean("password_set", s.password_set)
        .end_object();
    return send_json(req, json);
}

esp_err_t get_camera(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_stream(req);
}

esp_err_t post_camera(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the form is too long");
    }
    CameraStream s{};
    g_camera->stream(s);
    char value[kCameraLoginSize + 8];
    if (form_value(body, "enabled", value, sizeof(value))) {
        if (std::strcmp(value, "1") != 0 && std::strcmp(value, "0") != 0) {
            return send_error(req, "400 Bad Request", "enabled: 1 or 0");
        }
        s.enabled = value[0] == '1';
    }
    if (form_value(body, "size", value, sizeof(value))) {
        if (std::strcmp(value, "full") != 0 && std::strcmp(value, "half") != 0) {
            return send_error(req, "400 Bad Request", "size: full or half");
        }
        s.half = std::strcmp(value, "half") == 0;
    }
    if (form_value(body, "fps", value, sizeof(value))) {
        const long fps = std::strtol(value, nullptr, 10);
        if (fps != 5 && fps != 10 && fps != 15 && fps != 30) {
            return send_error(req, "400 Bad Request", "fps: 5, 10, 15 or 30");
        }
        s.max_fps = static_cast<uint8_t>(fps);
    }
    if (form_value(body, "quality", value, sizeof(value))) {
        const long quality = std::strtol(value, nullptr, 10);
        if (quality < 30 || quality > 95) {
            return send_error(req, "400 Bad Request", "quality: 30 to 95");
        }
        s.quality = static_cast<uint8_t>(quality);
    }
    if (form_value(body, "username", value, sizeof(value))) {
        if (std::strlen(value) >= kCameraLoginSize) {
            return send_error(req, "400 Bad Request", "username: at most 32 characters");
        }
        std::strcpy(s.username, value);
    }
    char password[kCameraLoginSize + 8] = {};
    const bool has_password = form_value(body, "password", password, sizeof(password)) && password[0] != 0;
    if (has_password && std::strlen(password) >= kCameraLoginSize) {
        return send_error(req, "400 Bad Request", "password: at most 32 characters");
    }
    char error[96] = {};
    if (!g_camera->set_stream(s, has_password ? password : nullptr, error, sizeof(error))) {
        return send_error(req, "400 Bad Request", error);
    }
    return send_stream(req);
}

} // namespace

void register_camera_routes(httpd_handle_t server, const CameraAccess *camera)
{
    g_camera = camera;
    if (camera == nullptr) {
        return;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/api/camera", .method = HTTP_GET, .handler = get_camera, .user_ctx = nullptr},
        {.uri = "/api/camera", .method = HTTP_POST, .handler = post_camera, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
