#pragma once

// The first setup (docs/ui/screen.md section 4): shown instead of the dashboard while no Wi-Fi
// network is saved, on the first boot and after a factory reset.
//
// Both paths run at the same time. On the screen, "Choose Wi-Fi" opens the network and password
// pages of Settings, which come back here. With a phone, the QR code joins the hotspot and the
// captive portal opens the web setup. As soon as a network is saved, from either side, the screen
// follows the connection and shows the result: the address on success, the reason and "Try
// again" / "Other network" on a failure. "Next" leads to the dashboard.

#include "hc/ui/backend.hpp"
#include "lvgl.h"

namespace hc::ui::setup {

// Watches the network: once it has loaded its networks and none is saved, the setup opens.
void init(const Backend &backend, lv_obj_t *dashboard);

// From any task (the console): the setup opens within half a second, also with networks saved.
void request();

bool active();

} // namespace hc::ui::setup
