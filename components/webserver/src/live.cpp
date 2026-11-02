// The live socket of the web app (docs/ui/web/web-app.md, "Live updates"), login required after the
// setup:
//
//   GET /api/live   WebSocket. The browser sends a text frame with the path of the GET it shows
//                   (/api/player); the server sends {"path": that path, "state": the GET's answer}
//                   as a text frame at once and again whenever the answer changes. An empty frame
//                   stops it (a hidden tab).
//
// One socket per tab instead of a page that asks every second: nothing to answer while nothing
// changes, and one session instead of the browser's pool of polling connections. Every
// `period_ms` of the topic the answer is built again and sent when its hash differs from the last
// one sent. A session whose login ended (logout, "log out everywhere", a new password, expiry) is
// closed. Everything here runs on the server's task: the handler, the close callback and the tick
// (httpd_queue_work), so nothing is locked.

#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "auth.hpp"
#include "http_util.hpp"

namespace hc::webserver::detail {

// The recovery app has no WebSocket support (its partition is full, sdkconfig.recovery): no
// /api/live there, the web app falls back to asking (live() in web/app.js).
#if CONFIG_HTTPD_WS_SUPPORT
#if !CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
#error "the live socket checks the login before the handshake: CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT"
#endif

namespace {

constexpr const char *kTag = "web";
constexpr size_t kMaxTopics = 8;
// a tab each; more than the browsers of a household
constexpr size_t kMaxClients = 8;
constexpr size_t kMaxPath = 32;
constexpr uint64_t kTickUs = 1000000;
// `{}` after this long without a frame: the browser sends nothing while it follows a page, so
// this is how it finds a socket that died (the device restarted, the laptop slept); the web app
// opens a new one after 25 s without a frame
constexpr int64_t kHeartbeatMs = 10000;

struct Topic {
    const char *path;
    uint32_t period_ms;
    StateWriter write;
};

// all zero at first: the table lives in PSRAM's .bss
struct Client {
    bool used = false;
    int fd = 0;
    // into g_topics, -1: none
    int topic = 0;
    uint32_t hash = 0;
    int64_t due = 0;
    // the last frame sent (ms): the heartbeat follows kHeartbeatMs after it
    int64_t sent = 0;
    // the session's token at the handshake, empty in setup mode
    char token[auth::kTokenSize] = {};
};

Topic g_topics[kMaxTopics];
size_t g_topic_count = 0;
// read once a second, PSRAM is fast enough; internal RAM is short on the JC3248W535
EXT_RAM_BSS_ATTR Client g_clients[kMaxClients];
httpd_handle_t g_server = nullptr;

int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

uint32_t fnv1a(const char *data, size_t length)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ static_cast<uint8_t>(data[i])) * 16777619u;
    }
    return hash;
}

Client *find(int fd)
{
    for (Client &client : g_clients) {
        if (client.used && client.fd == fd) {
            return &client;
        }
    }
    return nullptr;
}

Client *free_slot()
{
    for (Client &client : g_clients) {
        if (!client.used) {
            return &client;
        }
    }
    return nullptr;
}

void drop(Client &client)
{
    httpd_sess_trigger_close(g_server, client.fd);
    client = Client{};
}

// false when the frame could not be sent: the session is closed then
bool send_text(Client &client, const char *text, size_t length, int64_t now)
{
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(text));
    frame.len = length;
    if (httpd_ws_send_frame_async(g_server, client.fd, &frame) != ESP_OK) {
        drop(client);
        return false;
    }
    client.sent = now;
    return true;
}

void tick(void *)
{
    const int64_t now = now_ms();
    memory::Vector<char> buffer;
    for (Client &client : g_clients) {
        if (!client.used) {
            continue;
        }
        char user[auth::kUsernameSize];
        if (auth::has_user() && !auth::check_token(client.token, user)) {
            ESP_LOGD(kTag, "live %d: the login ended", client.fd);
            drop(client);
            continue;
        }
        if (client.topic >= 0 && now >= client.due) {
            const Topic &topic = g_topics[client.topic];
            client.due = now + topic.period_ms;
            const size_t length = build_state(topic.write, buffer);
            const uint32_t hash = length > 0 ? fnv1a(buffer.data(), length) : client.hash;
            if (hash != client.hash) {
                // {"path": ..., "state": <the GET's answer>}: a frame still on its way when the
                // page changed is not taken for the next page's
                memory::String text = "{\"path\":\"";
                text.append(topic.path).append("\",\"state\":").append(buffer.data(), length).append("}");
                if (!send_text(client, text.data(), text.size(), now)) {
                    continue;
                }
                client.hash = hash;
            }
        }
        if (now - client.sent >= kHeartbeatMs) {
            send_text(client, "{}", 2, now);
        }
    }
}

// Before the handshake, the last call with the request's headers (ESP-IDF does not call the
// handler for the handshake itself): no WebSocket without the login (401 then), and the client
// with its session's token. A handshake that fails after this closes the session, and
// close_session forgets the client.
esp_err_t accept_client(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_FAIL;
    }
    Client *client = free_slot();
    if (client == nullptr) {
        ESP_LOGW(kTag, "live: %u sockets open already", static_cast<unsigned>(kMaxClients));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "too many live sockets\n");
        return ESP_FAIL;
    }
    *client = Client{};
    client->used = true;
    client->fd = httpd_req_to_sockfd(req);
    client->topic = -1;
    client->sent = now_ms();
    if (!session_token(req, client->token)) {
        client->token[0] = 0;
    }
    return ESP_OK;
}

// a frame of the browser: the path it follows now
esp_err_t handle_live(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t frame{};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK || frame.len >= kMaxPath) {
        return ESP_FAIL;
    }
    char path[kMaxPath] = {};
    frame.payload = reinterpret_cast<uint8_t *>(path);
    if (frame.len > 0 && httpd_ws_recv_frame(req, &frame, sizeof(path) - 1) != ESP_OK) {
        return ESP_FAIL;
    }
    Client *client = find(fd);
    if (client == nullptr || frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_FAIL;
    }
    client->topic = -1;
    for (size_t i = 0; i < g_topic_count && path[0] != 0; i++) {
        if (std::strcmp(g_topics[i].path, path) == 0) {
            client->topic = static_cast<int>(i);
        }
    }
    if (client->topic < 0) {
        if (path[0] != 0) {
            ESP_LOGD(kTag, "live %d: no topic %s", fd, path);
        }
        return ESP_OK;
    }
    // the answer at once, the page draws from it
    client->hash = 0;
    client->due = 0;
    httpd_queue_work(g_server, tick, nullptr);
    return ESP_OK;
}

} // namespace

void add_live_topic(const char *path, uint32_t period_ms, StateWriter write)
{
    if (g_topic_count < kMaxTopics) {
        g_topics[g_topic_count++] = {path, period_ms, write};
    } else {
        ESP_LOGE(kTag, "live: no room for %s", path);
    }
}

void register_live_routes(httpd_handle_t server)
{
    g_server = server;
    httpd_uri_t route{};
    route.uri = "/api/live";
    route.method = HTTP_GET;
    route.handler = handle_live;
    route.is_websocket = true;
    route.ws_pre_handshake_cb = accept_client;
    httpd_register_uri_handler(server, &route);

    const esp_timer_create_args_t args = {
        .callback = [](void *) { httpd_queue_work(g_server, tick, nullptr); },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "web_live",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_periodic(timer, kTickUs) != ESP_OK) {
        ESP_LOGW(kTag, "live: no timer");
    }
}

void live_forget(int fd)
{
    if (Client *client = find(fd)) {
        *client = Client{};
    }
}

void live_close_token(const char *token)
{
    for (Client &client : g_clients) {
        if (client.used && token[0] != 0 && std::strcmp(client.token, token) == 0) {
            drop(client);
        }
    }
}

#else

void add_live_topic(const char *, uint32_t, StateWriter) {}
void register_live_routes(httpd_handle_t) {}
void live_forget(int) {}
void live_close_token(const char *) {}

#endif // CONFIG_HTTPD_WS_SUPPORT

} // namespace hc::webserver::detail
