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
// Over https the certificate bundle checks Home Assistant's certificate. A self-signed one is
// refused until the user trusts it on first use: status() shows its SHA-256, the web UI hands it
// to trust_certificate(). From then on that certificate is accepted besides the bundle.
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
#include "hc/core/byte_ring.hpp"
#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"
#include "hc/memory/large.hpp"

namespace hc::hass {

constexpr size_t kUrlSize = 128;
// a long-lived token is a JWT of about 180 characters
constexpr size_t kTokenSize = 512;
// the SHA-256 of a certificate, and as 64 hex digits with their terminator
constexpr size_t kFingerprintBytes = 32;
constexpr size_t kFingerprintSize = kFingerprintBytes * 2 + 1;

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

// What Home Assistant has of the motion sensor's helper input_boolean.<device>_motion
// (docs/architecture.md section 9.7b)
enum class MotionHelper : uint8_t {
    NoSensor, // no motion state published yet (the board has no sensor)
    Unknown,  // not connected, or Home Assistant did not answer yet
    Missing,  // no such entity: the user creates the toggle helper
    Found,
};

struct Status {
    State state;
    MotionHelper motion_helper;
    char motion_entity[48]; // "input_boolean.hassctrl_7e7d_motion", empty with NoSensor
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
    // https: the SHA-256 of the certificate trusted on first use, empty: none (only the bundle)
    char certificate[kFingerprintSize];
    // the SHA-256 of the server's certificate when the last try refused it (neither the bundle nor
    // the trusted certificate); empty once a connection works or the address changes
    char refused_certificate[kFingerprintSize];
};

// Loads URL and token and starts the task. The default event loop has to exist.
esp_err_t start();

Status status();

// Checks and stores the URL and, unless `token` is nullptr or empty, the token, and connects with
// them. An empty URL removes URL and token (not set up). ESP_ERR_INVALID_ARG with `error` filled
// when the URL is not usable.
esp_err_t set_config(const char *url, const char *token, char *error, size_t error_size);

// The checks of set_config() alone: ESP_ERR_INVALID_ARG with `error` filled when set_config()
// would refuse the URL or the token. Stores nothing (the backup import checks every section first).
esp_err_t check_config(const char *url, const char *token, char *error, size_t error_size);

// Asks Home Assistant at `url` whether it takes `token` (GET /api/), blocking for up to 10 s, and
// stores nothing. ESP_ERR_INVALID_ARG with `error` filled when the URL or the token is not usable
// or Home Assistant rejects the token (HTTP 401); ESP_OK otherwise, also when no answer came: the
// connection reports that after set_config().
esp_err_t test_token(const char *url, const char *token, char *error, size_t error_size);

// tries at once instead of after the backoff, also after a rejected token
void reconnect();

// Trusts the certificate with this SHA-256 (64 hex digits, colons allowed) besides the bundle,
// for a Home Assistant with a self-signed certificate, stores it and connects again; empty
// forgets it. One certificate: it belongs to the address and goes when the address changes.
// ESP_ERR_INVALID_ARG with `error` filled when `fingerprint` is not a SHA-256.
esp_err_t trust_certificate(const char *fingerprint, char *error, size_t error_size);

// The check of trust_certificate() alone, stores nothing (the backup import).
esp_err_t check_certificate(const char *fingerprint, char *error, size_t error_size);

// What the task hands to the UI (core/entity.hpp).
using core::HassEvent;
using core::History;
using core::PageValues;
using core::ServiceFailed;

// Subscribes the tiles of `page` of `layout` shown in orientation `o`, with the panel fields of
// the tile `panel` (kNone: no panel open), and ends the subscription before. A null layout ends
// it without a new one.
void subscribe_page(std::shared_ptr<const core::Layout> layout, core::PageIndex page, core::Orientation o,
                    core::TileIndex panel, uint32_t generation);

// Sends the service call of `intent` for `entity_id`. Without a connection it is dropped and
// reported as failed.
void call_service(const core::Intent &intent, std::string_view entity_id);

// Reads the history of `entity_id` over the last `hours` (the graph of a sensor panel), in chunks
// of hass_protocol::kHistoryChunkSeconds; the answer comes back as one core::History stamped with
// `generation`, or with its error. A new request replaces one that is still running.
void fetch_history(std::string_view entity_id, core::EntityIndex entity, core::HistoryKind kind, uint32_t hours,
                   uint32_t generation);

// Subscribes the forecast of the weather entity `entity_id` (the weather panel): the answer comes
// back through take_event as a core::Forecast stamped with `generation`, and again with every
// update of the integration, also after a reconnect. A new call replaces the subscription; type
// None ends it. An answer to an older generation is dropped by the UI.
void subscribe_forecast(std::string_view entity_id, core::EntityIndex entity, core::ForecastType type,
                        uint32_t generation);

// The next event for the UI, false when there is none. The task keeps at most 16; when nobody
// takes them the oldest go.
bool take_event(HassEvent &out);

// The camera image of `entity_id` as a frame of `width` x `height` (core::Image, through
// take_event), cut to cover the box: from Home Assistant's camera proxy (also for a tile with a
// stream: only play_stream opens it). `whole`: the whole picture, about as large as covers the box
// but not cut to it (the decoder's size, the UI scales it). `swap_bytes`: RGB565 with the bytes swapped (LVGL's
// LV_COLOR_16_SWAP). The images are fetched by up to three tasks of their own at the same time (one
// on the ESP32 and the S3, media.cpp), created as the requests come; one request per entity and size
// waits, the newest. A request that falls out of the full queue is answered with an error.
void fetch_image(std::string_view entity_id, core::EntityIndex entity, uint16_t width, uint16_t height, bool whole,
                 bool swap_bytes, uint32_t generation);

// Plays the MJPEG `stream` of `entity_id` as frames of the whole picture, about as large as covers
// `width` x `height` (core::Image with `live`) until stop_stream() or another play_stream(); the
// same stream in another size keeps its connection. One stream plays at a time, the still images go on
// in the other tasks (on the ESP32 and the S3 they wait meanwhile). A frame is decoded once the UI took the one before.
void play_stream(std::string_view entity_id, std::string_view stream, core::EntityIndex entity, uint16_t width,
                 uint16_t height, bool swap_bytes, uint32_t generation);
void stop_stream();

// A JPEG in memory (the cover of an MP3) as a `width` x `height` frame, cut to cover it, with the
// decoders of the camera images (one at a time); on the calling task. `swap_bytes` as fetch_image.
bool decode_jpeg(const uint8_t *jpeg, size_t size, uint16_t width, uint16_t height, bool swap_bytes, core::Image &out);

// For measurements: the decoder's scale of a stream's frames, 0..3 (1:1..1:8); -1 (default) the
// smallest that covers the box.
void set_stream_scale(int shift);

// The entity picker of the web editor (docs/ui/web/web-dashboard-editor.md section 3): a one-shot template
// over the connection, its result as JSON text (hass_protocol::entity_list_message and
// entity_details_message say what it holds). Both block the calling task until Home Assistant
// answered, at most `timeout_ms`; false with `error` set without a connection, on a template error
// or a timeout. One at a time: a second request replaces the first, which fails.
bool list_entities(std::string_view domain, std::string_view query, uint32_t timeout_ms, memory::String &json,
                   memory::String &error);
bool entity_details(std::string_view entity_id, uint32_t timeout_ms, memory::String &json, memory::String &error);

// The same for the screen's edit mode (docs/ui/lvgl/lvgl-dashboard.md section 7), without
// blocking: the answer comes back through take_event as a core::EntityList (at most `max` rows) or
// core::EntityDetails stamped with `generation`, with the error set when it failed. They share the
// one query of the web editor's picker: a newer request of either replaces the one on its way.
void find_entities(std::string_view query, size_t max, uint32_t timeout_ms, uint32_t generation);
void fetch_entity(std::string_view entity_id, uint32_t timeout_ms, uint32_t generation);

// Home Assistant's media library (Media in its sidebar, docs/architecture.md section 9.6). The
// folder `media_id` (empty: the top) comes back through take_event as a core::MediaList stamped
// with `generation`, with the error set when it failed; a newer browse replaces one on its way.
void browse_media(std::string_view media_id, uint32_t timeout_ms, uint32_t generation);

// The same, blocking the calling task until the answer came, at most `timeout_ms` (the console).
bool browse_media(std::string_view media_id, uint32_t timeout_ms, core::MediaList &out, memory::String &error);

// The address of the file `media_id` to stream: a path of Home Assistant signed for a short while
// ("/media/local/x.mp3?authSig=..."), and its MIME type. Blocks the calling task.
bool resolve_media(std::string_view media_id, uint32_t timeout_ms, memory::String &url, memory::String &mime,
                   memory::String &error);

// Downloads the music `source` into `ring` while the player plays it (media_audio.cpp): a
// media-source:// id of the media library, a path of Home Assistant ("/media/local/x.mp3") or an
// http(s) address. A task of its own until the ring is cancelled; the ring's seeks fetch the file
// again from there, a broken connection is tried again a few times before the ring fails. False
// when it cannot start (no memory).
bool stream_media(std::string_view source, std::shared_ptr<core::ByteRing> ring);

// A Home Assistant instance found on the network.
struct Found {
    char name[48]; // its location name ("Home")
    char url[kUrlSize];
    char version[24];
};

// Searches the network for Home Assistant (mDNS, _home-assistant._tcp) and blocks the calling
// task for `timeout_ms`.
size_t discover(Found *out, size_t max, uint32_t timeout_ms);

// Sounds from Home Assistant (docs/architecture.md section 9.6): the hassctrl_say event
// (hass_protocol::kSayEvent) names a text to speak or an MP3; a task of its own (say.cpp) asks
// Home Assistant for the speech, downloads the MP3 into PSRAM and hands it to the player, which
// takes the buffer (heap_caps_malloc, freed with heap_caps_free). A newer sound stops a download
// on its way. Set before start(); without a player the event is not subscribed.
using SoundPlayer = void (*)(uint8_t *mp3, size_t size);
void set_sound_player(SoundPlayer player);

// What the event does, without it (the console): `message` spoken by the tts entity `engine` in
// `language` (empty: the engine's), or the MP3 at `url` (a path of Home Assistant or an address).
void say(std::string_view message, std::string_view engine, std::string_view language, std::string_view url);

// Relays (docs/architecture.md section 9.7a): entities named input_boolean.<device>_relay_<n> or
// switch.<device>_relay_<n> (<device>: the host name with "_", hassctrl_7e7d_relay_1) switch the
// board's relays. One render_template subscription finds them and follows their state; the handler
// is called from the hass task for a relay (1 to hass_protocol::kMaxRelays) whenever its state
// changes, and once for each entity there is after a connection. Set before start(); without a
// handler nothing is subscribed.
using RelayHandler = void (*)(unsigned relay, bool on);
void set_relay_handler(RelayHandler handler);

// The motion sensor (docs/architecture.md section 9.7b): turns input_boolean.<device>_motion in
// Home Assistant on or off (hass_protocol::motion_entity_id, a helper the user creates).
// From any task. Sent when connected, and the last state again after every (re)connect; a call
// that fails (no such helper) is logged once per connection, the next change sends again.
void publish_motion(bool occupied);

} // namespace hc::hass
