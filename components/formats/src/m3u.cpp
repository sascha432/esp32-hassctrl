#include "hc/formats/m3u.hpp"

namespace hc::formats {

namespace {

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// `path` joined to `folder`, "." and ".." resolved; false when it leaves the root
bool join(std::string_view folder, std::string_view path, memory::String &out)
{
    memory::Vector<memory::String> parts;
    auto add = [&](std::string_view text) {
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find('/', start);
            if (end == std::string_view::npos) {
                end = text.size();
            }
            const std::string_view part = text.substr(start, end - start);
            if (part == "..") {
                if (parts.empty()) {
                    return false;
                }
                parts.pop_back();
            } else if (!part.empty() && part != ".") {
                parts.emplace_back(part);
            }
            start = end + 1;
        }
        return true;
    };
    memory::String normal(path);
    for (char &c : normal) {
        c = c == '\\' ? '/' : c;
    }
    const bool absolute = !normal.empty() && normal[0] == '/';
    if ((!absolute && !add(folder)) || !add(normal)) {
        return false;
    }
    out.clear();
    for (const memory::String &part : parts) {
        out += out.empty() ? "" : "/";
        out += part;
    }
    return !out.empty();
}

} // namespace

memory::Vector<PlaylistEntry> parse_m3u(std::string_view text, std::string_view folder)
{
    memory::Vector<PlaylistEntry> out;
    // a byte order mark (M3U8 written by Windows)
    if (text.substr(0, 3) == "\xef\xbb\xbf") {
        text.remove_prefix(3);
    }
    memory::String title;
    while (!text.empty()) {
        size_t end = text.find('\n');
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = trim(text.substr(0, end));
        text.remove_prefix(std::min(end + 1, text.size()));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '#') {
            if (line.substr(0, 8) == "#EXTINF:") {
                const size_t comma = line.find(',');
                title = comma != std::string_view::npos ? memory::String(trim(line.substr(comma + 1))) : "";
            }
            continue;
        }
        PlaylistEntry entry;
        entry.title = std::move(title);
        title.clear();
        if (line.find("://") != std::string_view::npos) {
            entry.location = memory::String(line);
            entry.address = true;
        } else if (!join(folder, line, entry.location)) {
            continue;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

} // namespace hc::formats
