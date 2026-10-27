// The media browser of a media tile (CONFIG_HC_UI_PLAYER, docs/ui/lvgl/lvgl-settings.md section 3):
// Home Assistant's media library, folder by folder as Media in Home Assistant's sidebar shows it,
// and the SD card. The tile's folder (a media-source:// id or a path of the card) is where it
// starts and the furthest `..` goes; without one the top shows two folders, "Home Assistant" and
// "SD card" (only the library on a device without the card). A tap on a folder opens it,
// its ▶ plays it with all its subfolders (hc/player/tree.hpp); a tap on a file plays the folder's
// files from there on (a playlist: its entries) and opens the player. The folder that was open
// stays open for the next time; the folders of the library come from Home Assistant one at a time
// (Backend::media_browse) and arrive through the dashboard's events, the card's are read at once.
// A folder of the library seen while the browser is open shows again at once, where it was
// scrolled to, and is asked for again quietly: the page is built again only if it changed.

#include "settings_pages.hpp"

#if CONFIG_HC_UI_PLAYER

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <strings.h>

#include "hc/core/natural.hpp"
#include "overlay.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

// the rows a folder shows at most
constexpr size_t kMaxShown = 200;
// the play button's user data of the two folders at the top
constexpr int kTopHass = -1;
constexpr int kTopSd = -2;
// A folder of the library that has not come by then is given up ("Again"): the hass task answers
// within 10 s, with an error at the latest; this catches an answer that got lost.
constexpr uint32_t kAnswerMs = 12000;
// the folders of the library kept while the browser is open
constexpr size_t kMaxCached = 8;

const Backend &be()
{
    return backend();
}

enum class Source : uint8_t { Hass, Sd };

// an opened folder: the library's id, or the path relative to the card
struct Level {
    Source source;
    memory::String id;
    memory::String title;
};

// a folder of the library seen, by the id it was asked for with
struct Cached {
    memory::String id;
    core::MediaList list;
    lv_coord_t scroll_y = 0;
};

struct Media {
    // the tile's folder: media-source://..., a path of the card, or empty (both tops)
    memory::String folder;
    // the folders opened below the top (empty: the top). Filled only when a folder opens: a
    // static that allocates would take the large heap before main sets it up.
    memory::Vector<Level> path;
    // the library's folder of the last answer, and the request on its way
    core::MediaList list;
    bool loaded = false;
    bool loading = false;
    uint32_t loading_since = 0;
    uint32_t generation = 0;
    // the list shown came from the cache, the request on its way checks it
    bool refreshing = false;
    // the folders seen while the browser is open, the last used last
    memory::Vector<Cached> cache;
    // where the page is scrolled to once it is built
    lv_coord_t scroll_y = 0;
    // a folder was opened, its page is not built yet: the rows on the screen are the old folder's,
    // taps on them are ignored
    bool changing = false;
    // the page is shown: an answer builds it again
    bool shown = false;
    // the page's scrolling parent while it is shown
    lv_obj_t *content = nullptr;
};
Media g;

#if CONFIG_HC_SDCARD
// the entries of the card's folder shown, for the rows' callbacks; freed with the page
std::unique_ptr<FileEntry[]> g_entries;
size_t g_count = 0;
#endif

bool has_sd()
{
#if CONFIG_HC_SDCARD
    return be().sd_list != nullptr && be().player_play_folder != nullptr;
#else
    return false;
#endif
}

// The folder shown: the top with both sources is none (false), else the opened folder or the
// tile's (a path of the card on a device without one is still the card's: the page says so).
bool current(Source &source, memory::String &id)
{
    if (!g.path.empty()) {
        source = g.path.back().source;
        id = g.path.back().id;
        return true;
    }
    if (g.folder.empty() && has_sd()) {
        return false;
    }
    source = g.folder.empty() || core::hass_media_folder(g.folder) ? Source::Hass : Source::Sd;
    // "/": the card's root, "media-source://": the library's top
    id = g.folder == "/" || g.folder == core::kMediaSourcePrefix ? memory::String() : g.folder;
    return true;
}

// the last name of the tile's folder, the title of its page
const char *folder_title()
{
    if (g.folder == "/") {
        return "SD card";
    }
    if (g.folder == core::kMediaSourcePrefix) {
        return "Home Assistant";
    }
    const size_t slash = g.folder.find_last_of('/');
    return slash == memory::String::npos ? g.folder.c_str() : g.folder.c_str() + slash + 1;
}

// the folder shown is one of the library: its id
bool hass_folder(memory::String &id)
{
    Source source = Source::Hass;
    return current(source, id) && source == Source::Hass;
}

Cached *find_cached(const memory::String &id)
{
    for (Cached &cached : g.cache) {
        if (cached.id == id) {
            return &cached;
        }
    }
    return nullptr;
}

// the folder shown came: kept as the last used, the oldest goes
void store(const core::MediaList &list)
{
    memory::String id;
    if (!hass_folder(id)) {
        return;
    }
    lv_coord_t scroll_y = 0;
    for (auto it = g.cache.begin(); it != g.cache.end(); ++it) {
        if (it->id == id) {
            scroll_y = it->scroll_y;
            g.cache.erase(it);
            break;
        }
    }
    if (g.cache.size() >= kMaxCached) {
        g.cache.erase(g.cache.begin());
    }
    g.cache.push_back(Cached{std::move(id), list, scroll_y});
}

// the folder on the screen is left: its scroll position for the way back
void remember_scroll()
{
    memory::String id;
    Cached *cached = g.shown && hass_folder(id) ? find_cached(id) : nullptr;
    if (cached != nullptr) {
        cached->scroll_y = lv_obj_get_scroll_y(g.content);
    }
}

// Asks for the folder shown; one in the cache shows at once and is checked quietly.
void request()
{
    memory::String id;
    g.scroll_y = 0;
    g.refreshing = false;
    if (!hass_folder(id)) {
        g.loading = false;
        return;
    }
    g.loading_since = lv_tick_get();
    if (Cached *cached = find_cached(id)) {
        g.list = cached->list;
        g.scroll_y = cached->scroll_y;
        g.loading = false;
        g.loaded = true;
        g.refreshing = true;
    } else {
        g.loading = true;
        g.loaded = false;
    }
    be().media_browse(std::string_view(id), ++g.generation);
}

void open_level(Level level)
{
    remember_scroll();
    g.path.push_back(std::move(level));
    g.changing = true;
    request();
    reload();
}

void go_up()
{
    remember_scroll();
    if (!g.path.empty()) {
        g.path.pop_back();
    }
    g.changing = true;
    request();
    reload();
}

// every second while the page is shown
void check_answer()
{
    if (g.refreshing && lv_tick_get() - g.loading_since >= kAnswerMs) {
        // the cached rows stay
        g.refreshing = false;
        g.generation++;
    }
    if (g.loading && lv_tick_get() - g.loading_since >= kAnswerMs) {
        g.loading = false;
        g.loaded = true;
        g.list = {};
        g.list.error = "Home Assistant did not answer";
        // a late answer is not taken
        g.generation++;
        reload();
    }
}

// a quiet check found what the page shows
bool same(const core::MediaList &a, const core::MediaList &b)
{
    if (a.title != b.title || a.items.size() != b.items.size()) {
        return false;
    }
    for (size_t i = 0; i < a.items.size(); i++) {
        const core::MediaItem &x = a.items[i];
        const core::MediaItem &y = b.items[i];
        if (x.id != y.id || x.title != y.title || x.type != y.type || x.folder != y.folder ||
            x.playable != y.playable) {
            return false;
        }
    }
    return true;
}

// a tap on a row of the page shown, not of a folder left
bool tap_ok()
{
    return !g.changing;
}

// "<dir>/<name>", or the name in the root
memory::String join(const memory::String &dir, const char *name)
{
    return dir.empty() ? memory::String(name) : dir + "/" + name;
}

void played(bool started)
{
    if (!started) {
        overlay::show(LV_SYMBOL_WARNING "  It does not play", t::kAlert, 4000);
        return;
    }
    push(Page::Player);
}

// ▶ of a folder: the folder with its subfolders
void play_folder(Source source, const memory::String &id, const char *title)
{
    if (source == Source::Hass) {
        played(be().player_play_media_folder != nullptr && be().player_play_media_folder(id, title));
        return;
    }
#if CONFIG_HC_SDCARD
    played(be().player_play_folder(id.c_str(), title));
#endif
}

// A folder's row with ▶ before the `>`: a tap on the row opens the folder, one on ▶ plays it
// (LVGL does not bubble the button's click to the row).
lv_obj_t *folder_row(lv_obj_t *col, const char *text, lv_event_cb_t on_open, lv_event_cb_t on_play, int data)
{
    lv_obj_t *row = t::row(col, text, "", true, on_open, as_data(data));
    lv_obj_t *button = lv_btn_create(row);
    const lv_coord_t extra = t::dim(100) != 100 ? 5 : 3;
    const lv_coord_t height = t::dim(36) + extra;
    lv_obj_set_size(button, t::dim(40) + extra, height);
    lv_obj_set_style_radius(button, t::radius(), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, t::color(t::kBorder), 0);
    lv_obj_set_style_bg_color(button, t::color(t::kActive), LV_STATE_PRESSED);
    lv_obj_center(t::label(button, LV_SYMBOL_PLAY, t::font_normal(), t::kText));
    lv_obj_add_event_cb(button, on_play, LV_EVENT_CLICKED, as_data(data));
    // before the `>`
    lv_obj_move_to_index(button, 2);
    // from the size set, not a layout pass: that laid out the whole page again for every row
    lv_obj_set_height(row, LV_MAX(t::row_height(), height + 12));
    return row;
}

// the name takes the row (no value), a smaller font before it is cut
void wide_name(lv_obj_t *row, const char *text, bool muted)
{
    lv_obj_t *name = lv_obj_get_child(row, 0);
    lv_obj_set_style_max_width(name, lv_pct(100), 0);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_add_flag(t::row_value(row), LV_OBJ_FLAG_HIDDEN);
    t::line_text(name, text, &t::font_normal());
    if (muted) {
        // a picture, a video, a text: the player does not take it
        lv_obj_set_style_text_color(name, t::color(t::kTextMuted), 0);
    }
}

// ------------------------------------------------------------------------- the top

constexpr const char *kHassTitle = "Home Assistant";
constexpr const char *kSdTitle = "SD card";

void on_top(lv_event_t *event)
{
    if (!tap_ok()) {
        return;
    }
    const bool sd = from_data(event) == kTopSd;
    open_level(Level{sd ? Source::Sd : Source::Hass, memory::String(), sd ? kSdTitle : kHassTitle});
}

void on_top_play(lv_event_t *event)
{
    if (!tap_ok()) {
        return;
    }
    const bool sd = from_data(event) == kTopSd;
    play_folder(sd ? Source::Sd : Source::Hass, memory::String(), sd ? kSdTitle : kHassTitle);
}

void top_rows(lv_obj_t *col)
{
    char text[64];
    std::snprintf(text, sizeof(text), LV_SYMBOL_DIRECTORY "  %s", kHassTitle);
    folder_row(col, text, on_top, on_top_play, kTopHass);
    std::snprintf(text, sizeof(text), LV_SYMBOL_DIRECTORY "  %s", kSdTitle);
    folder_row(col, text, on_top, on_top_play, kTopSd);
}

// ------------------------------------------------------------- Home Assistant's library

void on_item(lv_event_t *event)
{
    const auto index = static_cast<size_t>(from_data(event));
    if (!tap_ok() || g.loading || index >= g.list.items.size()) {
        return;
    }
    const core::MediaItem &item = g.list.items[index];
    if (item.folder) {
        open_level(Level{Source::Hass, item.id, item.title});
        return;
    }
    be().player_play_media(g.list, index);
    push(Page::Player);
}

void on_item_play(lv_event_t *event)
{
    const auto index = static_cast<size_t>(from_data(event));
    if (tap_ok() && !g.loading && index < g.list.items.size()) {
        play_folder(Source::Hass, g.list.items[index].id, g.list.items[index].title.c_str());
    }
}

// a playlist of the library (audio/x-mpegurl, .m3u, .m3u8), as player::is_media_playlist
bool media_playlist(const core::MediaItem &item)
{
    if (item.folder || !item.playable) {
        return false;
    }
    if (item.type.find("mpegurl") != memory::String::npos) {
        return true;
    }
    const char *dot = std::strrchr(item.title.c_str(), '.');
    return dot != nullptr && (strcasecmp(dot, ".m3u") == 0 || strcasecmp(dot, ".m3u8") == 0);
}

// an MP3, the player's format, or a playlist; a radio station of the library may be an MP3 too
bool playable(const core::MediaItem &item)
{
    if (media_playlist(item)) {
        return true;
    }
    if (item.folder || !item.playable) {
        return false;
    }
    if (item.type == "audio/mpeg" || item.type == "audio/mp3") {
        return true;
    }
    const size_t length = item.title.size();
    if (length <= 4) {
        return false;
    }
    const char *extension = item.title.c_str() + length - 4;
    for (size_t i = 0; i < 4; i++) {
        if (std::tolower(static_cast<unsigned char>(extension[i])) != ".mp3"[i]) {
            return false;
        }
    }
    return true;
}

void hass_rows(lv_obj_t *col)
{
    if (g.loading) {
        text_block(col, "Loading ...", t::kTextMuted);
        return;
    }
    if (!g.list.error.empty()) {
        char text[160];
        std::snprintf(text, sizeof(text), LV_SYMBOL_WARNING "  %s", g.list.error.c_str());
        text_block(col, text, t::kAlert);
        lv_obj_t *buttons = button_row(col);
        t::button(buttons, LV_SYMBOL_REFRESH "  Again", [](lv_event_t *) {
            request();
            reload();
        });
        return;
    }
    if (g.list.items.empty()) {
        text_block(col, "This folder is empty.", t::kTextMuted);
        return;
    }
    const size_t shown = std::min(g.list.items.size(), kMaxShown);
    for (size_t i = 0; i < shown; i++) {
        const core::MediaItem &item = g.list.items[i];
        const bool plays = playable(item);
        char text[160];
        std::snprintf(text, sizeof(text), "%s  %s",
                      item.folder             ? LV_SYMBOL_DIRECTORY
                      : media_playlist(item) ? LV_SYMBOL_LIST
                      : plays                ? LV_SYMBOL_AUDIO
                                             : LV_SYMBOL_FILE,
                      item.title.c_str());
        lv_obj_t *row = item.folder ? folder_row(col, text, on_item, on_item_play, static_cast<int>(i))
                                    : t::row(col, text, "", false, plays ? on_item : nullptr,
                                             as_data(static_cast<int>(i)));
        wide_name(row, text, !item.folder && !plays);
    }
    if (g.list.items.size() > shown) {
        char text[48];
        std::snprintf(text, sizeof(text), "%u more not shown", static_cast<unsigned>(g.list.items.size() - shown));
        text_block(col, text, t::kTextMuted);
    }
}

// ------------------------------------------------------------------------ the SD card

#if CONFIG_HC_SDCARD
const char *result_text(FileResult result)
{
    switch (result) {
    case FileResult::NoCard:
        return "No SD card";
    case FileResult::NotFound:
        return "The folder is not on the card";
    default:
        break;
    }
    return "The card cannot be read";
}

bool sd_before(const FileEntry &a, const FileEntry &b)
{
    if (a.dir != b.dir) {
        return a.dir;
    }
    return core::natural_less(a.name, b.name);
}

void on_sd_entry(lv_event_t *event)
{
    if (!tap_ok()) {
        return;
    }
    const auto index = static_cast<size_t>(from_data(event));
    Source source = Source::Sd;
    memory::String dir;
    if (index >= g_count || !current(source, dir)) {
        return;
    }
    const FileEntry &entry = g_entries[index];
    memory::String path = join(dir, entry.name);
    if (path.size() >= kFilePathSize) {
        return;
    }
    if (entry.dir) {
        open_level(Level{Source::Sd, std::move(path), entry.name});
        return;
    }
    played(be().player_play_file(path.c_str()));
}

void on_sd_play(lv_event_t *event)
{
    if (!tap_ok()) {
        return;
    }
    const auto index = static_cast<size_t>(from_data(event));
    Source source = Source::Sd;
    memory::String dir;
    if (index < g_count && current(source, dir)) {
        const memory::String path = join(dir, g_entries[index].name);
        if (path.size() < kFilePathSize) {
            play_folder(Source::Sd, path, g_entries[index].name);
        }
    }
}

void sd_rows(lv_obj_t *col, const memory::String &dir)
{
    g_count = 0;
    g_entries.reset(new (std::nothrow) FileEntry[kMaxShown]);
    if (g_entries == nullptr) {
        text_block(col, "Not enough memory to list the folder.", t::kAlert);
        return;
    }
    size_t count = 0;
    const FileResult result = be().sd_list(dir.c_str(), g_entries.get(), kMaxShown, count);
    if (result != FileResult::Ok) {
        char text[96];
        std::snprintf(text, sizeof(text), LV_SYMBOL_WARNING "  %s", result_text(result));
        text_block(col, text, t::kAlert);
        return;
    }
    g_count = std::min(count, kMaxShown);
    std::sort(g_entries.get(), g_entries.get() + g_count, sd_before);
    // the entries live as long as the page
    lv_obj_add_event_cb(
        col,
        [](lv_event_t *) {
            g_entries.reset();
            g_count = 0;
        },
        LV_EVENT_DELETE, nullptr);
    for (size_t i = 0; i < g_count; i++) {
        const FileEntry &entry = g_entries[i];
        const bool playlist = !entry.dir && be().player_is_playlist(entry.name);
        const bool plays = !entry.dir && be().player_supports(entry.name);
        char text[kFilePathSize + 8];
        std::snprintf(text, sizeof(text), "%s  %s",
                      entry.dir  ? LV_SYMBOL_DIRECTORY
                      : playlist ? LV_SYMBOL_LIST
                      : plays    ? LV_SYMBOL_AUDIO
                                 : LV_SYMBOL_FILE,
                      entry.name);
        lv_obj_t *row = entry.dir ? folder_row(col, text, on_sd_entry, on_sd_play, static_cast<int>(i))
                                  : t::row(col, text, "", false, plays ? on_sd_entry : nullptr,
                                           as_data(static_cast<int>(i)));
        wide_name(row, text, !entry.dir && !plays);
    }
    if (count == 0) {
        text_block(col, "This folder is empty.", t::kTextMuted);
    } else if (count > g_count) {
        char text[48];
        std::snprintf(text, sizeof(text), "%u more not shown", static_cast<unsigned>(count - g_count));
        text_block(col, text, t::kTextMuted);
    }
}
#endif

} // namespace

void media_open(const char *folder)
{
    const char *start = folder != nullptr ? folder : "";
    if (g.folder == start) {
        // the folder that stays open shows from the cache and is checked again
        g.loaded = false;
        g.loading = false;
        return;
    }
    g.folder = start;
    g.path.clear();
    g.list = {};
    g.cache.clear();
    g.loaded = false;
    g.loading = false;
    g.refreshing = false;
}

void on_media_list(core::MediaList list)
{
    if (list.generation != g.generation) {
        return;
    }
    // folders first, then by name as the card's, so a tap plays in the order shown
    std::stable_sort(list.items.begin(), list.items.end(), [](const core::MediaItem &a, const core::MediaItem &b) {
        if (a.folder != b.folder) {
            return a.folder;
        }
        return core::natural_less(a.title, b.title);
    });
    const bool quiet = g.refreshing;
    g.loading = false;
    g.refreshing = false;
    g.loaded = true;
    if (list.error.empty()) {
        store(list);
    } else if (quiet) {
        // the cached rows stay
        return;
    }
    if (quiet && same(list, g.list)) {
        return;
    }
    if (quiet && g.shown) {
        // built again where it is; the old rows take no taps until then
        g.scroll_y = lv_obj_get_scroll_y(g.content);
        g.changing = true;
    }
    g.list = std::move(list);
    if (g.shown) {
        reload();
    }
}

Built media_page(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    Source source = Source::Hass;
    memory::String id;
    const bool in_folder = current(source, id);
    if (g.loading && lv_tick_get() - g.loading_since >= kAnswerMs) {
        // the page opens again after an answer got lost: ask again
        g.loading = false;
    }
    if (in_folder && source == Source::Hass && !g.loaded && !g.loading) {
        request();
    }
    if (!g.path.empty()) {
        t::row(col, LV_SYMBOL_UP "  ..", "", false, [](lv_event_t *) {
            if (tap_ok()) {
                go_up();
            }
        });
    }
    if (!in_folder) {
        top_rows(col);
    } else if (source == Source::Hass) {
        hass_rows(col);
    } else if (!has_sd()) {
        text_block(col, LV_SYMBOL_WARNING "  This device has no SD card.", t::kAlert);
    } else {
#if CONFIG_HC_SDCARD
        sd_rows(col, id);
#endif
    }
    g.shown = true;
    g.changing = false;
    g.content = content;
    if (g.scroll_y > 0) {
        lv_obj_update_layout(content);
        lv_obj_scroll_to_y(content, g.scroll_y, LV_ANIM_OFF);
    }
    lv_obj_add_event_cb(
        col,
        [](lv_event_t *) {
            // left for another page (the player) or built again: back where it was
            if (!g.changing) {
                g.scroll_y = lv_obj_get_scroll_y(g.content);
            }
            g.shown = false;
            if (!settings::is_open()) {
                // closed: only the folder that stays open is kept
                memory::String id;
                Cached *cached = hass_folder(id) ? find_cached(id) : nullptr;
                Cached kept;
                if (cached != nullptr) {
                    kept = std::move(*cached);
                    kept.scroll_y = g.scroll_y;
                }
                g.cache.clear();
                if (cached != nullptr) {
                    g.cache.push_back(std::move(kept));
                }
                g.content = nullptr;
            }
        },
        LV_EVENT_DELETE, nullptr);
    // the folder's name; the library's own title once it came
    const char *title = !g.path.empty() ? g.path.back().title.c_str() : g.folder.empty() ? "Media" : folder_title();
    if (in_folder && source == Source::Hass && !id.empty() && g.loaded && !g.list.title.empty()) {
        title = g.list.title.c_str();
    }
    return {title, check_answer};
}

} // namespace hc::ui::settings::pages

#endif
