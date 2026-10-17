// HTTP for the media worker: a GET with the token of Home Assistant or the login of a stream's
// address, the body of a still image, and the parts of an MJPEG stream.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "esp_http_client.h"
#include "internal.hpp"
#include "lwip/sockets.h"
#include "media.hpp"

namespace hc::hass::media {

namespace {

// the longest header line of a part
constexpr size_t kMaxLine = 256;

// The login in a stream's address (http://user:password@host/...): none, the user alone (an
// imported dashboard: the export leaves the password out), or both
enum class UrlLogin : uint8_t { None, UserOnly, Both };

UrlLogin url_login(std::string_view url)
{
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string_view::npos ? 0 : scheme + 3;
    const size_t end = std::min(url.find('/', start), url.size());
    const size_t at = url.rfind('@', end);
    if (at == std::string_view::npos || at < start) {
        return UrlLogin::None;
    }
    const size_t colon = url.find(':', start);
    return colon < at && colon + 1 < at ? UrlLogin::Both : UrlLogin::UserOnly;
}

} // namespace

// Opens a GET of `url` and reads its headers; false with `error` set unless the answer is 200.
// `authorize`: with the token of Home Assistant. `connect_ms` (0: `timeout_ms`): the connection
// itself; the answer then gets `timeout_ms`. `announce`: the stream whose UI is told once the
// connection is up and the request sent (go2rtc answers only with its first frame).
bool open(Context &ctx, Connection &c, const char *url, bool authorize, int timeout_ms, int64_t &length,
          char *error, size_t error_size, int connect_ms, const Request *announce)
{
    esp_http_client_config_t config{};
    config.url = url;
    config.timeout_ms = connect_ms > 0 ? connect_ms : timeout_ms;
    config.crt_bundle_attach = detail::tls_attach;
    // the token goes into a header longer than the default send buffer
    config.buffer_size_tx = 1024;
    config.user_agent = "hassctrl";
    // esp_http_client takes user and password from the address, but sends them only with Basic
    // auth set: open() does not answer a 401 the way perform() does
    const UrlLogin login = authorize ? UrlLogin::None : url_login(url);
    if (login != UrlLogin::None) {
        config.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    c.client = esp_http_client_init(&config);
    if (c.client == nullptr) {
        std::snprintf(error, error_size, "no memory for the request");
        return false;
    }
    if (authorize) {
        std::snprintf(ctx.authorization, sizeof(ctx.authorization), "Bearer %s", ctx.token);
        esp_http_client_set_header(c.client, "Authorization", ctx.authorization);
    }
    if (esp_http_client_open(c.client, 0) != ESP_OK) {
        std::snprintf(error, error_size, authorize ? "Home Assistant cannot be reached" : "the stream cannot be reached");
        c.problem = core::StreamProblem::Unreachable;
        return false;
    }
    esp_http_client_set_timeout_ms(c.client, timeout_ms);
    if (announce != nullptr) {
        stream_connected(*announce);
    }
    length = esp_http_client_fetch_headers(c.client);
    if (length < 0) {
        // else the status of a response that never came reads "HTTP -1"
        if (length == -ESP_ERR_HTTP_EAGAIN) {
            std::snprintf(error, error_size, "no answer within %d s", timeout_ms / 1000);
        } else {
            std::snprintf(error, error_size, "no answer (the connection broke off or timed out)");
        }
        c.problem = core::StreamProblem::NoAnswer;
        return false;
    }
    const int status = esp_http_client_get_status_code(c.client);
    if (status == 401 && !authorize) {
        // the screen shows these words: a stream of its own wants a login
        std::snprintf(error, error_size, "%s", login == UrlLogin::None       ? "login needed"
                                               : login == UrlLogin::UserOnly ? "password missing"
                                                                             : "login rejected");
        c.problem = core::StreamProblem::Login;
        c.status = status;
        return false;
    }
    if (status != 200) {
        std::snprintf(error, error_size, status == 401 ? "the token was rejected" : "HTTP %d", status);
        c.problem = core::StreamProblem::Http;
        c.status = status;
        return false;
    }
    return true;
}

// The body of a response into `out` (a still image).
bool read_body(Connection &c, int64_t length, Buffer &out, char *error, size_t error_size)
{
    if (length > static_cast<int64_t>(kMaxJpeg)) {
        std::snprintf(error, error_size, "the image is too large (%lld KB)", static_cast<long long>(length / 1024));
        return false;
    }
    // without a length the buffer grows as the data arrives
    if (!out.reserve(length > 0 ? static_cast<size_t>(length) : 64 * 1024)) {
        std::snprintf(error, error_size, "no memory for the image");
        return false;
    }
    for (;;) {
        if (out.size == out.capacity) {
            if (out.capacity >= kMaxJpeg || !out.reserve(std::min(out.capacity * 2, kMaxJpeg))) {
                std::snprintf(error, error_size, "the image is too large");
                return false;
            }
        }
        const int read = esp_http_client_read(c.client, reinterpret_cast<char *>(out.data + out.size),
                                              static_cast<int>(out.capacity - out.size));
        if (read < 0) {
            std::snprintf(error, error_size, "the image broke off");
            return false;
        }
        if (read == 0) {
            break;
        }
        out.size += static_cast<size_t>(read);
    }
    if (out.size == 0) {
        std::snprintf(error, error_size, "the camera sent no image");
        return false;
    }
    return true;
}

bool StreamReader::fill()
{
    pos = 0;
    if (buffer == nullptr) {
        return false;
    }
    const int read = esp_http_client_read(c.client, reinterpret_cast<char *>(buffer), static_cast<int>(kReadBuffer));
    len = read > 0 ? static_cast<size_t>(read) : 0;
    if (read == -ESP_ERR_HTTP_EAGAIN) {
        problem = core::StreamProblem::NoAnswer;
    } else if (read == 0) {
        problem = core::StreamProblem::Ended;
    } else if (read < 0) {
        problem = core::StreamProblem::BrokeOff;
    }
    return len > 0;
}

// a line without its \r\n
bool StreamReader::line(char *out, size_t size)
{
    size_t n = 0;
    for (;;) {
        if (pos == len && !fill()) {
            return false;
        }
        const char ch = static_cast<char>(buffer[pos++]);
        if (ch == '\n') {
            if (n > 0 && out[n - 1] == '\r') {
                n--;
            }
            out[n] = 0;
            return true;
        }
        if (n + 1 < size) {
            out[n++] = ch;
        } else if (n + 1 >= kMaxLine) {
            return false; // not the header of a part
        }
    }
}

// `size` bytes into `out`, or skipped when `out` is null
bool StreamReader::bytes(uint8_t *out, size_t size)
{
    while (size > 0) {
        if (pos == len && !fill()) {
            return false;
        }
        const size_t n = std::min(size, len - pos);
        if (out != nullptr) {
            std::memcpy(out, buffer + pos, n);
            out += n;
        }
        pos += n;
        size -= n;
    }
    return true;
}

// The bytes that arrived and are not read yet: in the buffer and in the socket (TLS and chunked
// encoding count their own bytes, enough to tell a waiting frame).
size_t StreamReader::waiting() const
{
    size_t n = len - pos;
    const int fd = esp_http_client_get_socket(c.client);
    if (fd >= 0) {
        uint8_t peek[kBehindBytes];
        const int queued = recv(fd, peek, sizeof(peek), MSG_PEEK | MSG_DONTWAIT);
        n += queued > 0 ? static_cast<size_t>(queued) : 0;
    }
    return n;
}

// The length of the next part's JPEG. The boundary line is not compared: the parts of the
// streams of go2rtc, motion and ffmpeg all start with "--" and carry their length.
bool StreamReader::next_part(size_t &length, char *error, size_t error_size)
{
    char text[kMaxLine];
    bool boundary = false;
    bool have_length = false;
    for (int lines = 0; lines < 16; lines++) {
        if (!line(text, sizeof(text))) {
            if (problem == core::StreamProblem::NoAnswer) {
                std::snprintf(error, error_size, "no data within %d s", kStreamTimeoutMs / 1000);
            } else {
                std::snprintf(error, error_size, "the stream ended");
                problem = problem == core::StreamProblem::None ? core::StreamProblem::Ended : problem;
            }
            return false;
        }
        if (!boundary) {
            boundary = text[0] == '-' && text[1] == '-';
            continue; // the empty line after the last JPEG
        }
        if (text[0] == 0) {
            if (!have_length) {
                std::snprintf(error, error_size, "the stream sends no Content-Length per frame");
                problem = core::StreamProblem::NotMjpeg;
                return false;
            }
            return true;
        }
        have_length = have_length || core::mjpeg_content_length(text, length);
    }
    std::snprintf(error, error_size, "not an MJPEG stream");
    problem = core::StreamProblem::NotMjpeg;
    return false;
}

} // namespace hc::hass::media
