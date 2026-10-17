// The stored configuration of the connection (docs/architecture.md section 6): address, token and
// the trusted certificate in NVS, their checks and the token test. Runs in the caller's task (the
// web server, the backup import); what the connection's status shows is handed to hass.cpp.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "hc/platform/settings.hpp"

#include "internal.hpp"
#include "url.hpp"

namespace hc::hass {

namespace {

constexpr const char *kTag = "hass";
constexpr const char *kNamespace = "hass";
constexpr const char *kKeyUrl = "url";
constexpr const char *kKeyToken = "token";
// the SHA-256 of the certificate trusted on first use, as hex
constexpr const char *kKeyCertificate = "cert";

// a token is base64url with dots (a JWT); anything else is a mistake while pasting it
bool token_chars_ok(const char *token)
{
    for (const char *p = token; *p != 0; p++) {
        if (!std::isalnum(static_cast<unsigned char>(*p)) && *p != '-' && *p != '_' && *p != '.' && *p != '=') {
            return false;
        }
    }
    return true;
}

// The URL as stored and the token without the spaces around it; ESP_ERR_INVALID_ARG with `error`
// filled when one is not usable.
esp_err_t clean_config(const char *url, const char *token, char (&normalized)[kUrlSize],
                       char (&clean_token)[kTokenSize], char *error, size_t error_size)
{
    const auto fail = [&](const char *text) {
        std::snprintf(error, error_size, "%s", text);
        return ESP_ERR_INVALID_ARG;
    };

    if (url != nullptr && url[0] != 0) {
        const char *problem = nullptr;
        if (!detail::normalize_url(url, normalized, sizeof(normalized), problem)) {
            return fail(problem);
        }
    }
    // a pasted token often comes with a space or a line break around it
    if (token != nullptr) {
        while (std::isspace(static_cast<unsigned char>(*token))) {
            token++;
        }
        size_t length = std::strlen(token);
        while (length > 0 && std::isspace(static_cast<unsigned char>(token[length - 1]))) {
            length--;
        }
        if (length >= sizeof(clean_token)) {
            return fail("the token is too long");
        }
        std::memcpy(clean_token, token, length);
        if (!token_chars_ok(clean_token)) {
            return fail("the token contains characters a token cannot have");
        }
    }
    return ESP_OK;
}

} // namespace

namespace detail {

void read_stored_config(char (&url)[kUrlSize], char (&token)[kTokenSize], char (&certificate)[kFingerprintSize])
{
    url[0] = 0;
    token[0] = 0;
    certificate[0] = 0;
    platform::Settings settings(kNamespace, platform::Settings::Mode::Read);
    if (!settings.ok()) {
        return;
    }
    if (settings.get_string(kKeyUrl, url, sizeof(url)) != ESP_OK) {
        url[0] = 0;
    }
    if (settings.get_string(kKeyToken, token, sizeof(token)) != ESP_OK) {
        token[0] = 0;
    }
    uint8_t digest[kFingerprintBytes];
    if (settings.get_string(kKeyCertificate, certificate, sizeof(certificate)) == ESP_OK &&
        parse_fingerprint(certificate, digest)) {
        set_trusted_certificate(digest);
    } else {
        certificate[0] = 0;
    }
}

} // namespace detail

esp_err_t check_config(const char *url, const char *token, char *error, size_t error_size)
{
    char normalized[kUrlSize] = {};
    char clean_token[kTokenSize] = {};
    return clean_config(url, token, normalized, clean_token, error, error_size);
}

esp_err_t set_config(const char *url, const char *token, char *error, size_t error_size)
{
    char normalized[kUrlSize] = {};
    char clean_token[kTokenSize] = {};
    const bool remove = url == nullptr || url[0] == 0;
    if (const esp_err_t err = clean_config(url, token, normalized, clean_token, error, error_size); err != ESP_OK) {
        return err;
    }

    // the trusted certificate belongs to the address
    const bool new_address = !detail::is_current_url(normalized);

    platform::Settings settings(kNamespace, platform::Settings::Mode::ReadWrite);
    esp_err_t err = settings.ok() ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK && new_address) {
        settings.erase(kKeyCertificate);
    }
    if (err == ESP_OK && remove) {
        settings.erase(kKeyUrl);
        settings.erase(kKeyToken);
    } else if (err == ESP_OK) {
        err = settings.set_string(kKeyUrl, normalized);
        if (err == ESP_OK && clean_token[0] != 0) {
            err = settings.set_string(kKeyToken, clean_token);
        }
    }
    if (err == ESP_OK) {
        err = settings.commit();
    }
    if (err != ESP_OK) {
        std::snprintf(error, error_size, "the settings could not be stored (%s)", esp_err_to_name(err));
        return err;
    }

    detail::config_stored(normalized, clean_token, remove, new_address);
    ESP_LOGI(kTag, "address %s%s", remove ? "removed" : normalized,
             clean_token[0] != 0 ? ", new token" : "");
    return ESP_OK;
}

esp_err_t test_token(const char *url, const char *token, char *error, size_t error_size)
{
    // the token is 512 bytes: off the caller's stack (the web server's)
    struct Buffers {
        char normalized[kUrlSize];
        char token[kTokenSize];
        char api[kUrlSize + 8];
        char authorization[kTokenSize + 8];
    };
    auto *b = static_cast<Buffers *>(heap_caps_calloc_prefer(1, sizeof(Buffers), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                             MALLOC_CAP_DEFAULT));
    if (b == nullptr) {
        std::snprintf(error, error_size, "no memory for the test");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = clean_config(url, token, b->normalized, b->token, error, error_size);
    int status = 0;
    if (err == ESP_OK && b->normalized[0] != 0 && b->token[0] != 0) {
        std::snprintf(b->api, sizeof(b->api), "%s/api/", b->normalized);
        std::snprintf(b->authorization, sizeof(b->authorization), "Bearer %s", b->token);
        esp_http_client_config_t config{};
        config.url = b->api;
        config.timeout_ms = 10000;
        config.crt_bundle_attach = detail::tls_attach;
        // the token goes into a header longer than the default send buffer
        config.buffer_size_tx = 1024;
        config.user_agent = "hassctrl";
        if (esp_http_client_handle_t client = esp_http_client_init(&config); client != nullptr) {
            esp_http_client_set_header(client, "Authorization", b->authorization);
            if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
                status = esp_http_client_get_status_code(client);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
        // a refused certificate is for the connection to report; not left behind in this task
        uint8_t digest[kFingerprintBytes];
        detail::refused_certificate(digest);
        ESP_LOGI(kTag, "token test at %s: %s%d", b->normalized, status != 0 ? "HTTP " : "no answer ", status);
        if (status == 401) {
            std::snprintf(error, error_size, "Home Assistant rejected the token, nothing was stored");
            err = ESP_ERR_INVALID_ARG;
        }
    }
    std::memset(b, 0, sizeof(Buffers));
    std::free(b);
    return err;
}

esp_err_t check_certificate(const char *fingerprint, char *error, size_t error_size)
{
    uint8_t digest[kFingerprintBytes];
    if (fingerprint != nullptr && fingerprint[0] != 0 && !detail::parse_fingerprint(fingerprint, digest)) {
        std::snprintf(error, error_size, "not a SHA-256 fingerprint (64 hex digits)");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t trust_certificate(const char *fingerprint, char *error, size_t error_size)
{
    if (const esp_err_t err = check_certificate(fingerprint, error, error_size); err != ESP_OK) {
        return err;
    }
    const bool forget = fingerprint == nullptr || fingerprint[0] == 0;
    uint8_t digest[kFingerprintBytes] = {};
    char text[kFingerprintSize] = {};
    if (!forget) {
        detail::parse_fingerprint(fingerprint, digest);
        detail::fingerprint_text(digest, text);
    }

    platform::Settings settings(kNamespace, platform::Settings::Mode::ReadWrite);
    esp_err_t err = settings.ok() ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK && forget) {
        settings.erase(kKeyCertificate);
    } else if (err == ESP_OK) {
        err = settings.set_string(kKeyCertificate, text);
    }
    if (err == ESP_OK) {
        err = settings.commit();
    }
    if (err != ESP_OK) {
        std::snprintf(error, error_size, "the settings could not be stored (%s)", esp_err_to_name(err));
        return err;
    }

    detail::set_trusted_certificate(forget ? nullptr : digest);
    detail::certificate_stored(text);
    ESP_LOGI(kTag, "trusted certificate: %s", forget ? "none" : text);
    return ESP_OK;
}

} // namespace hc::hass
