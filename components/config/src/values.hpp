#pragma once

// Values shared by the readers and writers of the dashboard configuration.

#include <cstdio>
#include <string_view>

#include "hc/core/layout.hpp"

namespace hc::config::detail {

// "4x3" -> {4, 3}; false when it is not columns x rows of 1 to 9
inline bool parse_span(std::string_view text, core::Span &out)
{
    if (text.size() != 3 || text[1] != 'x' || text[0] < '1' || text[0] > '9' || text[2] < '1' || text[2] > '9') {
        return false;
    }
    out = {static_cast<uint8_t>(text[0] - '0'), static_cast<uint8_t>(text[2] - '0')};
    return true;
}

inline void format_span(core::Span span, char (&out)[8])
{
    std::snprintf(out, sizeof(out), "%ux%u", static_cast<unsigned>(span.cols), static_cast<unsigned>(span.rows));
}

// "2,1" -> column 2, row 1; false when it is not column,row of 0 to 7
inline bool parse_cell(std::string_view text, core::Cell &out)
{
    if (text.size() != 3 || text[1] != ',' || text[0] < '0' || text[0] > '7' || text[2] < '0' || text[2] > '7') {
        return false;
    }
    out = {static_cast<uint8_t>(text[0] - '0'), static_cast<uint8_t>(text[2] - '0')};
    return true;
}

inline void format_cell(core::Cell cell, char (&out)[8])
{
    std::snprintf(out, sizeof(out), "%u,%u", static_cast<unsigned>(cell.col), static_cast<unsigned>(cell.row));
}

// a whole number without sign, at most `max`; false otherwise
inline bool parse_whole(std::string_view text, long max, long &out)
{
    if (text.empty() || text.size() > 9) {
        return false;
    }
    long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value > max) {
        return false;
    }
    out = value;
    return true;
}

// "0.5", "10", "1.25": digits with at most one dot, no sign, no exponent
inline bool parse_decimal(std::string_view text, float &out)
{
    if (text.empty() || text.size() > 12) {
        return false;
    }
    double value = 0;
    double scale = 0;
    for (const char c : text) {
        if (c == '.' && scale == 0) {
            scale = 1;
        } else if (c >= '0' && c <= '9') {
            if (scale == 0) {
                value = value * 10 + (c - '0');
            } else {
                scale /= 10;
                value += (c - '0') * scale;
            }
        } else {
            return false;
        }
    }
    out = static_cast<float>(value);
    return true;
}

// %g without trailing zeros: 0.5, 10, 0.1
inline void format_decimal(float value, char (&out)[24])
{
    std::snprintf(out, sizeof(out), "%g", static_cast<double>(value));
}

} // namespace hc::config::detail
