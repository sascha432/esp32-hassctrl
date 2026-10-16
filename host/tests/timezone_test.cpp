// components/core/src/timezone.cpp: the zone table and the local time under a POSIX rule. The
// expected times were computed with Python's zoneinfo for the rule the table holds (the rule is
// checked next to the time, so a new tzdata release shows up here as a changed rule, not as a
// wrong time).

#include "check.hpp"
#include "hc/core/timezone.hpp"

#include <cstdio>

using namespace hc::core;

namespace {

// the local time of `epoch` in the zone (an IANA name) or under the rule (a POSIX string)
std::string local(const char *zone_or_rule, int64_t epoch)
{
    const char *rule = time_zone_rule(zone_or_rule);
    TzRule parsed;
    if (!parse_tz_rule(rule != nullptr ? rule : zone_or_rule, parsed)) {
        return "no rule";
    }
    std::tm t{};
    local_time(epoch, parsed, t);
    char text[64];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d wday=%d yday=%d dst=%d", t.tm_year + 1900,
                  t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday, t.tm_yday, t.tm_isdst);
    return text;
}

std::string rule_of(const char *zone)
{
    const char *rule = time_zone_rule(zone);
    return rule != nullptr ? rule : "(unknown)";
}

} // namespace

TEST(timezone_table_knows_the_names)
{
    CHECK_STR(rule_of("Europe/Berlin"), "CET-1CEST,M3.5.0,M10.5.0/3");
    CHECK_STR(rule_of("UTC"), "UTC0");
    CHECK(time_zone_rule("Mars/Olympus") == nullptr);
    CHECK(time_zone_rule("europe/berlin") == nullptr);
    CHECK(time_zone_rule("") == nullptr);
    CHECK(time_zone_count() == 598);
    CHECK(time_zone_name(0) == "Africa/Abidjan");
    CHECK(time_zone_name(time_zone_count() - 1) == "Zulu");
    CHECK(time_zone_name(time_zone_count()).empty());
    // sorted (the lookup is a binary search), no name longer than the limit, every rule parses
    for (size_t i = 0; i < time_zone_count(); ++i) {
        const std::string_view name = time_zone_name(i);
        if (i > 0) {
            CHECK(time_zone_name(i - 1) < name);
        }
        CHECK(name.size() <= kMaxZoneLength);
        TzRule rule;
        if (!parse_tz_rule(time_zone_rule(name), rule)) {
            hc::test::fail(__FILE__, __LINE__, std::string("rule does not parse: ") + time_zone_rule(name));
        }
    }
}

TEST(timezone_parses_rules)
{
    TzRule rule;
    REQUIRE(parse_tz_rule("CET-1CEST,M3.5.0,M10.5.0/3", rule));
    CHECK(rule.std_offset == 3600);
    CHECK(rule.dst_offset == 7200);
    CHECK(rule.has_dst);
    CHECK(rule.start.kind == TzTransition::Kind::MonthWeekDay);
    CHECK(rule.start.month == 3 && rule.start.week == 5 && rule.start.weekday == 0 && rule.start.seconds == 7200);
    CHECK(rule.end.month == 10 && rule.end.week == 5 && rule.end.weekday == 0 && rule.end.seconds == 10800);

    REQUIRE(parse_tz_rule("IST-5:30", rule));
    CHECK(rule.std_offset == 19800);
    CHECK(!rule.has_dst);

    REQUIRE(parse_tz_rule("<-04>4<-03>,M9.1.6/24,M4.1.6/24", rule));
    CHECK(rule.std_offset == -14400);
    CHECK(rule.dst_offset == -10800);
    CHECK(rule.start.seconds == 86400 && rule.end.seconds == 86400);

    REQUIRE(parse_tz_rule("<-02>2<-01>,M3.5.0/-1,M10.5.0/0", rule));
    CHECK(rule.start.seconds == -3600 && rule.end.seconds == 0);

    REQUIRE(parse_tz_rule("AAA3BBB,J60/2,299/1:30:15", rule));
    CHECK(rule.start.kind == TzTransition::Kind::Julian1 && rule.start.day == 60);
    CHECK(rule.end.kind == TzTransition::Kind::Julian0 && rule.end.day == 299 && rule.end.seconds == 5415);

    // DST with no offset of its own: one hour ahead
    REQUIRE(parse_tz_rule("EST5EDT,M3.2.0,M11.1.0", rule));
    CHECK(rule.std_offset == -18000 && rule.dst_offset == -14400);

    const TzRule before = rule;
    for (const char *bad : {"", "garbage", "CET", "CET-25", "<+00>", "<>0", "CET-1CEST", "CET-1CEST,M3.5.0",
                            "CET-1CEST,M3.5.0,M10.5.0/3x", "EST5EDT,M13.1.0,M11.1.0", "EST5EDT,M3.6.0,M11.1.0",
                            "EST5EDT,M3.2.7,M11.1.0", "AAA3BBB,J0/2,J300/2", "AAA3BBB,J366/2,J300/2",
                            "AAA3BBB,366/2,300/2", "CET-1:60"}) {
        if (parse_tz_rule(bad, rule)) {
            hc::test::fail(__FILE__, __LINE__, std::string("accepted: ") + bad);
        }
    }
    CHECK(rule.std_offset == before.std_offset && rule.dst_offset == before.dst_offset);
}

TEST(timezone_europe)
{
    // Berlin, summer and winter, and the two switches (01:00 UTC)
    CHECK_STR(local("Europe/Berlin", 1782907200), "2026-07-01 14:00:00 wday=3 yday=181 dst=1");
    CHECK_STR(local("Europe/Berlin", 1768478400), "2026-01-15 13:00:00 wday=4 yday=14 dst=0");
    CHECK_STR(local("Europe/Berlin", 1774745999), "2026-03-29 01:59:59 wday=0 yday=87 dst=0");
    CHECK_STR(local("Europe/Berlin", 1774746000), "2026-03-29 03:00:00 wday=0 yday=87 dst=1");
    CHECK_STR(local("Europe/Berlin", 1792889999), "2026-10-25 02:59:59 wday=0 yday=297 dst=1");
    CHECK_STR(local("Europe/Berlin", 1792890000), "2026-10-25 02:00:00 wday=0 yday=297 dst=0");
    // Dublin writes its rule the other way round: standard time is summer time, "DST" is GMT in
    // winter; the times are right, tm_isdst follows the rule
    CHECK_STR(rule_of("Europe/Dublin"), "IST-1GMT0,M10.5.0,M3.5.0/1");
    CHECK_STR(local("Europe/Dublin", 1768478400), "2026-01-15 12:00:00 wday=4 yday=14 dst=1");
    CHECK_STR(local("Europe/Dublin", 1782907200), "2026-07-01 13:00:00 wday=3 yday=181 dst=0");
}

TEST(timezone_america)
{
    CHECK_STR(rule_of("America/New_York"), "EST5EDT,M3.2.0,M11.1.0");
    CHECK_STR(local("America/New_York", 1772953199), "2026-03-08 01:59:59 wday=0 yday=66 dst=0");
    CHECK_STR(local("America/New_York", 1772953200), "2026-03-08 03:00:00 wday=0 yday=66 dst=1");
    CHECK_STR(local("America/New_York", 1793512799), "2026-11-01 01:59:59 wday=0 yday=304 dst=1");
    CHECK_STR(local("America/New_York", 1793512800), "2026-11-01 01:00:00 wday=0 yday=304 dst=0");
    // Nuuk switches at -1 h of local standard time (Saturday evening) and at 0 h
    CHECK_STR(rule_of("America/Nuuk"), "<-02>2<-01>,M3.5.0/-1,M10.5.0/0");
    CHECK_STR(local("America/Nuuk", 1774745999), "2026-03-28 22:59:59 wday=6 yday=86 dst=0");
    CHECK_STR(local("America/Nuuk", 1774746000), "2026-03-29 00:00:00 wday=0 yday=87 dst=1");
}

TEST(timezone_south_of_the_equator)
{
    // Lord Howe: 30 minutes of saving time, over New Year
    CHECK_STR(rule_of("Australia/Lord_Howe"), "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0");
    CHECK_STR(local("Australia/Lord_Howe", 1768435200), "2026-01-15 11:00:00 wday=4 yday=14 dst=1");
    CHECK_STR(local("Australia/Lord_Howe", 1782864000), "2026-07-01 10:30:00 wday=3 yday=181 dst=0");
    // Chatham: 12:45
    CHECK_STR(rule_of("Pacific/Chatham"), "<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45");
    CHECK_STR(local("Pacific/Chatham", 1768435200), "2026-01-15 13:45:00 wday=4 yday=14 dst=1");
    CHECK_STR(local("Pacific/Chatham", 1782864000), "2026-07-01 12:45:00 wday=3 yday=181 dst=0");
    // Santiago: the first Saturday of September at 24:00 (Sunday 00:00 becomes 01:00), the first
    // Saturday of April at 24:00 back; the saving time runs over New Year
    CHECK_STR(rule_of("America/Santiago"), "<-04>4<-03>,M9.1.6/24,M4.1.6/24");
    CHECK_STR(local("America/Santiago", 1788667199), "2026-09-05 23:59:59 wday=6 yday=247 dst=0");
    CHECK_STR(local("America/Santiago", 1788667200), "2026-09-06 01:00:00 wday=0 yday=248 dst=1");
    CHECK_STR(local("America/Santiago", 1798758000), "2026-12-31 20:00:00 wday=4 yday=364 dst=1");
    CHECK_STR(local("America/Santiago", 1798765200), "2026-12-31 22:00:00 wday=4 yday=364 dst=1");
    CHECK_STR(local("America/Santiago", 1806807599), "2027-04-03 23:59:59 wday=6 yday=92 dst=1");
    CHECK_STR(local("America/Santiago", 1806807600), "2027-04-03 23:00:00 wday=6 yday=92 dst=0");
}

TEST(timezone_fixed_offsets)
{
    CHECK_STR(rule_of("Asia/Kolkata"), "IST-5:30");
    CHECK_STR(local("Asia/Kolkata", 1767225600), "2026-01-01 05:30:00 wday=4 yday=0 dst=0");
    CHECK_STR(rule_of("Asia/Kathmandu"), "<+0545>-5:45");
    CHECK_STR(local("Asia/Kathmandu", 1767225600), "2026-01-01 05:45:00 wday=4 yday=0 dst=0");
    CHECK_STR(rule_of("America/Phoenix"), "MST7");
    CHECK_STR(local("America/Phoenix", 1782907200), "2026-07-01 05:00:00 wday=3 yday=181 dst=0");
    CHECK_STR(local("UTC", 1791903845), "2026-10-13 15:04:05 wday=2 yday=285 dst=0");
    CHECK_STR(rule_of("Pacific/Apia"), "<+13>-13");
    CHECK_STR(local("Pacific/Apia", 1782864000), "2026-07-01 13:00:00 wday=3 yday=181 dst=0");
    // before 1970
    CHECK_STR(local("UTC", -1), "1969-12-31 23:59:59 wday=3 yday=364 dst=0");
    CHECK_STR(local("MST7", 0), "1969-12-31 17:00:00 wday=3 yday=364 dst=0");
}

TEST(timezone_julian_days)
{
    // 2028 is a leap year: J60 is March 1 (February 29 not counted), day 59 is February 29
    CHECK_STR(local("AAA3BBB,J60/2,J300/2", 1835499599), "2028-03-01 01:59:59 wday=3 yday=60 dst=0");
    CHECK_STR(local("AAA3BBB,J60/2,J300/2", 1835499600), "2028-03-01 03:00:00 wday=3 yday=60 dst=1");
    CHECK_STR(local("AAA3BBB,59/2,299/2", 1835413199), "2028-02-29 01:59:59 wday=2 yday=59 dst=0");
    CHECK_STR(local("AAA3BBB,59/2,299/2", 1835413200), "2028-02-29 03:00:00 wday=2 yday=59 dst=1");
    CHECK_STR(local("nonsense", 0), "no rule");
}
