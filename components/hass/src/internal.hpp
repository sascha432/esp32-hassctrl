#pragma once

// What the parts of hass share: the connection's settings and the events for the UI (hass.cpp),
// the stored configuration (config.cpp), the certificate check (tls.cpp).

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "hc/core/entity.hpp"
#include "hc/hass/hass.hpp"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass::detail {

// The crt_bundle_attach of every connection to Home Assistant: the certificate bundle, or the
// certificate trusted on first use.
esp_err_t tls_attach(void *conf);

// the SHA-256 (kFingerprintBytes) of the trusted certificate; nullptr: none
void set_trusted_certificate(const uint8_t *digest);

// The server's certificate that the last handshake of the calling task refused, once: true with
// its SHA-256 in `digest`. Called in the task of the handshake (the WebSocket client's events).
bool refused_certificate(uint8_t *digest);

// a SHA-256 as 64 lowercase hex digits into `out` (kFingerprintSize)
void fingerprint_text(const uint8_t *digest, char *out);

// 64 hex digits, also with colons or spaces between them; false when it is not a SHA-256
bool parse_fingerprint(const char *text, uint8_t *digest);

// the address ("http://h:8123") and the token of Home Assistant; empty while not set up
void credentials(char *url, size_t url_size, char *token, size_t token_size);

// The stored configuration (config.cpp): address, token and the trusted certificate from NVS,
// empty when not stored; a valid certificate is trusted at once. Called by start().
void read_stored_config(char (&url)[kUrlSize], char (&token)[kTokenSize], char (&certificate)[kFingerprintSize]);

// What config.cpp stored, into the connection's status (hass.cpp), and the task is told: the
// address (`url` normalized) and the token (empty: kept; `remove`: both gone), `new_address` drops
// the trusted certificate. certificate_stored(): the trusted one as hex, empty: none.
bool is_current_url(const char *url);
void config_stored(const char *url, const char *token, bool remove, bool new_address);
void certificate_stored(const char *text);

// hands an event to the UI (take_event)
void deliver_event(core::HassEvent event);

// the UI took a frame of the stream that plays (media.cpp decodes the next one)
void frame_taken();

// the player of set_sound_player(), nullptr: none
SoundPlayer sound_player();

// fetches and plays a sound of the hassctrl_say event (say.cpp); a newer one replaces it
void start_say(hass_protocol::Say say);

// A zeroed buffer a decoder reads all the time (its tables and lines; media.cpp): internal RAM,
// which is faster, only while it leaves 32 KB free (Wi-Fi, lwIP and TLS allocate at any moment and
// would fail somewhere else), else PSRAM. Freed with free(). `align` a power of two.
void *hot_alloc(size_t size, size_t align);

} // namespace hc::hass::detail
