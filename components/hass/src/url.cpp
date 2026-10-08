#include "url.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hc::hass::detail {

namespace {

bool host_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == ':' || c == '[' ||
           c == ']';
}

// strncasecmp() is POSIX and missing in MSVC, which builds the host tests
bool equal_nocase(const char *a, const char *b, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
        if (a[i] == '\0') {
            return true;
        }
    }
    return true;
}

} // namespace

bool normalize_url(const char *typed, char *out, size_t size, const char *&error)
{
    while (std::isspace(static_cast<unsigned char>(*typed))) {
        typed++;
    }
    size_t length = std::strlen(typed);
    while (length > 0 && std::isspace(static_cast<unsigned char>(typed[length - 1]))) {
        length--;
    }
    if (length == 0) {
        error = "the address is empty";
        return false;
    }

    const char *scheme = "http";
    const char *rest = typed;
    if (const char *separator = std::strstr(typed, "://"); separator != nullptr && separator < typed + length) {
        const size_t scheme_length = static_cast<size_t>(separator - typed);
        const auto is = [&](const char *name) {
            return scheme_length == std::strlen(name) && equal_nocase(typed, name, scheme_length);
        };
        if (is("http") || is("ws")) {
            scheme = "http";
        } else if (is("https") || is("wss")) {
            scheme = "https";
        } else {
            error = "the address must start with http:// or https://";
            return false;
        }
        rest = separator + 3;
    }

    // host and port end at the path, the query or the fragment
    const char *end = typed + length;
    const char *host_end = rest;
    while (host_end < end && *host_end != '/' && *host_end != '?' && *host_end != '#') {
        if (!host_char(*host_end)) {
            error = "the address contains a character a host name cannot have";
            return false;
        }
        host_end++;
    }
    if (host_end == rest || *rest == ':') {
        error = "the address has no host name";
        return false;
    }
    // a port after the last colon, unless it is the end of an IPv6 address in brackets
    const char *colon = nullptr;
    for (const char *p = rest; p < host_end; p++) {
        if (*p == ':') {
            colon = p;
        } else if (*p == ']') {
            colon = nullptr;
        }
    }
    if (colon != nullptr) {
        const char *digits = colon + 1;
        if (digits == host_end || host_end - digits > 5) {
            error = "the port is not a number from 1 to 65535";
            return false;
        }
        long port = 0;
        for (const char *p = digits; p < host_end; p++) {
            if (!std::isdigit(static_cast<unsigned char>(*p))) {
                error = "the port is not a number from 1 to 65535";
                return false;
            }
            port = port * 10 + (*p - '0');
        }
        if (port < 1 || port > 65535) {
            error = "the port is not a number from 1 to 65535";
            return false;
        }
    }

    const int n = std::snprintf(out, size, "%s://%.*s", scheme, static_cast<int>(host_end - rest), rest);
    if (n < 0 || static_cast<size_t>(n) >= size) {
        error = "the address is too long";
        return false;
    }
    return true;
}

bool websocket_url(const char *url, char *out, size_t size)
{
    const bool tls = equal_nocase(url, "https://", 8);
    const char *rest = std::strstr(url, "://");
    if (rest == nullptr) {
        return false;
    }
    const int n = std::snprintf(out, size, "%s://%s/api/websocket", tls ? "wss" : "ws", rest + 3);
    return n > 0 && static_cast<size_t>(n) < size;
}

} // namespace hc::hass::detail
