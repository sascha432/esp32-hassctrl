#pragma once

// The alarm clock (docs/ui/screen.md section 5, Alarm): when the time of Settings, Alarm comes,
// the screen wakes and the alarm screen lies over everything (lv_layer_top) with the time,
// Snooze and Stop. The sound rings at full volume until one of them, or for 10 minutes. Snooze
// rings again after the snooze time; an alarm without days rings once and turns itself off.
// Only with Backend::alarm_settings (CONFIG_HC_AUDIO_ALARM) and a clock that is set.

#include <cstddef>

#include "hc/ui/backend.hpp"

namespace hc::ui::alarm {

void init(const Backend &backend);

// the backend has an alarm: Settings show the Alarm page
bool available();

// the settings changed (Alarm page): a snooze ends
void settings_changed();

// rings at the next check, from any task (hc::ui::ring_alarm())
void request_ring();

// the alarm screen is shown
bool ringing();

// "07:30, Mon-Fri", "Off" or "Snoozed until 07:40" for the row of quick settings
void summary(char *out, size_t size);

} // namespace hc::ui::alarm
