#include "captive_dns.hpp"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/network/network.hpp"
#include "lwip/sockets.h"

namespace hc::webserver::detail {

namespace {

constexpr const char *kTag = "dns";
constexpr uint16_t kPort = 53;
constexpr size_t kMaxPacket = 512;
constexpr uint16_t kTypeA = 1;
constexpr uint16_t kClassIn = 1;
// short, so a phone asks again once it is on another network
constexpr uint32_t kTtlSeconds = 10;
// how often the task looks whether the hotspot is on
constexpr int kCheckSeconds = 1;

uint32_t g_address = 0;

uint16_t read16(const uint8_t *p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void write16(uint8_t *p, uint16_t value)
{
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

// Builds the answer in place. Returns its length, 0 to send nothing.
size_t answer(uint8_t *packet, size_t length)
{
    constexpr size_t kHeader = 12;
    if (length < kHeader + 5) {
        return 0;
    }
    const uint16_t flags = read16(packet + 2);
    // only standard queries (QR = 0, opcode 0) with exactly one question
    if ((flags & 0x8000) != 0 || ((flags >> 11) & 0x0f) != 0 || read16(packet + 4) != 1) {
        return 0;
    }

    // walk the name of the question: labels up to the terminating zero
    size_t pos = kHeader;
    while (pos < length && packet[pos] != 0) {
        if ((packet[pos] & 0xc0) != 0) {
            return 0; // no compression in a question
        }
        pos += packet[pos] + 1;
    }
    if (pos + 5 > length) {
        return 0;
    }
    pos++; // the terminating zero
    const uint16_t type = read16(packet + pos);
    const uint16_t qclass = read16(packet + pos + 2);
    pos += 4;

    // response, recursion desired copied, recursion available, no error
    write16(packet + 2, static_cast<uint16_t>(0x8180 | (flags & 0x0100)));
    write16(packet + 8, 0);  // authority records
    write16(packet + 10, 0); // additional records

    if (type != kTypeA || qclass != kClassIn || pos + 16 > kMaxPacket) {
        write16(packet + 6, 0); // no answer for AAAA and the rest
        return pos;
    }
    write16(packet + 6, 1);
    uint8_t *a = packet + pos;
    write16(a, 0xc000 | kHeader); // the name: pointer to the question
    write16(a + 2, kTypeA);
    write16(a + 4, kClassIn);
    a[6] = static_cast<uint8_t>(kTtlSeconds >> 24);
    a[7] = static_cast<uint8_t>(kTtlSeconds >> 16);
    a[8] = static_cast<uint8_t>(kTtlSeconds >> 8);
    a[9] = static_cast<uint8_t>(kTtlSeconds);
    write16(a + 10, 4);
    std::memcpy(a + 12, &g_address, 4);
    return pos + 16;
}

// the socket on port 53, -1 when it cannot listen
int open_socket()
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    // the task looks at the hotspot between two packets
    const timeval timeout{.tv_sec = kCheckSeconds, .tv_usec = 0};
    if (sock < 0 || bind(sock, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        ESP_LOGE(kTag, "cannot listen on port %u", kPort);
        if (sock >= 0) {
            close(sock);
        }
        return -1;
    }
    return sock;
}

// The socket is open only while the hotspot is on: in station mode it would hold one of lwIP's
// sockets for nothing (docs/architecture.md section 9.4).
void task(void *)
{
    int sock = -1;
    uint8_t packet[kMaxPacket];
    for (;;) {
        const bool hotspot = network::status().hotspot_on;
        if (hotspot && sock < 0) {
            sock = open_socket();
        } else if (!hotspot && sock >= 0) {
            close(sock);
            sock = -1;
        }
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(kCheckSeconds * 1000));
            continue;
        }
        sockaddr_in from{};
        socklen_t from_length = sizeof(from);
        const int received =
            recvfrom(sock, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&from), &from_length);
        if (received <= 0) {
            continue;
        }
        const size_t length = answer(packet, static_cast<size_t>(received));
        if (length > 0) {
            sendto(sock, packet, length, 0, reinterpret_cast<sockaddr *>(&from), from_length);
        }
    }
}

} // namespace

esp_err_t start_captive_dns(uint32_t address)
{
    g_address = address;
    // 1.5 KB used at most (JC3248W535, 2026-11-03; the margin: docs/architecture.md section 4)
    return xTaskCreate(task, "captive_dns", 2560, nullptr, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

} // namespace hc::webserver::detail
