// components/config: the YAML import with includes, the stored JSON and the YAML export.

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "check.hpp"
#include "hc/config/dashboard.hpp"

using hc::config::File;
using namespace hc::core;

constexpr Orientation L = Orientation::Landscape;
constexpr Orientation P = Orientation::Portrait;

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

namespace {

// The sample's settings sections without their `#`: a line "#name:" starts one, the lines
// after it that start with "#  " belong to it.
std::string uncomment_sections(const std::string &text)
{
    std::istringstream in(text);
    std::string out, line;
    bool inside = false;
    while (std::getline(in, line)) {
        const bool starts = line.size() > 2 && line[0] == '#' && std::islower(static_cast<unsigned char>(line[1])) &&
                            line.back() == ':';
        inside = starts || (inside && line.rfind("#  ", 0) == 0);
        out += (inside ? line.substr(1) : line) + "\n";
    }
    return out;
}

} // namespace

TEST(config_sample_settings_sections_are_valid_yaml)
{
    // uncommented, the sections of the sample are read as the settings of the import
    FileSet files;
    REQUIRE(load_folder(kRoot / "docs/dashboard-sample", files));
    for (auto &[path, text] : files.contents) {
        if (path == "hass.yaml") {
            text = uncomment_sections(text);
        }
    }
    const auto imported = hc::config::import_yaml(files.view(), "hass.yaml");
    if (!imported) {
        hc::test::fail(__FILE__, __LINE__, std::string(imported.error()));
        return;
    }
    std::string keys;
    for (const auto &key : imported->other_keys) {
        keys += std::string(key.c_str()) + " ";
    }
    CHECK_STR(keys, "hass webui network camera display sound alarm ");
}

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
    CHECK(imported->other_keys.empty()); // the settings sections are commented out
    CHECK(layout.grid == (Span{4, 3}));
    CHECK(layout.pages.size() == 3);
    CHECK(layout.tiles.size() == 23);

    const Tile *ceiling = named(layout, "Ceiling");
    REQUIRE(ceiling != nullptr);
    CHECK(ceiling->kind == TileKind::Light && ceiling->dimmer && ceiling->step == 5);
    CHECK(ceiling->in(L).size == (Span{1, 2}) && ceiling->in(L).cell == (Cell{0, 0}));
    CHECK(ceiling->in(P).size == (Span{2, 1}) && !ceiling->in(P).has_size);
    CHECK(named(layout, "Hearth")->in(L).cell == (Cell{1, 0}));
    CHECK(named(layout, "Moonbase")->in(L).cell == (Cell{2, 1}));
    CHECK(named(layout, "Moonbase")->unit == "\xC2\xB0" "C" && named(layout, "Moonbase")->decimals == 1);
    const Tile *owl = named(layout, "Owl");
    REQUIRE(owl != nullptr);
    CHECK(owl->in(L).cell == (Cell{0, 2}) && owl->in(L).size == (Span{2, 1}) && owl->refresh_s == 30);
    const Tile *rooms = named(layout, "Rooms");
    REQUIRE(rooms != nullptr);
    CHECK(rooms->in(L).cell == (Cell{2, 2}));
    CHECK(named(layout, "Movie Night")->in(L).cell == (Cell{3, 2}));
    CHECK(layout.pages[rooms->opens].grid == (Span{4, 3}));
    CHECK(named(layout, "Workshop")->in(L).cell == (Cell{3, 0}));
    const Tile *closeup = named(layout, "Owl Closeup");
    REQUIRE(closeup != nullptr);
    CHECK(closeup->in(L).size == (Span{2, 1}) && closeup->in(P).size == (Span{2, 1}) && closeup->in(P).has_size);
    CHECK(closeup->in(P).cell == (Cell{1, 1}) && named(layout, "Weather")->in(P).cell == (Cell{0, 2}));
    const Tile *anvil = named(layout, "Anvil #2");
    REQUIRE(anvil != nullptr);
    CHECK(anvil->icon == "none" && layout.pages[anvil->page].depth == 2);
    CHECK(layout.pages[anvil->page].grid == (Span{4, 3})); // the default grid
    // the Workshop page is arranged in landscape, portrait is automatic
    CHECK(anvil->in(L).has_cell && anvil->in(L).cell == (Cell{2, 0}) && !anvil->in(P).has_cell);
    CHECK(named(layout, "Shutters")->in(L).cell == (Cell{0, 2}));
    const Tile *humidity = named(layout, "Humidity");
    REQUIRE(humidity != nullptr);
    CHECK(humidity->in(L).shown && humidity->in(P).hidden && !humidity->in(P).shown);
    const Tile *weather = named(layout, "Weather");
    REQUIRE(weather != nullptr);
    CHECK(weather->kind == TileKind::Weather && weather->in(L).cell == (Cell{2, 1}));
    // the Workshop area has swipe navigation: no back tile on its page
    CHECK(named(layout, "Workshop")->swipe && !has_back_tile(layout, anvil->page));
    CHECK(has_back_tile(layout, rooms->opens));
    // the two clocks: a London clock on the Rooms page, an analog one on the Workshop page
    const Tile *london = named(layout, "London");
    REQUIRE(london != nullptr);
    CHECK(london->kind == TileKind::Clock && london->zone == "Europe/London" && london->hours12 && london->date &&
          london->style == ClockStyle::Digital && london->in(L).cell == (Cell{3, 1}));
    const Tile *shop = named(layout, "Shop Clock");
    REQUIRE(shop != nullptr);
    CHECK(shop->style == ClockStyle::Analog && shop->ticks == ClockTicks::Minute && shop->border && shop->seconds &&
          shop->in(L).size == (Span{2, 1}) && shop->in(L).cell == (Cell{2, 1}) && shop->in(P).size == (Span{1, 2}));
    CHECK(layout.entities.size() == 17);
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
              "hass.yaml:2: type: is only for area, spacer and clock, the entity decides what the tile is");
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

    // no room is not a problem: the tile is left out on the screen
    FileSet full;
    full.add("hass.yaml", "defaults:\n"
                          "  grid: 1x1\n"
                          "tiles:\n"
                          "  - entity: sensor.a\n"
                          "  - entity: sensor.b\n");
    CHECK_STR(import_error(full), "ok");
    const auto imported = hc::config::import_yaml(full.view(), "hass.yaml");
    REQUIRE(imported.has_value());
    CHECK(imported->layout.tiles[0].in(L).shown && !imported->layout.tiles[1].in(L).shown);
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
    // the keys of version 1 of the format
    CHECK_STR(error("entity: switch.a\n    position: [2, 1]\n"),
              "hass.yaml:3: position: is gone: landscape: and portrait: take size, cell and hidden (version 2 of the format)");
    CHECK_STR(error("entity: switch.a\n    size: 1x2\n"),
              "hass.yaml:3: size: is gone: landscape: and portrait: take size, cell and hidden (version 2 of the format)");
    CHECK_STR(error("entity: switch.a\n    landscape: 2,1\n"),
              "hass.yaml:3: landscape: is a map with size, cell and hidden (version 2 of the format)");
    CHECK_STR(error("entity: switch.a\n    portrait:\n      cell: 32,0\n"), "hass.yaml:4: cell is column,row from 0, for example 2,1");
    CHECK_STR(error("entity: switch.a\n    grid: 4x3\n"), "hass.yaml:3: grid is only for areas");
    CHECK_STR(error("type: area\n    name: A\n    grid: 33x1\n    tiles:\n      - entity: switch.a\n"),
              "hass.yaml:4: grid is columns x rows, for example 4x3");
    CHECK_STR(error("type: area\n    name: A\n    grid: 32x32\n    tiles:\n      - entity: switch.a\n        landscape:\n          cell: 31,31\n          size: 8x8\n"), "ok");
    CHECK_STR(error("entity: switch.a\n    landscape:\n      size: 1 x 2\n"), "hass.yaml:4: size is columns x rows, for example 1x2");
    CHECK_STR(error("entity: switch.a\n    landscape:\n      hidden: yes\n"), "hass.yaml:4: hidden is true or false");
    CHECK_STR(error("entity: switch.a\n    landscape:\n      at: 2,1\n"),
              "hass.yaml:4: unknown key 'at' (landscape: and portrait: take size, cell and hidden)");
    CHECK_STR(error("entity: switch.a\n    landscape:\n      cell: 4,0\n"), "ok"); // outside the grid: left out
    CHECK_STR(error("type: sensor\n    entity: switch.a\n"),
              "hass.yaml:2: type: is only for area, spacer and clock, the entity decides what the tile is");
    CHECK_STR(error("name: X\n"), "hass.yaml:2: a tile needs an entity, or type: area, spacer or clock");
    CHECK_STR(error("type: clock\n    entity: sensor.a\n"), "hass.yaml:2: a clock has no entity");
    CHECK_STR(error("type: clock\n    hours: 13\n"), "hass.yaml:3: hours is 12 or 24");
    CHECK_STR(error("type: clock\n    seconds: yes\n"), "hass.yaml:3: seconds is true or false");
    CHECK_STR(error("type: clock\n    style: round\n"), "hass.yaml:3: style is digital or analog");
    CHECK_STR(error("type: clock\n    ticks: all\n"), "hass.yaml:3: ticks is none, quarter, hour or minute");
    CHECK_STR(error("type: clock\n    border: true\n"), "hass.yaml:2: border and ticks are only for analog clocks");
    CHECK_STR(error("type: clock\n    zone: Nowhere/Town\n"),
              "hass.yaml:2: unknown time zone 'Nowhere/Town' (an IANA name like Europe/Berlin)");
    CHECK_STR(error("entity: sensor.a\n    date: true\n"),
              "hass.yaml:2: hours, seconds, date, style, border, ticks and zone are only for clock tiles");
    CHECK_STR(error("entity: Sensor.a\n"),
              "hass.yaml:2: 'Sensor.a' is not an entity id (domain.name: lowercase letters, digits and _)");
    CHECK_STR(error("entity: light.a\n    dimmer: yes\n"), "hass.yaml:3: dimmer is true or false");
    CHECK_STR(error("entity: light.a\n    swipe: true\n"), "hass.yaml:2: swipe is only for areas");
    CHECK_STR(error("type: area\n    name: A\n    swipe: maybe\n    tiles:\n      - entity: switch.a\n"),
              "hass.yaml:4: swipe is true or false");
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

TEST(config_exports_a_stream_without_its_password)
{
    FileSet files;
    files.add("hass.yaml", "tiles:\n"
                           "  - entity: camera.a\n"
                           "    stream: http://viewer:s3cr@t@192.168.0.62:1984/api/stream.mjpeg?src=a\n"
                           "  - entity: camera.b\n"
                           "    stream: http://viewer@192.168.0.62/x\n"
                           "  - entity: camera.c\n"
                           "    stream: http://192.168.0.62:1984/a@b\n");
    const auto imported = hc::config::import_yaml(files.view(), "hass.yaml");
    REQUIRE(imported.has_value());
    // the device keeps the login, only the export drops the password
    CHECK(imported->layout.tiles[0].stream == "http://viewer:s3cr@t@192.168.0.62:1984/api/stream.mjpeg?src=a");
    const std::string yaml(hc::config::write_yaml(imported->layout));
    CHECK(yaml.find("s3cr") == std::string::npos);
    CHECK(yaml.find("http://viewer@192.168.0.62:1984/api/stream.mjpeg?src=a") != std::string::npos);
    CHECK(yaml.find("http://viewer@192.168.0.62/x") != std::string::npos);
    CHECK(yaml.find("http://192.168.0.62:1984/a@b") != std::string::npos);
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

TEST(config_keeps_a_clock)
{
    FileSet files;
    files.add("hass.yaml", "tiles:\n"
                           "  - type: clock\n"
                           "  - type: clock\n"
                           "    name: London\n"
                           "    zone: Europe/London\n"
                           "    hours: 12\n"
                           "    seconds: true\n"
                           "    date: true\n"
                           "    style: analog\n"
                           "    border: true\n"
                           "    ticks: minute\n"
                           "    icon: clock-outline\n"
                           "    landscape:\n"
                           "      size: 2x2\n"
                           "  - type: clock\n"
                           "    hours: 24\n"
                           "    ticks: hour\n"
                           "    style: digital\n");
    const auto imported = hc::config::import_yaml(files.view(), "hass.yaml");
    if (!imported) {
        hc::test::fail(__FILE__, __LINE__, std::string(imported.error()));
        return;
    }
    const Layout &layout = imported->layout;
    REQUIRE(layout.tiles.size() == 3);
    const Tile &plain = layout.tiles[0];
    CHECK(plain.kind == TileKind::Clock && plain.entity == hc::core::kNone && !plain.hours12 && !plain.seconds &&
          !plain.date && plain.style == ClockStyle::Digital && !plain.border && plain.ticks == ClockTicks::Hour &&
          plain.zone.empty());
    const Tile &london = layout.tiles[1];
    CHECK(london.zone == "Europe/London" && london.hours12 && london.seconds && london.date &&
          london.style == ClockStyle::Analog && london.border && london.ticks == ClockTicks::Minute &&
          london.icon == "clock-outline" && london.in(L).size == (Span{2, 2}));
    // the defaults written out are not written back
    const auto json = hc::config::write_json(layout);
    CHECK(json.find("\"hours\":12") != std::string::npos && json.find("\"hours\":24") == std::string::npos);
    CHECK(json.find("\"style\":\"digital\"") == std::string::npos && json.find("\"ticks\":\"hour\"") == std::string::npos);
    CHECK(json.find("\"type\":\"clock\",\"name\":\"London\"") != std::string::npos);
    const auto yaml = hc::config::write_yaml(layout);
    CHECK(yaml.find("- type: clock\n") != std::string::npos && yaml.find("zone: Europe/London") != std::string::npos);
    check_round_trips(layout);
}

TEST(config_json_is_strict)
{
    CHECK_STR(std::string(hc::config::read_json("{").error()), "the stored dashboard is not valid JSON");
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"x\",\"version\":1}").error()), "the stored file is not a dashboard");
    // version 1 (the keys size, landscape and portrait of 2026-10-08) is not read: tools/convert_dashboard.py
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":1}").error()),
              "the dashboard has version 1, this firmware reads version 2 (tools/convert_dashboard.py converts it)");
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2}").error()),
              "the stored dashboard has no tiles");
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2,"
                                                "\"tiles\":[{\"id\":1,\"entity\":\"switch.a\",\"nmae\":\"x\"}]}")
                              .error()),
              "tile 1: bad key or value 'nmae'");
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2,"
                                                "\"tiles\":[{\"id\":4,\"entity\":\"sensor.a\",\"dimmer\":true}]}")
                              .error()),
              "tile 4: dimmer is only for lights");
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2,"
                                                "\"tiles\":[{\"id\":6,\"type\":\"clock\",\"hours\":13}]}")
                              .error()),
              "tile 6: bad key or value 'hours'");
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2,"
                                                "\"tiles\":[{\"id\":6,\"type\":\"clock\",\"ticks\":\"all\"}]}")
                              .error()),
              "tile 6: bad key or value 'ticks'");

    const auto read = hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2,\"revision\":7,"
                                            "\"next_id\":20,\"tiles\":[{\"id\":5,\"entity\":\"switch.a\"},"
                                            "{\"id\":0,\"entity\":\"switch.b\"}]}");
    if (read) {
        hc::test::fail(__FILE__, __LINE__, "an id of 0 was accepted");
    }
    CHECK_STR(std::string(hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2,"
                                                "\"tiles\":[{\"entity\":\"switch.a\",\"nmae\":\"x\"}]}")
                              .error()),
              "a new tile: bad key or value 'nmae'");
    const auto ok = hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2,\"revision\":7,"
                                          "\"next_id\":20,\"tiles\":[{\"id\":5,\"entity\":\"switch.a\"}]}");
    REQUIRE(ok.has_value());
    CHECK(ok->revision == 7 && ok->next_id == 20 && ok->tiles[0].id == 5);
}

TEST(config_reads_new_tiles_without_an_id)
{
    // the web UI sends the tiles it added without an id
    const auto read = hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2,\"next_id\":9,"
                                            "\"tiles\":[{\"id\":5,\"entity\":\"switch.a\"},{\"entity\":\"switch.b\"}]}");
    REQUIRE(read.has_value());
    CHECK(read->tiles[0].id == 5 && read->tiles[1].id == 9 && read->next_id == 10);
}

TEST(config_keeps_the_placement_of_both_orientations)
{
    FileSet files;
    files.add("hass.yaml", "tiles:\n"
                           "  - entity: switch.a\n"
                           "    landscape:\n"
                           "      cell: 3,1\n"
                           "      size: 1x1\n"
                           "    portrait:\n"
                           "      cell: 0,3\n"
                           "      hidden: true\n"
                           "  - entity: switch.b\n"
                           "    landscape:\n"
                           "      cell: 0,0\n"
                           "  - entity: switch.c\n"
                           "  - type: area\n"
                           "    name: Rooms\n"
                           "    swipe: true\n"
                           "    tiles:\n"
                           "      - entity: switch.d\n");
    const auto imported = hc::config::import_yaml(files.view(), "hass.yaml");
    REQUIRE(imported.has_value());
    const Layout &layout = imported->layout;
    CHECK(layout.tiles[3].swipe && !has_back_tile(layout, 1) && layout.tiles[4].in(L).cell == (Cell{0, 0}));
    CHECK(layout.tiles[0].in(L).has_cell && layout.tiles[0].in(L).cell == (Cell{3, 1}) && layout.tiles[0].in(L).has_size);
    CHECK(layout.tiles[0].in(P).has_cell && layout.tiles[0].in(P).cell == (Cell{0, 3}) && layout.tiles[0].in(P).hidden);
    CHECK(layout.tiles[1].in(L).has_cell && !layout.tiles[1].in(P).has_cell);
    CHECK(!layout.tiles[2].in(L).has_cell && layout.tiles[2].in(L).cell == (Cell{1, 0})); // first free after b
    const auto json = hc::config::write_json(layout);
    CHECK(json.find("\"name\":\"Rooms\",\"swipe\":true") != std::string::npos);
    CHECK(json.find("\"landscape\":{\"size\":[1,1],\"cell\":[3,1]},\"portrait\":{\"cell\":[0,3],\"hidden\":true}") != std::string::npos);
    const auto yaml = hc::config::write_yaml(layout);
    CHECK(yaml.find("    name: Rooms\n    swipe: true\n") != std::string::npos);
    CHECK(yaml.find("    landscape:\n      size: 1x1\n      cell: 3,1\n    portrait:\n      cell: 0,3\n      hidden: true\n") != std::string::npos);
    check_round_trips(layout);

    // the answer of the web API: where the device draws a tile, null where it does not
    const auto with_cells = hc::config::write_json(layout, true);
    CHECK(with_cells.find("\"cells\":{\"landscape\":[3,1],\"portrait\":null}") != std::string::npos);
    CHECK(with_cells.find("\"cells\":{\"landscape\":[0,0],\"portrait\":[0,0]}") != std::string::npos);
    CHECK(hc::config::read_json(with_cells).has_value()); // the reader skips cells
}

TEST(config_names_the_path_of_a_problem)
{
    // the web UI finds the tile by its place in the lists, also a new one without an id
    const auto problem = [](const char *tiles) {
        const std::string text = std::string("{\"format\":\"hassctrl-dashboard\",\"version\":2,\"tiles\":") + tiles + "}";
        const auto read = hc::config::read_json_at(text);
        if (read) {
            return std::string("ok");
        }
        std::string out = std::string(read.error().message) + " @";
        for (const auto index : read.error().path) {
            out += " " + std::to_string(index);
        }
        return out;
    };
    CHECK_STR(problem("[{\"id\":1,\"entity\":\"switch.a\"},{\"id\":2,\"type\":\"area\",\"name\":\"A\",\"tiles\":["
                      "{\"id\":3,\"entity\":\"switch.b\"},{\"entity\":\"sensor.c\",\"dimmer\":true}]}]"),
              "tile 4: dimmer is only for lights @ 1 1");
    CHECK_STR(problem("[{\"id\":1,\"entity\":\"switch.a\"},{\"entity\":\"switch.b\",\"nmae\":\"x\"}]"),
              "a new tile: bad key or value 'nmae' @ 1");
    CHECK_STR(problem("[{\"id\":1,\"type\":\"area\",\"name\":\"A\",\"tiles\":[]}]"), "area 1: the area has no tiles @ 0");
    CHECK_STR(problem("[]"), "the dashboard has no tiles @");
}

TEST(config_writes_the_cells_for_the_web_ui)
{
    const auto read = hc::config::read_json("{\"format\":\"hassctrl-dashboard\",\"version\":2,\"defaults\":{\"grid\":[2,1]},"
                                            "\"tiles\":[{\"id\":1,\"entity\":\"camera.a\",\"landscape\":{\"size\":[2,1]}}]}");
    REQUIRE(read.has_value());
    const auto json = hc::config::write_json(*read, true);
    // landscape (0,0); the portrait grid is 1x2, the turned picture is 1x2 as well: (0,0)
    CHECK(json.find("\"cells\":{\"landscape\":[0,0],\"portrait\":[0,0]}") != std::string::npos);
    // an answer of the API can be sent back as it is: the reader skips "cells"
    const auto again = hc::config::read_json(json);
    REQUIRE(again.has_value());
    CHECK(hc::config::write_json(*again) == hc::config::write_json(*read));
}
