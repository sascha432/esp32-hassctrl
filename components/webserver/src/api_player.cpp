// The media player (docs/ui/web/web-app.md section 4, Player), login required after the setup:
//
//   GET  /api/player        {"state", "position_ms", "duration_ms", "index", "count", "shuffle",
//                            "repeat", "volume", "title", "artist", "album", "error",
//                            "error_serial", "cover_serial", "next": [title, ...]}; next: up to 20
//                            titles that come after the current one
//   POST /api/player        any of action=toggle|next|previous|stop, seek=ms, shuffle=1|0,
//                           repeat=off|all|one, volume=0..100; answers like GET
//   GET  /api/player/cover  the cover of what plays as it is in the file (JPEG or PNG); 404
//                           without one
//
// Only with the player (Config::player).

#include <cstdlib>
#include <cstring>

#include "hc/memory/large.hpp"
#include "hc/webserver/webserver.hpp"
#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

const PlayerAccess *g_player = nullptr;

// the titles of the up next list
constexpr size_t kUpcoming = 20;

void write_status(JsonWriter &json, const PlayerStatus &s, const memory::Vector<memory::String> &next)
{
    json.begin_object()
        .string("state", s.state)
        .number("position_ms", s.position_ms)
        .number("duration_ms", s.duration_ms)
        .number("index", s.index)
        .number("count", s.count)
        .boolean("shuffle", s.shuffle)
        .string("repeat", s.repeat)
        .number("volume", s.volume)
        .string("title", s.title)
        .string("artist", s.artist)
        .string("album", s.album)
        .string("error", s.error)
        .number("error_serial", s.error_serial)
        .number("cover_serial", s.cover_serial)
        .begin_array("next");
    for (const memory::String &title : next) {
        json.string(nullptr, title.c_str(), title.size());
    }
    json.end_array().end_object();
}

esp_err_t send_status(httpd_req_t *req)
{
    PlayerStatus s{};
    g_player->status(s);
    const memory::Vector<memory::String> next = g_player->upcoming(kUpcoming);
    // the titles have no limit: measured first
    JsonWriter measure(nullptr, 0);
    write_status(measure, s, next);
    memory::Vector<char> buffer(measure.needed() + 1);
    JsonWriter json(buffer.data(), buffer.size());
    write_status(json, s, next);
    return send_json(req, json);
}

esp_err_t get_player(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    return send_status(req);
}

esp_err_t post_player(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "the form is too long");
    }
    char value[16];
    if (form_value(body, "action", value, sizeof(value))) {
        if (std::strcmp(value, "toggle") == 0) {
            g_player->command(PlayerCommand::Toggle);
        } else if (std::strcmp(value, "next") == 0) {
            g_player->command(PlayerCommand::Next);
        } else if (std::strcmp(value, "previous") == 0) {
            g_player->command(PlayerCommand::Previous);
        } else if (std::strcmp(value, "stop") == 0) {
            g_player->command(PlayerCommand::Stop);
        } else {
            return send_error(req, "400 Bad Request", "action: toggle, next, previous or stop");
        }
    }
    if (form_value(body, "seek", value, sizeof(value))) {
        char *end = nullptr;
        const long ms = std::strtol(value, &end, 10);
        if (end == value || *end != 0 || ms < 0) {
            return send_error(req, "400 Bad Request", "seek: milliseconds");
        }
        g_player->seek(static_cast<uint32_t>(ms));
    }
    if (form_value(body, "shuffle", value, sizeof(value))) {
        if (std::strcmp(value, "1") != 0 && std::strcmp(value, "0") != 0) {
            return send_error(req, "400 Bad Request", "shuffle: 1 or 0");
        }
        g_player->set_shuffle(value[0] == '1');
    }
    if (form_value(body, "repeat", value, sizeof(value))) {
        const uint8_t repeat = std::strcmp(value, "off") == 0   ? 0
                               : std::strcmp(value, "all") == 0 ? 1
                               : std::strcmp(value, "one") == 0 ? 2
                                                                : 3;
        if (repeat == 3) {
            return send_error(req, "400 Bad Request", "repeat: off, all or one");
        }
        g_player->set_repeat(repeat);
    }
    if (form_value(body, "volume", value, sizeof(value))) {
        char *end = nullptr;
        const long volume = std::strtol(value, &end, 10);
        if (end == value || *end != 0 || volume < 0 || volume > 100) {
            return send_error(req, "400 Bad Request", "volume: 0 to 100");
        }
        g_player->set_volume(static_cast<uint8_t>(volume));
    }
    return send_status(req);
}

esp_err_t get_cover(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char mime[32] = {};
    size_t length = 0;
    char *data = g_player->cover(length, mime, sizeof(mime));
    if (data == nullptr) {
        return send_error(req, "404 Not Found", "no cover");
    }
    httpd_resp_set_type(req, mime);
    // the page asks again with the next cover_serial
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, data, static_cast<ssize_t>(length));
    memory::release_large(data);
    return err;
}

} // namespace

void register_player_routes(httpd_handle_t server, const PlayerAccess *player)
{
    g_player = player;
    if (player == nullptr) {
        return;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/api/player", .method = HTTP_GET, .handler = get_player, .user_ctx = nullptr},
        {.uri = "/api/player", .method = HTTP_POST, .handler = post_player, .user_ctx = nullptr},
        {.uri = "/api/player/cover", .method = HTTP_GET, .handler = get_cover, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
