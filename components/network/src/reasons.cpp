#include "reasons.hpp"

#include "esp_wifi_types.h"

namespace hc::network::detail {

bool is_auth_failure(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return true;
    default:
        return false;
    }
}

bool may_pass_on_retry(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return true;
    default:
        return false;
    }
}

const char *reason_text(uint8_t reason)
{
    if (is_auth_failure(reason)) {
        return "wrong Wi-Fi password";
    }
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "network not found";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "network security not supported";
    case WIFI_REASON_ASSOC_FAIL:
        return "the network refused the device";
    case WIFI_REASON_AUTH_EXPIRE:
        // also a router that still holds the connection of a device that was reset (WPA3, PMF)
        return "the network did not answer in time";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "connection lost, signal too weak?";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection failed";
    case WIFI_REASON_ASSOC_LEAVE:
        return "disconnected";
    default:
        return "connection lost";
    }
}

} // namespace hc::network::detail
