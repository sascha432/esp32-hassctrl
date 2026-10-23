#pragma once

// The file manager and the player of the screens' Backend (hc/ui/backend.hpp), made of the
// sdcard and audio components: CONFIG_HC_SDCARD and CONFIG_HC_UI_PLAYER. Wiring only, like the
// rest of main.

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

#if CONFIG_HC_UI_PLAYER
bool player_open(const char *path);
void player_pause(bool pause);
void player_seek(uint32_t ms);
void player_stop();
void player_info(hc::ui::PlayerInfo &out);
bool player_spectrum(float *bands, size_t count);
bool player_supports(const char *name);
#endif

} // namespace ui_files
