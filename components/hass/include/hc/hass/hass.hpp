#pragma once

// The connection to Home Assistant (docs/architecture.md section 6).
//
// One WebSocket connection to <url>/api/websocket, authenticated with a long-lived access token.
// URL and token are entered in the web UI and kept in the settings NVS; the token never leaves
// the device again. A task owns the connection: it connects once the station has an address,
// answers the authentication, sends the API's ping every 25 s, and reconnects with a backoff
// (2 s doubling to 30 s) when the connection breaks or stays silent for 70 s. A rejected token is
// not tried again until the token changes or reconnect() is called: every failed login is a
// notification in Home Assistant, and with ip_ban enabled it bans the device.
//
// The UI names the page it shows (subscribe_page): the task subscribes its template, renders it
// again every 60 s as a safety net and at once after a reconnect, and hands the decoded values to
// the UI (take_event). Service calls go over the same connection; a failure comes back as an
// event as well (docs/architecture.md section 4).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "esp_err.h"
#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"
#include "hc/memory/large.hpp"

namespace hc::hass {

constexpr size_t kUrlSize = 128;
// a long-lived token is a JWT of about 180 characters
constexpr size_t kTokenSize = 512;

enum class State : uint8_t {
    NotSetUp,          // no URL or no token
    WaitingForNetwork, // the station has no address
    Connecting,        // opening the WebSocket and authenticating
    Connected,
    TokenRejected,     // auth_invalid: waits for a new token or reconnect()
    Disconnected,      // lost or not reached: the next try follows after retry_in_s
};

// "connected", "token rejected", ...
const char *state_name(State state);

struct Status {
    State state;
    // as stored: scheme, host and port ("http://homeassistant.local:8123")
    char url[kUrlSize];
    bool token_set;
    // reported by Home Assistant when it asks for the authentication
    char version[24];
    // the last problem in words; it stays while reconnecting and is cleared once connected
    char error[96];
    uint32_t retry_in_s;  // Disconnected: seconds until the next try
    uint32_t connected_s; // Connected: seconds since the authentication
    uint32_t connections; // successful authentications since the start
};

// Loads URL and token and starts the task. The default event loop has to exist.
esp_err_t start();

Status status();

// Checks and stores the URL and, unless `token` is nullptr or empty, the token, and connects with
// them. An empty URL removes URL and token (not set up). ESP_ERR_INVALID_ARG with `error` filled
// when the URL is not usable.
esp_err_t set_config(const char *url, const char *token, char *error, size_t error_size);

// tries at once instead of after the backoff, also after a rejected token
void reconnect();

// What the task hands to the UI (core/entity.hpp).
using core::HassEvent;
using core::History;
using core::PageValues;
using core::ServiceFailed;

// Subscribes the tiles of `page` of `layout`, with the panel fields of the tile `panel` (kNone:
// no panel open), and ends the subscription before. A null layout ends it without a new one.
void subscribe_page(std::shared_ptr<const core::Layout> layout, core::PageIndex page, core::TileIndex panel,
                    uint32_t generation);

// Sends the service call of `intent` for `entity_id`. Without a connection it is dropped and
// reported as failed.
void call_service(const core::Intent &intent, std::string_view entity_id);

// Reads the history of `entity_id` over the last `hours` (the graph of a sensor panel), in chunks
// of hass_protocol::kHistoryChunkSeconds; the answer comes back as one core::History stamped with
// `generation`, or with its error. A new request replaces one that is still running.
void fetch_history(std::string_view entity_id, core::EntityIndex entity, core::HistoryKind kind, uint32_t hours,
                   uint32_t generation);

// The next event for the UI, false when there is none. The task keeps at most 16; when nobody
// takes them the oldest go.
bool take_event(HassEvent &out);

// The camera image of `entity_id` as a frame of `width` x `height` (core::Image, through
// take_event), cut to cover the box: from Home Assistant's camera proxy, or the first frame of the
// MJPEG `stream` when one is given. `swap_bytes`: RGB565 with the bytes swapped (LVGL's
// LV_COLOR_16_SWAP). The images are fetched one at a time by a task of their own (media.cpp),
// created with the first request; one request per entity waits, the newest.
void fetch_image(std::string_view entity_id, std::string_view stream, core::EntityIndex entity, uint16_t width,
                 uint16_t height, bool swap_bytes, uint32_t generation);

// Plays the MJPEG `stream` of `entity_id` as frames of `width` x `height` (core::Image with `live`)
// until stop_stream() or another play_stream(); one stream plays at a time, the still images wait
// meanwhile. A frame is decoded once the UI took the one before.
void play_stream(std::string_view entity_id, std::string_view stream, core::EntityIndex entity, uint16_t width,
                 uint16_t height, bool swap_bytes, uint32_t generation);
void stop_stream();

// For measurements: the decoder's scale of a stream's frames, 0..3 (1:1..1:8); -1 (default) the
// smallest that covers the box.
void set_stream_scale(int shift);

// A Home Assistant instance found on the network.
struct Found {
    char name[48]; // its location name ("Home")
    char url[kUrlSize];
    char version[24];
};

// Searches the network for Home Assistant (mDNS, _home-assistant._tcp) and blocks the calling
// task for `timeout_ms`.
size_t discover(Found *out, size_t max, uint32_t timeout_ms);

} // namespace hc::hass
