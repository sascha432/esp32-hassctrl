#include "dashboard_file.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "hc/config/dashboard.hpp"

namespace sim {

bool load_dashboard(const std::string &path, hc::core::Layout &layout, std::string &error)
{
    namespace fs = std::filesystem;
    const fs::path entry = fs::absolute(path);
    if (!fs::is_regular_file(entry)) {
        error = "no such file: " + entry.string();
        return false;
    }
    const fs::path folder = entry.parent_path();
    std::vector<std::pair<std::string, std::string>> texts;
    for (const auto &item : fs::recursive_directory_iterator(folder)) {
        if (!item.is_regular_file() || item.path().extension() != ".yaml") {
            continue;
        }
        std::ifstream stream(item.path(), std::ios::binary);
        std::stringstream text;
        text << stream.rdbuf();
        texts.emplace_back(fs::relative(item.path(), folder).generic_string(), text.str());
    }
    std::vector<hc::config::File> files;
    for (const auto &[name, text] : texts) {
        files.push_back({name, text});
    }
    const auto imported = hc::config::import_yaml(files, entry.filename().generic_string());
    if (!imported) {
        error = std::string(imported.error());
        return false;
    }
    layout = imported->layout;
    return true;
}

} // namespace sim
