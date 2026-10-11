#include <cmath>
#include <cstdio>

#include "cJSON.h"
#include "hc/hass_protocol/protocol.hpp"

namespace hc::hass_protocol {

namespace {

// days since 1970-01-01 -> year, month, day (Howard Hinnant's civil_from_days)
void civil_from_days(int64_t days, int &year, unsigned &month, unsigned &day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

// the entity's list in a result object
const cJSON *entity_list(const cJSON *result, std::string_view entity_id, memory::String &error)
{
    if (!cJSON_IsObject(result)) {
        error = "the history answer is not an object";
        return nullptr;
    }
    const memory::String key(entity_id);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(result, key.c_str());
    if (list != nullptr && !cJSON_IsArray(list)) {
        error = "the history of the entity is not a list";
        return nullptr;
    }
    // no list: nothing was recorded in the window
    return list;
}

// HA's start of a bucket: epoch milliseconds (since 2023.3)
bool epoch_of(const cJSON *item, double divisor, uint32_t &out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return false;
    }
    out = static_cast<uint32_t>(item->valuedouble / divisor);
    return true;
}

// the value of a state in the timeline; a lock that is not locked counts as on (HA's lock
// binary sensor), the words are the lock's own so no domain is needed
float state_value(std::string_view s)
{
    if (s == "on" || s == "unlocked" || s == "unlocking" || s == "open" || s == "opening") {
        return 1.0f;
    }
    if (s == "off" || s == "locked" || s == "locking") {
        return 0.0f;
    }
    return s == "jammed" ? core::kProblem : core::kUnknown;
}

} // namespace

void iso_time(uint32_t epoch, char *out, size_t size)
{
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(epoch / 86400, year, month, day);
    const uint32_t seconds = epoch % 86400;
    std::snprintf(out, size, "%04d-%02u-%02uT%02u:%02u:%02u+00:00", year, month, day,
                  static_cast<unsigned>(seconds / 3600), static_cast<unsigned>(seconds / 60 % 60),
                  static_cast<unsigned>(seconds % 60));
}

memory::String history_message(uint32_t id, core::HistoryKind kind, std::string_view entity_id, uint32_t start,
                               uint32_t end)
{
    char from[32];
    char to[32];
    iso_time(start, from, sizeof(from));
    iso_time(end, to, sizeof(to));
    const auto entity = static_cast<int>(entity_id.size());
    char text[384];
    if (kind == core::HistoryKind::Statistics) {
        std::snprintf(text, sizeof(text),
                      "{\"id\":%lu,\"type\":\"recorder/statistics_during_period\",\"start_time\":\"%s\","
                      "\"end_time\":\"%s\",\"statistic_ids\":[\"%.*s\"],\"period\":\"5minute\",\"types\":[\"mean\"]}",
                      static_cast<unsigned long>(id), from, to, entity, entity_id.data());
    } else {
        // the first state is the one at the start of the window
        std::snprintf(text, sizeof(text),
                      "{\"id\":%lu,\"type\":\"history/history_during_period\",\"start_time\":\"%s\","
                      "\"end_time\":\"%s\",\"entity_ids\":[\"%.*s\"],\"minimal_response\":true,"
                      "\"no_attributes\":true,\"significant_changes_only\":true}",
                      static_cast<unsigned long>(id), from, to, entity, entity_id.data());
    }
    return text;
}

bool decode_history(const cJSON *result, core::HistoryKind kind, std::string_view entity_id,
                    memory::Vector<core::HistoryPoint> &out, memory::String &error)
{
    const cJSON *list = entity_list(result, entity_id, error);
    if (!error.empty()) {
        return false;
    }
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, list)
    {
        core::HistoryPoint point;
        if (kind == core::HistoryKind::Statistics) {
            // {"start":1696600000000.0,"end":...,"mean":21.3}
            if (!epoch_of(cJSON_GetObjectItemCaseSensitive(item, "start"), 1000, point.time)) {
                error = "a statistics bucket without its start";
                return false;
            }
            const cJSON *mean = cJSON_GetObjectItemCaseSensitive(item, "mean");
            point.value = cJSON_IsNumber(mean) ? static_cast<float>(mean->valuedouble) : core::kUnknown;
        } else {
            // {"s":"on","lu":1696600000.123}; "lc" when the state changed before the last update
            const cJSON *changed = cJSON_GetObjectItemCaseSensitive(item, "lc");
            if (!epoch_of(changed != nullptr ? changed : cJSON_GetObjectItemCaseSensitive(item, "lu"), 1, point.time)) {
                error = "a state change without its time";
                return false;
            }
            const char *state = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "s"));
            point.value = state_value(state != nullptr ? state : "");
        }
        out.push_back(point);
    }
    return true;
}

} // namespace hc::hass_protocol
