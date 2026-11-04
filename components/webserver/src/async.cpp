#include "hc/webserver/async.hpp"

#include <atomic>
#include <cstdlib>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

namespace hc::webserver {

namespace {

constexpr const char *kTag = "web";
// long responses at once; each is one of the server's sessions as well
constexpr int kMaxAsync = 3;
// below the server's task (5): the API answers first, the transfer takes what is left
constexpr UBaseType_t kPriority = 4;

struct Job {
    httpd_req_t *req;
    void (*handler)(httpd_req_t *req);
    int fd;
};

std::atomic<int> g_running{0};
std::atomic<bool> g_busy[CONFIG_LWIP_MAX_SOCKETS];

std::atomic<bool> *busy_flag(int fd)
{
    const int index = fd - LWIP_SOCKET_OFFSET;
    return index >= 0 && index < CONFIG_LWIP_MAX_SOCKETS ? &g_busy[index] : nullptr;
}

void finish(Job *job)
{
    if (auto *flag = busy_flag(job->fd)) {
        flag->store(false);
    }
    httpd_req_async_handler_complete(job->req);
    std::free(job);
    g_running--;
}

void task(void *arg)
{
    auto *job = static_cast<Job *>(arg);
    job->handler(job->req);
    finish(job);
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

bool run_async(httpd_req_t *req, void (*handler)(httpd_req_t *req), AsyncStack stack, uint32_t stack_size)
{
    if (g_running.fetch_add(1) >= kMaxAsync) {
        g_running--;
        return false;
    }
    // The stack is checked before the request is handed over: a task that cannot be made after
    // that leaves the copy to be completed on the server's task, and the completion waits for a
    // slot in the server's work queue that only the server's task frees (a full one hangs it).
    const uint32_t caps = stack == AsyncStack::Internal ? MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT : MALLOC_CAP_SPIRAM;
    if (heap_caps_get_largest_free_block(caps) < stack_size + 1024) {
        g_running--;
        return false;
    }
    auto *job = static_cast<Job *>(std::malloc(sizeof(Job)));
    httpd_req_t *copy = nullptr;
    if (job == nullptr || httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        std::free(job);
        g_running--;
        return false;
    }
    *job = {copy, handler, httpd_req_to_sockfd(copy)};
    if (auto *flag = busy_flag(job->fd)) {
        flag->store(true);
    }
    if (xTaskCreateWithCaps(task, "web_async", stack_size, job, kPriority, nullptr, caps) != pdPASS) {
        // the request's body, if any, belongs to the copy now: the copy answers
        ESP_LOGW(kTag, "no task for %s", copy->uri);
        httpd_resp_set_status(copy, "503 Service Unavailable");
        httpd_resp_sendstr(copy, "busy, try again\n");
        finish(job);
    }
    return true;
}

bool async_in_progress(int fd)
{
    const auto *flag = busy_flag(fd);
    return flag != nullptr && flag->load();
}

} // namespace hc::webserver
