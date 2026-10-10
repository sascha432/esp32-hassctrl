// The backup file (see hc/config/backup.hpp).

#include "hc/config/backup.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace hc::config::backup {

namespace yaml = formats::yaml;

namespace {

memory::String text_of(std::string_view a, std::string_view b = {}, std::string_view c = {})
{
    memory::String out;
    out.append(a.data(), a.size());
    out.append(b.data(), b.size());
    out.append(c.data(), c.size());
    return out;
}

// the header keys and the dashboard's keys, which are no section
constexpr std::string_view kHeaderKeys[] = {"format", "version", "board", "firmware"};
constexpr std::string_view kDashboardKeys[] = {"defaults", "tiles"};

bool is_one_of(std::string_view key, std::span<const std::string_view> keys)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

void check_header(const yaml::Node &root, Report &report)
{
    if (const yaml::Node *format = root.find("format"); format != nullptr) {
        if (format->kind != yaml::Kind::Scalar || format->text != kFormat) {
            report.problem("format", text_of("not a backup of this firmware (expected ", kFormat, ")"));
        }
    }
    if (const yaml::Node *version = root.find("version"); version != nullptr) {
        long value = 0;
        const std::string_view text = version->kind == yaml::Kind::Scalar ? std::string_view(version->text) : "";
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || error != std::errc() || end != text.data() + text.size() || value < 1) {
            report.problem("version", "a whole number");
        } else if (value > kVersion) {
            report.problem("version", "made by a newer firmware, update this device first");
        }
    }
}

// one pass over the file: every section and the dashboard read it with `apply`
void pass(const yaml::Node &root, std::string_view text, std::span<const Section> sections,
          const Dashboard *dashboard, bool apply, Report &report)
{
    bool has_dashboard = false;
    for (const yaml::Entry &entry : root.map) {
        if (is_one_of(entry.key, kHeaderKeys)) {
            continue;
        }
        if (is_one_of(entry.key, kDashboardKeys)) {
            has_dashboard = true;
            continue;
        }
        const auto section = std::find_if(sections.begin(), sections.end(),
                                          [&](const Section &s) { return entry.key == s.name; });
        if (section == sections.end()) {
            report.ignored.push_back(entry.key);
        }
    }
    // in the order of the table: a section may depend on one before it
    for (const Section &section : sections) {
        const yaml::Node *value = root.find(section.name);
        if (value == nullptr) {
            continue;
        }
        if (section.available != nullptr && !section.available()) {
            report.ignored.push_back(text_of(section.name, " (not on this device)"));
            continue;
        }
        report.sections.push_back(section.name);
        section.read(*value, apply, report);
    }
    if (!has_dashboard) {
        return;
    }
    if (dashboard == nullptr) {
        report.ignored.push_back("defaults, tiles (this firmware has no dashboard)");
        return;
    }
    report.sections.push_back("dashboard");
    memory::String error;
    if (!dashboard->read(text, apply, error)) {
        report.problem("dashboard", error);
    }
}

} // namespace

// ------------------------------------------------------------------------------------ Report

void Report::problem(std::string_view where, std::string_view message)
{
    problems.push_back(text_of(where, ": ", message));
}

void Report::note(std::string_view where, std::string_view message)
{
    notes.push_back(text_of(where, ": ", message));
}

// ------------------------------------------------------------------------------------ Writer

void Writer::section(std::string_view name)
{
    if (!_out.empty()) {
        _out += '\n';
    }
    _out.append(name.data(), name.size());
    _out += ":\n";
}

void Writer::key(std::string_view key, int level, bool item)
{
    _out.append(static_cast<size_t>(level) * 2, ' ');
    if (item) {
        _out += "- ";
    }
    _out.append(key.data(), key.size());
    _out += ':';
}

void Writer::text(std::string_view key, std::string_view value, int level)
{
    this->key(key, level, false);
    _out += ' ';
    _out += yaml::quote(value);
    _out += '\n';
}

void Writer::number(std::string_view key, long value, int level)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%ld", value);
    this->key(key, level, false);
    _out += ' ';
    _out += text;
    _out += '\n';
}

void Writer::boolean(std::string_view key, bool value, int level)
{
    this->key(key, level, false);
    _out += value ? " true\n" : " false\n";
}

void Writer::flow(std::string_view key, std::span<const std::string_view> words, int level)
{
    this->key(key, level, false);
    _out += " [";
    for (size_t i = 0; i < words.size(); i++) {
        _out += i == 0 ? "" : ", ";
        _out.append(words[i].data(), words[i].size());
    }
    _out += "]\n";
}

void Writer::open(std::string_view key, int level)
{
    this->key(key, level, false);
    _out += '\n';
}

void Writer::item(std::string_view key, std::string_view value, int level)
{
    this->key(key, level, true);
    _out += ' ';
    _out += yaml::quote(value);
    _out += '\n';
}

void Writer::comment(std::string_view text, int level)
{
    _out.append(static_cast<size_t>(level) * 2, ' ');
    _out += "# ";
    _out.append(text.data(), text.size());
    _out += '\n';
}

// ------------------------------------------------------------------------------------ Fields

Fields::Fields(const yaml::Node &node, std::string_view path, Report &report)
    : _node(node), _path(path.data(), path.size()), _report(report)
{
    if (!is_map()) {
        _report.problem(path, "a map of fields expected");
    }
}

memory::String Fields::path(std::string_view key) const
{
    return text_of(_path, ".", key);
}

void Fields::problem(std::string_view key, std::string_view message)
{
    _report.problem(path(key), message);
}

const yaml::Node *Fields::node(std::string_view key)
{
    _asked.push_back(key);
    const yaml::Node *found = is_map() ? _node.find(key) : nullptr;
    // "key:" with nothing after it counts as not there
    return (found != nullptr && found->kind != yaml::Kind::Null) ? found : nullptr;
}

const yaml::Node *Fields::scalar(std::string_view key)
{
    const yaml::Node *found = node(key);
    if (found != nullptr && found->kind != yaml::Kind::Scalar) {
        problem(key, "a single value expected");
        return nullptr;
    }
    return found;
}

bool Fields::number(std::string_view key, long min, long max, long &value)
{
    const yaml::Node *found = scalar(key);
    if (found == nullptr) {
        return false;
    }
    const std::string_view text = found->text;
    long parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (text.empty() || error != std::errc() || end != text.data() + text.size() || parsed < min || parsed > max) {
        char message[64];
        std::snprintf(message, sizeof(message), "a whole number from %ld to %ld", min, max);
        problem(key, message);
        return false;
    }
    value = parsed;
    return true;
}

bool Fields::boolean(std::string_view key, bool &value)
{
    const yaml::Node *found = scalar(key);
    if (found == nullptr) {
        return false;
    }
    if (found->text != "true" && found->text != "false") {
        problem(key, "true or false");
        return false;
    }
    value = found->text == "true";
    return true;
}

bool Fields::text(std::string_view key, size_t max_length, memory::String &value)
{
    const yaml::Node *found = scalar(key);
    if (found == nullptr) {
        return false;
    }
    if (found->text.size() > max_length) {
        char message[48];
        std::snprintf(message, sizeof(message), "at most %u characters", static_cast<unsigned>(max_length));
        problem(key, message);
        return false;
    }
    value = found->text;
    return true;
}

bool Fields::choice(std::string_view key, std::span<const std::string_view> choices, size_t &value)
{
    const yaml::Node *found = scalar(key);
    if (found == nullptr) {
        return false;
    }
    const auto match = std::find(choices.begin(), choices.end(), std::string_view(found->text));
    if (match == choices.end()) {
        memory::String message = "one of ";
        for (size_t i = 0; i < choices.size(); i++) {
            message += i == 0 ? "" : ", ";
            message.append(choices[i].data(), choices[i].size());
        }
        problem(key, message);
        return false;
    }
    value = static_cast<size_t>(match - choices.begin());
    return true;
}

void Fields::finish()
{
    if (!is_map()) {
        return;
    }
    for (const yaml::Entry &entry : _node.map) {
        if (std::find(_asked.begin(), _asked.end(), std::string_view(entry.key)) == _asked.end()) {
            _report.ignored.push_back(path(entry.key));
        }
    }
}

// ------------------------------------------------------------------------------------- file

memory::String write(const Device &device, std::span<const Section> sections, const Dashboard *dashboard,
                     bool settings, bool with_dashboard)
{
    memory::String out;
    Writer writer(out);
    out += text_of("format: ", kFormat, "\n");
    char version[16];
    std::snprintf(version, sizeof(version), "version: %ld\n", kVersion);
    out += version;
    out += text_of("board: ", yaml::quote(device.board), "\n");
    out += text_of("firmware: ", yaml::quote(device.firmware), "\n");
    if (settings) {
        for (const Section &section : sections) {
            if (section.available == nullptr || section.available()) {
                section.write(writer);
            }
        }
    }
    if (with_dashboard && dashboard != nullptr) {
        const memory::String tiles = dashboard->write();
        if (!tiles.empty()) {
            out += '\n';
            out += tiles;
        }
    }
    return out;
}

Report read(std::string_view text, std::span<const Section> sections, const Dashboard *dashboard, bool apply)
{
    Report report;
    auto root = yaml::parse(text);
    if (!root) {
        char where[24];
        std::snprintf(where, sizeof(where), "line %u", static_cast<unsigned>(root.error().line));
        report.problem(where, root.error().message);
        return report;
    }
    if (root->kind != yaml::Kind::Map) {
        report.problem("file", "a map of sections expected (network:, display:, tiles:, ...)");
        return report;
    }
    check_header(*root, report);
    if (!report.problems.empty()) {
        return report;
    }
    pass(*root, text, sections, dashboard, false, report);
    if (!apply || !report.problems.empty()) {
        return report;
    }
    // the same again, applied; its report replaces the one of the check
    Report applied;
    pass(*root, text, sections, dashboard, true, applied);
    return applied;
}

} // namespace hc::config::backup
