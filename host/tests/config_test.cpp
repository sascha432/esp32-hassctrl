// components/config: the YAML import with includes, the stored JSON and the YAML export.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "check.hpp"
#include "hc/config/dashboard.hpp"

using hc::config::File;
using namespace hc::core;

namespace {

// The files of a folder as an upload: path relative to the folder, with '/'.
struct FileSet {
    std::vector<std::pair<std::string, std::string>> contents;
    std::vector<File> files;

    void add(std::string path, std::string text)
    {
        contents.emplace_back(std::move(path), std::move(text));
    }

    std::span<const File> view()
    {
        files.clear();
        for (const auto &[path, text] : contents) {
            files.push_back({path, text});
        }
        return files;
    }
};

bool load_folder(const std::filesystem::path &folder, FileSet &out)
{
    if (!std::filesystem::is_directory(folder)) {
        return false;
    }
    for (const auto &entry : std::filesystem::recursive_directory_iterator(folder)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".yaml") {
            continue;
        }
        std::ifstream stream(entry.path(), std::ios::binary);
        std::stringstream text;
        text << stream.rdbuf();
        out.add(std::filesystem::relative(entry.path(), folder).generic_string(), text.str());
    }
    return true;
}

const std::filesystem::path kRoot = HC_SOURCE_ROOT;

// "ok" or the error of importing `files` from hass.yaml
std::string import_error(FileSet &files)
{
    const auto imported = hc::config::import_yaml(files.view(), "hass.yaml");
    return imported ? std::string("ok") : std::string(imported.error());
}

const Tile *named(const Layout &layout, std::string_view name)
{
    for (const auto &tile : layout.tiles) {
        if (tile.name == name) {
            return &tile;
        }
    }
    return nullptr;
}

// JSON -> layout -> JSON and YAML -> layout -> JSON give the same text
void check_round_trips(const Layout &layout)
{
    const auto json = hc::config::write_json(layout);
    const auto from_json = hc::config::read_json(json);
    REQUIRE(from_json.has_value());
    CHECK(hc::config::write_json(*from_json) == json);

    const auto yaml = hc::config::write_yaml(layout);
    const File single[] = {{"backup.yaml", yaml}};
    const auto from_yaml = hc::config::import_yaml(single, "backup.yaml");
    if (!from_yaml) {
        hc::test::fail(__FILE__, __LINE__, "export does not import: " + std::string(from_yaml.error()));
        return;
    }
    CHECK(hc::config::write_json(from_yaml->layout) == json);
}

} // namespace

TEST(config_imports_the_documented_sample)
{
    // docs/dashboard-sample is the guide of the file format: it has to stay valid
    FileSet files;
    REQUIRE(load_folder(kRoot / "docs/dashboard-sample", files));
    const auto imported = hc::config::import_yaml(files.view(), "hass.yaml");
    if (!imported) {
        hc::test::fail(__FILE__, __LINE__, std::string(imported.error()));
        return;
    }
    const Layout &layout = imported->layout;
    REQUIRE(imported->other_keys.size() == 1);
    CHECK(imported->other_keys[0] == "hass");
    CHECK(layout.grid == (Span{4, 3}));
    CHECK(layout.pages.size() == 3);
    CHECK(layout.tiles.size() == 18);

    const Tile *ceiling = named(layout, "Ceiling");
    REQUIRE(ceiling != nullptr);
    CHECK(ceiling->kind == TileKind::Light && ceiling->dimmer && ceiling->step == 5);
    CHECK(ceiling->span == (Span{1, 2}) && ceiling->landscape == (Cell{0, 0}));
    CHECK(named(layout, "Hearth")->landscape == (Cell{1, 0}));
    CHECK(named(layout, "Moonbase")->landscape == (Cell{2, 1}));
    CHECK(named(layout, "Moonbase")->unit == "\xC2\xB0" "C" && named(layout, "Moonbase")->decimals == 1);
    const Tile *owl = named(layout, "Owl");
    REQUIRE(owl != nullptr);
    CHECK(owl->landscape == (Cell{0, 2}) && owl->span == (Span{2, 1}) && owl->refresh_s == 30);
    const Tile *rooms = named(layout, "Rooms");
    REQUIRE(rooms != nullptr);
    CHECK(rooms->landscape == (Cell{2, 2}));
    CHECK(named(layout, "Movie Night")->landscape == (Cell{3, 2}));
    CHECK(layout.pages[rooms->opens].grid == (Span{3, 3}));
    CHECK(named(layout, "Workshop")->landscape == (Cell{0, 1}));
    const Tile *anvil = named(layout, "Anvil #2");
    REQUIRE(anvil != nullptr);
    CHECK(anvil->icon == "none" && layout.pages[anvil->page].depth == 2);
    CHECK(layout.pages[anvil->page].grid == (Span{4, 3})); // the default grid
    CHECK(layout.entities.size() == 14);
    for (size_t i = 0; i < layout.tiles.size(); i++) {
        CHECK(layout.tiles[i].id == i + 1);
    }
    check_round_trips(layout);
}

TEST(config_imports_the_converted_dashboard_of_the_developer)
{
    // docs/.retracted is git-ignored: the test runs where the file set exists
    FileSet files;
    if (!load_folder(kRoot / "docs/.retracted/dashboard", files)) {
        std::printf("  skipped: docs/.retracted/dashboard is not there\n");
        return;
    }
    const auto imported = hc::config::import_yaml(files.view(), "hass.yaml");
    if (!imported) {
        hc::test::fail(__FILE__, __LINE__, std::string(imported.error()));
        return;
    }
    const Layout &layout = imported->layout;
    // 2026-10-08: the TestCams area added one page and seven tiles (two of them HA streams)
    CHECK(layout.pages.size() == 25);
    CHECK(layout.tiles.size() == 141); // tiles, 4 spacers of the old positions, 24 areas
    check_round_trips(layout);
}

TEST(config_rejects_the_format_of_the_old_firmware)
{
    const auto error = [](const char *text) {
        FileSet files;
        files.add("hass.yaml", text);
        return import_error(files);
    };
    CHECK_STR(error("tiles:\n  - type: dimmer\n    entity: light.a\n"),
              "hass.yaml:2: type: is only for area and spacer, the entity decides what the tile is");
    CHECK_STR(error("tiles:\n  - entity: camera.a\n    icon: bulb\n"), "hass.yaml:2: unknown icon 'bulb'");
    CHECK_STR(error("tiles:\n  - entity: switch.a\n    icon: auto\n"), "hass.yaml:2: unknown icon 'auto'");
    CHECK_STR(error("defaults:\n  grid:\n    cols: 4\ntiles:\n  - entity: switch.a\n"),
              "hass.yaml:3: grid is columns x rows, for example 4x3");
    CHECK_STR(error("tiles:\n  - type: area\n    name: A\n    grid:\n      rows: 3\n    tiles:\n      - entity: switch.a\n"),
              "hass.yaml:5: grid is columns x rows, for example 4x3");
    CHECK_STR(error("grid: 4x2\ntiles:\n  - entity: switch.a\n"), "ok"); // an unknown top-level key is handed on
}

TEST(config_names_file_line_and_include_chain)
{
    FileSet files;
    files.add("hass.yaml", "tiles:\n"
                           "  - include: rooms.yaml\n");
    files.add("rooms.yaml", "- type: area\n"
                            "  name: Rooms\n"
                            "  tiles:\n"
                            "    - include: rooms/office.yaml\n");
    files.add("rooms/office.yaml", "- entity: switch.desk\n"
                                   "  nmae: Desk\n");
    CHECK_STR(import_error(files),
              "rooms/office.yaml:2: unknown tile key 'nmae' (included from rooms.yaml:4, included from hass.yaml:2)");
}

TEST(config_reports_problems_of_core_at_the_tile)
{
    FileSet files;
    files.add("hass.yaml", "tiles:\n"
                           "  - include: more.yaml\n");
    files.add("more.yaml", "- entity: sensor.a\n"
                           "\n"
                           "- entity: sensor.b\n"
                           "  dimmer: true\n");
    CHECK_STR(import_error(files), "more.yaml:3: dimmer is only for lights (included from hass.yaml:2)");

    FileSet full;
    full.add("hass.yaml", "defaults:\n"
                          "  grid: 1x1\n"
                          "tiles:\n"
                          "  - entity: sensor.a\n"
                          "  - entity: sensor.b\n");
    CHECK_STR(import_error(full), "hass.yaml:5: no free 1x1 block left in the 1x1 grid of its page");
}

TEST(config_checks_includes)
{
    const auto error = [](const char *include, const char *other_path = "x.yaml", const char *other = "- entity: switch.x\n") {
        FileSet files;
        files.add("hass.yaml", std::string("tiles:\n  - include: ") + include + "\n");
        files.add(other_path, other);
        return import_error(files);
    };
    CHECK_STR(error("x.yaml"), "ok");
    CHECK_STR(error("./x.yaml"), "ok");
    CHECK_STR(error("missing.yaml"), "hass.yaml:2: missing.yaml is not in the uploaded files");
    CHECK_STR(error("../x.yaml"), "hass.yaml:2: the include leaves the uploaded files");
    CHECK_STR(error("/x.yaml"), "hass.yaml:2: the include leaves the uploaded files");
    CHECK_STR(error("C:/x.yaml"), "hass.yaml:2: the include leaves the uploaded files");
    CHECK_STR(error("x.yaml", "x.yaml", "entity: switch.x\n"), "x.yaml:1: an included file is a list of tiles (included from hass.yaml:2)");
    CHECK_STR(error("x.yaml", "x.yaml", "- include: x.yaml\n"), "x.yaml:1: x.yaml includes itself (included from hass.yaml:2)");

    FileSet deep;
    deep.add("hass.yaml", "tiles:\n  - include: 1.yaml\n");
    for (int level = 1; level <= 5; level++) {
        deep.add(std::to_string(level) + ".yaml", "- include: " + std::to_string(level + 1) + ".yaml\n");
    }
    deep.add("6.yaml", "- entity: switch.x\n");
    CHECK(import_error(deep).find("includes are nested more than 4 levels deep") != std::string::npos);

    FileSet extra;
    extra.add("hass.yaml", "tiles:\n  - include: x.yaml\n    name: X\n");
    extra.add("x.yaml", "- entity: switch.x\n");
    CHECK_STR(import_error(extra), "hass.yaml:2: an include entry has no other keys");
}

TEST(config_checks_tile_keys)
{
    const auto error = [](const char *tile) {
        FileSet files;
        files.add("hass.yaml", std::string("tiles:\n  - ") + tile);
        return import_error(files);
    };
    CHECK_STR(error("entity: switch.a\n    position: [2, 1]\n"),
              "hass.yaml:3: position: is gone: tiles fill the grid in list order, a spacer keeps a cell free");
    CHECK_STR(error("type: sensor\n    entity: switch.a\n"),
              "hass.yaml:2: type: is only for area and spacer, the entity decides what the tile is");
    CHECK_STR(error("name: X\n"), "hass.yaml:2: a tile needs an entity, or type: area or spacer");
    CHECK_STR(error("entity: Sensor.a\n"),
              "hass.yaml:2: 'Sensor.a' is not an entity id (domain.name: lowercase letters, digits and _)");
    CHECK_STR(error("entity: light.a\n    dimmer: yes\n"), "hass.yaml:3: dimmer is true or false");
    CHECK_STR(error("entity: light.a\n    size: 1 x 2\n"), "hass.yaml:3: size is columns x rows, for example 1x2");
    CHECK_STR(error("entity: switch.a\n    grid: 4x3\n"), "hass.yaml:3: grid is only for areas");
    CHECK_STR(error("type: area\n    name: A\n"), "hass.yaml:2: an area needs tiles: with a list of tiles");
    CHECK_STR(error("entity: climate.a\n    step: -1\n"), "hass.yaml:3: step is a number above 0, for example 0.5");
    CHECK_STR(error("entity: sensor.a\n    decimals: two\n"), "hass.yaml:3: decimals is a whole number");
    CHECK_STR(error("entity: camera.a\n    refresh: 0\n"), "hass.yaml:3: refresh is a number of seconds, 1 to 3600");
    CHECK_STR(error("entity: sensor.a\n    stream: http://cam/x.mjpeg\n"), "hass.yaml:2: stream is only for picture tiles");
    CHECK_STR(error("entity: camera.a\n    stream: rtsp://cam/x\n"),
              "hass.yaml:2: stream is hass or an http:// address of an MJPEG stream");
}

TEST(config_keeps_the_stream_of_a_camera)
{
    FileSet files;
    files.add("hass.yaml", "tiles:\n"
                           "  - entity: camera.balcony\n"
                           "    stream: http://192.168.0.62:1984/api/stream.mjpeg?src=balcony\n"
                           "  - entity: camera.door\n"
                           "    stream: hass\n");
    const auto imported = hc::config::import_yaml(files.view(), "hass.yaml");
    REQUIRE(imported.has_value());
    CHECK(imported->layout.tiles[0].stream == "http://192.168.0.62:1984/api/stream.mjpeg?src=balcony");
    CHECK(imported->layout.tiles[1].stream == hc::core::kHassStream);
    check_round_trips(imported->layout);
}

TEST(config_checks_the_main_file)
{
    FileSet list_only;
    list_only.add("hass.yaml", "- entity: switch.a\n");
    CHECK_STR(import_error(list_only), "hass.yaml:1: the main file is a map with defaults: and tiles:");

    FileSet no_tiles;
    no_tiles.add("hass.yaml", "defaults:\n  grid: 4x2\n");
    CHECK_STR(import_error(no_tiles), "hass.yaml:1: tiles: is a list of tiles");

    FileSet bad_default;
    bad_default.add("hass.yaml", "defaults:\n  rows: 3\ntiles:\n  - entity: switch.a\n");
    CHECK_STR(import_error(bad_default), "hass.yaml:2: unknown key 'rows' in defaults");

    FileSet missing;
    CHECK_STR(import_error(missing), "hass.yaml is not in the uploaded files");
}

TEST(config_json_is_strict)
{
    CHECK_STR(std::string(hc::config::read_json("{").error()), "the stored dashboard is not valid JSON");
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"x\",\"version\":1}").error()), "the stored file is not a dashboard");
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2}").error()),
              "the stored dashboard has a version this firmware does not read");
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":1,"
                                                "\"tiles\":[{\"id\":1,\"entity\":\"switch.a\",\"nmae\":\"x\"}]}")
                              .error()),
              "tile 1: bad key or value 'nmae'");
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":1,"
                                                "\"tiles\":[{\"id\":4,\"entity\":\"sensor.a\",\"dimmer\":true}]}")
                              .error()),
              "tile 4: dimmer is only for lights");

    const auto read = hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":1,\"revision\":7,"
                                            "\"next_id\":20,\"tiles\":[{\"id\":5,\"entity\":\"switch.a\"},"
                                            "{\"id\":0,\"entity\":\"switch.b\"}]}");
    if (read) {
        hc::test::fail(__FILE__, __LINE__, "an id of 0 was accepted");
    }
    const auto ok = hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":1,\"revision\":7,"
                                          "\"next_id\":20,\"tiles\":[{\"id\":5,\"entity\":\"switch.a\"}]}");
    REQUIRE(ok.has_value());
    CHECK(ok->revision == 7 && ok->next_id == 20 && ok->tiles[0].id == 5);
}
