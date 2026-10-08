#include "hc/dashboard/dashboard.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "hc/platform/storage.hpp"

namespace hc::dashboard {

namespace {

constexpr const char *kTag = "dashboard";
constexpr const char *kPath = "/fs/dashboard.json";
constexpr const char *kTempPath = "/fs/dashboard.json.tmp";
// the stored file of a large dashboard is a few dozen KB; this keeps a broken file from filling
// the heap
constexpr size_t kMaxFileSize = 512 * 1024;

std::mutex g_mutex;
std::shared_ptr<const core::Layout> g_layout;
char g_error[160];

void set_text(char *out, size_t size, const char *text)
{
    if (size > 0) {
        std::snprintf(out, size, "%s", text);
    }
}

bool fs_mounted()
{
    return platform::storage_status().fs == platform::FsState::Mounted;
}

// the whole file, or false; `missing` tells a file that does not exist from one that cannot be read
bool read_file(const char *path, memory::String &out, bool &missing)
{
    missing = false;
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        missing = true;
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0 || static_cast<size_t>(size) > kMaxFileSize) {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t read = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    return read == out.size();
}

// temporary file, then rename: the stored file is always the old or the new dashboard
bool write_file(const memory::String &text)
{
    FILE *file = std::fopen(kTempPath, "wb");
    if (file == nullptr) {
        return false;
    }
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::remove(kTempPath);
        return false;
    }
    if (std::rename(kTempPath, kPath) != 0) {
        // a file system whose rename does not replace: remove the old file first
        std::remove(kPath);
        if (std::rename(kTempPath, kPath) != 0) {
            std::remove(kTempPath);
            return false;
        }
    }
    return true;
}

// Stores `layout` as the next revision. The caller holds the mutex.
Result store(core::Layout &&layout, uint32_t revision, char *error, size_t error_size)
{
    if (!fs_mounted()) {
        set_text(error, error_size, "the file system is not available");
        return Result::Failed;
    }
    layout.revision = revision;
    const memory::String json = config::write_json(layout);
    if (!write_file(json)) {
        set_text(error, error_size, "the dashboard could not be written");
        return Result::Failed;
    }
    g_layout = std::make_shared<const core::Layout>(std::move(layout));
    g_error[0] = 0;
    return Result::Ok;
}

uint32_t revision_locked()
{
    return g_layout ? g_layout->revision : 0;
}

char *copy_to_heap(const memory::String &text, size_t &length)
{
    auto *copy = static_cast<char *>(memory::allocate_large(text.size() + 1));
    std::memcpy(copy, text.data(), text.size());
    copy[text.size()] = 0;
    length = text.size();
    return copy;
}

} // namespace

esp_err_t start()
{
    std::lock_guard lock(g_mutex);
    g_layout.reset();
    g_error[0] = 0;
    if (!fs_mounted()) {
        set_text(g_error, sizeof(g_error), "the file system is not available");
        return ESP_OK;
    }
    std::remove(kTempPath); // an interrupted write, the stored file is complete
    memory::String text;
    bool missing = false;
    if (!read_file(kPath, text, missing)) {
        if (!missing) {
            set_text(g_error, sizeof(g_error), "the stored dashboard could not be read");
            ESP_LOGE(kTag, "%s", g_error);
        } else {
            ESP_LOGI(kTag, "no stored dashboard");
        }
        return ESP_OK;
    }
    auto layout = config::read_json(text);
    if (!layout) {
        set_text(g_error, sizeof(g_error), layout.error().c_str());
        ESP_LOGE(kTag, "the stored dashboard is not usable: %s", g_error);
        return ESP_OK;
    }
    g_layout = std::make_shared<const core::Layout>(std::move(*layout));
    ESP_LOGI(kTag, "dashboard revision %u: %u pages, %u tiles", static_cast<unsigned>(g_layout->revision),
             static_cast<unsigned>(g_layout->pages.size()), static_cast<unsigned>(g_layout->tiles.size()));
    return ESP_OK;
}

Status status()
{
    std::lock_guard lock(g_mutex);
    Status status{};
    status.present = g_layout != nullptr;
    status.revision = revision_locked();
    status.pages = g_layout ? static_cast<uint32_t>(g_layout->pages.size()) : 0;
    status.tiles = g_layout ? static_cast<uint32_t>(g_layout->tiles.size()) : 0;
    set_text(status.error, sizeof(status.error), g_error);
    return status;
}

std::shared_ptr<const core::Layout> current()
{
    std::lock_guard lock(g_mutex);
    return g_layout;
}

char *export_json(size_t &length)
{
    const auto layout = current();
    return layout ? copy_to_heap(config::write_json(*layout), length) : nullptr;
}

char *export_yaml(size_t &length)
{
    const auto layout = current();
    return layout ? copy_to_heap(config::write_yaml(*layout), length) : nullptr;
}

void release(char *text)
{
    memory::release_large(text);
}

Result save_json(std::string_view json, uint32_t expected_revision, uint32_t &revision, char *error,
                 size_t error_size)
{
    auto layout = config::read_json(json);
    if (!layout) {
        set_text(error, error_size, layout.error().c_str());
        return Result::Invalid;
    }
    std::lock_guard lock(g_mutex);
    if (expected_revision != revision_locked()) {
        set_text(error, error_size, "the dashboard was changed in the meantime, load it again");
        return Result::Conflict;
    }
    revision = expected_revision + 1;
    return store(std::move(*layout), revision, error, error_size);
}

Result import_yaml(std::span<const config::File> files, bool check_only, ImportReport &report, char *error,
                   size_t error_size)
{
    auto imported = config::import_yaml(files, "hass.yaml");
    if (!imported) {
        set_text(error, error_size, imported.error().c_str());
        return Result::Invalid;
    }
    report = {};
    report.pages = static_cast<uint32_t>(imported->layout.pages.size());
    report.tiles = static_cast<uint32_t>(imported->layout.tiles.size());
    size_t used = 0;
    for (const auto &key : imported->other_keys) {
        const int n = std::snprintf(report.other_keys + used, sizeof(report.other_keys) - used, "%s%s",
                                    used > 0 ? ", " : "", key.c_str());
        if (n < 0 || static_cast<size_t>(n) >= sizeof(report.other_keys) - used) {
            break;
        }
        used += static_cast<size_t>(n);
    }
    if (check_only) {
        return Result::Ok;
    }
    std::lock_guard lock(g_mutex);
    report.revision = revision_locked() + 1;
    return store(std::move(imported->layout), report.revision, error, error_size);
}

} // namespace hc::dashboard
