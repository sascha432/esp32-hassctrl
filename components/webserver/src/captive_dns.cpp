#include "captive_dns.hpp"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

void task(void *)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || bind(sock, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        ESP_LOGE(kTag, "cannot listen on port %u", kPort);
        if (sock >= 0) {
            close(sock);
        }
        vTaskDelete(nullptr);
        return;
    }

    uint8_t packet[kMaxPacket];
    for (;;) {
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
    return xTaskCreate(task, "captive_dns", 3072, nullptr, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

} // namespace hc::webserver::detail
