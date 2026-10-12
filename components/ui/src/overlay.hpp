#pragma once

// What lies over the dashboard on lv_layer_top (docs/ui/lvgl/lvgl-settings.md sections 1 and 2):
//
//   * the pull-down zone, 160 x 40 px at the top center: dragging down from it opens
//     Settings;
//   * the alert strip, top center with rounded bottom corners: Wi-Fi problems, connecting, and the
//     one-time hint "Swipe down from the top for Settings". Tapping it opens Settings at the
//     matching page; the hint stays until the onboarding is complete (Wi-Fi up, Home Assistant
//     connected) and Settings was opened by the swipe.
//
// Both are hidden while Settings is open and while the dashboard is edited.

#include "hc/ui/backend.hpp"
#include "lvgl.h"

namespace hc::ui::overlay {

// the height of the pull-down zone: what lies below it at the top center takes taps
constexpr lv_coord_t kZoneHeight = 40;

void init(const Backend &backend);

// hides or shows the zone and the strip at once (the edit mode of the dashboard starts or ends),
// not only on the next check
void update();

} // namespace hc::ui::overlay
