// The forecast of a weather entity: the subscription and its events (docs/architecture.md section 6).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass_protocol {

namespace {

// year, month, day -> days since 1970-01-01 (Howard Hinnant's days_from_civil)
int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2 ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool digits(const char *text, int count, int &out)
{
    out = 0;
    for (int i = 0; i < count; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        out = out * 10 + (text[i] - '0');
    }
    return true;
}

float number_of(const cJSON *item)
{
    return cJSON_IsNumber(item) ? static_cast<float>(item->valuedouble) : core::kUnknown;
}

} // namespace

bool parse_iso_time(const char *text, uint32_t &epoch)
{
    // 2026-10-13T20:00:00+00:00, with fractions of a second and Z as well
    int year, month, day, hour, minute, second;
    if (text == nullptr || std::strlen(text) < 19 || !digits(text, 4, year) || text[4] != '-' ||
        !digits(text + 5, 2, month) || text[7] != '-' || !digits(text + 8, 2, day) || (text[10] != 'T' && text[10] != ' ') ||
        !digits(text + 11, 2, hour) || text[13] != ':' || !digits(text + 14, 2, minute) || text[16] != ':' ||
        !digits(text + 17, 2, second) || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    const char *rest = text + 19;
    if (*rest == '.') {
        rest++;
        while (*rest >= '0' && *rest <= '9') {
            rest++;
        }
    }
    int offset = 0; // seconds east of UTC
    if (*rest == '+' || *rest == '-') {
        int oh, om;
        if (!digits(rest + 1, 2, oh) || !digits(rest + (rest[3] == ':' ? 4 : 3), 2, om)) {
            return false;
        }
        offset = (oh * 3600 + om * 60) * (*rest == '-' ? -1 : 1);
    } else if (*rest != 'Z' && *rest != 0) {
        return false;
    }
    const int64_t seconds = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
                            hour * 3600 + minute * 60 + second - offset;
    if (seconds < 0 || seconds > 0xffffffffLL) {
        return false;
    }
    epoch = static_cast<uint32_t>(seconds);
    return true;
}

bool decode_forecast(const cJSON *event, memory::Vector<core::ForecastEntry> &out, memory::String &error)
{
    out.clear();
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(event, "forecast");
    if (cJSON_IsNull(list)) {
        return true; // the integration has no forecast right now
    }
    if (!cJSON_IsArray(list)) {
        error = "the forecast is not a list";
        return false;
    }
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, list)
    {
        if (out.size() >= core::kMaxForecastEntries) {
            break;
        }
        core::ForecastEntry entry;
        if (!parse_iso_time(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "datetime")), entry.time)) {
            error = "a forecast entry without its time";
            return false;
        }
        if (const char *condition = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "condition"))) {
            entry.condition = core::condition_of(condition);
        }
        entry.temperature = number_of(cJSON_GetObjectItemCaseSensitive(item, "temperature"));
        entry.templow = number_of(cJSON_GetObjectItemCaseSensitive(item, "templow"));
        const float probability = number_of(cJSON_GetObjectItemCaseSensitive(item, "precipitation_probability"));
        if (!std::isnan(probability)) {
            entry.precipitation_probability = static_cast<int8_t>(std::clamp(probability, 0.0f, 100.0f));
        }
        out.push_back(entry);
    }
    return true;
}

} // namespace hc::hass_protocol
