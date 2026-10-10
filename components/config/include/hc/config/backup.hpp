#pragma once

// The backup file: the settings and the dashboard in one YAML file (docs/TODO.md, "Settings
// export and import"). Plain C++, host tested.
//
//   format: hassctrl-backup
//   version: 1
//   board: WT32-SC01
//   firmware: 0.1.0
//   network:
//     ...
//   display:
//     ...
//   defaults:          the dashboard, as config::write_yaml writes it
//   tiles:
//     ...
//
// The sections come from a table that `main` fills (the recovery app has none): each one writes
// its named fields and reads them back. Never a password or a token in an export; an import
// may carry them, added by hand.
//
// An import is checked completely before anything is written: every section reads the file
// with `apply` false first, and only when none of them found a problem they read it again with
// `apply` true. A section that is not in the file stays as it is, a field that is not in the
// file keeps its value, an unknown section or field is named in the report and ignored.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "hc/formats/yaml.hpp"
#include "hc/memory/large.hpp"

namespace hc::config::backup {

constexpr std::string_view kFormat = "hassctrl-backup";
constexpr long kVersion = 1;

// What an import found and did, one line of text each.
struct Report {
    // the import is refused when there is one: "display.brightness: 1 to 100"
    memory::Vector<memory::String> problems;
    // unknown sections and fields: "display.colour"
    memory::Vector<memory::String> ignored;
    // what the user should know: "network: Office needs its password"
    memory::Vector<memory::String> notes;
    // the sections that were in the file (and applied, after an import that was not refused)
    memory::Vector<memory::String> sections;

    void problem(std::string_view where, std::string_view message);
    void note(std::string_view where, std::string_view message);
};

// Writes the fields of one section: "  key: value" under the section's "name:".
class Writer {
public:
    explicit Writer(memory::String &out) : _out(out)
    {
    }

    void section(std::string_view name);
    // `level` 1 is a field of the section; a list under it has its items on level 2 (item())
    // and their further fields on level 3
    void text(std::string_view key, std::string_view value, int level = 1);
    void number(std::string_view key, long value, int level = 1);
    void boolean(std::string_view key, bool value, int level = 1);
    // "  key: [a, b]": plain words only (no commas, brackets or quotes)
    void flow(std::string_view key, std::span<const std::string_view> words, int level = 1);
    // "  key:", a list or map follows on the next level
    void open(std::string_view key, int level = 1);
    // the first field of a list item: "    - key: value"
    void item(std::string_view key, std::string_view value, int level = 2);
    // a comment line, for a hint where a password may go
    void comment(std::string_view text, int level = 1);

private:
    void key(std::string_view key, int level, bool item);

    memory::String &_out;
};

// Reads the named fields of one map (a section, a list item). A field that is not there keeps the
// value it had; a field with a wrong value is a problem named by its path ("display.brightness"),
// and the value stays. finish() names the fields nobody asked for as ignored.
class Fields {
public:
    // `node` is the map; anything else is a problem for `path`
    Fields(const formats::yaml::Node &node, std::string_view path, Report &report);

    bool is_map() const
    {
        return _node.kind == formats::yaml::Kind::Map;
    }

    bool number(std::string_view key, long min, long max, long &value);
    bool boolean(std::string_view key, bool &value);
    // at most `max_length` bytes
    bool text(std::string_view key, size_t max_length, memory::String &value);
    // one of `choices`, `value` becomes its index
    bool choice(std::string_view key, std::span<const std::string_view> choices, size_t &value);
    // the node of `key` (a list or map the caller reads), nullptr when it is not there
    const formats::yaml::Node *node(std::string_view key);
    // a problem with a field, named by its path
    void problem(std::string_view key, std::string_view message);
    // the path of a field, for a nested reader ("network.networks")
    memory::String path(std::string_view key) const;

    // names the fields nobody asked for
    void finish();

    template <class T>
    bool number(std::string_view key, long min, long max, T &value)
    {
        long wide = static_cast<long>(value);
        if (!number(key, min, max, wide)) {
            return false;
        }
        value = static_cast<T>(wide);
        return true;
    }

private:
    const formats::yaml::Node *scalar(std::string_view key);

    const formats::yaml::Node &_node;
    memory::String _path;
    Report &_report;
    memory::Vector<std::string_view> _asked;
};

struct Section {
    const char *name;
    // appends "name:" and the fields; nothing when the section has nothing to save
    void (*write)(Writer &out);
    // reads the section's node: checks only with `apply` false, applies with `apply` true
    void (*read)(const formats::yaml::Node &value, bool apply, Report &report);
    // false: this device has nothing of the section (no audio, no alarm), it is neither written
    // nor read; nullptr: always there
    bool (*available)() = nullptr;
};

// The dashboard part (`defaults:` and `tiles:`).
struct Dashboard {
    // config::write_yaml of the stored dashboard, empty without one
    memory::String (*write)();
    // checks or imports the whole file `text` (it reads `defaults:` and `tiles:` only); false
    // with the problem in `error`
    bool (*read)(std::string_view text, bool apply, memory::String &error);
};

struct Device {
    std::string_view board;
    std::string_view firmware;
};

// The backup file with the settings sections (`settings`) and the dashboard (`dashboard`, when
// there is one).
memory::String write(const Device &device, std::span<const Section> sections, const Dashboard *dashboard,
                     bool settings, bool with_dashboard);

// Checks `text`, and with `apply` imports it when the check found no problem. `report.problems`
// empty: it was (or would be) imported.
Report read(std::string_view text, std::span<const Section> sections, const Dashboard *dashboard, bool apply);

} // namespace hc::config::backup
