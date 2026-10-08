#pragma once

// A small JSON writer into a fixed buffer: the API answers of the web server and the stored
// dashboard (docs/architecture.md section 5, "Dashboard configuration"). Plain C++, host tested.
//
// Output that does not fit is cut off and reported by ok(). needed() counts the whole output in
// any case, so a caller that does not know the size writes twice: once into an empty buffer to
// measure, then into a buffer of needed() + 1 bytes.

#include <cstddef>
#include <cstdint>

namespace hc::formats {

class JsonWriter {
public:
    JsonWriter(char *buffer, size_t size);

    JsonWriter &begin_object(const char *key = nullptr);
    JsonWriter &end_object();
    JsonWriter &begin_array(const char *key = nullptr);
    JsonWriter &end_array();
    JsonWriter &string(const char *key, const char *value);
    JsonWriter &string(const char *key, const char *value, size_t length);
    JsonWriter &number(const char *key, int64_t value);
    // a number with a fraction, shortest form ("0.5", "21", "0.1")
    JsonWriter &decimal(const char *key, double value);
    JsonWriter &boolean(const char *key, bool value);

    const char *c_str() const
    {
        return _buffer;
    }
    size_t length() const
    {
        return _length;
    }
    // the length of the whole output, also when it did not fit
    size_t needed() const
    {
        return _needed;
    }
    bool ok() const
    {
        return !_overflow;
    }

private:
    void key(const char *name);
    void raw(const char *text);
    void put(char c);
    void quoted(const char *text, size_t length);

    char *_buffer;
    size_t _size;
    size_t _length = 0;
    size_t _needed = 0;
    bool _overflow = false;
    // a value was written at this nesting level, the next one needs a comma
    bool _needs_comma = false;
};

} // namespace hc::formats
