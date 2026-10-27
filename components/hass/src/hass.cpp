#include "hc/hass/hass.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hc/hass_protocol/protocol.hpp"
#include "hc/platform/device.hpp"
#include "hc/platform/time_sync.hpp"

#include "internal.hpp"
#include "url.hpp"

namespace hc::hass {

namespace {

constexpr const char *kTag = "hass";

// the values of the old client, which worked (docs/architecture.md section 6)
constexpr int64_t kPingIntervalMs = 25000;
constexpr int64_t kSilenceTimeoutMs = 70000;
// opening the connection and the authentication together
constexpr int64_t kConnectTimeoutMs = 20000;
constexpr uint32_t kRetryMinMs = 2000;
constexpr uint32_t kRetryMaxMs = 30000;
// the largest message taken; the compact template results stay far below it. An hourly forecast
// of 48 hours is about 16 KB (the buffer of a connection is PSRAM).
constexpr size_t kMaxMessage = 32768;
// A larger one grows the buffer up to this (a folder of the media library is about 300 bytes a
// file); the buffer goes back to kMaxMessage after it.
constexpr size_t kMaxLargeMessage = 256 * 1024;
constexpr TickType_t kSendTimeout = pdMS_TO_TICKS(5000);
// the timers of the task are checked at least this often
constexpr TickType_t kTick = pdMS_TO_TICKS(500);
// the subscribed page is rendered again this often, a safety net (docs/architecture.md section 5)
constexpr int64_t kResyncMs = 60000;
// events for the UI that nobody took yet
constexpr size_t kMaxUiEvents = 16;
// service calls waiting for their result
constexpr size_t kMaxCalls = 8;

enum class EventType : uint8_t {
    Opened,
    Message,
    Closed,
    NetworkUp,
    NetworkDown,
    ConfigChanged,
    Reconnect,
    Command,
};

// What the UI asks of the task; on the heap, the task deletes it.
struct Subscribe {
    std::shared_ptr<const core::Layout> layout;
    core::PageIndex page = core::kNone;
    core::Orientation orientation = core::Orientation::Landscape;
    core::TileIndex panel = core::kNone;
    uint32_t generation = 0;
};

struct Call {
    core::Intent intent;
    memory::String entity_id;
};

struct FetchHistory {
    memory::String entity_id;
    core::EntityIndex entity = core::kNone;
    core::HistoryKind kind = core::HistoryKind::Statistics;
    uint32_t hours = 24;
    uint32_t generation = 0;
};

// The forecast the weather panel shows; type None ends it.
struct SubscribeForecast {
    memory::String entity_id;
    core::EntityIndex entity = core::kNone;
    core::ForecastType type = core::ForecastType::None;
    uint32_t generation = 0;
};

// A one-shot template of the entity picker: the web server's task waits for `wait`; without one
// the answer goes to the UI (the search of the screen's edit mode) stamped with `generation`.
struct QueryWait {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    memory::String json;
    memory::String error;

    ~QueryWait()
    {
        if (done != nullptr) {
            vSemaphoreDelete(done);
        }
    }
};

struct Query {
    // a render_template message without its id: entity_list_message or entity_details_message
    bool details = false;
    memory::String domain;
    memory::String text; // the search text, or the entity id of the details
    size_t max = hass_protocol::kEntityListMax;
    uint32_t timeout_ms = 0;
    std::shared_ptr<QueryWait> wait;
    uint32_t generation = 0;
};

// the state of the motion sensor for input_boolean.<device>_motion
struct Motion {
    bool occupied = false;
};

// What a caller of the media library waits for: the folder, or the address of a file.
struct MediaWait {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    core::MediaList list;
    memory::String url;
    memory::String mime;
    memory::String error;

    ~MediaWait()
    {
        if (done != nullptr) {
            vSemaphoreDelete(done);
        }
    }
};

// media_source/browse_media, or resolve_media; without `wait` a folder goes to the UI stamped
// with `generation`
struct MediaRequest {
    bool resolve = false;
    memory::String media_id;
    uint32_t timeout_ms = 0;
    std::shared_ptr<MediaWait> wait;
    uint32_t generation = 0;
    // set by the task when the request comes in: timeout_ms from then, waiting included
    int64_t deadline = 0;
};

using Command = std::variant<Subscribe, Call, FetchHistory, Query, SubscribeForecast, Motion, MediaRequest>;

struct Event {
    EventType type = EventType::Reconnect;
    // the connection that sent it; events of a connection that was closed meanwhile are dropped
    uint32_t generation = 0;
    // Message: heap, freed by the task
    char *data = nullptr;
    size_t length = 0;
    // Command: deleted by the task
    Command *command = nullptr;
    // Closed: what went wrong
    esp_websocket_error_codes_t error{};
    int close_code = 0;
    // Closed: the server's certificate that the handshake refused
    bool has_refused = false;
    uint8_t refused[kFingerprintBytes] = {};
};

// What the WebSocket task needs of one connection: its messages are put together here.
struct Connection {
    uint32_t generation = 0;
    char *buffer = nullptr; // kMaxMessage, up to kMaxLargeMessage; PSRAM
    size_t capacity = 0;
    size_t length = 0;
    bool too_large = false;
};

// state shared with other tasks, under g_mutex
std::mutex g_mutex;
Status g_status{};
char g_token[kTokenSize] = {};
// set before start(), read by the task
SoundPlayer g_sound_player = nullptr;
RelayHandler g_relay_handler = nullptr;

// The task's events, created with the first use: the UI subscribes its page before start() when
// the dashboard is loaded fast (the P4: 80 ms before), and that subscription waits in the queue.
QueueHandle_t queue()
{
    static const QueueHandle_t created = xQueueCreate(16, sizeof(Event));
    return created;
}

// events for the UI, under g_events_mutex
std::mutex g_events_mutex;
std::deque<HassEvent> g_events;

// ------------------------------------------------------------------------------ helpers

int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

void *json_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

void post(const Event &event, TickType_t wait = 0)
{
    if (queue() == nullptr || xQueueSend(queue(), &event, wait) != pdTRUE) {
        ESP_LOGW(kTag, "event %d dropped, the queue is full", static_cast<int>(event.type));
        std::free(event.data);
        delete event.command;
    }
}

void post_command(Command command)
{
    post({.type = EventType::Command, .command = new Command(std::move(command))}, pdMS_TO_TICKS(100));
}

void deliver(HassEvent event)
{
    std::lock_guard lock(g_events_mutex);
    if (g_events.size() >= kMaxUiEvents) {
        // Full (the UI was busy, a screen animation): the oldest page values or image make room,
        // which newer ones replace. An answer (a media folder, the search, a history) is asked for
        // once and waited for: it goes only when nothing else is there.
        auto drop = std::find_if(g_events.begin(), g_events.end(), [](const HassEvent &e) {
            return std::holds_alternative<core::PageValues>(e) || std::holds_alternative<core::Image>(e);
        });
        if (drop == g_events.end()) {
            drop = g_events.begin();
            ESP_LOGW(kTag, "UI events full: an answer is dropped");
        }
        // a frame of the stream that is dropped counts as taken, or the stream would wait for it
        if (const auto *image = std::get_if<core::Image>(&*drop); image != nullptr && image->live) {
            detail::frame_taken();
        }
        g_events.erase(drop);
    }
    g_events.push_back(std::move(event));
}

void report_failed(core::EntityIndex entity, const char *message)
{
    ESP_LOGW(kTag, "service call failed: %s", message);
    deliver(ServiceFailed{.entity = entity, .message = message});
}

bool sta_has_address()
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    return sta != nullptr && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0;
}

// a clock before this is not set yet (SNTP has not answered)
constexpr time_t kClockValid = 1700000000;
// a chunk of the history that did not arrive within this time is lost
constexpr int64_t kHistoryChunkTimeoutMs = 15000;

// What went wrong with a connection, in words for the web UI and the screen: short, no error
// numbers (they do not fit the screen); the numbers are in the log for advanced users.
// `trusted`: a certificate is trusted on first use.
void describe(const Event &event, bool trusted, char *out, size_t size)
{
    const esp_websocket_error_codes_t &e = event.error;
    const int status = e.esp_ws_handshake_status_code;
    ESP_LOGW(kTag, "connection problem: HTTP %d, errno %d, close code %d, TLS error %s (stack -0x%x, certificate flags 0x%x)",
             status, e.esp_transport_sock_errno, event.close_code, esp_err_to_name(e.esp_tls_last_esp_err),
             static_cast<unsigned>(-e.esp_tls_stack_err), static_cast<unsigned>(e.esp_tls_cert_verify_flags));
    // a web server that is not Home Assistant: it does not know /api/websocket or the upgrade
    if (status == 400 || status == 404 || status == 405) {
        std::snprintf(out, size, "no Home Assistant at this address (HTTP %d)", status);
        return;
    }
    if (status != 0 && status != 101) {
        std::snprintf(out, size, "the server answered HTTP %d instead of opening the connection", status);
        return;
    }
    switch (e.esp_tls_last_esp_err) {
    case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME:
        std::snprintf(out, size, "the host name cannot be resolved");
        return;
    case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:
        std::snprintf(out, size, "the host does not answer");
        return;
    case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:
    case ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET:
        if (e.esp_transport_sock_errno == ECONNREFUSED) {
            std::snprintf(out, size, "the connection was refused: is the port right?");
        } else if (e.esp_transport_sock_errno == EHOSTUNREACH) {
            std::snprintf(out, size, "the host cannot be reached");
        } else {
            std::snprintf(out, size, "cannot connect to the host");
        }
        return;
    case ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED:
        if (event.has_refused && trusted) {
            std::snprintf(out, size, "the server's certificate changed: it is not the trusted one");
        } else if (event.has_refused) {
            std::snprintf(out, size, "the server's certificate is not trusted (self-signed?)");
        } else if (e.esp_tls_cert_verify_flags != 0) {
            std::snprintf(out, size, "the server's certificate is not trusted");
        } else {
            std::snprintf(out, size, "the TLS handshake failed");
        }
        return;
    default:
        break;
    }
    if (e.error_type == WEBSOCKET_ERROR_TYPE_SERVER_CLOSE || event.close_code != 0) {
        std::snprintf(out, size, "Home Assistant closed the connection");
    } else if (e.esp_transport_sock_errno != 0) {
        std::snprintf(out, size, "the connection broke");
    } else {
        std::snprintf(out, size, "the connection broke");
    }
}

// ---------------------------------------------------------------- WebSocket task (its events)

void receive(Connection &c, const esp_websocket_event_data_t &d)
{
    // text frames and their continuations; the client answers pings itself
    constexpr uint8_t kContinuation = 0x0;
    constexpr uint8_t kText = 0x1;
    if (d.op_code != kText && d.op_code != kContinuation) {
        return;
    }
    if (d.op_code == kText && d.payload_offset == 0) {
        c.length = 0;
        c.too_large = false;
    }
    if (c.buffer == nullptr) {
        c.buffer = static_cast<char *>(json_malloc(kMaxMessage));
        c.capacity = c.buffer != nullptr ? kMaxMessage : 0;
    }
    const size_t needed = c.length + static_cast<size_t>(d.data_len);
    if (c.buffer != nullptr && !c.too_large && needed > c.capacity && needed <= kMaxLargeMessage) {
        const size_t grown = std::min(kMaxLargeMessage, std::max(needed, c.capacity * 2));
        if (auto *larger = static_cast<char *>(heap_caps_realloc_prefer(
                c.buffer, grown, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT))) {
            c.buffer = larger;
            c.capacity = grown;
        }
    }
    if (c.buffer == nullptr || needed > c.capacity) {
        if (!c.too_large) {
            ESP_LOGW(kTag, "a message of %d bytes is dropped (more than %u)", d.payload_len,
                     static_cast<unsigned>(kMaxLargeMessage));
        }
        c.too_large = true;
    } else if (!c.too_large && d.data_len > 0) {
        std::memcpy(c.buffer + c.length, d.data_ptr, static_cast<size_t>(d.data_len));
        c.length += static_cast<size_t>(d.data_len);
    }
    // a frame larger than the client's buffer arrives in several events
    if (d.payload_offset + d.data_len < d.payload_len || !d.fin) {
        return;
    }
    if (!c.too_large && c.length > 0) {
        auto *copy = static_cast<char *>(json_malloc(c.length + 1));
        if (copy != nullptr) {
            std::memcpy(copy, c.buffer, c.length);
            copy[c.length] = 0;
            post({.type = EventType::Message, .generation = c.generation, .data = copy, .length = c.length},
                 pdMS_TO_TICKS(100));
        }
    }
    c.length = 0;
    c.too_large = false;
    if (c.capacity > kMaxMessage) {
        std::free(c.buffer);
        c.buffer = nullptr;
        c.capacity = 0;
    }
}

void on_websocket_event(void *arg, esp_event_base_t, int32_t id, void *event_data)
{
    auto &c = *static_cast<Connection *>(arg);
    const auto *d = static_cast<const esp_websocket_event_data_t *>(event_data);
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        post({.type = EventType::Opened, .generation = c.generation});
        break;
    case WEBSOCKET_EVENT_DATA:
        receive(c, *d);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_FINISH: {
        // the first one ends the connection, the task drops the others by their generation
        Event event{.type = EventType::Closed, .generation = c.generation};
        if (d != nullptr) {
            event.error = d->error_handle;
            event.close_code = d->close_status_code;
        }
        // the handshake ran in this task
        event.has_refused = detail::refused_certificate(event.refused);
        post(event);
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------------- the hass task

// The page the UI shows and its render_template subscription. Owned by the hass task.
struct PageState {
    Subscribe wanted;              // no layout: nothing wanted
    hass_protocol::PageSubscription subscription;
    uint32_t id = 0;               // of the render_template request; 0: not subscribed
    int64_t next_render = 0;       // the next subscription (resync, or after an error)
    int64_t last_error = 0;
};

// The history request of the sensor panel: its chunks go out one after the other. Owned by the
// hass task.
struct HistoryState {
    FetchHistory wanted;
    bool active = false;
    uint32_t id = 0;          // of the chunk on its way; 0: none
    uint32_t next_start = 0;  // the start of the next chunk
    int64_t deadline = 0;     // of the chunk on its way
    History result;
};

// The forecast subscription of the weather panel. Owned by the hass task.
struct ForecastState {
    SubscribeForecast wanted;
    uint32_t id = 0; // of the weather/subscribe_forecast request; 0: not subscribed
};

// The picker's query on its way: one at a time, a new one replaces it. Owned by the hass task.
struct QueryState {
    bool active = false;
    bool details = false;
    std::shared_ptr<QueryWait> wait; // null: the answer goes to the UI
    uint32_t generation = 0;
    memory::String text;
    uint32_t id = 0;
    int64_t deadline = 0;
};

struct PendingCall {
    uint32_t id = 0;
    core::EntityIndex entity = core::kNone;
};

// A request of the media library on its way: a browse and a resolve have one each. One on its
// way is never replaced (the player's folder walk and the browser would cancel each other):
// newer ones wait behind it, the waited ones (a task blocks on them) in order, the UI's as the
// newest one only. Owned by the hass task.
struct MediaState {
    MediaRequest request;
    bool active = false;
    uint32_t id = 0;
    int64_t deadline = 0;
    std::deque<MediaRequest> pending;
};

// Owned by the hass task.
struct Session {
    esp_websocket_client_handle_t client = nullptr;
    Connection *connection = nullptr;
    uint32_t generation = 0;
    bool network_up = false;
    bool authenticated = false;
    // 0: no try planned
    int64_t next_try = 0;
    uint32_t retry_ms = kRetryMinMs;
    int64_t started = 0;
    int64_t connected_at = 0;
    int64_t last_message = 0;
    int64_t next_ping = 0;
    uint32_t next_id = 1;
    char url[kUrlSize] = {};
    char ws_url[kUrlSize + 16] = {};
    char token[kTokenSize] = {};
    PageState page;
    HistoryState history;
    ForecastState forecast;
    QueryState query;
    // the get_config request after the authentication (the time zone)
    uint32_t config_id = 0;
    // the subscription of the hassctrl_say event; 0: none
    uint32_t say_id = 0;
    // the render_template subscription of the relay entities; 0: none
    uint32_t relay_id = 0;
    // what the handler was told last, bit n-1 for relay n; a connection starts again
    hass_protocol::RelayStates relays;
    // the motion sensor's state to publish, -1: none yet; the call on its way (0: none), and
    // whether a failed one was logged on this connection
    int8_t motion = -1;
    uint32_t motion_id = 0;
    bool motion_failed = false;
    // the render_template subscription that tells whether the helper exists (0: none), and what it
    // said last: -1 nothing yet, 0 missing, 1 there
    uint32_t motion_watch_id = 0;
    int8_t motion_helper = -1;
    PendingCall calls[kMaxCalls] = {};
    size_t next_call = 0;
    MediaState browse;
    MediaState resolve;
};

Session s;

// the session's view of the motion helper into the status
void update_motion_status()
{
    const MotionHelper helper = s.motion < 0             ? MotionHelper::NoSensor
                                : s.motion_helper < 0    ? MotionHelper::Unknown
                                : s.motion_helper == 0   ? MotionHelper::Missing
                                                         : MotionHelper::Found;
    const memory::String entity =
        helper == MotionHelper::NoSensor ? memory::String() : hass_protocol::motion_entity_id(platform::device_name());
    std::lock_guard lock(g_mutex);
    g_status.motion_helper = helper;
    std::snprintf(g_status.motion_entity, sizeof(g_status.motion_entity), "%s", entity.c_str());
}

void set_state(State state, const char *error = nullptr)
{
    std::lock_guard lock(g_mutex);
    if (g_status.state != state) {
        ESP_LOGI(kTag, "%s%s%s", state_name(state), error != nullptr && error[0] != 0 ? ": " : "",
                 error != nullptr ? error : "");
    }
    g_status.state = state;
    if (error != nullptr) {
        std::snprintf(g_status.error, sizeof(g_status.error), "%s", error);
    }
}

bool configured()
{
    return s.url[0] != 0 && s.token[0] != 0;
}

bool send_text(const memory::String &text);
void send_history_chunk();

// the answer of the picker's query to the screen: the rows or the details, or the error
void query_to_ui(const QueryState &q, const char *error, const memory::String &json)
{
    if (q.details) {
        core::EntityDetails details;
        details.generation = q.generation;
        details.id = q.text;
        if (error == nullptr && !hass_protocol::parse_entity_details(json, details)) {
            error = "the answer is not an entity";
        }
        details.error = error != nullptr ? error : "";
        deliver(std::move(details));
        return;
    }
    core::EntityList list;
    list.generation = q.generation;
    if (error == nullptr && !hass_protocol::parse_entity_list(json, list)) {
        error = "the answer is not a list of entities";
    }
    list.error = error != nullptr ? error : "";
    deliver(std::move(list));
}

// the answer of the picker's query to the waiting task or the UI; its subscription ends after the
// first result
void query_done(const char *error, memory::String json = {})
{
    QueryState &q = s.query;
    if (!q.active) {
        return;
    }
    if (q.id != 0 && s.authenticated) {
        send_text(hass_protocol::unsubscribe_message(s.next_id++, q.id));
    }
    if (error != nullptr) {
        ESP_LOGW(kTag, "entity query: %s", error);
    }
    if (!q.wait) {
        query_to_ui(q, error, json);
    } else {
        if (error != nullptr) {
            q.wait->error = error;
        } else {
            q.wait->json = std::move(json);
        }
        xSemaphoreGive(q.wait->done);
    }
    q = {};
}

void start_query(Query query)
{
    query_done("replaced by a newer request");
    QueryState &q = s.query;
    q.active = true;
    q.details = query.details;
    q.wait = std::move(query.wait);
    q.generation = query.generation;
    q.text = std::move(query.text);
    if (!s.authenticated) {
        query_done("not connected to Home Assistant");
        return;
    }
    q.id = s.next_id++;
    q.deadline = now_ms() + query.timeout_ms;
    const memory::String message = q.details ? hass_protocol::entity_details_message(q.id, q.text)
                                             : hass_protocol::entity_list_message(q.id, query.domain, q.text, query.max);
    if (!send_text(message)) {
        q.id = 0;
        query_done("the request could not be sent");
    }
}

void send_media(MediaState &m, MediaRequest request);

// The answer of a media request (its `result`, or `error`) to the waiting task or the UI; the
// request is over, the next one waiting goes.
void media_done(MediaState &m, const char *error, const cJSON *result = nullptr)
{
    if (!m.active) {
        return;
    }
    MediaRequest &r = m.request;
    core::MediaList list;
    memory::String url, mime, decode_error;
    if (error == nullptr) {
        const bool ok = r.resolve ? hass_protocol::decode_resolve_media(result, url, mime, decode_error)
                                  : hass_protocol::decode_browse_media(result, list, decode_error);
        if (!ok) {
            error = decode_error.c_str();
        }
    }
    if (error != nullptr) {
        ESP_LOGW(kTag, "media %s of \"%s\": %s", r.resolve ? "resolve" : "browse", r.media_id.c_str(), error);
    }
    if (r.wait) {
        if (error != nullptr) {
            r.wait->error = error;
        } else {
            r.wait->list = std::move(list);
            r.wait->url = std::move(url);
            r.wait->mime = std::move(mime);
        }
        xSemaphoreGive(r.wait->done);
    } else if (!r.resolve) {
        list.generation = r.generation;
        list.id = std::move(r.media_id);
        list.error = error != nullptr ? error : "";
        deliver(std::move(list));
    }
    m.request = {};
    m.active = false;
    m.id = 0;
    m.deadline = 0;
    if (!m.pending.empty()) {
        MediaRequest next = std::move(m.pending.front());
        m.pending.pop_front();
        send_media(m, std::move(next));
    }
}

// `request` goes to Home Assistant; `m` is free
void send_media(MediaState &m, MediaRequest request)
{
    m.request = std::move(request);
    m.active = true;
    if (!s.authenticated) {
        media_done(m, "not connected to Home Assistant");
        return;
    }
    if (now_ms() >= m.request.deadline) {
        // its time went by waiting behind the others
        media_done(m, "Home Assistant did not answer in time");
        return;
    }
    m.id = s.next_id++;
    m.deadline = m.request.deadline;
    const memory::String message = m.request.resolve ? hass_protocol::resolve_media_message(m.id, m.request.media_id)
                                                     : hass_protocol::browse_media_message(m.id, m.request.media_id);
    if (!send_text(message)) {
        m.id = 0;
        media_done(m, "the request could not be sent");
    }
}

void start_media(MediaRequest request)
{
    MediaState &m = request.resolve ? s.resolve : s.browse;
    request.deadline = now_ms() + request.timeout_ms;
    if (!m.active) {
        send_media(m, std::move(request));
        return;
    }
    if (!request.wait) {
        // the UI's: only the newest waits, the older one was for a folder left
        std::erase_if(m.pending, [](const MediaRequest &r) { return !r.wait; });
    }
    m.pending.push_back(std::move(request));
}

// the answer (or its error) to the UI; the request is over
void history_done(const char *error = nullptr)
{
    HistoryState &h = s.history;
    if (!h.active) {
        return;
    }
    h.active = false;
    h.id = 0;
    if (error != nullptr) {
        ESP_LOGW(kTag, "history of %s: %s", h.wanted.entity_id.c_str(), error);
        h.result.points.clear();
        h.result.error = error;
    } else {
        ESP_LOGI(kTag, "history of %s: %u points", h.wanted.entity_id.c_str(), static_cast<unsigned>(h.result.points.size()));
    }
    deliver(std::move(h.result));
    h.result = {};
}

void start_history(FetchHistory wanted)
{
    HistoryState &h = s.history;
    // a request that is still running is replaced; its chunk's answer is dropped by its id
    h = {};
    h.wanted = std::move(wanted);
    h.active = true;
    h.result.generation = h.wanted.generation;
    h.result.entity = h.wanted.entity;
    h.result.kind = h.wanted.kind;
    const time_t now = std::time(nullptr);
    if (now < kClockValid) {
        history_done("the clock is not set yet");
        return;
    }
    if (!s.authenticated) {
        history_done("not connected to Home Assistant");
        return;
    }
    h.result.end = static_cast<uint32_t>(now);
    h.result.start = h.result.end - h.wanted.hours * 3600;
    h.next_start = h.result.start;
    send_history_chunk();
}

void send_history_chunk()
{
    HistoryState &h = s.history;
    if (h.next_start >= h.result.end) {
        history_done();
        return;
    }
    const uint32_t end = std::min(h.result.end, h.next_start + hass_protocol::history_chunk_seconds(h.wanted.kind));
    h.id = s.next_id++;
    h.deadline = now_ms() + kHistoryChunkTimeoutMs;
    const memory::String message =
        hass_protocol::history_message(h.id, h.wanted.kind, h.wanted.entity_id, h.next_start, end);
    h.next_start = end;
    if (!send_text(message)) {
        history_done("the request could not be sent");
    }
}

void on_history(const cJSON *root, bool success, const char *message)
{
    HistoryState &h = s.history;
    h.id = 0;
    if (!success) {
        history_done(message != nullptr ? message : "Home Assistant refused the request");
        return;
    }
    memory::String error;
    if (!hass_protocol::decode_history(cJSON_GetObjectItemCaseSensitive(root, "result"), h.wanted.kind,
                                       h.wanted.entity_id, h.result.points, error)) {
        history_done(error.c_str());
        return;
    }
    send_history_chunk();
}

// the stored configuration into the session
void load_config()
{
    std::lock_guard lock(g_mutex);
    std::snprintf(s.url, sizeof(s.url), "%s", g_status.url);
    std::snprintf(s.token, sizeof(s.token), "%s", g_token);
    if (!detail::websocket_url(s.url, s.ws_url, sizeof(s.ws_url))) {
        s.ws_url[0] = 0;
    }
}

void close_connection()
{
    if (s.client == nullptr) {
        return;
    }
    // events still on their way belong to the old generation
    s.generation++;
    esp_websocket_client_destroy(s.client);
    s.client = nullptr;
    std::free(s.connection->buffer);
    delete s.connection;
    s.connection = nullptr;
    s.authenticated = false;
    s.config_id = 0;
    // the subscription ended with the connection; the next one subscribes at once
    s.page.id = 0;
    s.page.next_render = 0;
    s.forecast.id = 0;
    s.say_id = 0;
    s.relay_id = 0;
    s.relays = {};
    s.motion_id = 0;
    s.motion_failed = false;
    s.motion_watch_id = 0;
    s.motion_helper = -1;
    update_motion_status();
    for (PendingCall &call : s.calls) {
        if (call.id != 0) {
            report_failed(call.entity, "the connection to Home Assistant broke");
            call = {};
        }
    }
    if (s.history.active) {
        history_done("the connection to Home Assistant broke");
    }
    s.query.id = 0;
    query_done("the connection to Home Assistant broke");
    media_done(s.browse, "the connection to Home Assistant broke");
    media_done(s.resolve, "the connection to Home Assistant broke");
}

// the state while no connection is open and none is planned at once
void idle_state()
{
    if (!configured()) {
        set_state(State::NotSetUp, "");
    } else if (!s.network_up) {
        set_state(State::WaitingForNetwork, "");
    }
}

void retry_later(const char *error)
{
    close_connection();
    ESP_LOGW(kTag, "%s, next try in %lu s", error, static_cast<unsigned long>(s.retry_ms / 1000));
    s.next_try = now_ms() + s.retry_ms;
    s.retry_ms = std::min(s.retry_ms * 2, kRetryMaxMs);
    set_state(State::Disconnected, error);
}

void open_connection()
{
    s.next_try = 0;
    if (!configured() || !s.network_up) {
        idle_state();
        return;
    }
    s.connection = new Connection{.generation = s.generation};
    esp_websocket_client_config_t config{};
    config.uri = s.ws_url;
    config.disable_auto_reconnect = true;
    config.network_timeout_ms = 10000;
    // the API's own ping proves that Home Assistant answers, not only its web server
    config.disable_pingpong_discon = true;
    config.ping_interval_sec = 30;
    // the TLS handshake runs in the client's task
    config.task_stack = std::strncmp(s.ws_url, "wss:", 4) == 0 ? 6144 : 4096;
    config.crt_bundle_attach = detail::tls_attach;
    config.user_agent = "hassctrl";
    s.client = esp_websocket_client_init(&config);
    if (s.client == nullptr) {
        delete s.connection;
        s.connection = nullptr;
        retry_later("no memory for the connection");
        return;
    }
    esp_websocket_register_events(s.client, WEBSOCKET_EVENT_ANY, on_websocket_event, s.connection);
    s.started = now_ms();
    s.next_id = 1;
    // the last problem stays until the connection works: a reconnect does not hide it
    set_state(State::Connecting);
    ESP_LOGI(kTag, "connecting to %s", s.ws_url);
    if (esp_websocket_client_start(s.client) != ESP_OK) {
        retry_later("the connection could not be started");
    }
}

bool send_json(cJSON *message)
{
    char *text = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (text == nullptr) {
        return false;
    }
    const int length = static_cast<int>(std::strlen(text));
    const bool ok = esp_websocket_client_send_text(s.client, text, length, kSendTimeout) == length;
    cJSON_free(text);
    return ok;
}

bool send_text(const memory::String &text)
{
    const int length = static_cast<int>(text.size());
    return esp_websocket_client_send_text(s.client, text.c_str(), length, kSendTimeout) == length;
}

void send_auth()
{
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", "auth");
    cJSON_AddStringToObject(message, "access_token", s.token);
    if (!send_json(message)) {
        retry_later("the authentication could not be sent");
    }
}

// Home Assistant's configuration: its time zone becomes the device's unless one is fixed on the
// web UI's Time page (docs/architecture.md section 9)
void send_get_config()
{
    s.config_id = s.next_id++;
    cJSON *message = cJSON_CreateObject();
    cJSON_AddNumberToObject(message, "id", s.config_id);
    cJSON_AddStringToObject(message, "type", "get_config");
    if (!send_json(message)) {
        retry_later("the configuration request could not be sent");
    }
}

void on_config(const cJSON *root)
{
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (const char *zone = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(result, "time_zone"))) {
        platform::set_hass_time_zone(zone);
    }
}

void send_ping()
{
    cJSON *message = cJSON_CreateObject();
    cJSON_AddNumberToObject(message, "id", s.next_id++);
    cJSON_AddStringToObject(message, "type", "ping");
    if (!send_json(message)) {
        retry_later("the ping could not be sent");
    }
}

// the hassctrl_say event, when there is a player for its sounds
void subscribe_say()
{
    if (g_sound_player == nullptr) {
        return;
    }
    s.say_id = s.next_id++;
    if (!send_text(hass_protocol::subscribe_events_message(s.say_id, hass_protocol::kSayEvent))) {
        retry_later("the event subscription could not be sent");
    }
}

void on_say_event(const cJSON *root)
{
    hass_protocol::Say say;
    memory::String error;
    switch (hass_protocol::decode_say(cJSON_GetObjectItemCaseSensitive(root, "event"), platform::device_name(), say,
                                      error)) {
    case hass_protocol::SayResult::Play:
        ESP_LOGI(kTag, "%s: %s", hass_protocol::kSayEvent, !say.message.empty() ? say.message.c_str() : say.url.c_str());
        detail::start_say(std::move(say));
        break;
    case hass_protocol::SayResult::NotForUs:
        break;
    case hass_protocol::SayResult::Invalid:
        ESP_LOGW(kTag, "%s: %s", hass_protocol::kSayEvent, error.c_str());
        break;
    }
}

// the entities of the relays, when the board has some
void subscribe_relays()
{
    if (g_relay_handler == nullptr) {
        return;
    }
    s.relay_id = s.next_id++;
    s.relays = {};
    if (!send_text(hass_protocol::render_template_message(s.relay_id, hass_protocol::relay_template(platform::device_name())))) {
        retry_later("the relay subscription could not be sent");
    }
}

// the motion sensor's state to Home Assistant's helper, when connected and there is one; the first
// one on a connection also subscribes the template that tells whether the helper exists
void send_motion()
{
    if (!s.authenticated || s.motion < 0) {
        return;
    }
    if (s.motion_watch_id == 0) {
        s.motion_watch_id = s.next_id++;
        if (!send_text(hass_protocol::render_template_message(s.motion_watch_id,
                                                              hass_protocol::motion_template(platform::device_name())))) {
            retry_later("the motion helper subscription could not be sent");
            return;
        }
    }
    s.motion_id = s.next_id++;
    const bool occupied = s.motion != 0;
    ESP_LOGI(kTag, "motion -> %s", occupied ? "on" : "off");
    if (!send_text(hass_protocol::motion_message(s.motion_id, platform::device_name(), occupied))) {
        retry_later("the motion state could not be sent");
    }
}

// a state of the motion helper's template: a missing helper is logged with the entity ID to create,
// once each time it goes missing; a helper that appears gets the current state at once
void on_motion_helper_event(const cJSON *root)
{
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    if (const char *error = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "error"))) {
        ESP_LOGW(kTag, "motion helper template: %s", error);
        return;
    }
    const bool exists = hass_protocol::motion_helper_exists(cJSON_GetObjectItemCaseSensitive(event, "result"));
    const int8_t before = s.motion_helper;
    s.motion_helper = exists ? 1 : 0;
    update_motion_status();
    if (!exists && before != 0) {
        ESP_LOGW(kTag, "motion helper missing: create the toggle helper %s in Home Assistant",
                 hass_protocol::motion_entity_id(platform::device_name()).c_str());
    } else if (exists && before == 0) {
        ESP_LOGI(kTag, "motion helper found");
        send_motion();
    }
}

// a state of the template: the handler hears of every relay that is new or changed. A relay whose
// entity is gone keeps its state.
void on_relay_event(const cJSON *root)
{
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    if (const char *error = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "error"))) {
        ESP_LOGW(kTag, "relay template: %s", error);
        return;
    }
    hass_protocol::RelayStates now;
    if (!hass_protocol::decode_relays(cJSON_GetObjectItemCaseSensitive(event, "result"), now)) {
        ESP_LOGW(kTag, "the relay template result is not an object");
        return;
    }
    for (unsigned relay = 1; relay <= hass_protocol::kMaxRelays; relay++) {
        const uint8_t bit = static_cast<uint8_t>(1u << (relay - 1));
        const bool on = (now.on & bit) != 0;
        if ((now.seen & bit) != 0 && ((s.relays.seen & bit) == 0 || ((s.relays.on & bit) != 0) != on)) {
            ESP_LOGI(kTag, "relay %u %s", relay, on ? "on" : "off");
            g_relay_handler(relay, on);
            s.relays.seen |= bit;
            s.relays.on = static_cast<uint8_t>((s.relays.on & ~bit) | (on ? bit : 0));
        }
    }
}

// Ends the subscription of the page and subscribes what the UI wants now.
void render_page()
{
    PageState &p = s.page;
    if (p.id != 0) {
        send_text(hass_protocol::unsubscribe_message(s.next_id++, p.id));
        p.id = 0;
    }
    p.next_render = 0;
    if (!p.wanted.layout || p.subscription.empty()) {
        return;
    }
    p.id = s.next_id++;
    p.next_render = now_ms() + kResyncMs;
    if (!send_text(hass_protocol::render_template_message(p.id, p.subscription.template_text))) {
        retry_later("the subscription could not be sent");
        return;
    }
    ESP_LOGD(kTag, "page %u subscribed (%u entities, %u bytes of template)", p.wanted.page,
             static_cast<unsigned>(p.subscription.entities.size()),
             static_cast<unsigned>(p.subscription.template_text.size()));
}

// the error of the forecast subscription to the UI
void forecast_done(const char *error)
{
    ForecastState &f = s.forecast;
    core::Forecast forecast;
    forecast.generation = f.wanted.generation;
    forecast.entity = f.wanted.entity;
    forecast.type = f.wanted.type;
    forecast.error = error;
    ESP_LOGW(kTag, "forecast of %s: %s", f.wanted.entity_id.c_str(), error);
    deliver(std::move(forecast));
}

// Ends the forecast subscription on its way and subscribes the wanted one.
void render_forecast()
{
    ForecastState &f = s.forecast;
    if (f.id != 0) {
        send_text(hass_protocol::unsubscribe_message(s.next_id++, f.id));
        f.id = 0;
    }
    if (f.wanted.type == core::ForecastType::None || f.wanted.entity_id.empty()) {
        return;
    }
    if (!s.authenticated) {
        return; // subscribed again once the connection is there (auth_ok)
    }
    f.id = s.next_id++;
    if (!send_text(hass_protocol::forecast_subscribe_message(f.id, f.wanted.entity_id, f.wanted.type))) {
        f.id = 0;
        retry_later("the forecast subscription could not be sent");
        return;
    }
    ESP_LOGD(kTag, "%s forecast of %s subscribed", f.wanted.type == core::ForecastType::Hourly ? "hourly" : "daily",
             f.wanted.entity_id.c_str());
}

void on_forecast_event(const cJSON *root)
{
    ForecastState &f = s.forecast;
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    core::Forecast forecast;
    forecast.generation = f.wanted.generation;
    forecast.entity = f.wanted.entity;
    forecast.type = f.wanted.type;
    memory::String error;
    if (!hass_protocol::decode_forecast(event, forecast.entries, error)) {
        forecast_done(error.c_str());
        return;
    }
    ESP_LOGD(kTag, "forecast of %s: %u entries", f.wanted.entity_id.c_str(),
             static_cast<unsigned>(forecast.entries.size()));
    deliver(std::move(forecast));
}

// After an error the page is subscribed again at once, but not more than once a resync period:
// a template that fails would otherwise loop.
void page_failed(const char *what)
{
    PageState &p = s.page;
    const int64_t now = now_ms();
    ESP_LOGW(kTag, "the subscription of page %u failed: %s", p.wanted.page, what);
    p.next_render = now - p.last_error > kResyncMs ? now : now + kResyncMs;
    p.last_error = now;
}

void on_command(Command &command)
{
    if (auto *wanted = std::get_if<Subscribe>(&command)) {
        PageState &p = s.page;
        p.wanted = std::move(*wanted);
        p.subscription = p.wanted.layout ? hass_protocol::build_page(*p.wanted.layout, p.wanted.page,
                                                                     p.wanted.orientation, p.wanted.panel)
                                         : hass_protocol::PageSubscription{};
        if (s.authenticated) {
            render_page();
        }
        return;
    }
    if (auto *history = std::get_if<FetchHistory>(&command)) {
        start_history(std::move(*history));
        return;
    }
    if (auto *query = std::get_if<Query>(&command)) {
        start_query(std::move(*query));
        return;
    }
    if (auto *forecast = std::get_if<SubscribeForecast>(&command)) {
        s.forecast.wanted = std::move(*forecast);
        render_forecast();
        return;
    }
    if (auto *media = std::get_if<MediaRequest>(&command)) {
        start_media(std::move(*media));
        return;
    }
    if (const auto *motion = std::get_if<Motion>(&command)) {
        const int8_t state = motion->occupied ? 1 : 0;
        if (state != s.motion) {
            s.motion = state;
            update_motion_status();
            send_motion();
        }
        return;
    }
    auto &call = std::get<Call>(command);
    if (!s.authenticated) {
        report_failed(call.intent.entity, "not connected to Home Assistant");
        return;
    }
    memory::String message, error;
    const uint32_t id = s.next_id++;
    if (!hass_protocol::call_service_message(id, call.intent, call.entity_id, message, error)) {
        report_failed(call.intent.entity, error.c_str());
        return;
    }
    PendingCall &slot = s.calls[s.next_call];
    s.next_call = (s.next_call + 1) % kMaxCalls;
    slot = {.id = id, .entity = call.intent.entity};
    ESP_LOGI(kTag, "call %s", message.c_str());
    if (!send_text(message)) {
        retry_later("the service call could not be sent");
    }
}

// a message with an id: the result of a request, or an event of the subscription
void on_reply(const cJSON *root, const char *type, uint32_t id)
{
    PageState &p = s.page;
    if (std::strcmp(type, "event") == 0 && id == p.id && p.id != 0) {
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
        if (const char *error = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "error"))) {
            // report_errors: a template error or warning; the subscription goes on
            ESP_LOGW(kTag, "template: %s", error);
            return;
        }
        PageValues values;
        values.generation = p.wanted.generation;
        memory::String error;
        if (!hass_protocol::decode_page(cJSON_GetObjectItemCaseSensitive(event, "result"), p.subscription,
                                        values.updates, error)) {
            page_failed(error.c_str());
            return;
        }
        ESP_LOGD(kTag, "page %u: %u entities", p.wanted.page, static_cast<unsigned>(values.updates.size()));
        deliver(std::move(values));
        return;
    }
    if (std::strcmp(type, "event") == 0 && id == s.forecast.id && s.forecast.id != 0) {
        on_forecast_event(root);
        return;
    }
    if (std::strcmp(type, "event") == 0 && id == s.say_id && s.say_id != 0) {
        on_say_event(root);
        return;
    }
    if (std::strcmp(type, "event") == 0 && id == s.relay_id && s.relay_id != 0) {
        on_relay_event(root);
        return;
    }
    if (std::strcmp(type, "event") == 0 && id == s.motion_watch_id && s.motion_watch_id != 0) {
        on_motion_helper_event(root);
        return;
    }
    QueryState &q = s.query;
    if (std::strcmp(type, "event") == 0 && id == q.id && q.id != 0) {
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
        if (const char *error = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "error"))) {
            // a warning is followed by the result
            const char *level = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "level"));
            if (level != nullptr && std::strcmp(level, "ERROR") == 0) {
                query_done(error);
            } else {
                ESP_LOGW(kTag, "entity query: %s", error);
            }
            return;
        }
        memory::String json;
        if (!hass_protocol::template_result_json(cJSON_GetObjectItemCaseSensitive(event, "result"), json)) {
            query_done("the template result is not JSON");
            return;
        }
        query_done(nullptr, std::move(json));
        return;
    }
    if (std::strcmp(type, "result") != 0) {
        return;
    }
    if (id == s.config_id && s.config_id != 0) {
        s.config_id = 0;
        on_config(root);
        return;
    }
    const bool success = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "success"));
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "message"));
    if (id == s.say_id && s.say_id != 0) {
        if (!success) {
            // only an administrator may subscribe events that are not on HA's short list
            s.say_id = 0;
            ESP_LOGW(kTag, "%s not subscribed: %s (the token of an administrator is needed)", hass_protocol::kSayEvent,
                     message != nullptr ? message : "refused");
        }
        return;
    }
    if (id == s.relay_id && s.relay_id != 0) {
        if (!success) {
            s.relay_id = 0;
            ESP_LOGW(kTag, "relays not subscribed: %s", message != nullptr ? message : "refused");
        }
        return;
    }
    if (id == s.motion_watch_id && s.motion_watch_id != 0) {
        if (!success) {
            s.motion_watch_id = 0;
            ESP_LOGW(kTag, "motion helper not watched: %s", message != nullptr ? message : "refused");
        }
        return;
    }
    if (id == s.motion_id && s.motion_id != 0) {
        s.motion_id = 0;
        // a missing helper is not an error here (Home Assistant answers success); the template
        // above reports it. Once per connection; the next change of the sensor tries again.
        if (!success && !s.motion_failed) {
            s.motion_failed = true;
            ESP_LOGW(kTag, "motion not published: %s", message != nullptr ? message : "refused");
        }
        return;
    }
    if (id == s.history.id && s.history.id != 0) {
        on_history(root, success, message);
        return;
    }
    if (id == s.forecast.id && s.forecast.id != 0) {
        if (!success) {
            // the integration has no such forecast: no subscription to end
            s.forecast.id = 0;
            const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(error, "code"));
            forecast_done(code != nullptr && std::strcmp(code, "forecast_not_supported") == 0
                              ? "no forecast of this kind"
                              : message != nullptr ? message : "Home Assistant refused the forecast");
        }
        return;
    }
    for (MediaState *m : {&s.browse, &s.resolve}) {
        if (id == m->id && m->id != 0) {
            media_done(*m, success ? nullptr : message != nullptr ? message : "Home Assistant refused the request",
                       cJSON_GetObjectItemCaseSensitive(root, "result"));
            return;
        }
    }
    if (id == q.id && q.id != 0) {
        if (!success) {
            // no subscription to end
            q.id = 0;
            query_done(message != nullptr ? message : "Home Assistant refused the template");
        }
        return;
    }
    if (id == p.id && p.id != 0) {
        if (!success) {
            p.id = 0;
            page_failed(message != nullptr ? message : "refused");
        }
        return;
    }
    for (PendingCall &call : s.calls) {
        if (call.id == id && id != 0) {
            if (!success) {
                report_failed(call.entity, message != nullptr ? message : "Home Assistant refused the call");
            }
            call = {};
            return;
        }
    }
}

void on_message(const char *data, size_t length)
{
    s.last_message = now_ms();
    cJSON *root = cJSON_ParseWithLength(data, length);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "type"));
    if (type == nullptr) {
        ESP_LOGW(kTag, "a message without a type: %.80s", data);
    } else if (std::strcmp(type, "auth_required") == 0) {
        if (const char *version = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "ha_version"))) {
            std::lock_guard lock(g_mutex);
            std::snprintf(g_status.version, sizeof(g_status.version), "%s", version);
        }
        send_auth();
    } else if (std::strcmp(type, "auth_ok") == 0) {
        s.authenticated = true;
        s.retry_ms = kRetryMinMs;
        s.connected_at = now_ms();
        s.next_ping = s.connected_at + kPingIntervalMs;
        {
            std::lock_guard lock(g_mutex);
            g_status.connections++;
            if (const char *version = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "ha_version"))) {
                std::snprintf(g_status.version, sizeof(g_status.version), "%s", version);
            }
        }
        set_state(State::Connected, "");
        send_get_config();
        render_page();
        render_forecast();
        subscribe_say();
        subscribe_relays();
        send_motion();
    } else if (std::strcmp(type, "auth_invalid") == 0) {
        const char *reason = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "message"));
        // Home Assistant's own words ("Invalid access token or password"); the state says the rest
        char error[96];
        std::snprintf(error, sizeof(error), "%.90s", reason != nullptr ? reason : "the token was rejected");
        close_connection();
        set_state(State::TokenRejected, error);
    } else if (const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id"); cJSON_IsNumber(id)) {
        on_reply(root, type, static_cast<uint32_t>(id->valuedouble));
    } else {
        ESP_LOGD(kTag, "message %s, %u bytes", type, static_cast<unsigned>(length));
    }
    cJSON_Delete(root);
}

void handle(Event &event)
{
    // the events of a connection that is closed already
    const bool current = s.client != nullptr && event.generation == s.generation;
    switch (event.type) {
    case EventType::Opened:
        if (current) {
            s.last_message = now_ms();
            ESP_LOGI(kTag, "open, waiting for the authentication request");
            std::lock_guard lock(g_mutex);
            g_status.refused_certificate[0] = 0;
        }
        break;
    case EventType::Message:
        if (current) {
            on_message(event.data, event.length);
        }
        break;
    case EventType::Closed:
        if (current) {
            char refused[kFingerprintSize] = {};
            if (event.has_refused) {
                detail::fingerprint_text(event.refused, refused);
                ESP_LOGW(kTag, "certificate refused, SHA-256 %s", refused);
            }
            bool trusted;
            {
                std::lock_guard lock(g_mutex);
                trusted = g_status.certificate[0] != 0;
                std::memcpy(g_status.refused_certificate, refused, sizeof(refused));
            }
            char error[96];
            describe(event, trusted, error, sizeof(error));
            retry_later(error);
        }
        break;
    case EventType::NetworkUp:
        s.network_up = true;
        // a rejected token waits for a new one: every try is a failed login in Home Assistant
        if (s.client == nullptr && status().state != State::TokenRejected) {
            s.retry_ms = kRetryMinMs;
            s.next_try = now_ms();
        }
        break;
    case EventType::NetworkDown:
        s.network_up = false;
        close_connection();
        s.next_try = 0;
        idle_state();
        break;
    case EventType::ConfigChanged:
    case EventType::Reconnect:
        close_connection();
        if (event.type == EventType::ConfigChanged) {
            load_config();
        }
        s.retry_ms = kRetryMinMs;
        s.next_try = now_ms();
        break;
    case EventType::Command:
        on_command(*event.command);
        break;
    }
    std::free(event.data);
    delete event.command;
}

void on_timers()
{
    const int64_t now = now_ms();
    if (s.client == nullptr && s.next_try != 0 && now >= s.next_try) {
        open_connection();
    } else if (s.client != nullptr && !s.authenticated && now - s.started > kConnectTimeoutMs) {
        retry_later("Home Assistant did not answer the authentication");
    } else if (s.authenticated && now - s.last_message > kSilenceTimeoutMs) {
        retry_later("Home Assistant stopped answering");
    } else if (s.authenticated && now >= s.next_ping) {
        s.next_ping = now + kPingIntervalMs;
        send_ping();
    } else if (s.authenticated && s.page.next_render != 0 && now >= s.page.next_render) {
        render_page();
    } else if (s.history.id != 0 && now >= s.history.deadline) {
        history_done("Home Assistant did not answer the history request");
    }
    if (s.query.active && now >= s.query.deadline) {
        query_done("Home Assistant did not answer in time");
    }
    for (MediaState *m : {&s.browse, &s.resolve}) {
        if (m->active && now >= m->deadline) {
            media_done(*m, "Home Assistant did not answer in time");
        }
    }

    std::lock_guard lock(g_mutex);
    g_status.retry_in_s = g_status.state == State::Disconnected && s.next_try > now
                              ? static_cast<uint32_t>((s.next_try - now + 999) / 1000)
                              : 0;
    g_status.connected_s = s.authenticated ? static_cast<uint32_t>((now - s.connected_at) / 1000) : 0;
}

void task(void *)
{
    load_config();
    s.network_up = sta_has_address();
    s.next_try = configured() && s.network_up ? now_ms() : 0;
    idle_state();
    for (;;) {
        Event event{};
        if (xQueueReceive(queue(), &event, kTick) == pdTRUE) {
            handle(event);
        }
        on_timers();
    }
}

void on_ip_event(void *, esp_event_base_t, int32_t id, void *)
{
    post({.type = id == IP_EVENT_STA_GOT_IP ? EventType::NetworkUp : EventType::NetworkDown});
}

} // namespace

const char *state_name(State state)
{
    switch (state) {
    case State::NotSetUp:
        return "not set up";
    case State::WaitingForNetwork:
        return "waiting for Wi-Fi";
    case State::Connecting:
        return "connecting";
    case State::Connected:
        return "connected";
    case State::TokenRejected:
        return "token rejected";
    case State::Disconnected:
        return "disconnected";
    }
    return "?";
}

esp_err_t start()
{
    {
        std::lock_guard lock(g_mutex);
        g_status.state = State::NotSetUp;
        detail::read_stored_config(g_status.url, g_token, g_status.certificate);
        g_status.token_set = g_token[0] != 0;
    }

    ESP_RETURN_ON_FALSE(queue() != nullptr, ESP_ERR_NO_MEM, kTag, "queue");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, nullptr), kTag,
                        "ip event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip_event, nullptr), kTag,
                        "ip event");
    // the template of a page is built and its results are decoded on it
    ESP_RETURN_ON_FALSE(xTaskCreate(task, "hass", 6144, nullptr, 4, nullptr) == pdPASS, ESP_ERR_NO_MEM, kTag, "task");
    ESP_LOGI(kTag, "started, %s", g_status.url[0] != 0 ? g_status.url : "no address set");
    return ESP_OK;
}

Status status()
{
    std::lock_guard lock(g_mutex);
    return g_status;
}

void reconnect()
{
    post({.type = EventType::Reconnect}, pdMS_TO_TICKS(100));
}

void subscribe_page(std::shared_ptr<const core::Layout> layout, core::PageIndex page, core::Orientation o,
                    core::TileIndex panel, uint32_t generation)
{
    post_command(Subscribe{
        .layout = std::move(layout), .page = page, .orientation = o, .panel = panel, .generation = generation});
}

void call_service(const core::Intent &intent, std::string_view entity_id)
{
    post_command(Call{.intent = intent, .entity_id = memory::String(entity_id)});
}

void subscribe_forecast(std::string_view entity_id, core::EntityIndex entity, core::ForecastType type,
                        uint32_t generation)
{
    post_command(SubscribeForecast{
        .entity_id = memory::String(entity_id), .entity = entity, .type = type, .generation = generation});
}

void fetch_history(std::string_view entity_id, core::EntityIndex entity, core::HistoryKind kind, uint32_t hours,
                   uint32_t generation)
{
    post_command(FetchHistory{.entity_id = memory::String(entity_id), .entity = entity, .kind = kind, .hours = hours,
                              .generation = generation});
}

namespace {

bool run_query(Query query, uint32_t timeout_ms, memory::String &json, memory::String &error)
{
    auto wait = std::make_shared<QueryWait>();
    if (wait->done == nullptr) {
        error = "no memory";
        return false;
    }
    query.timeout_ms = timeout_ms;
    query.wait = wait;
    post_command(std::move(query));
    // the task answers at its deadline at the latest; the margin covers its tick
    if (xSemaphoreTake(wait->done, pdMS_TO_TICKS(timeout_ms) + kTick * 2) != pdTRUE) {
        error = "Home Assistant did not answer in time";
        return false;
    }
    if (!wait->error.empty()) {
        error = std::move(wait->error);
        return false;
    }
    json = std::move(wait->json);
    return true;
}

} // namespace

bool list_entities(std::string_view domain, std::string_view query, uint32_t timeout_ms, memory::String &json,
                   memory::String &error)
{
    Query q;
    q.domain = memory::String(domain);
    q.text = memory::String(query);
    return run_query(std::move(q), timeout_ms, json, error);
}

bool entity_details(std::string_view entity_id, uint32_t timeout_ms, memory::String &json, memory::String &error)
{
    Query q;
    q.details = true;
    q.text = memory::String(entity_id);
    return run_query(std::move(q), timeout_ms, json, error);
}

void find_entities(std::string_view query, size_t max, uint32_t timeout_ms, uint32_t generation)
{
    Query q;
    q.text = memory::String(query);
    q.max = max;
    q.timeout_ms = timeout_ms;
    q.generation = generation;
    post_command(std::move(q));
}

void fetch_entity(std::string_view entity_id, uint32_t timeout_ms, uint32_t generation)
{
    Query q;
    q.details = true;
    q.text = memory::String(entity_id);
    q.timeout_ms = timeout_ms;
    q.generation = generation;
    post_command(std::move(q));
}

namespace {

// a media request for a waiting task: its answer, or null with `error` set
std::shared_ptr<MediaWait> run_media(bool resolve, std::string_view media_id, uint32_t timeout_ms,
                                     memory::String &error)
{
    auto wait = std::make_shared<MediaWait>();
    if (wait->done == nullptr) {
        error = "no memory";
        return nullptr;
    }
    post_command(MediaRequest{.resolve = resolve,
                              .media_id = memory::String(media_id),
                              .timeout_ms = timeout_ms,
                              .wait = wait,
                              .generation = 0});
    // the task answers at its deadline at the latest; the margin covers its tick
    if (xSemaphoreTake(wait->done, pdMS_TO_TICKS(timeout_ms) + kTick * 2) != pdTRUE) {
        error = "Home Assistant did not answer in time";
        return nullptr;
    }
    if (!wait->error.empty()) {
        error = std::move(wait->error);
        return nullptr;
    }
    return wait;
}

} // namespace

void browse_media(std::string_view media_id, uint32_t timeout_ms, uint32_t generation)
{
    post_command(MediaRequest{.resolve = false,
                              .media_id = memory::String(media_id),
                              .timeout_ms = timeout_ms,
                              .wait = {},
                              .generation = generation});
}

bool browse_media(std::string_view media_id, uint32_t timeout_ms, core::MediaList &out, memory::String &error)
{
    const auto result = run_media(false, media_id, timeout_ms, error);
    if (!result) {
        return false;
    }
    out = std::move(result->list);
    out.id = memory::String(media_id);
    return true;
}

bool resolve_media(std::string_view media_id, uint32_t timeout_ms, memory::String &url, memory::String &mime,
                   memory::String &error)
{
    const auto result = run_media(true, media_id, timeout_ms, error);
    if (!result) {
        return false;
    }
    url = std::move(result->url);
    mime = std::move(result->mime);
    return true;
}

namespace detail {

void credentials(char *url, size_t url_size, char *token, size_t token_size)
{
    std::lock_guard lock(g_mutex);
    std::snprintf(url, url_size, "%s", g_status.url);
    std::snprintf(token, token_size, "%s", g_token);
}

bool is_current_url(const char *url)
{
    std::lock_guard lock(g_mutex);
    return std::strcmp(g_status.url, url) == 0;
}

void config_stored(const char *url, const char *token, bool remove, bool new_address)
{
    {
        std::lock_guard lock(g_mutex);
        std::snprintf(g_status.url, sizeof(g_status.url), "%s", url);
        if (remove) {
            g_token[0] = 0;
        } else if (token[0] != 0) {
            std::snprintf(g_token, sizeof(g_token), "%s", token);
        }
        g_status.token_set = g_token[0] != 0;
        if (new_address) {
            g_status.certificate[0] = 0;
            set_trusted_certificate(nullptr);
        }
        g_status.refused_certificate[0] = 0;
        g_status.version[0] = 0;
        // what follows, so the answer to this change already shows it; the task takes over
        g_status.state = g_status.url[0] != 0 && g_status.token_set ? State::Connecting : State::NotSetUp;
        g_status.error[0] = 0;
        g_status.retry_in_s = 0;
        g_status.connected_s = 0;
    }
    post({.type = EventType::ConfigChanged}, pdMS_TO_TICKS(100));
}

void certificate_stored(const char *text)
{
    {
        std::lock_guard lock(g_mutex);
        std::snprintf(g_status.certificate, sizeof(g_status.certificate), "%s", text);
        g_status.refused_certificate[0] = 0;
        if (g_status.state == State::Disconnected) {
            g_status.state = State::Connecting;
            g_status.error[0] = 0;
            g_status.retry_in_s = 0;
        }
    }
    post({.type = EventType::Reconnect}, pdMS_TO_TICKS(100));
}

void deliver_event(core::HassEvent event)
{
    deliver(std::move(event));
}

SoundPlayer sound_player()
{
    return g_sound_player;
}

} // namespace detail

void set_sound_player(SoundPlayer player)
{
    g_sound_player = player;
}

void set_relay_handler(RelayHandler handler)
{
    g_relay_handler = handler;
}

void publish_motion(bool occupied)
{
    post_command(Motion{.occupied = occupied});
}

void say(std::string_view message, std::string_view engine, std::string_view language, std::string_view url)
{
    detail::start_say({.message = memory::String(message),
                       .engine = memory::String(engine),
                       .language = memory::String(language),
                       .options = {},
                       .url = memory::String(url)});
}

bool take_event(HassEvent &out)
{
    std::lock_guard lock(g_events_mutex);
    if (g_events.empty()) {
        return false;
    }
    out = std::move(g_events.front());
    g_events.pop_front();
    if (const auto *image = std::get_if<core::Image>(&out); image != nullptr && image->live) {
        detail::frame_taken();
    }
    return true;
}

} // namespace hc::hass
