// components/core/src/layout.cpp: the rules of the dashboard and the placement of the tiles.

#include "check.hpp"
#include "hc/core/layout.hpp"

using namespace hc::core;

namespace {

constexpr Orientation L = Orientation::Landscape;
constexpr Orientation P = Orientation::Portrait;

Tile entity_tile(Layout &layout, const char *entity)
{
    Tile tile;
    tile.entity = layout.intern(entity);
    tile.kind = kind_of_entity(entity);
    return tile;
}

// an entity tile with a landscape size of its own
Tile sized(Layout &layout, const char *entity, Span landscape)
{
    Tile tile = entity_tile(layout, entity);
    tile.in(L).has_size = true;
    tile.in(L).size = landscape;
    return tile;
}

Tile area_tile(const char *name)
{
    Tile tile;
    tile.kind = TileKind::Area;
    tile.name = name;
    return tile;
}

void pin(Tile &tile, Orientation o, Cell cell)
{
    tile.in(o).has_cell = true;
    tile.in(o).cell = cell;
}

std::string problem_of(Layout &layout)
{
    const auto finished = finish(layout);
    return finished ? std::string("ok") : std::string(finished.error().message);
}

// shown in both orientations, at these cells
bool at(const Tile &tile, Cell landscape, Cell portrait)
{
    return tile.in(L).shown && tile.in(P).shown && tile.in(L).cell == landscape && tile.in(P).cell == portrait;
}

} // namespace

TEST(layout_kind_follows_the_domain)
{
    CHECK(kind_of_entity("light.a") == TileKind::Light);
    CHECK(kind_of_entity("switch.a") == TileKind::Switch);
    CHECK(kind_of_entity("input_boolean.a") == TileKind::Switch);
    CHECK(kind_of_entity("fan.a") == TileKind::Switch);
    CHECK(kind_of_entity("automation.a") == TileKind::Automation);
    CHECK(kind_of_entity("button.a") == TileKind::Button);
    CHECK(kind_of_entity("input_button.a") == TileKind::Button);
    CHECK(kind_of_entity("scene.a") == TileKind::Button);
    CHECK(kind_of_entity("script.a") == TileKind::Automation);
    CHECK(std::string_view(kind_name(TileKind::Automation)) == "automation");
    CHECK(kind_of_entity("climate.a") == TileKind::Climate);
    CHECK(kind_of_entity("camera.a") == TileKind::Picture);
    CHECK(kind_of_entity("sensor.a") == TileKind::Sensor);
    CHECK(kind_of_entity("binary_sensor.a") == TileKind::Sensor);
    CHECK(kind_of_entity("lock.front_door") == TileKind::Lock);
    CHECK(kind_of_entity("weather.openweathermap") == TileKind::Weather);
    CHECK(std::string_view(kind_name(TileKind::Weather)) == "weather");
    CHECK(kind_of_entity("timer.bathroom_fan") == TileKind::Timer);
    CHECK(std::string_view(kind_name(TileKind::Timer)) == "timer");
}

TEST(layout_checks_entity_ids)
{
    CHECK(valid_entity_id("sensor.bme280_living_room"));
    CHECK(!valid_entity_id("sensor"));
    CHECK(!valid_entity_id(".a"));
    CHECK(!valid_entity_id("sensor."));
    CHECK(!valid_entity_id("Sensor.a"));
    CHECK(!valid_entity_id("sensor.a-b"));
    CHECK(!valid_entity_id("sensor.a.b"));
    CHECK(!valid_entity_id(std::string(70, 'a').insert(3, ".")));
}

TEST(layout_places_first_fit_in_reading_order)
{
    Layout layout; // 4x2
    layout.add_tile(0, entity_tile(layout, "switch.a"));
    layout.add_tile(0, sized(layout, "climate.b", {1, 2}));
    layout.add_tile(0, entity_tile(layout, "sensor.c"));
    layout.add_tile(0, entity_tile(layout, "sensor.d"));
    REQUIRE(problem_of(layout) == "ok");
    // landscape 4x2: a (0,0), b (1,0) two rows tall, c (2,0), d (3,0)
    // portrait 2x4: a (0,0); b turns into 2x1 and takes the row below, (0,1); c (1,0); d (0,2)
    CHECK(at(layout.tiles[0], {0, 0}, {0, 0}));
    CHECK(at(layout.tiles[1], {1, 0}, {0, 1}));
    CHECK(at(layout.tiles[2], {2, 0}, {1, 0}));
    CHECK(at(layout.tiles[3], {3, 0}, {0, 2}));
    CHECK(layout.tiles[1].in(L).size == (Span{1, 2}) && layout.tiles[1].in(P).size == (Span{2, 1}));
}

TEST(layout_sizes_default_to_one_cell_and_turn_in_portrait)
{
    Layout layout;
    Tile light = entity_tile(layout, "light.a");
    light.dimmer = true;
    layout.add_tile(0, light);
    layout.add_tile(0, entity_tile(layout, "climate.b"));
    layout.add_tile(0, entity_tile(layout, "camera.c"));
    Tile wide = sized(layout, "camera.d", {2, 1});
    wide.in(P).has_size = true;
    wide.in(P).size = {2, 1}; // stays wide on the turned screen
    layout.add_tile(0, wide);
    REQUIRE(problem_of(layout) == "ok");
    for (TileIndex i = 0; i < 3; i++) {
        CHECK(layout.tiles[i].in(L).size == (Span{1, 1}) && layout.tiles[i].in(P).size == (Span{1, 1}));
        CHECK(!layout.tiles[i].in(L).has_size && !layout.tiles[i].in(P).has_size);
    }
    CHECK(layout.tiles[3].in(L).size == (Span{2, 1}) && layout.tiles[3].in(P).size == (Span{2, 1}));

    const auto problem = [](auto change) {
        Layout l;
        Tile tile = entity_tile(l, "switch.a");
        change(tile);
        l.add_tile(0, tile);
        return problem_of(l);
    };
    CHECK_STR(problem([](Tile &t) { t.in(L).has_size = true; t.in(L).size = {9, 1}; }), "the landscape size is 1x1 to 8x8");
    CHECK_STR(problem([](Tile &t) { t.in(P).has_size = true; t.in(P).size = {1, 0}; }), "the portrait size is 1x1 to 8x8");
    CHECK_STR(problem([](Tile &t) { pin(t, L, {32, 0}); }), "the landscape cell is 0,0 to 31,31");
    CHECK_STR(problem([](Tile &t) { pin(t, L, {31, 0}); }), "ok"); // outside a 4x2 grid: left out
    CHECK_STR(problem([](Tile &t) { t.in(L).has_size = true; t.in(L).size = {8, 8}; }), "ok"); // larger than the grid: left out
}

TEST(layout_reserves_the_back_cell_of_an_area_page)
{
    Layout layout;
    const TileIndex area = layout.add_tile(0, area_tile("Rooms"));
    const PageIndex page = layout.add_page(0, area);
    layout.pages[page].has_grid = true;
    layout.pages[page].grid = {4, 3};
    layout.add_tile(page, entity_tile(layout, "light.a"));
    REQUIRE(problem_of(layout) == "ok");
    CHECK(layout.tiles[area].opens == page);
    CHECK(layout.pages[page].depth == 1);
    CHECK(has_back_tile(layout, page) && !has_back_tile(layout, 0));
    CHECK(at(layout.tiles[1], {1, 0}, {1, 0}));
}

TEST(layout_gives_ids_and_keeps_given_ones)
{
    Layout layout;
    Tile a = entity_tile(layout, "switch.a");
    a.id = 7;
    layout.add_tile(0, a);
    layout.add_tile(0, entity_tile(layout, "switch.b"));
    REQUIRE(problem_of(layout) == "ok");
    CHECK(layout.tiles[0].id == 7);
    CHECK(layout.tiles[1].id == 8);
    CHECK(layout.next_id == 9);

    Layout twice;
    Tile b = entity_tile(twice, "switch.a");
    b.id = 3;
    twice.add_tile(0, b);
    twice.add_tile(0, b);
    CHECK_STR(problem_of(twice), "the id 3 is used twice");
}

TEST(layout_leaves_out_tiles_without_room)
{
    Layout layout; // 4x2 with a 1x2 tile in every column, then one more
    for (const char *entity : {"climate.a", "climate.b", "climate.c", "climate.d", "climate.e"}) {
        layout.add_tile(0, sized(layout, entity, {1, 2}));
    }
    REQUIRE(problem_of(layout) == "ok");
    for (TileIndex i = 0; i < 4; i++) {
        CHECK(layout.tiles[i].in(L).shown && layout.tiles[i].in(P).shown);
    }
    // the fifth has no room in either orientation (portrait 2x4: four 2x1 rows)
    CHECK(!layout.tiles[4].in(L).shown && !layout.tiles[4].in(P).shown);
    CHECK(!layout.tiles[4].in(L).has_cell); // it stays automatic: room later brings it back

    Layout huge; // 32x32 with a cell at the far corner
    huge.grid = {32, 32};
    Tile corner = entity_tile(huge, "switch.a");
    pin(corner, L, {31, 31});
    huge.add_tile(0, corner);
    REQUIRE(problem_of(huge) == "ok");
    CHECK(huge.tiles[0].in(L).shown && huge.tiles[0].in(P).cell == (Cell{0, 0}));
    huge.grid = {33, 1};
    CHECK_STR(problem_of(huge), "the grid is 1x1 to 32x32");

    Layout small;
    small.grid = {1, 1};
    small.add_tile(0, sized(small, "camera.a", {2, 2}));
    REQUIRE(problem_of(small) == "ok");
    CHECK(!small.tiles[0].in(L).shown && !small.tiles[0].in(P).shown);

    // a portrait size of its own that has no room there: the tile is on the landscape screen only
    Layout turned;
    Tile tall = entity_tile(turned, "sensor.a");
    tall.in(P).has_size = true;
    tall.in(P).size = {3, 3};
    turned.add_tile(0, tall);
    REQUIRE(problem_of(turned) == "ok");
    CHECK(turned.tiles[0].in(L).shown && !turned.tiles[0].in(P).shown);
}

TEST(layout_checks_the_keys_per_kind)
{
    const auto problem = [](auto change) {
        Layout layout;
        Tile tile = entity_tile(layout, "sensor.a");
        change(layout, tile);
        layout.add_tile(0, tile);
        return problem_of(layout);
    };
    CHECK_STR(problem([](Layout &, Tile &t) { t.dimmer = true; }), "dimmer is only for lights");
    CHECK_STR(problem([](Layout &, Tile &t) { t.step = 5; }), "step is only for lights and climate tiles");
    CHECK_STR(problem([](Layout &, Tile &t) { t.refresh_s = 5; }), "refresh is only for picture tiles");
    CHECK_STR(problem([](Layout &, Tile &t) { t.seconds = true; }),
              "hours, seconds, date, style, border, ticks and zone are only for clock tiles");
    CHECK_STR(problem([](Layout &, Tile &t) { t.zone = "Europe/Berlin"; }),
              "hours, seconds, date, style, border, ticks and zone are only for clock tiles");
    CHECK_STR(problem([](Layout &, Tile &t) { t.decimals = 7; }), "decimals is 0 to 6");
    CHECK_STR(problem([](Layout &, Tile &t) { t.unit = "a very long unit"; }), "the unit is longer than 12 bytes");
    CHECK_STR(problem([](Layout &, Tile &t) { t.icon = "nope"; }), "unknown icon 'nope'");
    CHECK_STR(problem([](Layout &, Tile &t) { t.name = std::string(33, 'x').c_str(); }), "the name is longer than 32 bytes");
    CHECK_STR(problem([](Layout &l, Tile &t) { t.entity = l.intern("sensor.A"); }),
              "'sensor.A' is not an entity id (domain.name: lowercase letters, digits and _)");
    CHECK_STR(problem([](Layout &, Tile &t) { t.kind = TileKind::Light; }), "sensor.a does not make a light tile");
    CHECK_STR(problem([](Layout &, Tile &t) { t.icon = "none"; t.decimals = 2; t.unit = "hPa"; }), "ok");
    // any kind takes any size
    CHECK_STR(problem([](Layout &, Tile &t) { t.in(L).has_size = true; t.in(L).size = {2, 2}; }), "ok");
}

TEST(layout_checks_lights_climate_and_pictures)
{
    const auto problem = [](const char *entity, auto change) {
        Layout layout;
        Tile tile = entity_tile(layout, entity);
        change(tile);
        layout.add_tile(0, tile);
        return problem_of(layout);
    };
    CHECK_STR(problem("light.a", [](Tile &t) { t.step = 60; }), "the step of a light is 1 to 50 percent");
    CHECK_STR(problem("climate.a", [](Tile &t) { t.step = 0.05f; }), "the step of a climate tile is 0.1 to 5 degrees");
    CHECK_STR(problem("climate.a", [](Tile &t) { t.step = 0.1f; }), "ok");
    CHECK_STR(problem("camera.a", [](Tile &t) { t.refresh_s = 4000; }), "refresh is 1 to 3600 seconds");
    CHECK_STR(problem("light.a", [](Tile &t) { t.dimmer = true; }), "ok");
    CHECK_STR(problem("weather.a", [](Tile &t) { t.unit = "C"; }), "unit is only for sensors");
}

TEST(layout_checks_clocks)
{
    const auto problem = [](auto change) {
        Layout layout;
        Tile tile;
        tile.kind = TileKind::Clock;
        change(layout, tile);
        layout.add_tile(0, tile);
        return problem_of(layout);
    };
    CHECK_STR(kind_name(TileKind::Clock), "clock");
    CHECK(!has_entity(TileKind::Clock) && !has_entity(TileKind::Spacer) && has_entity(TileKind::Weather));
    CHECK_STR(problem([](Layout &, Tile &) {}), "ok");
    CHECK_STR(problem([](Layout &l, Tile &t) { t.entity = l.intern("sensor.a"); }), "a clock has no entity");
    CHECK_STR(problem([](Layout &, Tile &t) { t.border = true; }), "border and ticks are only for analog clocks");
    CHECK_STR(problem([](Layout &, Tile &t) { t.ticks = ClockTicks::Minute; }), "border and ticks are only for analog clocks");
    CHECK_STR(problem([](Layout &, Tile &t) {
                  t.style = ClockStyle::Analog;
                  t.border = true;
                  t.ticks = ClockTicks::None;
                  t.seconds = true;
              }),
              "ok");
    CHECK_STR(problem([](Layout &, Tile &t) {
                  t.zone = "Europe/London";
                  t.hours12 = true;
                  t.date = true;
                  t.icon = "clock-outline";
                  t.name = "London";
              }),
              "ok");
    CHECK_STR(problem([](Layout &, Tile &t) { t.zone = "Nowhere/Town"; }),
              "unknown time zone 'Nowhere/Town' (an IANA name like Europe/Berlin)");
    CHECK_STR(problem([](Layout &, Tile &t) { t.zone = "europe/london"; }),
              "unknown time zone 'europe/london' (an IANA name like Europe/Berlin)");
    CHECK_STR(problem([](Layout &, Tile &t) { t.unit = "x"; }), "unit is only for sensors");
    // the clock keys read back as they are written
    ClockStyle style = ClockStyle::Digital;
    ClockTicks ticks = ClockTicks::Hour;
    CHECK(clock_style_of("analog", style) && style == ClockStyle::Analog);
    CHECK(!clock_style_of("round", style) && style == ClockStyle::Analog);
    CHECK(clock_ticks_of("quarter", ticks) && ticks == ClockTicks::Quarter);
    CHECK(!clock_ticks_of("all", ticks) && ticks == ClockTicks::Quarter);
    CHECK_STR(clock_ticks_name(ClockTicks::Minute), "minute");
}

TEST(layout_checks_media_tiles)
{
    const auto problem = [](auto change) {
        Layout layout;
        Tile tile;
        tile.kind = TileKind::Media;
        change(layout, tile);
        layout.add_tile(0, tile);
        return problem_of(layout);
    };
    CHECK_STR(kind_name(TileKind::Media), "media");
    CHECK(!has_entity(TileKind::Media));
    CHECK_STR(problem([](Layout &, Tile &) {}), "ok");
    CHECK_STR(problem([](Layout &, Tile &t) { t.folder = "music/albums"; }), "ok");
    CHECK_STR(problem([](Layout &, Tile &t) { t.folder = "/"; }), "ok"); // the card's root
    CHECK_STR(problem([](Layout &, Tile &t) { t.folder = "media-source://"; }), "ok"); // the library's top
    CHECK_STR(problem([](Layout &, Tile &t) { t.folder = "media-source://media_source/local/music/Ein Album"; }), "ok");
    CHECK_STR(problem([](Layout &l, Tile &t) { t.entity = l.intern("media_player.a"); }), "a media has no entity");
    for (const char *bad : {"/music", "music/", "a//b", "../x", "a/../b", "a\\b", "a/./b", "http://x/music"}) {
        CHECK_STR(problem([&](Layout &, Tile &t) { t.folder = bad; }),
                  "folder is media-source://... or a folder of the SD card like music/albums (no .., no \\, no / at "
                  "the ends)");
    }
    CHECK_STR(problem([](Layout &, Tile &t) { t.folder = "media-source://" + std::string(250, 'a'); }),
              "folder: a media-source:// id of at most 255 bytes");
    CHECK_STR(problem([](Layout &, Tile &t) { t.zone = "Europe/London"; }),
              "hours, seconds, date, style, border, ticks and zone are only for clock tiles");
    // the media key on another kind
    Layout layout;
    Tile clock;
    clock.kind = TileKind::Clock;
    clock.folder = "music";
    layout.add_tile(0, clock);
    CHECK_STR(problem_of(layout), "folder is only for media tiles");
    CHECK(hass_media_folder("media-source://tts") && !hass_media_folder("music"));
    CHECK(valid_media_folder("") && valid_media_folder("Musik/Über") && !valid_media_folder(std::string(97, 'a')));
}

TEST(layout_checks_pages)
{
    Layout empty;
    CHECK_STR(problem_of(empty), "the dashboard has no tiles");

    Layout layout;
    const TileIndex area = layout.add_tile(0, area_tile("Rooms"));
    layout.add_page(0, area);
    CHECK_STR(problem_of(layout), "the area has no tiles");

    Layout deep;
    PageIndex page = 0;
    for (int level = 0; level < 6; level++) {
        const TileIndex tile = deep.add_tile(page, area_tile("A"));
        page = deep.add_page(page, tile);
    }
    deep.add_tile(page, entity_tile(deep, "switch.a"));
    CHECK_STR(problem_of(deep), "areas are nested more than 5 levels deep");

    Layout unnamed;
    const TileIndex nameless = unnamed.add_tile(0, area_tile(""));
    const PageIndex inner = unnamed.add_page(0, nameless);
    unnamed.add_tile(inner, entity_tile(unnamed, "switch.a"));
    CHECK_STR(problem_of(unnamed), "an area needs a name");
}

namespace {

// 1x1 switches on a 4x2 page: a..d in the first row, e..h in the second
Layout switches(int count)
{
    Layout layout;
    const char *names[] = {"switch.a", "switch.b", "switch.c", "switch.d", "switch.e", "switch.f", "switch.g", "switch.h", "switch.i"};
    for (int i = 0; i < count; i++) {
        layout.add_tile(0, entity_tile(layout, names[i]));
    }
    return layout;
}

} // namespace

TEST(layout_places_tiles_with_a_cell_first)
{
    Layout layout = switches(3);
    pin(layout.tiles[1], L, {0, 0});
    REQUIRE(problem_of(layout) == "ok");
    // landscape: b takes its cell, a and c fill the free cells in list order; portrait is automatic
    CHECK(at(layout.tiles[0], {1, 0}, {0, 0}));
    CHECK(at(layout.tiles[1], {0, 0}, {1, 0}));
    CHECK(at(layout.tiles[2], {2, 0}, {0, 1}));
}

TEST(layout_leaves_out_a_tile_whose_cell_cannot_be_taken)
{
    // a 1x2 climate with a cell of its own, and a switch pinned at (3,1) / (1,3)
    const auto place = [](Cell cell, Orientation o = L, bool area = false) {
        Layout layout;
        PageIndex page = 0;
        if (area) {
            page = layout.add_page(0, layout.add_tile(0, area_tile("A")));
        }
        Tile tile = sized(layout, "climate.a", {1, 2});
        pin(tile, o, cell);
        layout.add_tile(page, tile);
        Tile other = entity_tile(layout, "switch.b");
        pin(other, L, {3, 1});
        pin(other, P, {1, 3});
        layout.add_tile(page, other);
        CHECK(problem_of(layout) == "ok");
        return layout;
    };
    Layout fits = place({0, 0});
    CHECK(fits.tiles[0].in(L).shown && fits.tiles[0].in(L).cell == (Cell{0, 0}) && fits.tiles[1].in(L).shown);

    Layout outside = place({0, 1}); // the second row of a 1x2 lies outside the 4x2 grid
    CHECK(!outside.tiles[0].in(L).shown && outside.tiles[0].in(L).has_cell && outside.tiles[0].in(P).shown);

    Layout outside_portrait = place({1, 3}, P); // 2x1 at column 1 of the 2x4 grid
    CHECK(outside_portrait.tiles[0].in(L).shown && !outside_portrait.tiles[0].in(P).shown);

    Layout overlap = place({3, 0}); // covers (3,1): the switch, later in the list, loses its cell
    CHECK(overlap.tiles[0].in(L).shown && !overlap.tiles[1].in(L).shown && overlap.tiles[1].in(P).shown);

    Layout back = place({0, 0}, L, true); // the back tile's cell of an area page
    CHECK(!back.tiles[1].in(L).shown && back.tiles[1].in(P).shown);
}

TEST(layout_hides_a_tile_per_orientation)
{
    Layout layout = switches(3);
    layout.tiles[1].in(P).hidden = true;
    REQUIRE(problem_of(layout) == "ok");
    CHECK(at(layout.tiles[0], {0, 0}, {0, 0}));
    CHECK(layout.tiles[1].in(L).shown && layout.tiles[1].in(L).cell == (Cell{1, 0}));
    CHECK(!layout.tiles[1].in(P).shown && layout.tiles[1].in(P).hidden && !layout.tiles[1].in(L).hidden);
    CHECK(at(layout.tiles[2], {2, 0}, {1, 0})); // c takes b's place in portrait

    // a hidden tile is neither pushed nor pinned by a move in that orientation
    REQUIRE(move_tile(layout, 2, P, {0, 1}));
    CHECK(layout.tiles[2].in(P).cell == (Cell{0, 1}) && layout.tiles[2].in(P).has_cell);
    CHECK(!layout.tiles[1].in(P).has_cell && !layout.tiles[1].in(P).shown);
    CHECK(!move_tile(layout, 1, P, {1, 0})); // a tile not on the screen cannot be moved
}

TEST(layout_swipe_areas_have_no_back_tile_and_count_their_depth)
{
    Layout layout;
    layout.add_tile(0, entity_tile(layout, "switch.a"));
    const TileIndex rooms = layout.add_tile(0, area_tile("Rooms"));
    const PageIndex rooms_page = layout.add_page(0, rooms);
    layout.add_tile(rooms_page, entity_tile(layout, "light.x"));
    const TileIndex office = layout.add_tile(rooms_page, area_tile("Office"));
    const PageIndex office_page = layout.add_page(rooms_page, office);
    layout.add_tile(office_page, entity_tile(layout, "switch.desk"));
    REQUIRE(problem_of(layout) == "ok");
    // without swipe: the back tiles take the first cells, no dots anywhere
    CHECK(has_back_tile(layout, rooms_page) && has_back_tile(layout, office_page) && !has_back_tile(layout, 0));
    CHECK(swipe_chain(layout, office_page).size() == 1 && swipe_chain(layout, 0).size() == 1);
    CHECK(layout.tiles[2].in(L).cell == (Cell{1, 0})); // light.x after the back tile

    // Rooms with swipe: its page has no back tile, light.x takes the first cell; the Office inside
    // keeps its back tile; the dots of Rooms stand for the main page and Rooms
    layout.tiles[rooms].swipe = true;
    REQUIRE(problem_of(layout) == "ok");
    CHECK(swipe_page(layout, rooms_page) && !has_back_tile(layout, rooms_page) && has_back_tile(layout, office_page));
    CHECK(layout.tiles[rooms].in(L).shown && layout.tiles[rooms].in(L).cell == (Cell{1, 0})); // the tile stays
    CHECK(layout.tiles[2].in(L).cell == (Cell{0, 0}));
    CHECK(layout.tiles[4].in(L).cell == (Cell{1, 0})); // desk after the back tile
    auto chain = swipe_chain(layout, rooms_page);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0] == 0 && chain[1] == rooms_page);
    CHECK(swipe_chain(layout, office_page).size() == 1); // no dots on the Office page

    // the Office with swipe as well: three dots there, and its first cell is free for a move
    layout.tiles[office].swipe = true;
    REQUIRE(problem_of(layout) == "ok");
    chain = swipe_chain(layout, office_page);
    REQUIRE(chain.size() == 3);
    CHECK(chain[0] == 0 && chain[1] == rooms_page && chain[2] == office_page);
    CHECK(layout.tiles[4].in(L).cell == (Cell{0, 0}));
    CHECK(move_tile(layout, 4, L, {1, 0}) && move_tile(layout, 4, L, {0, 0}));

    // only the Office with swipe: two dots, the main page is not in its chain
    layout.tiles[rooms].swipe = false;
    REQUIRE(problem_of(layout) == "ok");
    chain = swipe_chain(layout, office_page);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0] == rooms_page && chain[1] == office_page);
    CHECK(has_back_tile(layout, rooms_page));
    CHECK(!move_tile(layout, 2, L, {0, 0})); // the back cell of Rooms

    Layout wrong;
    Tile tile = entity_tile(wrong, "switch.a");
    tile.swipe = true;
    wrong.add_tile(0, tile);
    CHECK_STR(problem_of(wrong), "swipe is only for areas");
}

TEST(layout_moves_a_tile_and_pushes_others_aside)
{
    Layout layout = switches(5); // a b c d / e
    REQUIRE(problem_of(layout) == "ok");
    REQUIRE(move_tile(layout, 4, L, {0, 0}));
    // e takes a's cell; a takes the first free cell from there on: the one e left
    CHECK(layout.tiles[4].in(L).cell == (Cell{0, 0}));
    CHECK(layout.tiles[0].in(L).cell == (Cell{0, 1}));
    CHECK(layout.tiles[1].in(L).cell == (Cell{1, 0}));
    // the page is arranged in landscape now, portrait stays automatic
    for (const Tile &tile : layout.tiles) {
        CHECK(tile.in(L).has_cell && !tile.in(P).has_cell);
    }
    // a free cell: nothing is pushed
    REQUIRE(move_tile(layout, 1, L, {3, 1}));
    CHECK(layout.tiles[1].in(L).cell == (Cell{3, 1}) && layout.tiles[2].in(L).cell == (Cell{2, 0}));
}

TEST(layout_pushes_in_reading_order_and_wraps)
{
    Layout layout = switches(6); // a b c d / e f, free: (2,1) (3,1)
    REQUIRE(problem_of(layout) == "ok");
    // a onto d: d takes the first free cell after (3,0) in reading order, (2,1), not the one a left
    REQUIRE(move_tile(layout, 0, L, {3, 0}));
    CHECK(layout.tiles[0].in(L).cell == (Cell{3, 0}));
    CHECK(layout.tiles[3].in(L).cell == (Cell{2, 1}));

    Layout full = switches(8); // a b c d / e f g h
    REQUIRE(problem_of(full) == "ok");
    // d onto h: nothing is free after (3,1), so h wraps to the start and finds the cell d left
    REQUIRE(move_tile(full, 3, L, {3, 1}));
    CHECK(full.tiles[3].in(L).cell == (Cell{3, 1}) && full.tiles[7].in(L).cell == (Cell{3, 0}));
}

TEST(layout_refuses_a_move_that_does_not_fit)
{
    Layout full = switches(8);
    REQUIRE(problem_of(full) == "ok");
    const Layout before = full;
    CHECK(!move_tile(full, 0, L, {4, 0})); // outside
    CHECK(full.tiles[0].in(L).cell == before.tiles[0].in(L).cell && !full.tiles[0].in(L).has_cell);

    Layout pictures;
    pictures.grid = {3, 2};
    pictures.add_tile(0, sized(pictures, "camera.a", {2, 2}));
    pictures.add_tile(0, entity_tile(pictures, "switch.b"));
    pictures.add_tile(0, entity_tile(pictures, "switch.c"));
    REQUIRE(problem_of(pictures) == "ok"); // picture (0,0), b (2,0), c (2,1)
    // the picture onto (1,0) covers b and c; from (1,0) on b finds (0,1), c wraps to (0,0)
    REQUIRE(move_tile(pictures, 0, L, {1, 0}));
    CHECK(pictures.tiles[1].in(L).cell == (Cell{0, 1}) && pictures.tiles[2].in(L).cell == (Cell{0, 0}));

    // 2x2 with a 1x2 climate in the left column: a switch onto it leaves the climate one free cell
    Layout tight;
    tight.grid = {2, 2};
    tight.add_tile(0, sized(tight, "climate.a", {1, 2}));
    tight.add_tile(0, entity_tile(tight, "switch.b"));
    tight.add_tile(0, entity_tile(tight, "switch.c"));
    REQUIRE(problem_of(tight) == "ok");
    CHECK(!move_tile(tight, 1, L, {0, 0})); // nothing vanishes into the overflow by a move
    CHECK(tight.tiles[1].in(L).cell == (Cell{1, 0}) && !tight.tiles[1].in(L).has_cell);

    Layout area;
    const PageIndex page = area.add_page(0, area.add_tile(0, area_tile("A")));
    area.add_tile(page, entity_tile(area, "switch.a"));
    REQUIRE(problem_of(area) == "ok");
    CHECK(!move_tile(area, 1, L, {0, 0})); // the back tile's cell

    // a tile without room is not pushed, not pinned and stays out; room made by a move brings it back
    Layout nine = switches(9);
    REQUIRE(problem_of(nine) == "ok");
    CHECK(!nine.tiles[8].in(L).shown);
    REQUIRE(move_tile(nine, 0, L, {1, 0}));
    CHECK(!nine.tiles[8].in(L).has_cell && !nine.tiles[8].in(L).shown);
    nine.tiles[7].in(L).hidden = true;
    REQUIRE(problem_of(nine) == "ok");
    CHECK(nine.tiles[8].in(L).shown && nine.tiles[8].in(L).cell == (Cell{3, 1}));
}

TEST(layout_moves_in_portrait_only_there)
{
    Layout layout = switches(3); // portrait 2x4: a (0,0) b (1,0) c (0,1)
    REQUIRE(problem_of(layout) == "ok");
    REQUIRE(move_tile(layout, 2, P, {1, 3}));
    CHECK(layout.tiles[2].in(P).cell == (Cell{1, 3}) && layout.tiles[2].in(P).has_cell);
    CHECK(!layout.tiles[2].in(L).has_cell && layout.tiles[2].in(L).cell == (Cell{2, 0}));
}

TEST(layout_resets_an_arrangement)
{
    Layout layout = switches(3);
    REQUIRE(problem_of(layout) == "ok");
    REQUIRE(move_tile(layout, 2, L, {0, 1}));
    reset_arrangement(layout, 0, L);
    CHECK(!layout.tiles[2].in(L).has_cell && layout.tiles[2].in(L).cell == (Cell{2, 0}));

    // 2x2: a, b, then a 1x2 climate fits only when arranged; the reset leaves it out
    Layout tight;
    tight.grid = {2, 2};
    tight.add_tile(0, entity_tile(tight, "switch.a"));
    tight.add_tile(0, entity_tile(tight, "switch.b"));
    tight.add_tile(0, sized(tight, "climate.c", {1, 2}));
    const Cell cells[] = {{0, 0}, {0, 1}, {1, 0}};
    for (TileIndex i = 0; i < 3; i++) {
        pin(tight.tiles[i], L, cells[i]);
    }
    REQUIRE(problem_of(tight) == "ok");
    CHECK(tight.tiles[2].in(L).shown);
    reset_arrangement(tight, 0, L);
    CHECK(!tight.tiles[2].in(L).has_cell && !tight.tiles[2].in(L).shown);
    CHECK(tight.tiles[1].in(L).cell == (Cell{1, 0}));
}

TEST(layout_removes_a_tile_and_an_area_with_its_pages)
{
    Layout layout;
    layout.add_tile(0, entity_tile(layout, "switch.a"));
    const TileIndex rooms = layout.add_tile(0, area_tile("Rooms"));
    const PageIndex page = layout.add_page(0, rooms);
    layout.add_tile(page, entity_tile(layout, "sensor.inside"));
    const TileIndex office = layout.add_tile(page, area_tile("Office"));
    layout.add_tile(layout.add_page(page, office), entity_tile(layout, "switch.desk"));
    layout.add_tile(0, entity_tile(layout, "switch.b"));
    REQUIRE(problem_of(layout) == "ok"); // ids: a 1, Rooms 2, inside 3, Office 4, desk 5, b 6

    const auto without_rooms = remove_tile(layout, rooms);
    REQUIRE(without_rooms.has_value());
    CHECK(without_rooms->pages.size() == 1 && without_rooms->tiles.size() == 2);
    CHECK(without_rooms->tiles[1].id == 6 && without_rooms->entity_id(without_rooms->tiles[1]) == "switch.b");
    CHECK(without_rooms->entities.size() == 4); // kept: the indices of the entity store stay valid
    CHECK(without_rooms->next_id == layout.next_id);
    CHECK(without_rooms->tiles[1].in(L).cell == (Cell{1, 0}));

    const auto without_inside = remove_tile(layout, 2);
    REQUIRE(without_inside.has_value());
    CHECK(without_inside->pages.size() == 3 && without_inside->tiles.size() == 5);
    CHECK(without_inside->tiles[2].opens == 2 && without_inside->pages[2].parent == 1);

    const auto empty = remove_tile(layout, 4); // desk: Office has no tile left
    REQUIRE(!empty.has_value());
    CHECK_STR(std::string(empty.error().message), "the area has no tiles");
}

NewTile entity_new(const char *entity, bool dimmer = false)
{
    NewTile added;
    added.entity = entity;
    added.tile.dimmer = dimmer;
    return added;
}

TEST(layout_adds_an_entity_tile_at_a_cell)
{
    Layout layout = switches(5); // a b c d / e, free: (1,1) (2,1) (3,1)
    REQUIRE(problem_of(layout) == "ok");
    const auto added = add_tile(layout, 0, entity_new("light.x", true), L, {3, 1});
    REQUIRE(added.has_value());
    REQUIRE(added->tiles.size() == 6);
    const Tile &x = added->tiles[5];
    CHECK(x.kind == TileKind::Light && x.dimmer && x.id == 6);
    CHECK(added->entities.size() == 6 && added->entity_id(x) == "light.x");
    CHECK(x.in(L).shown && x.in(L).cell == (Cell{3, 1}));
    // the others keep their cells, the page is arranged in landscape, portrait stays automatic
    CHECK(added->tiles[4].in(L).cell == (Cell{0, 1}) && added->tiles[4].in(L).has_cell);
    CHECK(x.in(P).shown && !x.in(P).has_cell);

    // an entity of the dashboard: no new entry, the tile shares it; dimmer only for lights
    const auto again = add_tile(*added, 0, entity_new("switch.a", true), L, {1, 1});
    REQUIRE(again.has_value());
    CHECK(again->entities.size() == 6 && again->tiles[6].entity == 0 && !again->tiles[6].dimmer);

    // a full page has no room, and an id that is no entity id is refused
    Layout full = switches(8);
    REQUIRE(problem_of(full) == "ok");
    const auto no_room = add_tile(full, 0, entity_new("switch.i"), L, {3, 1});
    REQUIRE(!no_room.has_value());
    CHECK_STR(std::string(no_room.error().message), "no room for the tile there");
    const auto wrong = add_tile(layout, 0, entity_new("Light.X"), L, {3, 1});
    REQUIRE(!wrong.has_value());
    CHECK_STR(std::string(wrong.error().message), "'Light.X' is not an entity id");
}

TEST(layout_adds_a_clock_and_an_area_with_its_first_tile)
{
    Layout layout = switches(5);
    REQUIRE(problem_of(layout) == "ok");

    // a 2x1 clock with its options and a name
    NewTile clock;
    clock.tile.kind = TileKind::Clock;
    clock.tile.name = "Time";
    clock.tile.seconds = true;
    clock.tile.in(L).has_size = true;
    clock.tile.in(L).size = {2, 1};
    const auto with_clock = add_tile(layout, 0, clock, L, {1, 1});
    REQUIRE(with_clock.has_value());
    const Tile &c = with_clock->tiles[5];
    CHECK(c.kind == TileKind::Clock && c.seconds && c.name == "Time" && c.entity == kNone);
    CHECK(c.in(L).cell == (Cell{1, 1}) && c.in(L).size == (Span{2, 1}));

    // an area opens a page with its first tile
    NewTile area;
    area.tile.kind = TileKind::Area;
    area.tile.name = "Kitchen";
    area.tile.swipe = true;
    area.first_entity = "light.kitchen";
    area.first.dimmer = true;
    const auto with_area = add_tile(layout, 0, area, L, {3, 1});
    REQUIRE(with_area.has_value());
    REQUIRE(with_area->pages.size() == 2 && with_area->tiles.size() == 7);
    const Tile &a = with_area->tiles[5];
    CHECK(a.kind == TileKind::Area && a.swipe && a.opens == 1 && a.in(L).cell == (Cell{3, 1}));
    const Tile &first = with_area->tiles[with_area->pages[1].tiles[0]];
    CHECK(first.kind == TileKind::Light && first.dimmer && with_area->entity_id(first) == "light.kitchen");

    // an area needs a name
    area.tile.name.clear();
    const auto unnamed = add_tile(layout, 0, area, L, {3, 1});
    REQUIRE(!unnamed.has_value());
    CHECK_STR(std::string(unnamed.error().message), "an area needs a name");
}

TEST(layout_changes_a_tile)
{
    Layout layout = switches(5); // a b c d / e, free: (1,1) (2,1) (3,1)
    layout.add_tile(0, entity_tile(layout, "sensor.t"));
    REQUIRE(problem_of(layout) == "ok"); // t at (1,1)

    // a name and the decimals of a sensor; nothing else changes
    NewTile changed;
    changed.tile.name = "Inside";
    changed.tile.decimals = 1;
    changed.tile.in(L).size = {1, 1};
    const auto named = change_tile(layout, 5, changed, L);
    REQUIRE(named.has_value());
    CHECK(named->tiles[5].name == "Inside" && named->tiles[5].decimals == 1);
    CHECK(named->tiles[5].in(L).cell == (Cell{1, 1}) && !named->tiles[5].in(L).has_cell);

    // another entity of another domain: the kind follows, the sensor's keys go, a light's dimmer stays
    changed.entity = "light.x";
    changed.tile.dimmer = true;
    const auto light = change_tile(*named, 5, changed, L);
    REQUIRE(light.has_value());
    const Tile &x = light->tiles[5];
    CHECK(x.kind == TileKind::Light && x.dimmer && x.decimals == -1 && light->entity_id(x) == "light.x");

    // 2x1 keeps the cell and pushes nothing (2,1 is free); 4x1 has no room at (1,1)
    changed.tile.in(L).size = {2, 1};
    const auto wide = change_tile(*light, 5, changed, L);
    REQUIRE(wide.has_value());
    CHECK(wide->tiles[5].in(L).cell == (Cell{1, 1}) && wide->tiles[5].in(L).size == (Span{2, 1}));
    CHECK(wide->tiles[4].in(L).cell == (Cell{0, 1}));
    changed.tile.in(L).size = {4, 1};
    const auto too_wide = change_tile(*light, 5, changed, L);
    REQUIRE(!too_wide.has_value());
    CHECK_STR(std::string(too_wide.error().message), "no room for 4x1 there");

    // a clock turned digital loses its border
    Layout clocks;
    Tile clock;
    clock.kind = TileKind::Clock;
    clock.style = ClockStyle::Analog;
    clock.border = true;
    clocks.add_tile(0, clock);
    REQUIRE(problem_of(clocks) == "ok");
    NewTile digital;
    digital.tile.seconds = true;
    digital.tile.in(L).size = {1, 1};
    const auto changed_clock = change_tile(clocks, 0, digital, L);
    REQUIRE(changed_clock.has_value());
    CHECK(changed_clock->tiles[0].style == ClockStyle::Digital && !changed_clock->tiles[0].border &&
          changed_clock->tiles[0].seconds);
}
