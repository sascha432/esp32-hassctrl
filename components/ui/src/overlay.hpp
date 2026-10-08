#pragma once

// What lies over the dashboard on lv_layer_top (docs/ui/screen.md sections 2 and 3):
//
//   * the pull-down zone, 160 x 40 px at the top center: dragging down from it opens
//     Settings;
//   * the alert strip, top center with rounded bottom corners: Wi-Fi problems, connecting, and the
//     one-time hint "Swipe down from the top for Settings". Tapping it opens Settings at the
//     matching page; the hint stays until Settings was opened by the swipe.
//
// Both are hidden while Settings is open.

#include "hc/ui/backend.hpp"
#include "lvgl.h"

namespace hc::ui::overlay {

void init(const Backend &backend);

} // namespace hc::ui::overlay
