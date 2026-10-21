#pragma once

// The alarm clock (docs/ui/lvgl/lvgl-settings.md section 3, Alarm): when the time of an alarm of
// Settings, Alarm comes, the screen wakes and the alarm screen lies over everything (lv_layer_top)
// with the time, Snooze and Stop. The sound rings at full volume until one of them, or for 10
// minutes. Snooze rings the same alarm again after its snooze time (one snooze at a time: the
// next alarm that rings ends it); an alarm without days rings once and turns itself off. Two
// alarms in the same minute ring once, with the settings of the first in the list.
// Only with Backend::alarms (CONFIG_HC_AUDIO_ALARM) and a clock that is set.

#include <cstddef>
#include <cstdint>
#include <ctime>

#include "hc/ui/backend.hpp"

namespace hc::ui::alarm {

void init(const Backend &backend);

// the backend has an alarm: Settings show the Alarm page
bool available();

// the settings changed (Alarm page): a snooze ends
void settings_changed();

// rings the first alarm at the next check (the beeps without one), from any task
// (hc::ui::ring_alarm())
void request_ring();

// the alarm screen is shown
bool ringing();

// the index of the alarm that is on and rings next after `local`, with the minutes until then;
// -1 when none is on
int next(const Alarms &alarms, const std::tm &local, int32_t &minutes);

// the next alarm "07:30, Mon-Fri", "Off" (none is on), "None" or "Snoozed until 07:40" for the
// row of quick settings
void summary(char *out, size_t size);

} // namespace hc::ui::alarm
