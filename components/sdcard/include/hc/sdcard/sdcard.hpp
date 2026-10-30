#pragma once

// The microSD card (docs/architecture.md section 9.3), compiled in with CONFIG_HC_SDCARD: FAT at
// /sd on SDMMC slot 0, the pins from hc::board::Info::sd_*. The SDMMC controller is shared with
// the ESP32-C6's SDIO link (esp_hosted, slot 1): ESP-IDF creates it once for both and removes it
// with the last slot, so mounting and unmounting the card leave Wi-Fi alone.
//
// The card is mounted at boot and on request (console `sd mount`, the web UI): the slot has no
// card-detect pin, so a card put in later is not seen until then. A card without a FAT is
// reported and only formatted on request, never by itself.
//
// Files are read and written with the C library under kMountPoint, from any task; mount, unmount
// and format take a lock and wait for each other.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "hc/board/board.hpp"

namespace hc::sdcard {

constexpr const char *kMountPoint = "/sd";
// a path under kMountPoint, terminated (make_path)
constexpr size_t kMaxPath = 128;

enum class State : uint8_t {
    // the slot did not start (no power, no pins)
    Off,
    // no card answered: none in the slot, or one that does not work
    NoCard,
    // a card without a FAT (or a damaged one): format() makes it usable
    Unformatted,
    Mounted,
    // unmounted on request
    Unmounted,
};

struct Status {
    State state;
    uint64_t total_bytes;
    uint64_t free_bytes;
    // the card's product name and clock, while mounted
    char name[8];
    uint32_t clock_khz;
};

// Powers the slot and mounts the card. Call once, after hc::board::init. ESP_OK also without a
// card (the state says it); an error when the slot itself does not start.
esp_err_t start(const board::Info &board);

// The state and the space; the free space is counted once after mounting (seconds on a large
// card with no FSInfo), then FAT keeps it.
Status status();
const char *state_name(State state);
// "7.4 of 29.0 GB free" (MB below 1 GB), or the state's name when no card is mounted
void describe(const Status &status, char *out, size_t size);

// Mounts the card once more (after unmount, or a card put in later). ESP_ERR_NOT_FOUND: no card,
// ESP_ERR_INVALID_STATE: it has no FAT.
esp_err_t mount();
esp_err_t unmount();

// Erases the card and writes an empty FAT (the web UI asks for the password first). Also for a
// card without one. Mounted afterwards.
esp_err_t format();

// "/sd/<relative>" in `out` (kMaxPath) for a path from the web UI or the console: relative to the
// card, '/' between names, no "..", no backslash, no control characters. A leading '/' is taken
// away; "" is the card's root. False when the path is not allowed or too long.
bool make_path(const char *relative, char *out, size_t size);

// ------------------------------------------------------------------------------ the files
// For the web UI, the screen's file manager and the console; the paths are relative (make_path).

enum class Result : uint8_t {
    Ok,
    NoCard,   // no card mounted
    BadPath,  // make_path refused it, or the card's root
    NotFound, // the file, or the folder it goes into
    Exists,   // the new name is taken
    NotEmpty, // a folder with something in it
    Failed,
};

struct Entry {
    const char *name;
    uint64_t size; // 0 for a folder
    bool dir;
};

// Every entry of the folder `dir` ("" is the root), in the order of the FAT; `add` returns false
// to stop. Without `sizes` every size is 0: no stat() per entry, which on FAT searches the folder
// again (a large folder's list goes from n² to n).
Result list(const char *dir, bool (*add)(const Entry &entry, void *context), void *context, bool sizes = true);
// a file, or an empty folder
Result remove(const char *path);
Result make_dir(const char *path);
// Renames or moves `from` to `to` (another folder in `to` moves it); never over an existing name,
// a folder not into itself.
Result rename(const char *from, const char *to);

// ----------------------------------------------------------------------- download from a URL

enum class FetchState : uint8_t { Idle, Running, Done, Failed };

struct FetchStatus {
    FetchState state;
    size_t done;
    size_t total; // 0: the server did not say
    char path[kMaxPath];
    char error[64];
};

// Downloads http:// or https:// `url` (IDF certificate bundle, up to 5 redirects) into `path`
// (make_path's form) in a task of its own. It goes to "<path>.part" first and is renamed when it
// is complete; a failed download leaves nothing. ESP_ERR_INVALID_STATE while one runs or no card
// is mounted, ESP_ERR_INVALID_ARG for a URL or path that is not allowed.
esp_err_t fetch_start(const char *url, const char *path);
FetchStatus fetch_status();

// Writes 16 bit mono samples as a WAV file (the microphone's recording).
esp_err_t save_wav(const char *path, const int16_t *samples, size_t count, uint32_t sample_rate);

// The microphone's test recording as /sd/recordings/rec-<local date and time>.wav (rec-<uptime
// ms>.wav while the clock is not set); its path in `path` (kMaxPath). ESP_ERR_INVALID_STATE
// without a mounted card.
esp_err_t save_recording(const int16_t *samples, size_t count, uint32_t sample_rate, char *path, size_t size);

// The console command `sd` (CONFIG_HC_DEV_CONSOLE): status, ls, rm, mkdir, mv, mount, unmount,
// format.
void register_console();

} // namespace hc::sdcard
