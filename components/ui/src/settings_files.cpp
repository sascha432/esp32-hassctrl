// Settings, System, SD card: the file manager (docs/ui/lvgl/lvgl-settings.md section 3). One page
// for every folder (the folder is kept here, `..` goes up), a page for a file or folder with
// Rename and Delete, and the page of a new name. A tap on a WAV or MP3 file opens the player
// (settings_player.cpp), a long press on any entry its page.

#include "settings_pages.hpp"

#if CONFIG_HC_SDCARD

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "hc/core/natural.hpp"
#include "keyboard.hpp"
#include "overlay.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

// a folder shows this many entries at most
constexpr size_t kMaxShown = 200;

const Backend &be()
{
    return backend();
}

// the folder shown, relative to the card ("" is its root)
char g_dir[kFilePathSize] = {};
// the entry of the detail page, relative to the card
char g_selected[kFilePathSize] = {};
FileEntry g_selected_entry{};
// the entries of the folder shown, for the rows' callbacks; freed with the page
std::unique_ptr<FileEntry[]> g_entries;
size_t g_shown = 0;
// a long press was just handled: the click that follows it is not
bool g_long_pressed = false;

// the form of the name page
lv_obj_t *g_name_area = nullptr;
lv_obj_t *g_name_keyboard = nullptr;
int g_name_mode = 0;

constexpr int kNewFolder = 0;
constexpr int kRename = 1;

const char *result_text(FileResult result)
{
    switch (result) {
    case FileResult::Ok:
        return "Done";
    case FileResult::NoCard:
        return "No SD card";
    case FileResult::BadName:
        return "Not a name the card takes";
    case FileResult::NotFound:
        return "Not found";
    case FileResult::Exists:
        return "The name is taken";
    case FileResult::NotEmpty:
        return "The folder is not empty";
    case FileResult::Failed:
        break;
    }
    return "The card did not do it";
}

// false with the problem in the alert strip
bool check(FileResult result)
{
    if (result == FileResult::Ok) {
        return true;
    }
    char text[64];
    std::snprintf(text, sizeof(text), LV_SYMBOL_WARNING "  %s", result_text(result));
    overlay::show(text);
    return false;
}

// "<dir>/<name>", or the name in the root; false when it is too long
bool join(const char *dir, const char *name, char *out, size_t size)
{
    const int n = dir[0] == 0 ? std::snprintf(out, size, "%s", name) : std::snprintf(out, size, "%s/%s", dir, name);
    return n > 0 && static_cast<size_t>(n) < size;
}

// the last name of a path
const char *base_name(const char *path)
{
    const char *slash = std::strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

void size_text(uint64_t bytes, char *out, size_t size)
{
    constexpr uint64_t kKb = 1024;
    if (bytes < kKb) {
        std::snprintf(out, size, "%u B", static_cast<unsigned>(bytes));
    } else if (bytes < kKb * kKb) {
        std::snprintf(out, size, "%.1f KB", static_cast<double>(bytes) / kKb);
    } else if (bytes < kKb * kKb * kKb) {
        std::snprintf(out, size, "%.1f MB", static_cast<double>(bytes) / (kKb * kKb));
    } else {
        std::snprintf(out, size, "%.2f GB", static_cast<double>(bytes) / (kKb * kKb * kKb));
    }
}

bool playable(const char *name)
{
#if CONFIG_HC_UI_PLAYER
    return be().player_supports != nullptr && be().player_supports(name);
#else
    (void)name;
    return false;
#endif
}

bool playlist(const char *name)
{
#if CONFIG_HC_UI_PLAYER
    return be().player_is_playlist != nullptr && be().player_is_playlist(name);
#else
    (void)name;
    return false;
#endif
}

// The selected file plays: its folder from it on, or the playlist; the player opens. A file the
// player does not take is reported in the alert strip.
void play_selected()
{
#if CONFIG_HC_UI_PLAYER
    if (!be().player_play_file(g_selected)) {
        overlay::show(LV_SYMBOL_WARNING "  It does not play", t::kAlert, 4000);
        return;
    }
    push(Page::Player);
#endif
}

// folders first, then by name without case, numbers by value (the player's order)
bool before(const FileEntry &a, const FileEntry &b)
{
    if (a.dir != b.dir) {
        return a.dir;
    }
    return core::natural_less(a.name, b.name);
}

void select(const FileEntry &entry)
{
    if (join(g_dir, entry.name, g_selected, sizeof(g_selected))) {
        g_selected_entry = entry;
    } else {
        g_selected[0] = 0;
    }
}

void on_entry(lv_event_t *event)
{
    if (g_long_pressed) {
        g_long_pressed = false;
        return;
    }
    const auto index = static_cast<size_t>(from_data(event));
    if (index >= g_shown) {
        return;
    }
    const FileEntry &entry = g_entries[index];
    if (entry.dir) {
        char dir[kFilePathSize];
        if (join(g_dir, entry.name, dir, sizeof(dir))) {
            std::memcpy(g_dir, dir, sizeof(g_dir));
            reload();
        }
        return;
    }
    select(entry);
    if (g_selected[0] == 0) {
        return;
    }
    if (playable(entry.name)) {
        play_selected();
    } else {
        push(Page::FileDetail);
    }
}

void on_entry_long(lv_event_t *event)
{
    const auto index = static_cast<size_t>(from_data(event));
    if (index >= g_shown) {
        return;
    }
    g_long_pressed = true;
    select(g_entries[index]);
    if (g_selected[0] != 0) {
        push(Page::FileDetail);
    }
}

void go_up()
{
    char *slash = std::strrchr(g_dir, '/');
    if (slash != nullptr) {
        *slash = 0;
    } else {
        g_dir[0] = 0;
    }
    reload();
}

void on_name_ready(lv_event_t *)
{
    const char *text = lv_textarea_get_text(g_name_area);
    while (*text == '/') {
        text++;
    }
    if (text[0] == 0) {
        return;
    }
    if (g_name_mode == kNewFolder) {
        char path[kFilePathSize];
        if (!join(g_dir, text, path, sizeof(path))) {
            check(FileResult::BadName);
            return;
        }
        if (!check(be().sd_mkdir(path))) {
            return;
        }
        set_form_dirty(false);
        pop();
        return;
    }
    if (!check(be().sd_rename(g_selected, text))) {
        return;
    }
    // the folder list shows where it went
    char dir[kFilePathSize];
    std::snprintf(dir, sizeof(dir), "%s", text);
    char *slash = std::strrchr(dir, '/');
    if (slash != nullptr) {
        *slash = 0;
    } else {
        dir[0] = 0;
    }
    std::memcpy(g_dir, dir, sizeof(g_dir));
    set_form_dirty(false);
    pop(2); // past the detail page, whose entry is gone
}

} // namespace

const char *selected_file()
{
    return g_selected;
}

void select_file(const char *path, uint64_t size, bool dir)
{
    std::snprintf(g_dir, sizeof(g_dir), "%s", path);
    char *slash = std::strrchr(g_dir, '/');
    const char *name = slash != nullptr ? path + (slash - g_dir) + 1 : path;
    if (slash != nullptr) {
        *slash = 0;
    } else {
        g_dir[0] = 0;
    }
    FileEntry entry{};
    std::snprintf(entry.name, sizeof(entry.name), "%s", name);
    entry.size = size;
    entry.dir = dir;
    select(entry);
}

Built files_page(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    g_long_pressed = false;
    g_shown = 0;
    g_entries.reset(new (std::nothrow) FileEntry[kMaxShown]);
    if (g_entries == nullptr) {
        text_block(col, "Not enough memory to list the folder.", t::kAlert);
        return {"SD card", nullptr};
    }
    size_t count = 0;
    FileResult result = be().sd_list(g_dir, g_entries.get(), kMaxShown, count);
    if (result == FileResult::NotFound && g_dir[0] != 0) {
        // the folder is gone (renamed in the web UI, another card): the root
        g_dir[0] = 0;
        result = be().sd_list(g_dir, g_entries.get(), kMaxShown, count);
    }
    if (result != FileResult::Ok) {
        DeviceInfo device{};
        be().device_info(device);
        text_block(col, result == FileResult::NoCard ? device.sd_card : result_text(result), t::kText);
        if (result == FileResult::NoCard) {
            text_block(col, "A card put in while the device runs is mounted in the web UI or after a restart.");
        }
        return {"SD card", nullptr};
    }
    g_shown = std::min(count, kMaxShown);
    std::sort(g_entries.get(), g_entries.get() + g_shown, before);
    // the entries live as long as the page
    lv_obj_add_event_cb(
        col,
        [](lv_event_t *) {
            g_entries.reset();
            g_shown = 0;
        },
        LV_EVENT_DELETE, nullptr);

    char text[kFilePathSize + 8];
    std::snprintf(text, sizeof(text), "/%s", g_dir);
    lv_obj_t *path = section(col, text);
    lv_label_set_long_mode(path, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(path, lv_pct(100));
    if (g_dir[0] != 0) {
        t::row(col, LV_SYMBOL_UP "  ..", "", false, [](lv_event_t *) { go_up(); });
    }
    for (size_t i = 0; i < g_shown; i++) {
        const FileEntry &entry = g_entries[i];
        const char *symbol = entry.dir                ? LV_SYMBOL_DIRECTORY
                             : playlist(entry.name)    ? LV_SYMBOL_LIST
                             : playable(entry.name)    ? LV_SYMBOL_AUDIO
                                                       : LV_SYMBOL_FILE;
        char name[kFilePathSize + 8];
        std::snprintf(name, sizeof(name), "%s  %s", symbol, entry.name);
        char size[16] = "";
        if (!entry.dir) {
            size_text(entry.size, size, sizeof(size));
        }
        lv_obj_t *row = t::row(col, name, size, entry.dir, on_entry, as_data(static_cast<int>(i)));
        // Names that differ only at the end (the recordings' times): the name gets more of the
        // row than a settings row's 60 %, and a smaller font before it is cut
        lv_obj_t *name_label = lv_obj_get_child(row, 0);
        lv_obj_set_style_max_width(name_label, lv_pct(75), 0);
        t::line_text(name_label, name, &t::font_normal());
        lv_obj_set_style_text_font(t::row_value(row), &t::font_small(), 0);
        t::line_text(t::row_value(row), size);
        lv_obj_add_event_cb(row, on_entry_long, LV_EVENT_LONG_PRESSED, as_data(static_cast<int>(i)));
    }
    if (count == 0) {
        text_block(col, "Empty");
    } else if (count > g_shown) {
        std::snprintf(text, sizeof(text), "%u more not shown", static_cast<unsigned>(count - g_shown));
        text_block(col, text);
    }
    t::button(button_row(col), "New folder", [](lv_event_t *) { push(Page::FileName, nullptr, kNewFolder); });
    return {g_dir[0] != 0 ? base_name(g_dir) : "SD card", nullptr};
}

Built file_detail_page(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    const FileEntry &entry = g_selected_entry;
    t::caption_value(col, "Name", entry.name);
    char text[kFilePathSize + 8];
    std::snprintf(text, sizeof(text), "/%s", g_dir);
    t::caption_value(col, "Folder", text);
    if (!entry.dir) {
        size_text(entry.size, text, sizeof(text));
        t::caption_value(col, "Size", text);
    }
    lv_obj_t *buttons = button_row(col);
    if (!entry.dir && playable(entry.name)) {
        t::button(buttons, LV_SYMBOL_PLAY "  Play", [](lv_event_t *) { play_selected(); });
    }
    t::button(buttons, "Rename / move", [](lv_event_t *) { push(Page::FileName, nullptr, kRename); });
    t::button(
        buttons, "Delete",
        [](lv_event_t *) {
            char question[kFilePathSize + 16];
            std::snprintf(question, sizeof(question), "Delete %s?", g_selected_entry.name);
            confirm(question,
                    g_selected_entry.dir ? "Only an empty folder can be deleted." : "The file is gone for good.",
                    "Delete", true,
                    [](void *) {
                        if (check(be().sd_delete(g_selected))) {
                            pop();
                        }
                    },
                    nullptr);
        },
        nullptr, true);
    return {entry.name, nullptr};
}

Built file_name_page(lv_obj_t *content, int index)
{
    g_name_mode = index == kRename ? kRename : kNewFolder;
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *form = t::plain(content);
    lv_obj_set_width(form, lv_pct(100));
    lv_obj_set_flex_grow(form, 1);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(form, t::pad(), 0);
    lv_obj_set_style_pad_row(form, t::dim(6), 0);
    lv_obj_add_flag(form, LV_OBJ_FLAG_SCROLLABLE);

    if (g_name_mode == kRename) {
        text_block(form, "The path on the card: another folder in front moves it there.");
    }
    lv_obj_t *line = button_row(form);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(line, 0, 0);
    g_name_area = text_area(line, g_name_mode == kRename ? "new name" : "folder name", false);
    lv_textarea_set_max_length(g_name_area, kFilePathSize - 1);
    if (g_name_mode == kRename) {
        lv_textarea_set_text(g_name_area, g_selected);
    }
    t::button(line, g_name_mode == kRename ? "Rename" : "Create", on_name_ready);

    g_name_keyboard = keyboard::create(content);
    lv_obj_set_size(g_name_keyboard, lv_pct(100), lv_pct(55));
    set_form(g_name_keyboard, g_name_area, nullptr, on_name_ready);
    focus_area(g_name_area);
    // the prefilled name is no input yet: `<` leaves without asking
    set_form_dirty(false);
    return {g_name_mode == kRename ? "Rename / move" : "New folder", nullptr};
}

} // namespace hc::ui::settings::pages

#endif
