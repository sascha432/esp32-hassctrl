#pragma once

// The dashboard of the device (docs/architecture.md section 5, "Dashboard configuration"): the
// stored file /fs/dashboard.json and the layout built from it.
//
// start() loads the file. A layout never changes once built: a save or an import builds a new one
// and swaps it in, so the UI keeps drawing from the one it took with current(). Every write goes
// to a temporary file first and is renamed over the stored one, so a power loss leaves the old
// dashboard or the new one. Every change has to name the revision it was based on (save_json), so
// a page that is out of date gets a conflict instead of overwriting a newer dashboard; an import
// replaces the dashboard as a whole.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "esp_err.h"
#include "hc/config/dashboard.hpp"
#include "hc/core/layout.hpp"

namespace hc::dashboard {

struct Status {
    bool present;      // a dashboard is loaded
    uint32_t revision; // 0 without one
    uint32_t pages;
    uint32_t tiles;
    // why the stored file could not be used, or why the file system is not available; empty when
    // there is no problem or no file
    char error[160];
};

// Loads /fs/dashboard.json when it exists. A missing file is not an error; a file that cannot be
// used is reported in status() and left alone on the file system.
esp_err_t start();

Status status();

// The current layout, null without a dashboard. Safe to keep: it is not changed afterwards.
std::shared_ptr<const core::Layout> current();

// The dashboard as JSON or as the YAML of the backup, in a buffer of the large heap; null without
// a dashboard. Free it with release().
char *export_json(size_t &length);
char *export_yaml(size_t &length);
void release(char *text);

enum class Result : uint8_t {
    Ok,
    Conflict, // the stored dashboard is newer than the revision the change was based on
    Invalid,  // the text is not a valid dashboard; `error` says where
    Failed,   // the file system or the memory failed
};

// Replaces the dashboard with the one in `json` (the format of export_json) if the stored
// revision is `expected_revision` (0 when there is none). `revision` is the new one.
Result save_json(std::string_view json, uint32_t expected_revision, uint32_t &revision, char *error,
                 size_t error_size);

struct ImportReport {
    uint32_t revision; // the new revision, 0 for "check only"
    uint32_t pages;
    uint32_t tiles;
    // top-level keys of the main file that are not the dashboard's ("hass"), comma separated
    char other_keys[96];
};

// Reads `hass.yaml` and what it includes from the uploaded files and replaces the dashboard with
// the result, or only checks the files when `check_only` is set.
Result import_yaml(std::span<const config::File> files, bool check_only, ImportReport &report, char *error,
                   size_t error_size);

} // namespace hc::dashboard
