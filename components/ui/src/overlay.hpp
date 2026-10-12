#pragma once

// What lies over the dashboard on lv_layer_top (docs/ui/lvgl/lvgl-dashboard.md section 1.1,
// docs/ui/lvgl/lvgl-design.md section 8):
//
//   * the pull-down zone, 160 x 40 px at the top center: dragging down from it opens
//     Settings;
//   * the alert strip, top center with rounded bottom corners, sliding down from the top when it
//     appears and back up when it goes. It shows the state messages (Wi-Fi problems, connecting,
//     a Home Assistant problem, the one-time hint "Swipe down for Settings"; a tap opens Settings
//     at the matching page; the hint stays until the onboarding is complete and Settings was
//     opened by the swipe) and the event messages of show() (a stream stopped, an edit not
//     saved; before the state messages while they last, a tap closes them).
//
// Both are hidden while Settings or the first setup is open. While the dashboard is edited the
// zone is off and the strip shows only event messages.

#include <cstdint>

#include "hc/ui/backend.hpp"
#include "lvgl.h"
#include "theme.hpp"

namespace hc::ui::overlay {

// the height of the pull-down zone: what lies below it at the top center takes taps
constexpr lv_coord_t kZoneHeight = 40;

// how long an event message stays
constexpr uint32_t kEventMs = 5000;

void init(const Backend &backend);

// hides or shows the zone and the strip at once (the edit mode of the dashboard starts or ends),
// not only on the next check
void update();

// Shows `text` in the alert strip for `ms`, before the state messages; a newer one replaces it
// (without a new slide). `ms` 0: until clear(), for something that goes on (a stream that is
// loaded again). Every error and note on the screen that needs no answer goes here (not into a
// popup). The caller writes the symbol: LV_SYMBOL_WARNING "  Stream stopped, retrying".
// `on_tap`: a tap on the message calls it (a failed stream reloads; the text then starts with
// LV_SYMBOL_REFRESH) and the message stays; without one a tap closes the message.
void show(const char *text, uint32_t color = theme::kAlert, uint32_t ms = kEventMs, void (*on_tap)() = nullptr);
// removes the event message (the strip slides up unless a state message follows)
void clear();

// The lowest row the strip covers now, 0 while it is gone; it changes while it slides. Controls at
// the top center stay below max(kZoneHeight, strip_bottom()) (the camera view's buttons).
lv_coord_t strip_bottom();
// `callback` is called whenever strip_bottom() changed (one listener, nullptr removes it)
void on_strip_moved(void (*callback)());

} // namespace hc::ui::overlay
