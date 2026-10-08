#pragma once

// What the parts of hass share: the connection's settings and the events for the UI (hass.cpp).

#include <cstddef>

#include "hc/core/entity.hpp"

namespace hc::hass::detail {

// the address ("http://h:8123") and the token of Home Assistant; empty while not set up
void credentials(char *url, size_t url_size, char *token, size_t token_size);

// hands an event to the UI (take_event)
void deliver_event(core::HassEvent event);

// the UI took a frame of the stream that plays (media.cpp decodes the next one)
void frame_taken();

} // namespace hc::hass::detail
