#include "hc/platform/device.hpp"

#include <cstdint>
#include <cstdio>

#include "esp_mac.h"
#include "soc/soc_caps.h"

namespace hc::platform {

const char *device_name()
{
    static char name[16] = {};
    if (name[0] == 0) {
        uint8_t mac[6] = {};
#if SOC_WIFI_SUPPORTED
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
#else
        // no radio in the chip (ESP32-P4, Wi-Fi on a companion chip): its own factory address
        esp_read_mac(mac, ESP_MAC_BASE);
#endif
        std::snprintf(name, sizeof(name), "hassctrl-%02x%02x", mac[4], mac[5]);
    }
    return name;
}

} // namespace hc::platform
