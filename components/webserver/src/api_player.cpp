// The media player (docs/ui/web/web-app.md section 4, Player), login required after the setup:
//
//   GET  /api/player        {"state", "position_ms", "duration_ms", "index", "count", "shuffle",
//                            "repeat", "volume", "title", "artist", "album", "error",
//                            "error_serial", "cover_serial", "next": [title, ...]}; next: up to 20
//                            titles that come after the current one
//   POST /api/player        any of action=toggle|next|previous|stop, seek=ms, shuffle=1|0,
//                           repeat=off|all|one, volume=0..100; answers like GET
//   GET  /api/player/cover  the cover of what plays as it is in the file (JPEG or PNG); 404
//                           without one; sent on a task of its own (hc/webserver/async.hpp)
//
// Only with the player (Config::player).

#include <cstdlib>
#include <cstring>

#include "hc/memory/large.hpp"
#include "hc/webserver/async.hpp"
#include "hc/webserver/webserver.hpp"
#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

const PlayerAccess *g_player = nullptr;

// the titles of the up next list
constexpr size_t kUpcoming = 20;
// the stack of the cover's task (PSRAM: the cover is a copy in PSRAM, no flash is read)
constexpr uint32_t kCoverStack = 4096;

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

// GET /api/player, pushed on the live socket as well
void write_player(JsonWriter &json)
{
    PlayerStatus s{};
    g_player->status(s);
    write_status(json, s, g_player->upcoming(kUpcoming));
}

esp_err_t send_status(httpd_req_t *req)
{
    return send_state(req, write_player);
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

esp_err_t send_cover(httpd_req_t *req)
{
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

// A cover is up to some 100s of KB: sent on the server's task, it held a pause pressed meanwhile
// for 6 to 15 s (2026-10-31, 327 KB at about 100 KB/s while music downloads).
esp_err_t get_cover(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    if (run_async(req, [](httpd_req_t *copy) { send_cover(copy); }, AsyncStack::Psram, kCoverStack)) {
        return ESP_OK;
    }
    return send_cover(req);
}

} // namespace

void register_player_routes(httpd_handle_t server, const PlayerAccess *player)
{
    g_player = player;
    if (player == nullptr) {
        return;
    }
    add_live_topic("/api/player", 1000, write_player);
    const httpd_uri_t routes[] = {
        route("/api/player", HTTP_GET, get_player),
        route("/api/player", HTTP_POST, post_player),
        route("/api/player/cover", HTTP_GET, get_cover),
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
