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

} // namespace hc::formats
