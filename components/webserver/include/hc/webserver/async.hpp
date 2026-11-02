#pragma once

// A long response on a task of its own (docs/architecture.md section 9.4): the web server runs
// every handler on its one task, so a file download, an archive or a camera stream sent there
// holds every other request until it is done. ESP-IDF's async requests
// (httpd_req_async_handler_begin) hand the request to another task; this does it for all of them,
// with one cap on how many run at once.

#include <cstdint>

#include "esp_http_server.h"

namespace hc::webserver {

// Where the task's stack lives. A task that reads the flash (LittleFS, NVS) needs Internal: the
// flash read turns the cache off, and a stack in PSRAM goes with it. The SD card, PSRAM data and
// the network are fine with Psram.
enum class AsyncStack { Internal, Psram };

// Runs `handler(copy)` on a task of its own and completes the copy after it; the handler answers
// the request as it would on the server's task. False when nothing was taken over (3 run already,
// or no memory for the copy): the caller answers on the server's task itself. A task that cannot
// be started after the copy was made gets a 503 here, which counts as taken over.
bool run_async(httpd_req_t *req, void (*handler)(httpd_req_t *req), AsyncStack stack, uint32_t stack_size);

// true while the session of `fd` belongs to such a task
bool async_in_progress(int fd);

} // namespace hc::webserver
