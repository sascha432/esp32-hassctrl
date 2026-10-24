#include "ui_files.hpp"

#include <cstdio>
#include <cstring>
#include <strings.h>

#if CONFIG_HC_SDCARD
#include "hc/sdcard/sdcard.hpp"
#endif

namespace ui_files {

#if CONFIG_HC_SDCARD
namespace {

using hc::ui::FileResult;

FileResult result_of(hc::sdcard::Result result)
{
    switch (result) {
    case hc::sdcard::Result::Ok:
        return FileResult::Ok;
    case hc::sdcard::Result::NoCard:
        return FileResult::NoCard;
    case hc::sdcard::Result::BadPath:
        return FileResult::BadName;
    case hc::sdcard::Result::NotFound:
        return FileResult::NotFound;
    case hc::sdcard::Result::Exists:
        return FileResult::Exists;
    case hc::sdcard::Result::NotEmpty:
        return FileResult::NotEmpty;
    case hc::sdcard::Result::Failed:
        break;
    }
    return FileResult::Failed;
}

struct Listing {
    hc::ui::FileEntry *out;
    size_t max;
    size_t count;
};

} // namespace

FileResult list(const char *dir, hc::ui::FileEntry *out, size_t max, size_t &count)
{
    Listing listing{out, max, 0};
    const hc::sdcard::Result r = hc::sdcard::list(
        dir,
        [](const hc::sdcard::Entry &entry, void *context) {
            auto &l = *static_cast<Listing *>(context);
            if (l.count < l.max) {
                hc::ui::FileEntry &e = l.out[l.count];
                std::snprintf(e.name, sizeof(e.name), "%s", entry.name);
                e.size = entry.size;
                e.dir = entry.dir;
            }
            l.count++;
            return true;
        },
        &listing);
    count = listing.count;
    return result_of(r);
}

FileResult remove(const char *path)
{
    return result_of(hc::sdcard::remove(path));
}

FileResult make_dir(const char *path)
{
    return result_of(hc::sdcard::make_dir(path));
}

FileResult rename(const char *from, const char *to)
{
    return result_of(hc::sdcard::rename(from, to));
}
#endif

} // namespace ui_files
