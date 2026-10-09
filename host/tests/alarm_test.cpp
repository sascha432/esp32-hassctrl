// components/core/src/alarm.cpp: when the alarm of Settings rings.

#include "check.hpp"
#include "hc/core/alarm.hpp"

using hc::core::AlarmTime;

namespace {

// a local time on weekday `wday` (0 = Sunday)
std::tm at(int wday, int hour, int minute)
{
    std::tm t{};
    t.tm_wday = wday;
    t.tm_hour = hour;
    t.tm_min = minute;
    return t;
}

std::string days(uint8_t mask)
{
    char text[40];
    hc::core::days_text(mask, text, sizeof(text));
    return text;
}

} // namespace

TEST(alarm_rings_in_its_minute_on_its_days)
{
    const AlarmTime weekdays{7, 30, hc::core::kWeekdays};
    CHECK(hc::core::alarm_due(weekdays, at(1, 7, 30)));
    CHECK(!hc::core::alarm_due(weekdays, at(1, 7, 31)));
    CHECK(!hc::core::alarm_due(weekdays, at(0, 7, 30))); // Sunday
    CHECK(hc::core::alarm_due(AlarmTime{7, 30, 0}, at(0, 7, 30))); // once: any day
}

TEST(alarm_minutes_until_the_next_ring)
{
    const AlarmTime weekdays{7, 30, hc::core::kWeekdays};
    CHECK(hc::core::minutes_until(weekdays, at(1, 7, 30)) == 0);
    CHECK(hc::core::minutes_until(weekdays, at(1, 6, 30)) == 60);
    // Friday after the alarm: Monday morning
    CHECK(hc::core::minutes_until(weekdays, at(5, 8, 0)) == 2 * 24 * 60 + 23 * 60 + 30);
    // only on Wednesday, asked on Wednesday after it: a week less 10 minutes
    CHECK(hc::core::minutes_until(AlarmTime{6, 0, 1 << 3}, at(3, 6, 10)) == 7 * 24 * 60 - 10);
    // once: tomorrow when today's time has passed
    CHECK(hc::core::minutes_until(AlarmTime{6, 0, 0}, at(3, 23, 0)) == 7 * 60);
}

TEST(alarm_days_in_words)
{
    CHECK_STR(days(0).c_str(), "Once");
    CHECK_STR(days(hc::core::kEveryDay).c_str(), "Every day");
    CHECK_STR(days(hc::core::kWeekdays).c_str(), "Mon-Fri");
    CHECK_STR(days(hc::core::kWeekend).c_str(), "Weekend");
    CHECK_STR(days((1 << 1) | (1 << 3) | (1 << 0)).c_str(), "Mon, Wed, Sun");
}
