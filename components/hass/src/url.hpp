#pragma once

// The Home Assistant address as the user types it, and the WebSocket address made from it. Plain
// C++ without ESP-IDF headers (host tests later).

#include <cstddef>

namespace hc::hass::detail {

// Makes "scheme://host[:port]" of what was typed: http:// is added when the scheme is missing,
// ws:// and wss:// become http:// and https://, a path ("/lovelace/0", "/api/websocket") is
// dropped. False with `error` set when it is not an address.
bool normalize_url(const char *typed, char *out, size_t size, const char *&error);

// "http://h:8123" -> "ws://h:8123/api/websocket", "https://..." -> "wss://..."
bool websocket_url(const char *url, char *out, size_t size);

} // namespace hc::hass::detail
