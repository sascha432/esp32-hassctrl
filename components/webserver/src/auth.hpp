#pragma once

// The web UI login (docs/ui/web/web-app.md section 3).
//
//   stored login = PBKDF2-HMAC-SHA256(salt, username + password)
//   signing key  = HMAC(stored login, counter)
//   token        = username "." expiry "." HMAC(signing key, username "." expiry)
//
// A new username or password gives a new stored login, "log out everywhere" increases the
// counter; either way every token issued before fails the check. Nothing but the token lives in
// the browser, and no session table has to survive a restart.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace hc::webserver::detail::auth {

constexpr size_t kUsernameSize = 33;  // 1..32 characters: letters, digits, . _ -
constexpr size_t kMinPassword = 8;
constexpr size_t kMaxPassword = 64;
constexpr size_t kTokenSize = 112;
// "remember me": 90 days (decided); otherwise the cookie ends with the browser session and the
// token after one day
constexpr uint32_t kRememberSeconds = 90 * 24 * 3600;
constexpr uint32_t kSessionSeconds = 24 * 3600;

// Loads the stored login and checks PBKDF2 against a published test vector.
esp_err_t init();

// a login exists; without one the device is in setup mode and the web UI is open
bool has_user();

bool valid_username(const char *username);

// Creates or replaces the login. Ends every token issued before.
esp_err_t set_user(const char *username, const char *password);

bool check_password(const char *username, const char *password);

// a token for the cookie, valid for `lifetime_s` seconds
bool make_token(const char *username, uint32_t lifetime_s, char (&out)[kTokenSize]);

// true when the token is genuine and not expired; its username goes to `username`
bool check_token(const char *token, char (&username)[kUsernameSize]);

// "log out everywhere": ends every token issued before
esp_err_t revoke_all();

// removes the login: the web UI is open again (setup mode) until a new one is created
esp_err_t remove_user();

} // namespace hc::webserver::detail::auth
