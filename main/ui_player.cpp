#include "ui_player.hpp"

#if CONFIG_HC_UI_PLAYER

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "esp_log.h"
#include "hc/audio/audio.hpp"
#include "hc/core/image.hpp"
#include "hc/hass/hass.hpp"
#include "hc/player/player.hpp"
#include "lvgl.h"

// LVGL's lodepng (CONFIG_LV_USE_PNG, selected by CONFIG_HC_UI_PLAYER): C, and its header declares
// C++ wrappers besides, so the two functions are declared here
extern "C" unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h, const unsigned char *in,
                                     size_t insize);
extern "C" const char *lodepng_error_text(unsigned code);

namespace ui_player {

namespace {

constexpr const char *kTag = "player";
// a folder of the media library comes within this
constexpr uint32_t kBrowseTimeoutMs = 10000;

hc::ui::PlayerState state_of(hc::audio::MediaState state)
{
    switch (state) {
    case hc::audio::MediaState::Idle:
        return hc::ui::PlayerState::Idle;
    case hc::audio::MediaState::Playing:
        return hc::ui::PlayerState::Playing;
    case hc::audio::MediaState::Paused:
        return hc::ui::PlayerState::Paused;
    case hc::audio::MediaState::Failed:
        return hc::ui::PlayerState::Failed;
    case hc::audio::MediaState::Buffering:
        return hc::ui::PlayerState::Buffering;
    case hc::audio::MediaState::Interrupted:
        return hc::ui::PlayerState::Interrupted;
    }
    return hc::ui::PlayerState::Idle;
}

// an MP3 of the media library (the player's format)
bool playable(const hc::core::MediaItem &item)
{
    if (item.folder || !item.playable) {
        return false;
    }
    const size_t length = item.title.size();
    return item.type == "audio/mpeg" || item.type == "audio/mp3" ||
           (length > 4 && strcasecmp(item.title.c_str() + length - 4, ".mp3") == 0);
}

// A PNG as RGB565 (the display's lv_color_t), its alpha over black; the whole picture.
hc::core::Pixels decode_png(const uint8_t *png, size_t size, uint32_t &width, uint32_t &height)
{
    unsigned char *rgba = nullptr;
    unsigned w = 0;
    unsigned h = 0;
    const unsigned error = lodepng_decode32(&rgba, &w, &h, png, size);
    if (error != 0 || rgba == nullptr) {
        ESP_LOGW(kTag, "cover: %s", lodepng_error_text(error));
        lv_mem_free(rgba);
        return nullptr;
    }
    hc::core::Pixels pixels = hc::core::allocate_pixels(static_cast<size_t>(w) * h);
    if (pixels) {
        for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
            const unsigned char *p = rgba + i * 4;
            const unsigned a = p[3];
            pixels.get()[i] = lv_color_make(p[0] * a / 255, p[1] * a / 255, p[2] * a / 255).full;
        }
    }
    lv_mem_free(rgba);
    width = w;
    height = h;
    return pixels;
}

} // namespace

#if CONFIG_HC_SDCARD
bool play_file(const char *path)
{
    return hc::player::play_file(path);
}
#endif

void play_media(const hc::core::MediaList &folder, size_t index)
{
    hc::memory::Vector<hc::player::Item> items;
    size_t start = 0;
    for (size_t i = 0; i < folder.items.size(); i++) {
        const hc::core::MediaItem &entry = folder.items[i];
        if (!playable(entry)) {
            continue;
        }
        if (i == index) {
            start = items.size();
        }
        hc::player::Item item;
        item.origin = hc::player::Origin::Media;
        item.location = entry.id;
        item.title = entry.title;
        // "01 - Engel.mp3" until the tags come
        const size_t dot = item.title.find_last_of('.');
        if (dot != hc::memory::String::npos && dot > 0) {
            item.title.resize(dot);
        }
        items.push_back(std::move(item));
    }
    if (!items.empty()) {
        hc::player::play(std::move(items), start);
    }
}

void toggle()
{
    hc::player::toggle();
}

void seek(uint32_t ms)
{
    hc::player::seek(ms);
}

void next()
{
    hc::player::next();
}

void previous()
{
    hc::player::previous();
}

void stop()
{
    hc::player::stop();
}

void set_shuffle(bool shuffle)
{
    hc::player::set_shuffle(shuffle);
}

void set_repeat(hc::ui::PlayerRepeat repeat)
{
    hc::player::set_repeat(static_cast<hc::player::Repeat>(repeat));
}

void info(hc::ui::PlayerInfo &out)
{
    const hc::player::Status s = hc::player::status();
    out.state = state_of(s.state);
    out.position_ms = s.position_ms;
    out.duration_ms = s.duration_ms;
    out.index = static_cast<uint16_t>(s.index);
    out.count = static_cast<uint16_t>(std::min<size_t>(s.count, UINT16_MAX));
    out.shuffle = s.shuffle;
    out.repeat = static_cast<hc::ui::PlayerRepeat>(s.repeat);
    std::snprintf(out.title, sizeof(out.title), "%s", s.title.c_str());
    std::snprintf(out.artist, sizeof(out.artist), "%s", s.artist.c_str());
    std::snprintf(out.album, sizeof(out.album), "%s", s.album.c_str());
    std::snprintf(out.error, sizeof(out.error), "%s", s.error.c_str());
    out.error_serial = s.error_serial;
    // the tags' cover belongs to what plays; nothing loaded, no cover
    uint32_t serial = 0;
    hc::player::tags(serial);
    out.cover_serial = hc::player::active() ? serial : 0;
}

bool spectrum(float *bands, size_t count)
{
    return hc::audio::media_spectrum(bands, count);
}

bool supports(const char *name)
{
    return hc::player::playable(name);
}

bool is_playlist(const char *name)
{
    return hc::player::is_playlist(name);
}

bool cover(uint16_t size, hc::core::Image &out)
{
    uint32_t serial = 0;
    const auto tags = hc::player::tags(serial);
    if (!hc::player::active() || !tags || !tags->cover.data || size == 0) {
        return false;
    }
    const hc::formats::Cover &cover = tags->cover;
    if (cover.mime == "image/jpeg") {
        return hc::hass::decode_jpeg(cover.data.get(), cover.size, size, size, LV_COLOR_16_SWAP != 0, out);
    }
    if (cover.mime != "image/png") {
        return false;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    const hc::core::Pixels whole = decode_png(cover.data.get(), cover.size, width, height);
    if (!whole) {
        return false;
    }
    hc::core::Pixels pixels = hc::core::allocate_pixels(static_cast<size_t>(size) * size);
    if (!pixels) {
        return false;
    }
    hc::core::cover_copy(whole.get(), width, height, pixels.get(), size, size);
    out.width = size;
    out.height = size;
    out.pixels = std::move(pixels);
    return true;
}

void browse(std::string_view id, uint32_t generation)
{
    hc::hass::browse_media(id, kBrowseTimeoutMs, generation);
}

} // namespace ui_player

#endif
