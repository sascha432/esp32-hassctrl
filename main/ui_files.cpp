#include "ui_files.hpp"

#include <cstdio>
#include <cstring>
#include <strings.h>

#if CONFIG_HC_SDCARD
#include "hc/sdcard/sdcard.hpp"
#endif
#if CONFIG_HC_UI_PLAYER
#include "hc/audio/audio.hpp"
#endif

namespace ui_files {

#if CONFIG_HC_SDCARD
namespace {

using hc::ui::FileResult;

FileResult result_of(hc::sdcard::Result result)
{
    switch (result) {
    case hc::sdcard::Result::Ok:
        return FileResult::Ok;
    case hc::sdcard::Result::NoCard:
        return FileResult::NoCard;
    case hc::sdcard::Result::BadPath:
        return FileResult::BadName;
    case hc::sdcard::Result::NotFound:
        return FileResult::NotFound;
    case hc::sdcard::Result::Exists:
        return FileResult::Exists;
    case hc::sdcard::Result::NotEmpty:
        return FileResult::NotEmpty;
    case hc::sdcard::Result::Failed:
        break;
    }
    return FileResult::Failed;
}

struct Listing {
    hc::ui::FileEntry *out;
    size_t max;
    size_t count;
};

} // namespace

FileResult list(const char *dir, hc::ui::FileEntry *out, size_t max, size_t &count)
{
    Listing listing{out, max, 0};
    const hc::sdcard::Result r = hc::sdcard::list(
        dir,
        [](const hc::sdcard::Entry &entry, void *context) {
            auto &l = *static_cast<Listing *>(context);
            if (l.count < l.max) {
                hc::ui::FileEntry &e = l.out[l.count];
                std::snprintf(e.name, sizeof(e.name), "%s", entry.name);
                e.size = entry.size;
                e.dir = entry.dir;
            }
            l.count++;
            return true;
        },
        &listing);
    count = listing.count;
    return result_of(r);
}

FileResult remove(const char *path)
{
    return result_of(hc::sdcard::remove(path));
}

FileResult make_dir(const char *path)
{
    return result_of(hc::sdcard::make_dir(path));
}

FileResult rename(const char *from, const char *to)
{
    return result_of(hc::sdcard::rename(from, to));
}
#endif

#if CONFIG_HC_UI_PLAYER
bool player_open(const char *path)
{
    char full[hc::sdcard::kMaxPath];
    return hc::sdcard::make_path(path, full, sizeof(full)) && hc::audio::play_media(full);
}

void player_pause(bool pause)
{
    hc::audio::pause_media(pause);
}

void player_seek(uint32_t ms)
{
    hc::audio::seek_media(ms);
}

void player_stop()
{
    hc::audio::stop_media();
}

void player_info(hc::ui::PlayerInfo &out)
{
    const hc::audio::MediaStatus s = hc::audio::media_status();
    switch (s.state) {
    case hc::audio::MediaState::Idle:
        out.state = hc::ui::PlayerState::Idle;
        break;
    case hc::audio::MediaState::Playing:
        out.state = hc::ui::PlayerState::Playing;
        break;
    case hc::audio::MediaState::Paused:
        out.state = hc::ui::PlayerState::Paused;
        break;
    case hc::audio::MediaState::Failed:
        out.state = hc::ui::PlayerState::Failed;
        break;
    }
    out.position_ms = s.position_ms;
    out.duration_ms = s.duration_ms;
}

bool player_spectrum(float *bands, size_t count)
{
    return hc::audio::media_spectrum(bands, count);
}

bool player_supports(const char *name)
{
    const size_t length = std::strlen(name);
    const char *extension = length > 4 ? name + length - 4 : "";
#if CONFIG_HC_AUDIO_MP3
    if (strcasecmp(extension, ".mp3") == 0) {
        return true;
    }
#endif
    return strcasecmp(extension, ".wav") == 0;
}
#endif

} // namespace ui_files
