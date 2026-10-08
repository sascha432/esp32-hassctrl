#include "auth.hpp"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "hc/platform/settings.hpp"
#include "hc/platform/time_sync.hpp"
#include "psa/crypto.h"

namespace hc::webserver::detail::auth {

namespace {

constexpr const char *kTag = "auth";
constexpr const char *kNamespace = "webui";
constexpr const char *kKeyUser = "user";
constexpr const char *kKeySalt = "salt";
constexpr const char *kKeyHash = "hash";
constexpr const char *kKeyIterations = "iterations";
constexpr const char *kKeyCounter = "counter";

constexpr size_t kSaltSize = 16;
constexpr size_t kHashSize = 32;
// PBKDF2 cost. Measured on the ESP32 through PSA crypto: 10000 iterations take 5.3 s, which blocks
// the web server for every login. 1000 take about half a second (logged by set_user()).
constexpr uint32_t kIterations = 1000;

struct Login {
    char user[kUsernameSize];
    uint8_t salt[kSaltSize];
    uint8_t hash[kHashSize];
    uint32_t iterations;
    uint32_t counter;
    // the signing key derived from hash and counter
    uint8_t key[kHashSize];
    bool present;
};

Login g{};

using hc::platform::Settings;

esp_err_t pbkdf2(const char *password, size_t password_length, const uint8_t *salt, size_t salt_length,
                 uint32_t iterations, uint8_t *out, size_t out_length)
{
    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(&operation, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_integer(&operation, PSA_KEY_DERIVATION_INPUT_COST, iterations);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_length);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                                reinterpret_cast<const uint8_t *>(password), password_length);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&operation, out, out_length);
    }
    psa_key_derivation_abort(&operation);
    return status == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}

esp_err_t hmac(const uint8_t *key, size_t key_length, const void *message, size_t message_length,
               uint8_t (&out)[kHashSize])
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, key_length * 8);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id = 0;
    if (psa_import_key(&attributes, key, key_length, &id) != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    size_t length = 0;
    const psa_status_t status = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                                static_cast<const uint8_t *>(message), message_length, out,
                                                sizeof(out), &length);
    psa_destroy_key(id);
    return (status == PSA_SUCCESS && length == kHashSize) ? ESP_OK : ESP_FAIL;
}

// the stored login: PBKDF2 over username and password, so renaming the user changes it too
esp_err_t stored_login(const char *username, const char *password, const uint8_t *salt, uint32_t iterations,
                       uint8_t (&out)[kHashSize])
{
    char input[kUsernameSize + kMaxPassword + 2];
    const int length = std::snprintf(input, sizeof(input), "%s:%s", username, password);
    const esp_err_t err = pbkdf2(input, static_cast<size_t>(length), salt, kSaltSize, iterations, out, kHashSize);
    std::memset(input, 0, sizeof(input));
    return err;
}

void derive_key()
{
    hmac(g.hash, sizeof(g.hash), &g.counter, sizeof(g.counter), g.key);
}

bool equal(const uint8_t *a, const uint8_t *b, size_t length)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < length; i++) {
        difference |= a[i] ^ b[i];
    }
    return difference == 0;
}

void to_hex(const uint8_t *data, size_t length, char *out)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) {
        out[i * 2] = kDigits[data[i] >> 4];
        out[i * 2 + 1] = kDigits[data[i] & 0x0f];
    }
    out[length * 2] = 0;
}

// seconds for the expiry: wall clock when it is set, otherwise none can be checked
uint32_t now_seconds()
{
    return hc::platform::time_synced() ? static_cast<uint32_t>(std::time(nullptr)) : 0;
}

esp_err_t save()
{
    Settings settings(kNamespace, Settings::Mode::ReadWrite);
    esp_err_t err = settings.set_string(kKeyUser, g.user);
    if (err == ESP_OK) {
        err = settings.set_blob(kKeySalt, g.salt, sizeof(g.salt));
    }
    if (err == ESP_OK) {
        err = settings.set_blob(kKeyHash, g.hash, sizeof(g.hash));
    }
    if (err == ESP_OK) {
        err = settings.set_u32(kKeyIterations, g.iterations);
    }
    if (err == ESP_OK) {
        err = settings.set_u32(kKeyCounter, g.counter);
    }
    return err == ESP_OK ? settings.commit() : err;
}

// RFC 7914 section 11: PBKDF2-HMAC-SHA256, P = "passwd", S = "salt", c = 1, first 32 bytes
bool self_test()
{
    static constexpr uint8_t kExpected[] = {0x55, 0xac, 0x04, 0x6e, 0x56, 0xe3, 0x08, 0x9f, 0xec, 0x16, 0x91,
                                            0xc2, 0x25, 0x44, 0xb6, 0x05, 0xf9, 0x41, 0x85, 0x21, 0x6d, 0xde,
                                            0x04, 0x65, 0xe6, 0x8b, 0x9d, 0x57, 0xc2, 0x0d, 0xac, 0xbc};
    uint8_t out[sizeof(kExpected)];
    return pbkdf2("passwd", 6, reinterpret_cast<const uint8_t *>("salt"), 4, 1, out, sizeof(out)) == ESP_OK &&
           std::memcmp(out, kExpected, sizeof(out)) == 0;
}

} // namespace

esp_err_t init()
{
    ESP_RETURN_ON_FALSE(psa_crypto_init() == PSA_SUCCESS, ESP_FAIL, kTag, "PSA crypto");
    ESP_RETURN_ON_FALSE(self_test(), ESP_FAIL, kTag, "PBKDF2 self test failed");

    g = {};
    Settings settings(kNamespace, Settings::Mode::Read);
    if (!settings.ok()) {
        return ESP_OK;
    }
    size_t salt_size = sizeof(g.salt);
    size_t hash_size = sizeof(g.hash);
    g.present = settings.get_string(kKeyUser, g.user, sizeof(g.user)) == ESP_OK &&
                settings.get_blob(kKeySalt, g.salt, salt_size) == ESP_OK && salt_size == sizeof(g.salt) &&
                settings.get_blob(kKeyHash, g.hash, hash_size) == ESP_OK && hash_size == sizeof(g.hash) &&
                settings.get_u32(kKeyIterations, g.iterations) == ESP_OK &&
                settings.get_u32(kKeyCounter, g.counter) == ESP_OK;
    if (g.present) {
        derive_key();
        ESP_LOGI(kTag, "web UI login '%s'", g.user);
    } else {
        g = {};
        ESP_LOGI(kTag, "no web UI login yet: setup mode");
    }
    return ESP_OK;
}

bool has_user()
{
    return g.present;
}

bool valid_username(const char *username)
{
    const size_t length = strnlen(username, kUsernameSize);
    if (length == 0 || length >= kUsernameSize) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        const unsigned char c = static_cast<unsigned char>(username[i]);
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

esp_err_t set_user(const char *username, const char *password)
{
    const size_t password_length = strnlen(password, kMaxPassword + 1);
    ESP_RETURN_ON_FALSE(valid_username(username), ESP_ERR_INVALID_ARG, kTag, "username");
    ESP_RETURN_ON_FALSE(password_length >= kMinPassword && password_length <= kMaxPassword, ESP_ERR_INVALID_ARG,
                        kTag, "password length");

    Login login{};
    std::snprintf(login.user, sizeof(login.user), "%s", username);
    esp_fill_random(login.salt, sizeof(login.salt));
    login.iterations = kIterations;
    // a new login keeps counting, so no token of the old one can match by accident
    login.counter = g.counter + 1;
    const int64_t started = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(stored_login(username, password, login.salt, login.iterations, login.hash), kTag, "PBKDF2");
    ESP_LOGI(kTag, "PBKDF2 with %" PRIu32 " iterations took %lld ms", login.iterations,
             static_cast<long long>((esp_timer_get_time() - started) / 1000));

    login.present = true;
    g = login;
    derive_key();
    return save();
}

bool check_password(const char *username, const char *password)
{
    if (!g.present || std::strcmp(username, g.user) != 0 || strnlen(password, kMaxPassword + 1) > kMaxPassword) {
        return false;
    }
    uint8_t hash[kHashSize];
    return stored_login(username, password, g.salt, g.iterations, hash) == ESP_OK &&
           equal(hash, g.hash, sizeof(hash));
}

bool make_token(const char *username, uint32_t lifetime_s, char (&out)[kTokenSize])
{
    if (!g.present || !valid_username(username)) {
        return false;
    }
    const uint32_t now = now_seconds();
    // without a clock the token carries no expiry (0); the cookie lifetime still applies
    const uint32_t expiry = now != 0 ? now + lifetime_s : 0;
    char payload[kUsernameSize + 10];
    const int length = std::snprintf(payload, sizeof(payload), "%s.%08" PRIx32, username, expiry);
    uint8_t mac[kHashSize];
    if (hmac(g.key, sizeof(g.key), payload, static_cast<size_t>(length), mac) != ESP_OK) {
        return false;
    }
    char mac_hex[kHashSize * 2 + 1];
    to_hex(mac, sizeof(mac), mac_hex);
    std::snprintf(out, sizeof(out), "%s.%s", payload, mac_hex);
    return true;
}

bool check_token(const char *token, char (&username)[kUsernameSize])
{
    if (!g.present) {
        return false;
    }
    // username "." expiry(8 hex) "." mac(64 hex)
    const char *last_dot = std::strrchr(token, '.');
    if (last_dot == nullptr || std::strlen(last_dot + 1) != kHashSize * 2) {
        return false;
    }
    const size_t payload_length = static_cast<size_t>(last_dot - token);
    if (payload_length < 10 || payload_length >= kUsernameSize + 9 || token[payload_length - 9] != '.') {
        return false;
    }
    uint8_t mac[kHashSize];
    if (hmac(g.key, sizeof(g.key), token, payload_length, mac) != ESP_OK) {
        return false;
    }
    char mac_hex[kHashSize * 2 + 1];
    to_hex(mac, sizeof(mac), mac_hex);
    if (!equal(reinterpret_cast<const uint8_t *>(mac_hex), reinterpret_cast<const uint8_t *>(last_dot + 1),
               kHashSize * 2)) {
        return false;
    }
    const uint32_t expiry = static_cast<uint32_t>(std::strtoul(token + payload_length - 8, nullptr, 16));
    const uint32_t now = now_seconds();
    if (expiry != 0 && now != 0 && now > expiry) {
        return false;
    }
    const size_t user_length = payload_length - 9;
    std::memcpy(username, token, user_length);
    username[user_length] = 0;
    return std::strcmp(username, g.user) == 0;
}

esp_err_t remove_user()
{
    Settings settings(kNamespace, Settings::Mode::ReadWrite);
    const char *const keys[] = {kKeyUser, kKeySalt, kKeyHash, kKeyIterations};
    for (const char *key : keys) {
        settings.erase(key);
    }
    // the counter stays, so a token of the old login can never match a new one
    g.counter++;
    settings.set_u32(kKeyCounter, g.counter);
    const uint32_t counter = g.counter;
    g = {};
    g.counter = counter;
    ESP_LOGI(kTag, "web UI login removed: setup mode");
    return settings.commit();
}

esp_err_t revoke_all()
{
    ESP_RETURN_ON_FALSE(g.present, ESP_ERR_INVALID_STATE, kTag, "no login");
    g.counter++;
    derive_key();
    return save();
}

} // namespace hc::webserver::detail::auth
