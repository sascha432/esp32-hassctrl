#pragma once

// The web server of the device (docs/ui/web.md, docs/architecture.md section 9.4).
//
// HTTP on port 80, on the Wi-Fi and the hotspot. On the hotspot it is a captive portal: a DNS
// responder answers every name with the hotspot address, the hotspot's DHCP announces the portal
// address (option 114), and a request for any other host is redirected to the portal, which is
// what makes a phone open the setup page by itself.
//
// Static files come from LittleFS (/fs/www, `file.gz` is served gzip encoded). Without them the
// built-in page is served.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "esp_err.h"
#include "hc/memory/large.hpp"

namespace hc::webserver {

// The Home Assistant connection as the web UI sees it.
struct HassStatus {
    // not_set_up, waiting_for_network, connecting, connected, token_rejected, disconnected
    const char *state;
    char url[128];
    bool token_set;
    char version[24];
    char error[96];
    uint32_t retry_in_s;
    uint32_t connected_s;
    // SHA-256 as hex: the certificate trusted on first use, and the one the last try refused;
    // empty: none
    char certificate[65];
    char refused_certificate[65];
};

// a Home Assistant instance found on the network
struct HassFound {
    char name[48];
    char url[128];
    char version[24];
};

// What the web server needs of the Home Assistant connection, filled in `main` (the web server
// does not depend on it: the recovery app has none and passes nullptr).
struct HassAccess {
    void (*status)(HassStatus &out);
    // stores the URL and, unless empty, the token, and connects; an empty URL removes both
    esp_err_t (*set)(const char *url, const char *token, char *error, size_t error_size);
    void (*reconnect)();
    // trusts the certificate with this SHA-256 besides the bundle and connects; empty forgets it
    esp_err_t (*trust)(const char *fingerprint, char *error, size_t error_size);
    // searches the network (mDNS) for about 2 s, blocking
    size_t (*discover)(HassFound *out, size_t max);
    // The entity picker of the editor (docs/ui/web-dashboard.md section 2.3), blocking for a few
    // seconds at most: Home Assistant's answer as JSON in a buffer of the large heap
    // (memory::allocate_large, the web server frees it), null with `error` set. `entities`: the
    // entities of `domain` (empty: all) whose id or name contains `query`; `entity`: the details of
    // one, `entity_id` is valid.
    char *(*entities)(const char *domain, const char *query, size_t &length, char *error, size_t error_size);
    char *(*entity)(const char *entity_id, size_t &length, char *error, size_t error_size);
};

// One uploaded file of a dashboard import: its path inside the folder and its text.
struct DashboardFile {
    const char *path;
    size_t path_length;
    const char *text;
    size_t text_length;
};

enum class DashboardResult : uint8_t {
    Ok,
    Conflict, // 409: the dashboard changed since the revision of the request
    Invalid,  // 400: the text is not a valid dashboard, `error` says where
    Failed,   // 500: the file system or the memory failed
};

// What a save or a check of the dashboard in JSON answers.
struct DashboardSaved {
    uint32_t revision;
    // where the tile of a problem stands: its index in each level of tile lists; 0 entries: none
    uint16_t path[8];
    uint8_t path_length;
    // Ok: the dashboard as stored (or checked), with the ids of new tiles and the cells of every
    // tile; free it with `release`
    char *json;
    size_t json_length;
};

struct DashboardInfo {
    uint32_t revision;
    uint32_t pages;
    uint32_t tiles;
    // top-level keys of an import that the dashboard does not read ("hass"), comma separated
    char other_keys[96];
};

// What the web server needs of the dashboard, filled in `main` (the recovery app has no
// dashboard and passes nullptr). Texts are in buffers of the large heap, freed with `release`.
struct DashboardAccess {
    // the stored dashboard as JSON or YAML; null without one, `error` is then set when the stored
    // file could not be used
    char *(*export_json)(size_t &length, char *error, size_t error_size);
    char *(*export_yaml)(size_t &length);
    void (*release)(char *text);
    // replaces the dashboard if the stored revision is `expected_revision` (0 without one), or
    // only checks it
    DashboardResult (*save_json)(const char *text, size_t length, uint32_t expected_revision, bool check_only,
                                 DashboardSaved &out, char *error, size_t error_size);
    // replaces the dashboard with the import of the files, or only checks them
    DashboardResult (*import_yaml)(const DashboardFile *files, size_t count, bool check_only, DashboardInfo &out,
                                   char *error, size_t error_size);
    // the icon names a tile may use (besides "none"), nullptr after the last
    const char *(*icon_name)(size_t index);
    // the screen the dashboard is drawn on, in landscape, and the gap between its tiles (the
    // editor's preview is this screen)
    void (*screen)(const char *&name, uint16_t &width, uint16_t &height, uint8_t &tile_gap);
};

// What a backup import found and did, one line of text each (hc/config/backup.hpp).
struct BackupReport {
    // the import is refused when there is one
    memory::Vector<memory::String> problems;
    // unknown sections and fields
    memory::Vector<memory::String> ignored;
    // what the user should know ("network: Office needs its password")
    memory::Vector<memory::String> notes;
    // the sections that were in the file
    memory::Vector<memory::String> sections;
};

// The backup of the settings and the dashboard in one YAML file, filled in `main` (the recovery
// app has none and passes nullptr; it gets the device back on the network, the import follows in
// the main firmware).
struct BackupAccess {
    // the file with the settings, the dashboard or both
    memory::String (*export_file)(bool settings, bool dashboard);
    // checks `text` and, unless `check_only`, imports it when the check found no problem
    BackupReport (*import_file)(std::string_view text, bool check_only);
};

struct Config {
    const char *device_name;
    const HassAccess *hass = nullptr;
    const DashboardAccess *dashboard = nullptr;
    const BackupAccess *backup = nullptr;
    // registers more routes on the server (an httpd_handle_t), before the files take the rest:
    // the camera's routes (components/camera); up to 2
    void (*add_routes)(void *server) = nullptr;
};

// Starts the server and the DNS responder. The network component has to be started.
esp_err_t start(const Config &config);

// Removes the web UI login (Settings on the screen, docs/ui/screen.md section 5): the web UI is
// open in setup mode until a new login is created.
esp_err_t reset_login();

} // namespace hc::webserver
