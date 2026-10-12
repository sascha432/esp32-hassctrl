#pragma once

// The backup file of the main firmware (hc/config/backup.hpp): the sections network, display,
// sound, alarm, camera, hass and webui (import only), and the dashboard, for the web server. Wiring only, like the rest of main.

#include <string_view>

#include "hc/config/backup.hpp"
#include "hc/webserver/webserver.hpp"

const hc::webserver::BackupAccess &backup_access();

// The UI runs: from now on an import changes its settings under the LVGL lock and applies them
// at once (hc::ui::settings_imported).
void backup_ui_started();

// The settings sections in the main file of a dashboard import (hass.yaml with `hass:`,
// `camera:`, ...), read like the backup file's: checked with `apply` false, applied with true.
hc::config::backup::Report import_dashboard_settings(std::string_view text, bool apply);
