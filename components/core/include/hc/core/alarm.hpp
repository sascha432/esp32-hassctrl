#pragma once

// When the alarm rings (Settings, Alarm; docs/ui/lvgl/lvgl-settings.md section 3). Plain C++ in local time:
// the UI asks every few seconds with the clock of the device.

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace hc::core {

// The days an alarm rings on: bit n is weekday n of std::tm (0 = Sunday). None: once, at the
// next time the clock shows `hour`:`minute`.
constexpr uint8_t kEveryDay = 0x7f;
constexpr uint8_t kWeekdays = 0x3e;
constexpr uint8_t kWeekend = 0x41;

struct AlarmTime {
    uint8_t hour;
    uint8_t minute;
    uint8_t days;
};

// the alarm rings in the minute of `local`
bool alarm_due(const AlarmTime &alarm, const std::tm &local);

// minutes from the minute of `local` until the alarm rings next, 0 when it rings in this minute
int32_t minutes_until(const AlarmTime &alarm, const std::tm &local);

// "Every day", "Mon-Fri", "Weekend", "Once" or the days ("Mon, Wed, Fri"), in `out`
void days_text(uint8_t days, char *out, size_t size);

// the short name of weekday `day` (0 = Sunday)
const char *day_name(uint8_t day);

} // namespace hc::core
