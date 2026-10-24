// Settings, Media (CONFIG_HC_UI_PLAYER, docs/ui/lvgl/lvgl-settings.md section 3): Home
// Assistant's media library, folder by folder as Media in Home Assistant's sidebar shows it. A tap
// on a folder opens it, a tap on an MP3 plays the folder's files from there on and opens the
// player. The folder that was open stays open for the next time; the folders come from Home
// Assistant one at a time (Backend::media_browse) and arrive through the dashboard's events.

#include "settings_pages.hpp"

#if CONFIG_HC_UI_PLAYER

#include <cctype>
#include <cstdio>
#include <cstring>

#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

// the rows a folder shows at most
constexpr size_t kMaxShown = 200;

const Backend &be()
{
    return backend();
}

struct Folder {
    memory::String id;
    memory::String title;
};

struct Media {
    // the folders opened below the top of the library (empty: the top). Filled only when a
    // folder opens: a static that allocates would take the large heap before main sets it up.
    memory::Vector<Folder> path;
    // the folder of the last answer, and the request on its way
    core::MediaList list;
    bool loaded = false;
    bool loading = false;
    uint32_t generation = 0;
    // the page is shown: an answer builds it again
    bool shown = false;
};
Media g;

void request()
{
    g.loading = true;
    g.loaded = false;
    be().media_browse(g.path.empty() ? std::string_view() : std::string_view(g.path.back().id), ++g.generation);
}

void open_folder(size_t index)
{
    const core::MediaItem &item = g.list.items[index];
    g.path.push_back(Folder{item.id, item.title});
    request();
    reload();
}

void go_up()
{
    if (!g.path.empty()) {
        g.path.pop_back();
    }
    request();
    reload();
}

// an MP3, the player's format; a radio station of the library may be one too
bool playable(const core::MediaItem &item)
{
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

void on_item(lv_event_t *event)
{
    const auto index = static_cast<size_t>(from_data(event));
    if (index >= g.list.items.size()) {
        return;
    }
    if (g.list.items[index].folder) {
        open_folder(index);
        return;
    }
    be().player_play_media(g.list, index);
    push(Page::Player);
}

} // namespace

void on_media_list(core::MediaList list)
{
    if (list.generation != g.generation) {
        return;
    }
    g.loading = false;
    g.loaded = true;
    g.list = std::move(list);
    if (g.shown) {
        reload();
    }
}

Built media_page(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    if (!g.loaded && !g.loading) {
        request();
    }
    if (!g.path.empty()) {
        t::row(col, LV_SYMBOL_UP "  ..", "", false, [](lv_event_t *) { go_up(); });
    }
    if (g.loading) {
        text_block(col, "Loading ...", t::kTextMuted);
    } else if (!g.list.error.empty()) {
        char text[160];
        std::snprintf(text, sizeof(text), LV_SYMBOL_WARNING "  %s", g.list.error.c_str());
        text_block(col, text, t::kAlert);
        lv_obj_t *buttons = button_row(col);
        t::button(buttons, LV_SYMBOL_REFRESH "  Again", [](lv_event_t *) {
            request();
            reload();
        });
    } else if (g.list.items.empty()) {
        text_block(col, "This folder is empty.", t::kTextMuted);
    }
    if (!g.loading && g.list.error.empty()) {
        const size_t shown = std::min(g.list.items.size(), kMaxShown);
        for (size_t i = 0; i < shown; i++) {
            const core::MediaItem &item = g.list.items[i];
            const bool folder = item.folder;
            const bool plays = playable(item);
            char text[160];
            std::snprintf(text, sizeof(text), "%s  %s",
                          folder ? LV_SYMBOL_DIRECTORY : plays ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE, item.title.c_str());
            lv_obj_t *row = t::row(col, text, "", folder, folder || plays ? on_item : nullptr, as_data(static_cast<int>(i)));
            // no value: the name takes the row, a smaller font before it is cut
            lv_obj_t *name = lv_obj_get_child(row, 0);
            lv_obj_set_style_max_width(name, lv_pct(100), 0);
            lv_obj_set_flex_grow(name, 1);
            lv_obj_add_flag(t::row_value(row), LV_OBJ_FLAG_HIDDEN);
            t::line_text(name, text, &t::font_normal());
            if (!folder && !plays) {
                // a picture, a video: the player does not take it
                lv_obj_set_style_text_color(lv_obj_get_child(row, 0), t::color(t::kTextMuted), 0);
            }
        }
        if (g.list.items.size() > shown) {
            char text[48];
            std::snprintf(text, sizeof(text), "%u more not shown", static_cast<unsigned>(g.list.items.size() - shown));
            text_block(col, text, t::kTextMuted);
        }
    }
    g.shown = true;
    lv_obj_add_event_cb(col, [](lv_event_t *) { g.shown = false; }, LV_EVENT_DELETE, nullptr);
    const char *title = g.path.empty()                      ? "Media"
                        : !g.list.title.empty() && g.loaded ? g.list.title.c_str()
                                                            : g.path.back().title.c_str();
    return {title, nullptr};
}

} // namespace hc::ui::settings::pages

#endif
