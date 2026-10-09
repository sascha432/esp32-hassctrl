#include "hc/core/alarm.hpp"

#include <cstdio>
#include <cstring>

namespace hc::core {

namespace {

constexpr int32_t kMinutesPerDay = 24 * 60;
constexpr const char *kDayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

bool rings_on(const AlarmTime &alarm, int weekday)
{
    return alarm.days == 0 || (alarm.days & (1u << weekday)) != 0;
}

} // namespace

bool alarm_due(const AlarmTime &alarm, const std::tm &local)
{
    return local.tm_hour == alarm.hour && local.tm_min == alarm.minute && rings_on(alarm, local.tm_wday);
}

int32_t minutes_until(const AlarmTime &alarm, const std::tm &local)
{
    const int32_t now = local.tm_hour * 60 + local.tm_min;
    const int32_t at = alarm.hour * 60 + alarm.minute;
    // today (if not past) and the next seven days: one of them is a day the alarm rings on
    for (int day = 0; day <= 7; day++) {
        const int32_t minutes = day * kMinutesPerDay + at - now;
        if (minutes >= 0 && rings_on(alarm, (local.tm_wday + day) % 7)) {
            return minutes;
        }
    }
    return -1;
}

void days_text(uint8_t days, char *out, size_t size)
{
    if (size == 0) {
        return;
    }
    days &= kEveryDay;
    const char *fixed = days == 0 ? "Once"
                        : days == kEveryDay ? "Every day"
                        : days == kWeekdays ? "Mon-Fri"
                        : days == kWeekend  ? "Weekend"
                                            : nullptr;
    if (fixed != nullptr) {
        std::snprintf(out, size, "%s", fixed);
        return;
    }
    // Monday first, as the days are shown on the page
    out[0] = 0;
    for (int i = 1; i <= 7; i++) {
        const int day = i % 7;
        if ((days & (1u << day)) != 0) {
            const size_t used = std::strlen(out);
            std::snprintf(out + used, size - used, "%s%s", used > 0 ? ", " : "", kDayNames[day]);
        }
    }
}

const char *day_name(uint8_t day)
{
    return kDayNames[day % 7];
}

} // namespace hc::core
