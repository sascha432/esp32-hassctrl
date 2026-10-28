#include "hc/player/player.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <strings.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/core/natural.hpp"
#include "hc/formats/m3u.hpp"
#include "hc/hass/hass.hpp"
#include "hc/platform/settings.hpp"
#include "hc/player/tree.hpp"
#if CONFIG_HC_SDCARD
#include <fcntl.h>
#include <unistd.h>

#include "hc/sdcard/sdcard.hpp"
#endif

namespace hc::player {

namespace {

constexpr const char *kTag = "player";
constexpr const char *kNamespace = "ui";
constexpr const char *kKey = "player";
// What a download holds ahead of the player, PSRAM: 32 s at 256 kbit/s. It comes in bursts of
// half of it (hass/src/media_audio.cpp), one every 16 s at that rate.
constexpr size_t kRingSize = 1024 * 1024;
// previous() restarts the item after this much of it
constexpr uint32_t kRestartMs = 3000;
// this many items in a row that cannot be played stop the queue
constexpr int kMaxFailures = 3;
// a playlist larger than this is cut
constexpr size_t kMaxPlaylist = 64 * 1024;
// the most files of a folder in the queue, also of a folder with its subfolders
constexpr size_t kMaxFolder = 1000;
// a folder of the media library comes within this
constexpr uint32_t kBrowseTimeoutMs = 10000;
// the folders of the media library a folder's play button lists at most: one request each
constexpr size_t kMaxMediaFolders = 100;
// a playlist of the media library comes within this
constexpr TickType_t kPlaylistTimeout = pdMS_TO_TICKS(15000);
constexpr TickType_t kPlaylistPoll = pdMS_TO_TICKS(50);
constexpr const char *kMediaScheme = "media-source://";
// the task that collects a folder with its subfolders or a playlist of the media library; PSRAM
constexpr uint32_t kWalkStack = 8192;

struct Stored {
    uint8_t shuffle;
    uint8_t repeat;
};

std::mutex g_mutex;
// under g_mutex
Queue g_queue;
bool g_shuffle = false;
Repeat g_repeat = Repeat::Off;
int g_failures = 0;
memory::String g_error;
uint32_t g_error_serial = 0;
// a folder with its subfolders is being collected (play_folder, play_media_folder): its title; a
// newer one, play() or stop() makes the older one's result void
memory::String g_collecting;
uint32_t g_walk = 0;

// the folder's items are still collected: the queue is the old one, the buttons wait
bool collecting()
{
    std::lock_guard lock(g_mutex);
    return !g_collecting.empty();
}

void store()
{
    const Stored stored{static_cast<uint8_t>(g_shuffle), static_cast<uint8_t>(g_repeat)};
    platform::set_blob_later(kNamespace, kKey, &stored, sizeof(stored));
}

// "music/01 - Engel.mp3" -> "01 - Engel"
memory::String name_of(const memory::String &location)
{
    const size_t slash = location.find_last_of('/');
    memory::String name = slash == memory::String::npos ? location : location.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != memory::String::npos && dot > 0) {
        name.resize(dot);
    }
    return name;
}

void report(const char *what, const Item &item)
{
    g_error = memory::String(what) + ": " + (item.title.empty() ? name_of(item.location) : item.title);
    g_error_serial++;
    ESP_LOGW(kTag, "%s", g_error.c_str());
}

// Starts the queue's current item, under g_mutex; false when it cannot (the item is reported).
bool start_current()
{
    const Item *item = g_queue.current();
    if (item == nullptr) {
        return false;
    }
    ESP_LOGI(kTag, "%u/%u: %s", static_cast<unsigned>(g_queue.position() + 1), static_cast<unsigned>(g_queue.size()),
             item->location.c_str());
    if (audio::volume() == 0) {
        report("the volume is 0", *item);
        return false;
    }
    if (item->origin == Origin::File) {
#if CONFIG_HC_SDCARD
        char path[sdcard::kMaxPath];
        if (sdcard::make_path(item->location.c_str(), path, sizeof(path)) && audio::play_media(path)) {
            return true;
        }
#endif
        report("cannot be played", *item);
        return false;
    }
    auto ring = std::make_shared<core::ByteRing>(kRingSize);
    if (!ring->valid()) {
        report("no memory for the download", *item);
        return false;
    }
    if (!hass::stream_media(item->location, ring) || !audio::play_stream(ring)) {
        ring->cancel();
        report("cannot be played", *item);
        return false;
    }
    return true;
}

// Starts the current item, or the next ones while they fail; under g_mutex.
void start_from_current()
{
    while (!start_current()) {
        if (++g_failures >= kMaxFailures || !g_queue.next(g_repeat)) {
            ESP_LOGW(kTag, "stopped");
            return;
        }
    }
}

// the audio task: an item ended by itself
void on_ended(bool failed)
{
    std::lock_guard lock(g_mutex);
    if (failed) {
        if (const Item *item = g_queue.current()) {
            report("cannot be played", *item);
        }
        if (++g_failures >= kMaxFailures) {
            ESP_LOGW(kTag, "stopped after %d items that failed", g_failures);
            return;
        }
        if (g_queue.next(g_repeat)) {
            start_from_current();
        }
        return;
    }
    g_failures = 0;
    if (g_queue.advance(g_repeat)) {
        start_from_current();
    }
}

bool has_extension(const char *name, const char *extension)
{
    const size_t length = std::strlen(name);
    const size_t wanted = std::strlen(extension);
    return length > wanted && strcasecmp(name + length - wanted, extension) == 0;
}

#if CONFIG_HC_SDCARD
struct FolderList {
    memory::Vector<Item> items;
    const char *folder;
};

bool add_file(const sdcard::Entry &entry, void *context)
{
    auto &list = *static_cast<FolderList *>(context);
    if (!entry.dir && playable(entry.name) && !is_playlist(entry.name)) {
        Item item;
        item.origin = Origin::File;
        item.location = list.folder[0] != 0 ? memory::String(list.folder) + "/" + entry.name : memory::String(entry.name);
        item.title = name_of(item.location);
        list.items.push_back(std::move(item));
    }
    return list.items.size() < kMaxFolder;
}

// the entries of the playlist `path` (relative to the card)
bool read_playlist(const char *path, memory::Vector<Item> &items)
{
    char full[sdcard::kMaxPath];
    if (!sdcard::make_path(path, full, sizeof(full))) {
        return false;
    }
    const int file = ::open(full, O_RDONLY);
    if (file < 0) {
        return false;
    }
    memory::String text(kMaxPlaylist, '\0');
    const ssize_t got = ::read(file, text.data(), kMaxPlaylist);
    ::close(file);
    if (got <= 0) {
        return false;
    }
    text.resize(static_cast<size_t>(got));
    const std::string_view relative(path);
    const size_t slash = relative.find_last_of('/');
    const auto entries =
        formats::parse_m3u(text, slash == std::string_view::npos ? std::string_view() : relative.substr(0, slash));
    for (const auto &entry : entries) {
        Item item;
        item.origin = entry.address ? Origin::Address : Origin::File;
        item.location = entry.location;
        item.title = entry.title.empty() ? name_of(entry.location) : entry.title;
        items.push_back(std::move(item));
    }
    return !items.empty();
}

bool play_playlist(const char *path)
{
    memory::Vector<Item> items;
    if (!read_playlist(path, items)) {
        return false;
    }
    play(std::move(items), 0);
    return true;
}

struct TreeList {
    memory::Vector<TreeEntry> *out;
    const memory::String *folder;
};

bool add_tree_entry(const sdcard::Entry &entry, void *context)
{
    auto &list = *static_cast<TreeList *>(context);
    if (!entry.dir && !playable(entry.name)) {
        return true;
    }
    TreeEntry one;
    one.id = list.folder->empty() ? memory::String(entry.name) : *list.folder + "/" + entry.name;
    // a path the card cannot open is left out
    if (one.id.size() + std::strlen(sdcard::kMountPoint) + 1 >= sdcard::kMaxPath) {
        return true;
    }
    one.name = entry.name;
    one.dir = entry.dir;
    one.playlist = !entry.dir && is_playlist(entry.name);
    list.out->push_back(std::move(one));
    return list.out->size() < kMaxFolder;
}

memory::Vector<Item> collect_sd(const memory::String &folder)
{
    return collect_tree(
        folder,
        [](const memory::String &id, memory::Vector<TreeEntry> &out) {
            TreeList list{&out, &id};
            return sdcard::list(id.c_str(), add_tree_entry, &list) == sdcard::Result::Ok;
        },
        [](const memory::String &id, memory::Vector<Item> &out) { return read_playlist(id.c_str(), out); },
        [](const TreeEntry &entry) {
            Item item;
            item.origin = Origin::File;
            item.location = entry.id;
            item.title = name_of(entry.id);
            return item;
        });
}
#endif

// The entries of the media library's playlist `id`, downloaded as a file that plays (blocks the
// calling task): a path relative to the playlist's folder is an id of the library, an address
// streams as it is.
bool read_media_playlist(const memory::String &id, memory::Vector<Item> &items)
{
    auto ring = std::make_shared<core::ByteRing>(kMaxPlaylist);
    if (!ring->valid() || !hass::stream_media(id, ring)) {
        return false;
    }
    memory::String text;
    const TickType_t start = xTaskGetTickCount();
    char buffer[512];
    while (!ring->ended() && text.size() < kMaxPlaylist && xTaskGetTickCount() - start < kPlaylistTimeout) {
        const size_t got = ring->read(reinterpret_cast<uint8_t *>(buffer), sizeof(buffer));
        if (got == 0) {
            vTaskDelay(kPlaylistPoll);
        }
        text.append(buffer, got);
    }
    const bool failed = ring->failed() || text.empty();
    ring->cancel();
    if (failed) {
        ESP_LOGW(kTag, "%s: cannot be read", id.c_str());
        return false;
    }
    // "media-source://media_source/local/music/x.m3u": the folder without the scheme, which the
    // parser would take for a path; it comes back in front of each path
    std::string_view folder(id);
    if (folder.rfind(kMediaScheme, 0) == 0) {
        folder.remove_prefix(std::strlen(kMediaScheme));
    }
    const size_t slash = folder.find_last_of('/');
    folder = slash == std::string_view::npos ? std::string_view() : folder.substr(0, slash);
    for (const auto &entry : formats::parse_m3u(text, folder)) {
        Item item;
        item.origin = entry.address ? Origin::Address : Origin::Media;
        item.location = entry.address ? entry.location : kMediaScheme + entry.location;
        item.title = entry.title.empty() ? name_of(entry.location) : entry.title;
        items.push_back(std::move(item));
    }
    return !items.empty();
}

memory::Vector<Item> collect_media(const memory::String &folder)
{
    TreeLimits limits;
    limits.items = kMaxFolder;
    limits.folders = kMaxMediaFolders;
    return collect_tree(
        folder,
        [](const memory::String &id, memory::Vector<TreeEntry> &out) {
            core::MediaList list;
            memory::String error;
            if (!hass::browse_media(id, kBrowseTimeoutMs, list, error)) {
                ESP_LOGW(kTag, "%s: %s", id.c_str(), error.c_str());
                return false;
            }
            for (const core::MediaItem &entry : list.items) {
                const bool playlist = is_media_playlist(entry);
                if (!entry.folder && !playlist && !playable_media(entry)) {
                    continue;
                }
                TreeEntry one;
                one.id = entry.id;
                one.name = entry.title;
                one.dir = entry.folder;
                one.playlist = playlist;
                out.push_back(std::move(one));
            }
            return true;
        },
        [](const memory::String &id, memory::Vector<Item> &out) { return read_media_playlist(id, out); },
        [](const TreeEntry &entry) {
            Item item;
            item.origin = Origin::Media;
            item.location = entry.id;
            // "01 - Engel.mp3" until the tags come
            item.title = name_of(entry.name);
            return item;
        },
        limits);
}

// what the walk task collects
enum class Walked : uint8_t { SdFolder, MediaFolder, MediaPlaylist };

struct Walk {
    Walked what;
    memory::String folder;
    uint32_t serial;
};

memory::Vector<Item> collect(const Walk &walk)
{
    memory::Vector<Item> items;
    switch (walk.what) {
    case Walked::SdFolder:
#if CONFIG_HC_SDCARD
        items = collect_sd(walk.folder);
#endif
        break;
    case Walked::MediaFolder:
        items = collect_media(walk.folder);
        break;
    case Walked::MediaPlaylist:
        read_media_playlist(walk.folder, items);
        break;
    }
    return items;
}

void walk_task(void *arg)
{
    std::unique_ptr<Walk> walk(static_cast<Walk *>(arg));
    memory::Vector<Item> items = collect(*walk);
    {
        std::lock_guard lock(g_mutex);
        if (walk->serial == g_walk) {
            if (items.empty()) {
                g_error = "Nothing to play in " + g_collecting;
                g_error_serial++;
                ESP_LOGW(kTag, "%s", g_error.c_str());
                g_collecting.clear();
            } else {
                ESP_LOGI(kTag, "%u items under %s", static_cast<unsigned>(items.size()), walk->folder.c_str());
                g_collecting.clear();
                g_queue.set(std::move(items), 0, g_shuffle, esp_random());
                g_failures = 0;
                start_from_current();
            }
        }
    }
    walk.reset();
    vTaskDeleteWithCaps(nullptr);
}

bool start_walk(Walked what, std::string_view folder, std::string_view title)
{
    auto *walk = new Walk{what, memory::String(folder), 0};
    {
        std::lock_guard lock(g_mutex);
        walk->serial = ++g_walk;
        g_collecting = title.empty() ? memory::String(folder) : memory::String(title);
        if (g_collecting.empty()) {
            g_collecting = "the folder";
        }
    }
    // what played stops: the new queue starts once it is collected
    audio::stop_media();
    if (xTaskCreateWithCaps(walk_task, "player_walk", kWalkStack, walk, 3, nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
        delete walk;
        std::lock_guard lock(g_mutex);
        g_collecting.clear();
        return false;
    }
    return true;
}

} // namespace

void start()
{
    platform::Settings settings(kNamespace, platform::Settings::Mode::Read);
    Stored stored{};
    size_t size = sizeof(stored);
    if (settings.ok() && settings.get_blob(kKey, &stored, size) == ESP_OK && size == sizeof(stored)) {
        g_shuffle = stored.shuffle != 0;
        g_repeat = stored.repeat <= static_cast<uint8_t>(Repeat::One) ? static_cast<Repeat>(stored.repeat) : Repeat::Off;
    }
    audio::set_media_ended(on_ended);
}

void play(memory::Vector<Item> items, size_t index)
{
    std::lock_guard lock(g_mutex);
    // a folder still collected does not replace this
    g_walk++;
    g_collecting.clear();
    g_queue.set(std::move(items), index, g_shuffle, esp_random());
    g_failures = 0;
    start_from_current();
}

bool playable(const char *name)
{
    return has_extension(name, ".mp3") || has_extension(name, ".wav") || is_playlist(name);
}

bool is_playlist(const char *name)
{
    return has_extension(name, ".m3u") || has_extension(name, ".m3u8");
}

bool playable_media(const core::MediaItem &item)
{
    if (item.folder || !item.playable) {
        return false;
    }
    return item.type == "audio/mpeg" || item.type == "audio/mp3" || has_extension(item.title.c_str(), ".mp3");
}

bool is_media_playlist(const core::MediaItem &item)
{
    if (item.folder || !item.playable) {
        return false;
    }
    // audio/x-mpegurl, audio/mpegurl, application/vnd.apple.mpegurl
    return item.type.find("mpegurl") != memory::String::npos || is_playlist(item.title.c_str());
}

bool play_folder(const char *path, std::string_view title)
{
#if CONFIG_HC_SDCARD
    char full[sdcard::kMaxPath];
    if (!sdcard::make_path(path, full, sizeof(full))) {
        return false;
    }
    // relative to the card as the walk joins it: "/music/" -> "music"
    std::string_view folder(path);
    while (!folder.empty() && folder.front() == '/') {
        folder.remove_prefix(1);
    }
    while (!folder.empty() && folder.back() == '/') {
        folder.remove_suffix(1);
    }
    return start_walk(Walked::SdFolder, folder, title);
#else
    (void)path;
    (void)title;
    return false;
#endif
}

bool play_media_folder(std::string_view id, std::string_view title)
{
    return start_walk(Walked::MediaFolder, id, title);
}

bool play_media_playlist(std::string_view id, std::string_view title)
{
    return start_walk(Walked::MediaPlaylist, id, title);
}

bool play_file(const char *path)
{
#if CONFIG_HC_SDCARD
    if (is_playlist(path)) {
        return play_playlist(path);
    }
    const std::string_view relative(path);
    const size_t slash = relative.find_last_of('/');
    const memory::String folder(slash == std::string_view::npos ? std::string_view() : relative.substr(0, slash));
    FolderList list{.items = {}, .folder = folder.c_str()};
    if (sdcard::list(folder.c_str(), add_file, &list) != sdcard::Result::Ok) {
        return false;
    }
    // in the order of the names, as the file manager shows them
    std::stable_sort(list.items.begin(), list.items.end(),
                     [](const Item &a, const Item &b) { return core::natural_less(a.location, b.location); });
    const auto found = std::find_if(list.items.begin(), list.items.end(),
                                    [&](const Item &item) { return item.location == relative; });
    if (found == list.items.end()) {
        return false;
    }
    const size_t index = static_cast<size_t>(found - list.items.begin());
    play(std::move(list.items), index);
    return true;
#else
    (void)path;
    return false;
#endif
}

void pause(bool pause)
{
    audio::pause_media(pause);
}

void toggle()
{
    if (collecting()) {
        return;
    }
    const audio::MediaState state = audio::media_status().state;
    if (state == audio::MediaState::Idle || state == audio::MediaState::Failed) {
        std::lock_guard lock(g_mutex);
        g_failures = 0;
        start_from_current();
        return;
    }
    audio::pause_media(state != audio::MediaState::Paused);
}

void seek(uint32_t ms)
{
    audio::seek_media(ms);
}

void next()
{
    std::lock_guard lock(g_mutex);
    if (!g_collecting.empty()) {
        return;
    }
    g_failures = 0;
    // the next button goes on also with repeat one, and from the last to the first
    if (g_queue.next(g_repeat == Repeat::Off ? Repeat::All : g_repeat)) {
        start_from_current();
    }
}

void previous()
{
    if (collecting()) {
        return;
    }
    if (audio::media_status().position_ms > kRestartMs) {
        audio::seek_media(0);
        return;
    }
    std::lock_guard lock(g_mutex);
    g_failures = 0;
    if (g_queue.previous(Repeat::All)) {
        start_from_current();
    }
}

void stop()
{
    {
        std::lock_guard lock(g_mutex);
        // a folder still collected does not start
        g_walk++;
        g_collecting.clear();
    }
    audio::stop_media();
}

void set_shuffle(bool shuffle)
{
    std::lock_guard lock(g_mutex);
    if (shuffle != g_shuffle) {
        g_shuffle = shuffle;
        g_queue.set_shuffle(shuffle, esp_random());
        store();
    }
}

void set_repeat(Repeat repeat)
{
    std::lock_guard lock(g_mutex);
    if (repeat != g_repeat) {
        g_repeat = repeat;
        store();
    }
}

Status status()
{
    const audio::MediaStatus media = audio::media_status();
    uint32_t serial = 0;
    const auto tags = audio::media_tags(serial);
    Status out;
    out.state = media.state;
    out.position_ms = media.position_ms;
    out.duration_ms = media.duration_ms;
    std::lock_guard lock(g_mutex);
    out.count = g_queue.size();
    out.index = g_queue.position();
    out.shuffle = g_shuffle;
    out.repeat = g_repeat;
    out.error = g_error;
    out.error_serial = g_error_serial;
    if (!g_collecting.empty()) {
        // the items of a folder come; what played was stopped
        out.state = audio::MediaState::Buffering;
        out.position_ms = 0;
        out.duration_ms = 0;
        out.count = 0;
        out.index = 0;
        out.title = g_collecting;
        return out;
    }
    if (const Item *item = g_queue.current()) {
        out.origin = item->origin;
        out.title = item->title.empty() ? name_of(item->location) : item->title;
    }
    // the tags belong to what plays once it plays; before, they are the last file's
    const bool loaded = media.state != audio::MediaState::Idle && media.state != audio::MediaState::Failed;
    if (tags && loaded) {
        if (!tags->title.empty()) {
            out.title = tags->title;
        }
        out.artist = tags->artist;
        out.album = tags->album;
    }
    return out;
}

memory::Vector<memory::String> upcoming(size_t max)
{
    memory::Vector<memory::String> titles;
    std::lock_guard lock(g_mutex);
    if (!g_collecting.empty() || max == 0) {
        return titles;
    }
    memory::Vector<const Item *> items(max);
    items.resize(g_queue.upcoming(items.data(), max, g_repeat));
    titles.reserve(items.size());
    for (const Item *item : items) {
        titles.push_back(item->title.empty() ? name_of(item->location) : item->title);
    }
    return titles;
}

bool active()
{
    const audio::MediaState state = audio::media_status().state;
    return state != audio::MediaState::Idle && state != audio::MediaState::Failed;
}

std::shared_ptr<const formats::Mp3Tags> tags(uint32_t &serial)
{
    return audio::media_tags(serial);
}

} // namespace hc::player
