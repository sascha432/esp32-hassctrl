#pragma once

namespace hc::platform {

// "hassctrl-" and the last two bytes of the Wi-Fi MAC address in hex, for example
// "hassctrl-1a2b": the hotspot name and the host name of the device.
const char *device_name();

} // namespace hc::platform
