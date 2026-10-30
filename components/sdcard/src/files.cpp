// The file operations of the web UI, the screen's file manager and the console (sdcard.hpp).

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hc/sdcard/sdcard.hpp"

namespace hc::sdcard {

namespace {

bool mounted()
{
    return status().state == State::Mounted;
}

// a path below the root, as "/sd/..."
Result below_root(const char *relative, char *out)
{
    if (!mounted()) {
        return Result::NoCard;
    }
    if (!make_path(relative, out, kMaxPath) || std::strcmp(out, kMountPoint) == 0) {
        return Result::BadPath;
    }
    return Result::Ok;
}

} // namespace

Result list(const char *dir, bool (*add)(const Entry &entry, void *context), void *context, bool sizes)
{
    if (!mounted()) {
        return Result::NoCard;
    }
    char path[kMaxPath];
    if (!make_path(dir, path, sizeof(path))) {
        return Result::BadPath;
    }
    DIR *handle = opendir(path);
    if (handle == nullptr) {
        return Result::NotFound;
    }
    char entry_path[kMaxPath + 256];
    while (const dirent *entry = readdir(handle)) {
        // FAT's readdir says folder or file; stat() searches the folder again for the size
        bool is_dir = entry->d_type == DT_DIR;
        uint64_t size = 0;
        if (sizes || entry->d_type == DT_UNKNOWN) {
            std::snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);
            struct stat info {};
            stat(entry_path, &info);
            is_dir = S_ISDIR(info.st_mode);
            size = is_dir ? 0 : static_cast<uint64_t>(info.st_size);
        }
        if (!add({entry->d_name, size, is_dir}, context)) {
            break;
        }
    }
    closedir(handle);
    return Result::Ok;
}

Result remove(const char *path)
{
    char full[kMaxPath];
    const Result checked = below_root(path, full);
    if (checked != Result::Ok) {
        return checked;
    }
    struct stat info {};
    if (stat(full, &info) != 0) {
        return Result::NotFound;
    }
    if (S_ISDIR(info.st_mode)) {
        return rmdir(full) == 0 ? Result::Ok : Result::NotEmpty;
    }
    return unlink(full) == 0 ? Result::Ok : Result::Failed;
}

Result make_dir(const char *path)
{
    char full[kMaxPath];
    const Result checked = below_root(path, full);
    if (checked != Result::Ok) {
        return checked;
    }
    if (mkdir(full, 0755) == 0) {
        return Result::Ok;
    }
    return errno == EEXIST ? Result::Exists : errno == ENOENT ? Result::NotFound : Result::Failed;
}

Result rename(const char *from, const char *to)
{
    char source[kMaxPath];
    char target[kMaxPath];
    Result checked = below_root(from, source);
    if (checked == Result::Ok) {
        checked = below_root(to, target);
    }
    if (checked != Result::Ok) {
        return checked;
    }
    struct stat info {};
    if (stat(source, &info) != 0) {
        return Result::NotFound;
    }
    if (std::strcmp(source, target) == 0) {
        return Result::Ok;
    }
    // a folder into itself or one of its folders
    const size_t length = std::strlen(source);
    if (std::strncmp(source, target, length) == 0 && target[length] == '/') {
        return Result::BadPath;
    }
    struct stat other {};
    if (stat(target, &other) == 0) {
        return Result::Exists;
    }
    if (std::rename(source, target) == 0) {
        return Result::Ok;
    }
    return errno == ENOENT ? Result::NotFound : Result::Failed;
}

} // namespace hc::sdcard
