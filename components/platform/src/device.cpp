#include "hc/platform/device.hpp"

#include <cstdint>
#include <cstdio>

#include "esp_mac.h"

namespace hc::platform {

const char *device_name()
{
    static char name[16] = {};
    if (name[0] == 0) {
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        std::snprintf(name, sizeof(name), "hassctrl-%02x%02x", mac[4], mac[5]);
    }
    return name;
}

} // namespace hc::platform
