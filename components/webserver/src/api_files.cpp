// The files of the file system (docs/ui/web.md section 7), login required after the setup:
//
//   GET  /api/fs/files.zip   every file that can be read, as one .zip (stored, not compressed)
//   POST /api/fs/format      confirm=yes: erases the file system (after the export)
//
// The archive is streamed while it is sent: each file goes out with a local header, its data and
// a data descriptor with the CRC and the size, which are only known at the end of the file; the
// central directory at the end lists them all. Nothing larger than one read buffer is held in RAM
// besides the list of entries. Files that cannot be opened or read to the end are named in
// unreadable.txt inside the archive. With a file system that does not mount, the archive holds
// only that note.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_rom_crc.h"
#include "hc/platform/storage.hpp"

#include "http_util.hpp"

namespace hc::webserver::detail {

namespace {

constexpr size_t kBufferSize = 4096;
constexpr size_t kMaxEntries = 256;
constexpr size_t kMaxPath = 160;
constexpr size_t kMaxNotes = 2048;

struct Entry {
    char name[kMaxPath]; // relative to /fs, no leading slash
    uint32_t crc;
    uint32_t size;
    uint32_t offset; // of its local header in the archive
};

// The archive being sent. One at a time: the web server has one task.
struct Zip {
    httpd_req_t *req = nullptr;
    Entry *entries = nullptr;
    size_t count = 0;
    uint32_t offset = 0;
    bool failed = false; // the client went away
    char *notes = nullptr;
    size_t notes_length = 0;
};

void put16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t *p, uint32_t v)
{
    put16(p, static_cast<uint16_t>(v));
    put16(p + 2, static_cast<uint16_t>(v >> 16));
}

void send(Zip &zip, const void *data, size_t size)
{
    if (zip.failed || size == 0) {
        return;
    }
    if (httpd_resp_send_chunk(zip.req, static_cast<const char *>(data), size) != ESP_OK) {
        zip.failed = true;
        return;
    }
    zip.offset += static_cast<uint32_t>(size);
}

void note(Zip &zip, const char *format, const char *name)
{
    if (zip.notes == nullptr) {
        return;
    }
    const int n = std::snprintf(zip.notes + zip.notes_length, kMaxNotes - zip.notes_length, format, name);
    if (n > 0) {
        zip.notes_length = std::min(kMaxNotes - 1, zip.notes_length + static_cast<size_t>(n));
    }
}

// flags: bit 3 data descriptor follows, bit 11 the name is UTF-8
constexpr uint16_t kFlags = (1u << 3) | (1u << 11);
// 2000-01-01 00:00 in DOS format: the device clock may not be set, and the files carry no time
constexpr uint16_t kDosTime = 0;
constexpr uint16_t kDosDate = (20u << 9) | (1u << 5) | 1u;

Entry *begin_entry(Zip &zip, const char *name)
{
    if (zip.count >= kMaxEntries) {
        note(zip, "%s: not in the archive, more than 256 files\n", name);
        return nullptr;
    }
    Entry &entry = zip.entries[zip.count++];
    std::snprintf(entry.name, sizeof(entry.name), "%.*s", static_cast<int>(sizeof(entry.name) - 1), name);
    entry.crc = 0;
    entry.size = 0;
    entry.offset = zip.offset;
    const size_t length = std::strlen(entry.name);
    uint8_t header[30] = {};
    put32(header, 0x04034b50);
    put16(header + 4, 20);
    put16(header + 6, kFlags);
    put16(header + 8, 0); // stored
    put16(header + 10, kDosTime);
    put16(header + 12, kDosDate);
    // CRC and sizes are 0 here, they follow in the data descriptor
    put16(header + 26, static_cast<uint16_t>(length));
    send(zip, header, sizeof(header));
    send(zip, entry.name, length);
    return &entry;
}

void end_entry(Zip &zip, const Entry &entry)
{
    uint8_t descriptor[16];
    put32(descriptor, 0x08074b50);
    put32(descriptor + 4, entry.crc);
    put32(descriptor + 8, entry.size);
    put32(descriptor + 12, entry.size);
    send(zip, descriptor, sizeof(descriptor));
}

void add_data(Zip &zip, Entry &entry, const void *data, size_t size)
{
    entry.crc = esp_rom_crc32_le(entry.crc, static_cast<const uint8_t *>(data), static_cast<uint32_t>(size));
    entry.size += static_cast<uint32_t>(size);
    send(zip, data, size);
}

void add_file(Zip &zip, const char *path, const char *name, char *buffer)
{
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        note(zip, "%s: cannot be opened\n", name);
        return;
    }
    Entry *entry = begin_entry(zip, name);
    if (entry == nullptr) {
        std::fclose(file);
        return;
    }
    size_t n = 0;
    while ((n = std::fread(buffer, 1, kBufferSize, file)) > 0 && !zip.failed) {
        add_data(zip, *entry, buffer, n);
    }
    if (std::ferror(file)) {
        note(zip, "%s: read error, the file in the archive is cut short\n", name);
    }
    std::fclose(file);
    end_entry(zip, *entry);
}

void add_directory(Zip &zip, const char *path, const char *prefix, char *buffer, int depth)
{
    DIR *dir = opendir(path);
    if (dir == nullptr) {
        note(zip, "%s/: cannot be listed\n", prefix[0] != 0 ? prefix : ".");
        return;
    }
    // the paths of this level on the heap: the server task's stack is small and this recurses
    constexpr size_t kPathSize = kMaxPath + 280; // a directory entry name has up to 255 characters
    auto *paths = static_cast<char *>(std::malloc(2 * kPathSize));
    if (paths == nullptr) {
        closedir(dir);
        note(zip, "%s/: no memory to list it\n", prefix[0] != 0 ? prefix : ".");
        return;
    }
    char *child = paths;
    char *name = paths + kPathSize;
    while (dirent *item = readdir(dir)) {
        if (zip.failed) {
            break;
        }
        if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0) {
            continue;
        }
        std::snprintf(child, kPathSize, "%s/%s", path, item->d_name);
        std::snprintf(name, kPathSize, "%s%s%s", prefix, prefix[0] != 0 ? "/" : "", item->d_name);
        struct stat info {};
        if (stat(child, &info) != 0) {
            note(zip, "%s: cannot be read\n", name);
        } else if (S_ISDIR(info.st_mode)) {
            if (depth < 8) {
                add_directory(zip, child, name, buffer, depth + 1);
            }
        } else {
            add_file(zip, child, name, buffer);
        }
    }
    closedir(dir);
    std::free(paths);
}

void finish(Zip &zip)
{
    const uint32_t directory_offset = zip.offset;
    for (size_t i = 0; i < zip.count && !zip.failed; i++) {
        const Entry &entry = zip.entries[i];
        const size_t length = std::strlen(entry.name);
        uint8_t header[46] = {};
        put32(header, 0x02014b50);
        put16(header + 4, 20);
        put16(header + 6, 20);
        put16(header + 8, kFlags);
        put16(header + 10, 0);
        put16(header + 12, kDosTime);
        put16(header + 14, kDosDate);
        put32(header + 16, entry.crc);
        put32(header + 20, entry.size);
        put32(header + 24, entry.size);
        put16(header + 28, static_cast<uint16_t>(length));
        put32(header + 42, entry.offset);
        send(zip, header, sizeof(header));
        send(zip, entry.name, length);
    }
    uint8_t end[22] = {};
    put32(end, 0x06054b50);
    put16(end + 8, static_cast<uint16_t>(zip.count));
    put16(end + 10, static_cast<uint16_t>(zip.count));
    put32(end + 12, zip.offset - directory_offset);
    put32(end + 16, directory_offset);
    send(zip, end, sizeof(end));
}

esp_err_t get_files_zip(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    Zip zip;
    zip.req = req;
    zip.entries = static_cast<Entry *>(std::calloc(kMaxEntries, sizeof(Entry)));
    zip.notes = static_cast<char *>(std::calloc(1, kMaxNotes));
    auto *buffer = static_cast<char *>(std::malloc(kBufferSize));
    if (zip.entries == nullptr || zip.notes == nullptr || buffer == nullptr) {
        std::free(zip.entries);
        std::free(zip.notes);
        std::free(buffer);
        return send_error(req, "503 Service Unavailable", "no memory");
    }

    httpd_resp_set_type(req, "application/zip");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"hassctrl-files.zip\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (platform::storage_status().fs == platform::FsState::Mounted) {
        add_directory(zip, platform::kFsMountPoint, "", buffer, 0);
    } else {
        note(zip, "%s: the file system does not mount, no file can be read\n", platform::kFsPartition);
    }
    if (zip.notes_length > 0) {
        Entry *entry = begin_entry(zip, "unreadable.txt");
        if (entry != nullptr) {
            add_data(zip, *entry, zip.notes, zip.notes_length);
            end_entry(zip, *entry);
        }
    }
    finish(zip);
    if (!zip.failed) {
        httpd_resp_send_chunk(req, nullptr, 0);
    }
    std::free(zip.entries);
    std::free(zip.notes);
    std::free(buffer);
    return ESP_OK;
}

esp_err_t post_format(httpd_req_t *req)
{
    if (!require_login(req)) {
        return ESP_OK;
    }
    char body[32];
    char confirm[8] = {};
    if (!read_body(req, body, sizeof(body)) || !form_value(body, "confirm", confirm, sizeof(confirm)) ||
        std::strcmp(confirm, "yes") != 0) {
        return send_error(req, "400 Bad Request", "confirm=yes required");
    }
    if (platform::format_fs() != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "the file system could not be formatted");
    }
    char buffer[32];
    JsonWriter json(buffer, sizeof(buffer));
    json.begin_object().boolean("formatted", true).end_object();
    return send_json(req, json);
}

} // namespace

void register_file_routes(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/fs/files.zip", .method = HTTP_GET, .handler = get_files_zip, .user_ctx = nullptr},
        {.uri = "/api/fs/format", .method = HTTP_POST, .handler = post_format, .user_ctx = nullptr},
    };
    for (const auto &route : routes) {
        httpd_register_uri_handler(server, &route);
    }
}

} // namespace hc::webserver::detail
