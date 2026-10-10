// The certificate check of the connections to Home Assistant: the certificate bundle, or the one
// certificate the user trusted on first use (docs/architecture.md section 6).
//
// The bundle's verify callback is wrapped. mbedTLS calls it for every certificate of the chain,
// the top one first and the server's own (depth 0) last, and ORs the flags each call leaves. The
// wrapper keeps the bundle's verdict of the whole chain in a thread-local value and gives
// mbedTLS the result at depth 0 alone: no flags when the server's certificate is the trusted one
// (checked first) or the bundle trusts the chain. A handshake runs in one task, so the thread-local value is its
// own; the WebSocket client also hands its events out in the task of the handshake.

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_crt_bundle.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "internal.hpp"

// the bundle's own callback (esp_crt_bundle.c), not in its header
extern "C" int esp_crt_verify_callback(void *buf, mbedtls_x509_crt *crt, int depth, uint32_t *flags);

namespace hc::hass::detail {

namespace {

using Digest = std::array<uint8_t, kFingerprintBytes>;

std::mutex g_mutex;
Digest g_trusted{};
bool g_has_trusted = false;

// the handshake of this task
struct Handshake {
    uint32_t chain_flags = 0; // the bundle's verdict on the certificates above the server's
    Digest refused{};         // the server's certificate, when it was refused
    bool has_refused = false;
};
thread_local Handshake t_handshake;

// the bundle's verdict on one certificate
uint32_t bundle_verdict(mbedtls_x509_crt *crt, int depth, uint32_t flags)
{
    // the bundle fails with an error when it knows no issuer; that is "not trusted" here, the
    // trusted certificate may still match
    if (esp_crt_verify_callback(nullptr, crt, depth, &flags) != 0) {
        flags |= MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    }
    return flags;
}

bool is_trusted(const Digest &digest)
{
    std::lock_guard lock(g_mutex);
    return g_has_trusted && digest == g_trusted;
}

int verify(void *, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    Handshake &h = t_handshake;
    if (depth > 0) {
        h.chain_flags |= bundle_verdict(crt, depth, *flags);
        *flags = 0;
        return 0;
    }
    const uint32_t chain_flags = h.chain_flags;
    h.chain_flags = 0;
    h.has_refused = false;
    // the trusted certificate first: the bundle would log an error for it on every connection
    Digest digest{};
    size_t length = 0;
    const bool hashed = psa_hash_compute(PSA_ALG_SHA_256, crt->raw.p, crt->raw.len, digest.data(), digest.size(),
                                         &length) == PSA_SUCCESS;
    if (hashed && is_trusted(digest)) {
        *flags = 0;
        return 0;
    }
    *flags = chain_flags | bundle_verdict(crt, depth, *flags);
    if (*flags != 0 && hashed) {
        h.refused = digest;
        h.has_refused = true;
    }
    return 0;
}

} // namespace

esp_err_t tls_attach(void *conf)
{
    const esp_err_t err = esp_crt_bundle_attach(conf);
    if (err == ESP_OK && conf != nullptr) {
        mbedtls_ssl_conf_verify(static_cast<mbedtls_ssl_config *>(conf), verify, nullptr);
    }
    return err;
}

void set_trusted_certificate(const uint8_t *digest)
{
    std::lock_guard lock(g_mutex);
    g_has_trusted = digest != nullptr;
    if (digest != nullptr) {
        std::memcpy(g_trusted.data(), digest, g_trusted.size());
    }
}

bool refused_certificate(uint8_t *digest)
{
    Handshake &h = t_handshake;
    if (!h.has_refused) {
        return false;
    }
    std::memcpy(digest, h.refused.data(), h.refused.size());
    h.has_refused = false;
    return true;
}

void fingerprint_text(const uint8_t *digest, char *out)
{
    for (size_t i = 0; i < kFingerprintBytes; i++) {
        std::snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
}

bool parse_fingerprint(const char *text, uint8_t *digest)
{
    // also "AB:CD:..." as browsers and openssl show it
    size_t count = 0;
    int high = -1;
    for (const char *p = text; *p != 0; p++) {
        if (*p == ':' || *p == ' ') {
            continue;
        }
        int nibble;
        if (*p >= '0' && *p <= '9') {
            nibble = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            nibble = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            nibble = *p - 'A' + 10;
        } else {
            return false;
        }
        if (high < 0) {
            high = nibble;
            continue;
        }
        if (count == kFingerprintBytes) {
            return false;
        }
        digest[count++] = static_cast<uint8_t>(high << 4 | nibble);
        high = -1;
    }
    return count == kFingerprintBytes && high < 0;
}

} // namespace hc::hass::detail
