#include "hc/formats/json_writer.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace hc::formats {

JsonWriter::JsonWriter(char *buffer, size_t size) : _buffer(buffer), _size(size)
{
    if (_size > 0) {
        _buffer[0] = 0;
    }
}

void JsonWriter::put(char c)
{
    _needed++;
    if (_length + 1 < _size) {
        _buffer[_length++] = c;
        _buffer[_length] = 0;
    } else {
        _overflow = true;
    }
}

void JsonWriter::raw(const char *text)
{
    while (*text != 0) {
        put(*text++);
    }
}

void JsonWriter::quoted(const char *text, size_t length)
{
    put('"');
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
        case '"':
            raw("\\\"");
            break;
        case '\\':
            raw("\\\\");
            break;
        case '\n':
            raw("\\n");
            break;
        case '\r':
            raw("\\r");
            break;
        case '\t':
            raw("\\t");
            break;
        default:
            if (c < 0x20) {
                char escape[8];
                std::snprintf(escape, sizeof(escape), "\\u%04x", c);
                raw(escape);
            } else {
                put(static_cast<char>(c));
            }
        }
    }
    put('"');
}

void JsonWriter::key(const char *name)
{
    if (_needs_comma) {
        put(',');
    }
    if (name != nullptr) {
        quoted(name, std::strlen(name));
        put(':');
    }
    _needs_comma = true;
}

JsonWriter &JsonWriter::begin_object(const char *name)
{
    key(name);
    put('{');
    _needs_comma = false;
    return *this;
}

JsonWriter &JsonWriter::end_object()
{
    put('}');
    _needs_comma = true;
    return *this;
}

JsonWriter &JsonWriter::begin_array(const char *name)
{
    key(name);
    put('[');
    _needs_comma = false;
    return *this;
}

JsonWriter &JsonWriter::end_array()
{
    put(']');
    _needs_comma = true;
    return *this;
}

JsonWriter &JsonWriter::string(const char *name, const char *value)
{
    return string(name, value != nullptr ? value : "", value != nullptr ? std::strlen(value) : 0);
}

JsonWriter &JsonWriter::string(const char *name, const char *value, size_t length)
{
    key(name);
    quoted(value, length);
    return *this;
}

JsonWriter &JsonWriter::number(const char *name, int64_t value)
{
    key(name);
    char text[24];
    std::snprintf(text, sizeof(text), "%" PRId64, value);
    raw(text);
    return *this;
}

JsonWriter &JsonWriter::decimal(const char *name, double value)
{
    key(name);
    char text[32];
    std::snprintf(text, sizeof(text), "%.6g", value);
    raw(text);
    return *this;
}

JsonWriter &JsonWriter::boolean(const char *name, bool value)
{
    key(name);
    raw(value ? "true" : "false");
    return *this;
}

JsonWriter &JsonWriter::null(const char *name)
{
    key(name);
    raw("null");
    return *this;
}

bool valid_utf8(const char *text, size_t length)
{
    const auto *p = reinterpret_cast<const unsigned char *>(text);
    for (size_t i = 0; i < length;) {
        const unsigned char c = p[i];
        size_t more;
        uint32_t code;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xe0) == 0xc0) {
            more = 1;
            code = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            more = 2;
            code = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            more = 3;
            code = c & 0x07;
        } else {
            return false;
        }
        if (length - i <= more) {
            return false;
        }
        for (size_t k = 1; k <= more; k++) {
            if ((p[i + k] & 0xc0) != 0x80) {
                return false;
            }
            code = (code << 6) | (p[i + k] & 0x3f);
        }
        // the shortest form only, no surrogates, nothing above U+10FFFF
        constexpr uint32_t kSmallest[] = {0, 0x80, 0x800, 0x10000};
        if (code < kSmallest[more] || (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff) {
            return false;
        }
        i += more + 1;
    }
    return true;
}

} // namespace hc::formats
