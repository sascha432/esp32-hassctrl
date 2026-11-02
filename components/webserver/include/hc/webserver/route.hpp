#pragma once

// One entry of a route table (register_*_routes). With CONFIG_HTTPD_WS_SUPPORT httpd_uri_t has the
// WebSocket fields as well; an entry built here leaves them off, where a designated initializer
// would have to name them all.

#include "esp_http_server.h"

namespace hc::webserver {

inline httpd_uri_t route(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *req),
                         void *user_ctx = nullptr)
{
    httpd_uri_t entry{};
    entry.uri = uri;
    entry.method = method;
    entry.handler = handler;
    entry.user_ctx = user_ctx;
    return entry;
}

} // namespace hc::webserver
