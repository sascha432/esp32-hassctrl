// components/config/src/backup.cpp: the backup file, with two made-up sections and a made-up
// dashboard.

#include <string>

#include "check.hpp"
#include "hc/config/backup.hpp"

namespace backup = hc::config::backup;
using hc::memory::String;

namespace {

// The made-up settings: what the sections read and what they applied. std::string, not
// memory::String: a global of the large heap would allocate before main() sets it up (MSVC's debug
// containers allocate even when empty).
struct Light {
    long level = 50;
    bool on = true;
    std::string name = "Desk";
    size_t mode = 0;
};

struct State {
    Light light;
    Light applied_light;
    int light_applies = 0;
    std::string networks;
    std::string applied_networks;
    bool dashboard_applied = false;
    std::string dashboard_text;
};

State g;

constexpr std::string_view kModes[] = {"warm", "cold"};

void write_light(backup::Writer &out)
{
    out.section("light");
    out.number("level", g.light.level);
    out.boolean("on", g.light.on);
    out.text("name", g.light.name);
    out.text("mode", kModes[g.light.mode]);
}

void read_light(const hc::formats::yaml::Node &value, bool apply, backup::Report &report)
{
    Light light = g.light;
    String name(light.name.c_str());
    backup::Fields fields(value, "light", report);
    fields.number("level", 0, 100, light.level);
    fields.boolean("on", light.on);
    fields.text("name", 8, name);
    light.name = name.c_str();
    fields.choice("mode", kModes, light.mode);
    fields.finish();
    if (apply) {
        g.applied_light = light;
        g.light_applies++;
    }
}

void write_networks(backup::Writer &out)
{
    out.section("networks");
    out.open("list");
    out.item("name", "Home");
    out.boolean("open", true, 3);
    out.item("name", "with: colon");
}

void read_networks(const hc::formats::yaml::Node &value, bool apply, backup::Report &report)
{
    backup::Fields fields(value, "networks", report);
    std::string names;
    if (const auto *list = fields.node("list"); list != nullptr) {
        for (const auto &item : list->list) {
            backup::Fields entry(item, fields.path("list"), report);
            String name;
            bool open = false;
            entry.text("name", 32, name);
            entry.boolean("open", open);
            entry.finish();
            names += std::string(name.c_str()) + (open ? "(open)" : "") + ";";
        }
    }
    fields.finish();
    g.networks = names;
    if (apply) {
        g.applied_networks = names;
    }
}

const backup::Section kSections[] = {
    {"light", write_light, read_light},
    {"networks", write_networks, read_networks},
    {"radio", write_networks, read_networks, [] { return false; }},
};

String write_dashboard()
{
    return "defaults:\n  grid: 4x3\n\ntiles:\n  - entity: light.desk\n";
}

bool read_dashboard(std::string_view text, bool apply, String &error)
{
    if (text.find("broken") != std::string_view::npos) {
        error = "tiles: broken";
        return false;
    }
    g.dashboard_text = std::string(text);
    g.dashboard_applied |= apply;
    return true;
}

const backup::Dashboard kDashboard{write_dashboard, read_dashboard};

const backup::Device kDevice{"WT32-SC01", "0.1.0"};

std::string joined(const hc::memory::Vector<String> &lines)
{
    std::string out;
    for (const auto &line : lines) {
        out += (out.empty() ? "" : "|") + std::string(line.c_str());
    }
    return out;
}

} // namespace

TEST(backup_writes_header_sections_and_dashboard)
{
    g = {};
    const String text = backup::write(kDevice, kSections, &kDashboard, true, true);
    CHECK_STR(text.c_str(), "format: hassctrl-backup\n"
                            "version: 1\n"
                            "board: WT32-SC01\n"
                            "firmware: 0.1.0\n"
                            "\n"
                            "light:\n"
                            "  level: 50\n"
                            "  on: true\n"
                            "  name: Desk\n"
                            "  mode: warm\n"
                            "\n"
                            "networks:\n"
                            "  list:\n"
                            "    - name: Home\n"
                            "      open: true\n"
                            "    - name: \"with: colon\"\n"
                            "\n"
                            "defaults:\n"
                            "  grid: 4x3\n"
                            "\n"
                            "tiles:\n"
                            "  - entity: light.desk\n");

    const String settings_only = backup::write(kDevice, kSections, &kDashboard, true, false);
    CHECK(settings_only.find("tiles:") == String::npos);
    const String dashboard_only = backup::write(kDevice, kSections, &kDashboard, false, true);
    CHECK(dashboard_only.find("light:") == String::npos);
    CHECK(dashboard_only.find("tiles:") != String::npos);
}

TEST(backup_reads_back_what_it_wrote)
{
    g = {};
    g.light = {80, false, "Sofa", 1};
    const String text = backup::write(kDevice, kSections, &kDashboard, true, true);
    g.light = {};
    const backup::Report report = backup::read(text, kSections, &kDashboard, true);
    CHECK_STR(joined(report.problems), "");
    CHECK_STR(joined(report.ignored), "");
    CHECK_STR(joined(report.sections), "light|networks|dashboard");
    CHECK(g.applied_light.level == 80 && !g.applied_light.on && g.applied_light.mode == 1);
    CHECK_STR(g.applied_light.name, "Sofa");
    CHECK_STR(g.applied_networks, "Home(open);with: colon;");
    CHECK(g.dashboard_applied);
    CHECK(g.light_applies == 1);
}

TEST(backup_check_only_applies_nothing)
{
    g = {};
    const backup::Report report = backup::read("light:\n  level: 10\n", kSections, &kDashboard, false);
    CHECK_STR(joined(report.problems), "");
    CHECK(g.light_applies == 0);
    CHECK_STR(joined(report.sections), "light");
}

TEST(backup_missing_fields_keep_their_value_and_unknown_ones_are_named)
{
    g = {};
    const backup::Report report =
        backup::read("light:\n  level: 10\n  colour: red\nweather:\n  city: Berlin\n", kSections, &kDashboard, true);
    CHECK_STR(joined(report.problems), "");
    CHECK_STR(joined(report.ignored), "weather|light.colour");
    CHECK(g.applied_light.level == 10 && g.applied_light.on);
    CHECK_STR(g.applied_light.name, "Desk");
    CHECK(g.applied_networks.empty());
    CHECK(!g.dashboard_applied);
}

TEST(backup_one_problem_anywhere_applies_nothing)
{
    g = {};
    const backup::Report report = backup::read("light:\n  level: 101\n  on: yes\n  name: much too long\n  mode: blue\n"
                                               "networks:\n  list:\n    - name: Home\n",
                                               kSections, &kDashboard, true);
    CHECK_STR(joined(report.problems), "light.level: a whole number from 0 to 100|light.on: true or false|"
                                       "light.name: at most 8 characters|light.mode: one of warm, cold");
    CHECK(g.light_applies == 0);
    CHECK(g.applied_networks.empty());

    const backup::Report dashboard = backup::read("light:\n  level: 5\ntiles: broken\n", kSections, &kDashboard, true);
    CHECK_STR(joined(dashboard.problems), "dashboard: tiles: broken");
    CHECK(g.light_applies == 0);
}

TEST(backup_header_is_checked)
{
    CHECK_STR(joined(backup::read("format: something-else\n", kSections, &kDashboard, false).problems),
              "format: not a backup of this firmware (expected hassctrl-backup)");
    CHECK_STR(joined(backup::read("version: 2\n", kSections, &kDashboard, false).problems),
              "version: made by a newer firmware, update this device first");
    CHECK_STR(joined(backup::read("version: x\n", kSections, &kDashboard, false).problems),
              "version: a whole number");
    CHECK_STR(joined(backup::read("- a\n", kSections, &kDashboard, false).problems),
              "file: a map of sections expected (network:, display:, tiles:, ...)");
    CHECK_STR(joined(backup::read("light:\n\tlevel: 1\n", kSections, &kDashboard, false).problems).substr(0, 7),
              "line 2:");
    // a section that is not a map, and a field that is not a value
    CHECK_STR(joined(backup::read("light: 5\n", kSections, &kDashboard, false).problems),
              "light: a map of fields expected");
    CHECK_STR(joined(backup::read("light:\n  level:\n    - 1\n", kSections, &kDashboard, false).problems),
              "light.level: a single value expected");
}

TEST(backup_sections_that_are_not_on_the_device_are_named)
{
    g = {};
    const backup::Report report = backup::read("radio:\n  list: []\n", kSections, &kDashboard, true);
    CHECK_STR(joined(report.problems), "");
    CHECK_STR(joined(report.sections), "");
    CHECK_STR(joined(report.ignored), "radio (not on this device)");
    CHECK(backup::write(kDevice, kSections, nullptr, true, false).find("radio") == String::npos);
}

TEST(backup_without_a_dashboard_names_its_keys)
{
    g = {};
    const backup::Report report = backup::read("tiles:\n  - entity: light.a\n", kSections, nullptr, true);
    CHECK_STR(joined(report.problems), "");
    CHECK_STR(joined(report.ignored), "defaults, tiles (this firmware has no dashboard)");
}

TEST(backup_settings_of_a_dashboard_file_leave_the_dashboard_alone)
{
    g = {};
    const backup::Report report = backup::read_settings(
        "light:\n  level: 20\ndefaults:\n  grid: 4x3\ntiles:\n  - entity: light.a\nweather: 1\n", kSections, true);
    CHECK_STR(joined(report.problems), "");
    CHECK_STR(joined(report.sections), "light");
    CHECK_STR(joined(report.ignored), "weather");
    CHECK(g.applied_light.level == 20);
    CHECK(!g.dashboard_applied);

    const backup::Report none = backup::read_settings("tiles:\n  - entity: light.a\n", kSections, true);
    CHECK_STR(joined(none.problems) + joined(none.sections) + joined(none.ignored), "");
}

TEST(backup_writes_lists_on_one_line)
{
    String out;
    backup::Writer writer(out);
    writer.section("alarm");
    constexpr std::string_view days[] = {"mon", "fri"};
    writer.flow("days", days);
    writer.flow("none", {});
    CHECK_STR(out.c_str(), "alarm:\n  days: [mon, fri]\n  none: []\n");
    const auto parsed = hc::formats::yaml::parse(out);
    REQUIRE(parsed.has_value());
    const auto *alarm = parsed->find("alarm");
    REQUIRE(alarm != nullptr);
    CHECK(alarm->find("days")->list.size() == 2);
    CHECK(alarm->find("none")->kind == hc::formats::yaml::Kind::List && alarm->find("none")->list.empty());
}

TEST(backup_list_items_are_read_with_their_path)
{
    g = {};
    const backup::Report report =
        backup::read("networks:\n  list:\n    - name: A\n      open: maybe\n    - nmae: B\n", kSections, &kDashboard, false);
    CHECK_STR(joined(report.problems), "networks.list.open: true or false");
    CHECK_STR(joined(report.ignored), "networks.list.nmae");
}
