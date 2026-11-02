// Closes web sessions that have been silent for 30 s (docs/architecture.md section 9.4).
//
// Browsers keep connections open after a request and use them again; one that went away without
// closing them (a laptop that sleeps, a phone that leaves the network) leaves them open on the
// device for good: ESP-IDF's server has no idle timeout, only the LRU purge when all its sessions
// are taken (2026-10-31: 11 of them locked the web UI out). Every send and receive of a session
// goes through the overrides here, which note the time; a timer closes what has been silent for
// kIdleMs. Not the live socket (silent while nothing changes, TCP keep-alive finds a dead one) and
// not a long response on its own task (it sends, and the server must not close its socket).
// Every byte a browser sends is activity for the Wi-Fi's modem sleep (network::wake_activity).

#include <atomic>
#include <cerrno>

#include "esp_log.h"
#include "esp_timer.h"
#include "hc/network/network.hpp"
#include "hc/webserver/async.hpp"
#include "lwip/sockets.h"

#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

constexpr const char *kTag = "web";
constexpr int64_t kIdleMs = 30000;
constexpr uint64_t kCheckUs = 5000000;

// the time of the last byte per session, by fd (ms since the start)
std::atomic<int64_t> g_last[CONFIG_LWIP_MAX_SOCKETS];

int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

std::atomic<int64_t> *last_of(int fd)
{
    const int index = fd - LWIP_SOCKET_OFFSET;
    return index >= 0 && index < CONFIG_LWIP_MAX_SOCKETS ? &g_last[index] : nullptr;
}

void touch(int fd)
{
    if (auto *last = last_of(fd)) {
        last->store(now_ms());
    }
}

// the warning and the error codes of ESP-IDF's own send and receive (httpd_txrx.c, httpd_sock_err)
int socket_error(const char *what)
{
    ESP_LOGW(kTag, "error in %s: %d", what, errno);
    if (errno == EAGAIN || errno == EINTR) {
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    if (errno == EINVAL || errno == EBADF || errno == EFAULT || errno == ENOTSOCK) {
        return HTTPD_SOCK_ERR_INVALID;
    }
    return HTTPD_SOCK_ERR_FAIL;
}

int receive(httpd_handle_t, int fd, char *buffer, size_t length, int flags)
{
    if (buffer == nullptr) {
        return HTTPD_SOCK_ERR_INVALID;
    }
    const int n = recv(fd, buffer, length, flags);
    if (n < 0) {
        return socket_error("recv");
    }
    if (n > 0) {
        touch(fd);
        network::wake_activity();
    }
    return n;
}

int transmit(httpd_handle_t, int fd, const char *buffer, size_t length, int flags)
{
    if (buffer == nullptr) {
        return HTTPD_SOCK_ERR_INVALID;
    }
    const int n = send(fd, buffer, length, flags);
    if (n < 0) {
        return socket_error("send");
    }
    touch(fd);
    return n;
}

// on the server's task: no session is in a handler there meanwhile
void close_idle(void *arg)
{
    auto server = static_cast<httpd_handle_t>(arg);
    size_t count = CONFIG_LWIP_MAX_SOCKETS;
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    if (httpd_get_client_list(server, &count, fds) != ESP_OK) {
        return;
    }
    const int64_t now = now_ms();
    for (size_t i = 0; i < count; i++) {
        const int fd = fds[i];
        const auto *last = last_of(fd);
        if (last == nullptr || async_in_progress(fd)) {
            continue;
        }
#if CONFIG_HTTPD_WS_SUPPORT
        if (httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
            continue;
        }
#endif
        const int64_t silent = now - last->load();
        if (silent >= kIdleMs) {
            ESP_LOGD(kTag, "closing session %d, silent for %lld s", fd, static_cast<long long>(silent / 1000));
            httpd_sess_trigger_close(server, fd);
        }
    }
}

} // namespace

esp_err_t open_session(httpd_handle_t server, int fd)
{
    touch(fd);
    httpd_sess_set_recv_override(server, fd, receive);
    httpd_sess_set_send_override(server, fd, transmit);
    return ESP_OK;
}

void close_session(httpd_handle_t, int fd)
{
    live_forget(fd);
    // with a close_fn the server leaves the socket to it
    close(fd);
}

void start_idle_close(httpd_handle_t server)
{
    const esp_timer_create_args_t args = {
        .callback = [](void *arg) { httpd_queue_work(static_cast<httpd_handle_t>(arg), close_idle, arg); },
        .arg = server,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "web_idle",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_periodic(timer, kCheckUs) != ESP_OK) {
        ESP_LOGW(kTag, "no idle close");
    }
}

} // namespace hc::webserver::detail
