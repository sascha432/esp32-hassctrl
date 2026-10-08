// components/hass/src/url.cpp: the Home Assistant address as typed, and its WebSocket address.

#include "check.hpp"
#include "url.hpp"

using hc::hass::detail::normalize_url;
using hc::hass::detail::websocket_url;

namespace {

// the normalized address, or "error: <reason>"
std::string normalized(const char *typed, size_t size = 128)
{
    char out[256] = {};
    const char *error = nullptr;
    if (!normalize_url(typed, out, size, error)) {
        return std::string("error: ") + error;
    }
    return out;
}

} // namespace

TEST(url_adds_http_when_the_scheme_is_missing)
{
    CHECK_STR(normalized("homeassistant.local:8123").c_str(), "http://homeassistant.local:8123");
    CHECK_STR(normalized("192.168.0.4").c_str(), "http://192.168.0.4");
}

TEST(url_turns_websocket_schemes_into_http)
{
    CHECK_STR(normalized("ws://h:8123/api/websocket").c_str(), "http://h:8123");
    CHECK_STR(normalized("wss://h").c_str(), "https://h");
    CHECK_STR(normalized("HTTPS://Host:443/lovelace/0").c_str(), "https://Host:443");
}

TEST(url_drops_path_query_fragment_and_spaces)
{
    CHECK_STR(normalized("  http://h:8123/lovelace/0  ").c_str(), "http://h:8123");
    CHECK_STR(normalized("http://h?x=1").c_str(), "http://h");
    CHECK_STR(normalized("http://h#top").c_str(), "http://h");
}

TEST(url_keeps_an_ipv6_address_with_its_port)
{
    CHECK_STR(normalized("http://[fe80::1]:8123").c_str(), "http://[fe80::1]:8123");
    CHECK_STR(normalized("http://[fe80::1]").c_str(), "http://[fe80::1]");
}

TEST(url_rejects_what_is_not_an_address)
{
    CHECK_STR(normalized("").c_str(), "error: the address is empty");
    CHECK_STR(normalized("   ").c_str(), "error: the address is empty");
    CHECK_STR(normalized("ftp://h").c_str(), "error: the address must start with http:// or https://");
    CHECK_STR(normalized("http://").c_str(), "error: the address has no host name");
    CHECK_STR(normalized("http://:8123").c_str(), "error: the address has no host name");
    CHECK_STR(normalized("http://ho st").c_str(), "error: the address contains a character a host name cannot have");
}

TEST(url_checks_the_port)
{
    CHECK_STR(normalized("http://h:99999").c_str(), "error: the port is not a number from 1 to 65535");
    CHECK_STR(normalized("http://h:0").c_str(), "error: the port is not a number from 1 to 65535");
    CHECK_STR(normalized("http://h:").c_str(), "error: the port is not a number from 1 to 65535");
    CHECK_STR(normalized("http://h:80a").c_str(), "error: the port is not a number from 1 to 65535");
    CHECK_STR(normalized("http://h:65535").c_str(), "http://h:65535");
}

TEST(url_reports_an_address_too_long_for_the_buffer)
{
    CHECK_STR(normalized("http://homeassistant.local", 16).c_str(), "error: the address is too long");
}

TEST(websocket_url_follows_the_scheme)
{
    char out[128];
    REQUIRE(websocket_url("http://h:8123", out, sizeof(out)));
    CHECK_STR(out, "ws://h:8123/api/websocket");
    REQUIRE(websocket_url("HTTPS://h", out, sizeof(out)));
    CHECK_STR(out, "wss://h/api/websocket");
    CHECK(!websocket_url("no scheme", out, sizeof(out)));
    CHECK(!websocket_url("http://h", out, 10));
}
