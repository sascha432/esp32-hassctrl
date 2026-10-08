// Home Assistant instances on the network (docs/ui/web.md section 6): Home Assistant announces
// itself over mDNS as _home-assistant._tcp, with its own address in the TXT record
// (internal_url, base_url), its name (location_name) and its version.

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "hc/hass/hass.hpp"
#include "mdns.h"

namespace hc::hass {

namespace {

constexpr const char *kTag = "hass";

// a TXT value of the result, empty when it is missing
void txt(const mdns_result_t &r, const char *key, char *out, size_t size)
{
    out[0] = 0;
    for (size_t i = 0; i < r.txt_count; i++) {
        if (r.txt[i].key != nullptr && std::strcmp(r.txt[i].key, key) == 0 && r.txt[i].value != nullptr) {
            const int length = r.txt_value_len != nullptr ? r.txt_value_len[i] : static_cast<int>(std::strlen(r.txt[i].value));
            std::snprintf(out, size, "%.*s", length, r.txt[i].value);
            return;
        }
    }
}

} // namespace

size_t discover(Found *out, size_t max, uint32_t timeout_ms)
{
    mdns_result_t *results = nullptr;
    const esp_err_t err = mdns_query_ptr("_home-assistant", "_tcp", timeout_ms, max * 2, &results);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "mDNS search failed: %s", esp_err_to_name(err));
        return 0;
    }
    size_t count = 0;
    for (const mdns_result_t *r = results; r != nullptr && count < max; r = r->next) {
        Found found{};
        // the address Home Assistant gives itself; without it the host name, then the address
        txt(*r, "internal_url", found.url, sizeof(found.url));
        if (found.url[0] == 0) {
            txt(*r, "base_url", found.url, sizeof(found.url));
        }
        if (found.url[0] == 0 && r->hostname != nullptr) {
            std::snprintf(found.url, sizeof(found.url), "http://%.80s.local:%u", r->hostname, r->port);
        }
        if (found.url[0] == 0) {
            for (const mdns_ip_addr_t *a = r->addr; a != nullptr; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    std::snprintf(found.url, sizeof(found.url), "http://" IPSTR ":%u", IP2STR(&a->addr.u_addr.ip4),
                                  r->port);
                    break;
                }
            }
        }
        if (found.url[0] == 0) {
            continue;
        }
        txt(*r, "location_name", found.name, sizeof(found.name));
        if (found.name[0] == 0 && r->instance_name != nullptr) {
            std::snprintf(found.name, sizeof(found.name), "%s", r->instance_name);
        }
        txt(*r, "version", found.version, sizeof(found.version));
        // one entry per address: the answer arrives on every interface
        bool known = false;
        for (size_t i = 0; i < count; i++) {
            known = known || std::strcmp(out[i].url, found.url) == 0;
        }
        if (!known) {
            out[count++] = found;
        }
    }
    mdns_query_results_free(results);
    return count;
}

} // namespace hc::hass
