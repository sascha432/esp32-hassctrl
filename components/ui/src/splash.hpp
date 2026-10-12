#pragma once

// The start screen (docs/ui/lvgl/lvgl-dashboard.md section 1.2): the Home Assistant logo, 160 x 160 at the
// center, with the runner going round the house, over the dashboard until Home Assistant is
// connected and the values of the first page are in. The dashboard is built under it, so it is
// complete when the logo goes. Settings (the pull-down) and the alert strip lie above it on the
// top layer and work as usual; the first setup is a screen of its own.
//
// Without a dashboard there are no values to wait for: once Home Assistant is connected the runner
// stops and a line below the logo says where the web UI is, until a dashboard comes.

#include "hc/ui/backend.hpp"
#include "lvgl.h"

namespace hc::ui::splash {

// Shows the logo on `screen` (the dashboard's) at once.
void init(const Backend &backend, lv_obj_t *screen);

} // namespace hc::ui::splash
