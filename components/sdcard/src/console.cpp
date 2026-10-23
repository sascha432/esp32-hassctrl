// The console command `sd` (CONFIG_HC_DEV_CONSOLE, registered by main like the camera's).

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_console.h"
#include "hc/sdcard/sdcard.hpp"

namespace hc::sdcard {

namespace {

int print_status()
{
    const Status s = status();
    if (s.state != State::Mounted) {
        std::printf("SD card: %s\n", state_name(s.state));
        return 0;
    }
    char space[40];
    describe(s, space, sizeof(space));
    std::printf("SD card %s at %s: %s, %lu kHz\n", s.name, kMountPoint, space, static_cast<unsigned long>(s.clock_khz));
    const FetchStatus f = fetch_status();
    if (f.state != FetchState::Idle) {
        std::printf("download %s: %s %zu of %zu bytes%s%s\n", f.path,
                    f.state == FetchState::Running ? "running" : f.state == FetchState::Done ? "done" : "failed",
                    f.done, f.total, f.error[0] != 0 ? ", " : "", f.error);
    }
    return 0;
}

int list(const char *relative)
{
    char path[kMaxPath];
    if (!make_path(relative, path, sizeof(path))) {
        std::printf("not a path on the card: %s\n", relative);
        return 1;
    }
    DIR *dir = opendir(path);
    if (dir == nullptr) {
        std::printf("cannot open %s\n", path);
        return 1;
    }
    char entry_path[kMaxPath + 256];
    while (const dirent *entry = readdir(dir)) {
        std::snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);
        struct stat info {};
        stat(entry_path, &info);
        if (S_ISDIR(info.st_mode)) {
            std::printf("       <dir> %s/\n", entry->d_name);
        } else {
            std::printf("%12ld %s\n", static_cast<long>(info.st_size), entry->d_name);
        }
    }
    closedir(dir);
    return 0;
}

int result(esp_err_t err)
{
    if (err != ESP_OK) {
        std::printf("failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return print_status();
}

int cmd_sd(int argc, char **argv)
{
    if (argc == 1) {
        return print_status();
    }
    if (std::strcmp(argv[1], "ls") == 0 && argc <= 3) {
        return list(argc == 3 ? argv[2] : "");
    }
    if (std::strcmp(argv[1], "mount") == 0 && argc == 2) {
        return result(mount());
    }
    if (std::strcmp(argv[1], "unmount") == 0 && argc == 2) {
        return result(unmount());
    }
    if (std::strcmp(argv[1], "format") == 0 && argc == 3 && std::strcmp(argv[2], "yes") == 0) {
        return result(format());
    }
    if (std::strcmp(argv[1], "get") == 0 && argc == 4) {
        const esp_err_t err = fetch_start(argv[2], argv[3]);
        if (err != ESP_OK) {
            std::printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        std::printf("downloading, `sd` shows the progress\n");
        return 0;
    }
    std::printf("usage: sd [ls [<dir>]|mount|unmount|format yes|get <url> <path>]\n");
    return 1;
}

} // namespace

void register_console()
{
    esp_console_cmd_t command{};
    command.command = "sd";
    command.help = "SD card: status, list a directory, mount, unmount, format (erases it), download a URL";
    command.hint = "[ls [<dir>]|mount|unmount|format yes|get <url> <path>]";
    command.func = cmd_sd;
    esp_console_cmd_register(&command);
}

} // namespace hc::sdcard
