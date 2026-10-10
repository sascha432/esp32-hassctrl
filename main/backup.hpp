#pragma once

// The backup file of the main firmware (hc/config/backup.hpp): the sections network, display,
// sound, alarm and hass, and the dashboard, for the web server. Wiring only, like the rest of main.

#include "hc/webserver/webserver.hpp"

const hc::webserver::BackupAccess &backup_access();

// The UI runs: from now on an import changes its settings under the LVGL lock and applies them
// at once (hc::ui::settings_imported).
void backup_ui_started();
