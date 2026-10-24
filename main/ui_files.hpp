#pragma once

// The file manager of the screens' Backend (hc/ui/backend.hpp), made of the sdcard component:
// CONFIG_HC_SDCARD (the player is ui_player). Wiring only, like the rest of main.

#include <cstddef>
#include <cstdint>

#include "hc/ui/backend.hpp"
#include "sdkconfig.h"

namespace ui_files {

#if CONFIG_HC_SDCARD
hc::ui::FileResult list(const char *dir, hc::ui::FileEntry *out, size_t max, size_t &count);
hc::ui::FileResult remove(const char *path);
hc::ui::FileResult make_dir(const char *path);
hc::ui::FileResult rename(const char *from, const char *to);
#endif

} // namespace ui_files
