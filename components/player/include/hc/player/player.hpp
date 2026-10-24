#pragma once

// The media player (docs/architecture.md section 9.6, CONFIG_HC_UI_PLAYER): what plays, in the
// background and independent of any screen. A queue of items from the SD card (a folder from a
// file on, a playlist) or from Home Assistant's media library (a folder from a file on), with
// next, previous, shuffle and repeat; the audio task plays them (audio::play_media for a file,
// audio::play_stream with a download of hass::stream_media for the media library). When an item
// ends the next one starts, also with no screen open; an item that cannot be played is skipped
// with a message, three in a row stop the queue.
//
// Any task; the calls return at once (a download starts in a task of its own).

#include <cstddef>
#include <cstdint>
#include <memory>

#include "hc/audio/audio.hpp"
#include "hc/formats/mp3_tags.hpp"
#include "hc/player/queue.hpp"

namespace hc::player {

// Restores shuffle and repeat and hooks into the audio task. Once, after audio::start().
void start();

// Plays `items` from `index` on; the queue is replaced.
void play(memory::Vector<Item> items, size_t index);

// A file on the card (relative to it): its folder's playable files from it on, or a playlist's
// entries (.m3u, .m3u8). False when it is neither or the card cannot be read.
bool play_file(const char *path);

// the name is a file the player plays: .mp3, .wav, or a playlist
bool playable(const char *name);
bool is_playlist(const char *name);

void pause(bool pause);
// Playing <-> paused; a queue that ended or was stopped starts its item again.
void toggle();
void seek(uint32_t ms);
void next();
// to the start of the item after its first 3 s, else the one before
void previous();
void stop();
void set_shuffle(bool shuffle);
void set_repeat(Repeat repeat);

struct Status {
    audio::MediaState state = audio::MediaState::Idle;
    uint32_t position_ms = 0;
    uint32_t duration_ms = 0;
    // the item in the play order (0-based), and how many there are; 0: no queue
    size_t index = 0;
    size_t count = 0;
    bool shuffle = false;
    Repeat repeat = Repeat::Off;
    Origin origin = Origin::File;
    // the tags' title, else what the list showed, else the file's name
    memory::String title;
    memory::String artist;
    memory::String album;
    // what went wrong last (an item skipped), and a counter of such messages
    memory::String error;
    uint32_t error_serial = 0;
};
Status status();

// something is loaded: playing, paused, waiting for its download or interrupted
bool active();

// The tags of what plays, with its cover (audio::media_tags); null before they were read.
std::shared_ptr<const formats::Mp3Tags> tags(uint32_t &serial);

} // namespace hc::player
