// The console command `sd` (CONFIG_HC_DEV_CONSOLE, registered by main like the camera's).

#include <cstdio>
#include <cstring>

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

const char *result_name(Result result)
{
    switch (result) {
    case Result::Ok:
        return "done";
    case Result::NoCard:
        return "no SD card mounted";
    case Result::BadPath:
        return "not a path on the card";
    case Result::NotFound:
        return "not found";
    case Result::Exists:
        return "the name is taken";
    case Result::NotEmpty:
        return "the folder is not empty";
    case Result::Failed:
        break;
    }
    return "failed";
}

int done(Result result)
{
    std::printf("%s\n", result_name(result));
    return result == Result::Ok ? 0 : 1;
}

int print_entries(const char *relative)
{
    const Result r = list(
        relative,
        [](const Entry &entry, void *) {
            if (entry.dir) {
                std::printf("       <dir> %s/\n", entry.name);
            } else {
                std::printf("%12llu %s\n", static_cast<unsigned long long>(entry.size), entry.name);
            }
            return true;
        },
        nullptr);
    return r == Result::Ok ? 0 : done(r);
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
        return print_entries(argc == 3 ? argv[2] : "");
    }
    if (std::strcmp(argv[1], "rm") == 0 && argc == 3) {
        return done(remove(argv[2]));
    }
    if (std::strcmp(argv[1], "mkdir") == 0 && argc == 3) {
        return done(make_dir(argv[2]));
    }
    if (std::strcmp(argv[1], "mv") == 0 && argc == 4) {
        return done(rename(argv[2], argv[3]));
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
    std::printf("usage: sd [ls [<dir>]|rm <path>|mkdir <path>|mv <from> <to>|mount|unmount|format yes|get <url> <path>]\n");
    return 1;
}

} // namespace

void register_console()
{
    esp_console_cmd_t command{};
    command.command = "sd";
    command.help = "SD card: status, list a directory, delete, make a folder, rename or move, mount, unmount, format (erases it), download a URL";
    command.hint = "[ls [<dir>]|rm <path>|mkdir <path>|mv <from> <to>|mount|unmount|format yes|get <url> <path>]";
    command.func = cmd_sd;
    esp_console_cmd_register(&command);
}

} // namespace hc::sdcard
