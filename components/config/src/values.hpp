#pragma once

// Values shared by the readers and writers of the dashboard configuration.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "hc/core/layout.hpp"

namespace hc::config::detail {

// "4x3" -> {4, 3}; false when it is not columns x rows of 1 to 32 (a tile's size is checked
// against core::kMaxTileSize by finish())
inline bool parse_pair(std::string_view text, char separator, long low, long high, long &first, long &second)
{
    const size_t at = text.find(separator);
    if (at == std::string_view::npos || at == 0 || at + 1 == text.size() || at > 2 || text.size() - at - 1 > 2) {
        return false;
    }
    for (const char c : text) {
        if (c != separator && (c < '0' || c > '9')) {
            return false;
        }
    }
    first = std::strtol(std::string(text.substr(0, at)).c_str(), nullptr, 10);
    second = std::strtol(std::string(text.substr(at + 1)).c_str(), nullptr, 10);
    return first >= low && first <= high && second >= low && second <= high;
}

inline bool parse_span(std::string_view text, core::Span &out)
{
    long cols = 0;
    long rows = 0;
    if (!parse_pair(text, 'x', 1, core::kMaxGrid, cols, rows)) {
        return false;
    }
    out = {static_cast<uint8_t>(cols), static_cast<uint8_t>(rows)};
    return true;
}

inline void format_span(core::Span span, char (&out)[8])
{
    std::snprintf(out, sizeof(out), "%ux%u", static_cast<unsigned>(span.cols), static_cast<unsigned>(span.rows));
}

// "2,1" -> column 2, row 1; false when it is not column,row of 0 to 31
inline bool parse_cell(std::string_view text, core::Cell &out)
{
    long col = 0;
    long row = 0;
    if (!parse_pair(text, ',', 0, core::kMaxGrid - 1, col, row)) {
        return false;
    }
    out = {static_cast<uint8_t>(col), static_cast<uint8_t>(row)};
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
    float value = 0;
    float scale = 0;
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
    out = value;
    return true;
}

// %g without trailing zeros: 0.5, 10, 0.1
inline void format_decimal(float value, char (&out)[24])
{
    std::snprintf(out, sizeof(out), "%g", static_cast<double>(value));
}

} // namespace hc::config::detail
