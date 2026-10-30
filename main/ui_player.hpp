#pragma once

// The media player of the screens' Backend (hc/ui/backend.hpp), made of the player, audio and
// hass components: CONFIG_HC_UI_PLAYER. Wiring only, like the rest of main; the cover is decoded
// here (PNG with LVGL's lodepng, JPEG with the decoders of the camera images).

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hc/ui/backend.hpp"
#include "hc/webserver/webserver.hpp"
#include "sdkconfig.h"

namespace ui_player {

#if CONFIG_HC_UI_PLAYER
#if CONFIG_HC_SDCARD
bool play_file(const char *path);
bool play_folder(const char *path, std::string_view title);
#endif
void play_media(const hc::core::MediaList &folder, size_t index);
bool play_media_folder(std::string_view id, std::string_view title);
void toggle();
void seek(uint32_t ms);
void next();
void previous();
void stop();
void set_shuffle(bool shuffle);
void set_repeat(hc::ui::PlayerRepeat repeat);
void info(hc::ui::PlayerInfo &out);
bool spectrum(float *bands, size_t count);
bool supports(const char *name);
bool is_playlist(const char *name);
bool plays_media(const hc::core::MediaItem &item);
bool is_media_playlist(const hc::core::MediaItem &item);
bool cover(uint16_t size, hc::core::Image &out);
void browse(std::string_view id, uint32_t generation);
// the web UI's Player page (docs/ui/web/web-app.md section 4)
const hc::webserver::PlayerAccess &web_access();
#endif

} // namespace ui_player
