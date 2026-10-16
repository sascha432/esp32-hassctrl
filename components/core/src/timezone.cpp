#include "hc/core/timezone.hpp"

#include <algorithm>
#include <iterator>

namespace hc::core {

namespace {

struct Zone {
    std::string_view name;
    unsigned char rule;
};

#include "timezones.inc"

constexpr int64_t kSecondsPerDay = 86400;

int64_t floor_div(int64_t a, int64_t b)
{
    const int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

int64_t floor_mod(int64_t a, int64_t b)
{
    return a - floor_div(a, b) * b;
}

bool leap_year(int64_t year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

unsigned days_in_month(int64_t year, unsigned month)
{
    constexpr unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap_year(year) ? 29 : kDays[month - 1];
}

// Days since 1970-01-01 of a civil date, and back (Howard Hinnant's date algorithms).
int64_t days_from_civil(int64_t year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int64_t era = floor_div(year, 400);
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civil_from_days(int64_t days, int64_t &year, unsigned &month, unsigned &day)
{
    days += 719468;
    const int64_t era = floor_div(days, 146097);
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = static_cast<int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
}

// 0 = Sunday; 1970-01-01 was a Thursday
unsigned weekday_of(int64_t days)
{
    return static_cast<unsigned>(floor_mod(days + 4, 7));
}

// the day (since 1970) a transition falls on in `year`
int64_t transition_day(int64_t year, const TzTransition &t)
{
    switch (t.kind) {
    case TzTransition::Kind::MonthWeekDay: {
        const int64_t first = days_from_civil(year, t.month, 1);
        unsigned day = 1 + (t.weekday + 7 - weekday_of(first)) % 7 + (t.week - 1u) * 7;
        const unsigned last = days_in_month(year, t.month);
        while (day > last) {
            day -= 7; // week 5: the last such weekday of the month
        }
        return first + day - 1;
    }
    case TzTransition::Kind::Julian1: {
        unsigned yday = t.day - 1u;
        if (leap_year(year) && t.day >= 60) {
            ++yday; // March 1 stays day 60
        }
        return days_from_civil(year, 1, 1) + yday;
    }
    case TzTransition::Kind::Julian0:
        return days_from_civil(year, 1, 1) + t.day;
    }
    return 0;
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// One pass over the POSIX string: name, offset, [name, [offset], ",start[/time],end[/time]"].
class Parser {
public:
    explicit Parser(std::string_view text) : _text(text) {}

    bool done() const
    {
        return _pos >= _text.size();
    }

    char peek() const
    {
        return done() ? '\0' : _text[_pos];
    }

    bool take(char c)
    {
        if (peek() != c) {
            return false;
        }
        ++_pos;
        return true;
    }

    // "CET" (three letters or more) or "<+0530>" (anything but '>' in angle brackets)
    bool name()
    {
        const size_t start = _pos;
        if (take('<')) {
            while (!done() && peek() != '>') {
                ++_pos;
            }
            return _pos > start + 1 && take('>');
        }
        while (is_letter(peek())) {
            ++_pos;
        }
        return _pos - start >= 3;
    }

    // [+-]h[h[h]][:m[m][:s[s]]] in seconds, hours at most `max_hours`
    bool time(int32_t &out, long max_hours)
    {
        int sign = 1;
        if (take('-')) {
            sign = -1;
        } else {
            take('+');
        }
        long hours = 0;
        long minutes = 0;
        long seconds = 0;
        if (!number(hours, 3) || hours > max_hours) {
            return false;
        }
        if (take(':')) {
            if (!number(minutes, 2) || minutes > 59) {
                return false;
            }
            if (take(':') && (!number(seconds, 2) || seconds > 59)) {
                return false;
            }
        }
        out = static_cast<int32_t>(sign * (hours * 3600 + minutes * 60 + seconds));
        return true;
    }

    // Jn, n or Mm.w.d, with /time
    bool transition(TzTransition &out)
    {
        long n = 0;
        if (take('J')) {
            if (!number(n, 3) || n < 1 || n > 365) {
                return false;
            }
            out.kind = TzTransition::Kind::Julian1;
            out.day = static_cast<uint16_t>(n);
        } else if (take('M')) {
            long month = 0;
            long week = 0;
            long weekday = 0;
            if (!number(month, 2) || month < 1 || month > 12 || !take('.') || !number(week, 1) || week < 1 || week > 5 ||
                !take('.') || !number(weekday, 1) || weekday > 6) {
                return false;
            }
            out.kind = TzTransition::Kind::MonthWeekDay;
            out.month = static_cast<uint8_t>(month);
            out.week = static_cast<uint8_t>(week);
            out.weekday = static_cast<uint8_t>(weekday);
        } else {
            if (!number(n, 3) || n > 365) {
                return false;
            }
            out.kind = TzTransition::Kind::Julian0;
            out.day = static_cast<uint16_t>(n);
        }
        out.seconds = 7200;
        // POSIX allows -167 to 167 hours
        return !take('/') || time(out.seconds, 167);
    }

private:
    bool number(long &out, int max_digits)
    {
        if (!is_digit(peek())) {
            return false;
        }
        out = 0;
        for (int digits = 0; is_digit(peek()); ++_pos) {
            if (++digits > max_digits) {
                return false;
            }
            out = out * 10 + (peek() - '0');
        }
        return true;
    }

    std::string_view _text;
    size_t _pos = 0;
};

} // namespace

const char *time_zone_rule(std::string_view iana_name)
{
    const auto *end = std::end(kZones);
    const auto *found = std::lower_bound(std::begin(kZones), end, iana_name,
                                         [](const Zone &zone, std::string_view key) { return zone.name < key; });
    return found != end && found->name == iana_name ? kRules[found->rule] : nullptr;
}

size_t time_zone_count()
{
    return std::size(kZones);
}

std::string_view time_zone_name(size_t index)
{
    return index < std::size(kZones) ? kZones[index].name : std::string_view();
}

bool parse_tz_rule(std::string_view text, TzRule &out)
{
    Parser parser(text);
    TzRule rule;
    int32_t offset = 0;
    if (!parser.name() || !parser.time(offset, 24)) {
        return false;
    }
    rule.std_offset = -offset; // POSIX counts west of UTC
    if (parser.done()) {
        out = rule;
        return true;
    }
    if (!parser.name()) {
        return false;
    }
    rule.has_dst = true;
    rule.dst_offset = rule.std_offset + 3600;
    if (parser.peek() != ',') {
        if (!parser.time(offset, 24)) {
            return false;
        }
        rule.dst_offset = -offset;
    }
    if (!parser.take(',') || !parser.transition(rule.start) || !parser.take(',') || !parser.transition(rule.end) ||
        !parser.done()) {
        return false;
    }
    out = rule;
    return true;
}

void local_time(int64_t epoch, const TzRule &rule, std::tm &out)
{
    bool dst = false;
    if (rule.has_dst) {
        // the transitions of the year the standard time is in; a transition within hours of New
        // Year would be looked up in the wrong year (no zone has one)
        int64_t year = 0;
        unsigned month = 0;
        unsigned day = 0;
        civil_from_days(floor_div(epoch + rule.std_offset, kSecondsPerDay), year, month, day);
        // `start` is given in standard time, `end` in daylight saving time
        const int64_t start = transition_day(year, rule.start) * kSecondsPerDay + rule.start.seconds - rule.std_offset;
        const int64_t end = transition_day(year, rule.end) * kSecondsPerDay + rule.end.seconds - rule.dst_offset;
        // south of the equator the saving time runs over New Year: it ends before it starts
        dst = start < end ? epoch >= start && epoch < end : !(epoch >= end && epoch < start);
    }
    const int64_t local = epoch + (dst ? rule.dst_offset : rule.std_offset);
    const int64_t days = floor_div(local, kSecondsPerDay);
    const auto seconds = static_cast<int>(floor_mod(local, kSecondsPerDay));
    int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, year, month, day);
    out.tm_year = static_cast<int>(year - 1900);
    out.tm_mon = static_cast<int>(month - 1);
    out.tm_mday = static_cast<int>(day);
    out.tm_hour = seconds / 3600;
    out.tm_min = seconds / 60 % 60;
    out.tm_sec = seconds % 60;
    out.tm_wday = static_cast<int>(weekday_of(days));
    out.tm_yday = static_cast<int>(days - days_from_civil(year, 1, 1));
    out.tm_isdst = dst ? 1 : 0;
}

} // namespace hc::core
