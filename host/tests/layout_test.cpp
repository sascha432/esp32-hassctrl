// components/core/src/layout.cpp: the rules of the dashboard and the placement of the tiles.

#include "check.hpp"
#include "hc/core/layout.hpp"

using namespace hc::core;

namespace {

Tile entity_tile(Layout &layout, const char *entity)
{
    Tile tile;
    tile.entity = layout.intern(entity);
    tile.kind = kind_of_entity(entity);
    return tile;
}

Tile area_tile(const char *name)
{
    Tile tile;
    tile.kind = TileKind::Area;
    tile.name = name;
    return tile;
}

std::string problem_of(Layout &layout)
{
    const auto finished = finish(layout);
    return finished ? std::string("ok") : std::string(finished.error().message);
}

bool at(const Tile &tile, Cell landscape, Cell portrait)
{
    return tile.landscape == landscape && tile.portrait == portrait;
}

} // namespace

TEST(layout_kind_follows_the_domain)
{
    CHECK(kind_of_entity("light.a") == TileKind::Light);
    CHECK(kind_of_entity("switch.a") == TileKind::Switch);
    CHECK(kind_of_entity("input_boolean.a") == TileKind::Switch);
    CHECK(kind_of_entity("fan.a") == TileKind::Switch);
    CHECK(kind_of_entity("automation.a") == TileKind::Switch);
    CHECK(kind_of_entity("button.a") == TileKind::Button);
    CHECK(kind_of_entity("input_button.a") == TileKind::Button);
    CHECK(kind_of_entity("scene.a") == TileKind::Button);
    CHECK(kind_of_entity("script.a") == TileKind::Button);
    CHECK(kind_of_entity("climate.a") == TileKind::Climate);
    CHECK(kind_of_entity("camera.a") == TileKind::Picture);
    CHECK(kind_of_entity("sensor.a") == TileKind::Sensor);
    CHECK(kind_of_entity("binary_sensor.a") == TileKind::Sensor);
    CHECK(kind_of_entity("lock.front_door") == TileKind::Lock);
    CHECK(kind_of_entity("weather.openweathermap") == TileKind::Weather);
    CHECK(std::string_view(kind_name(TileKind::Weather)) == "weather");
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
    Tile climate = entity_tile(layout, "climate.b"); // 1x2
    layout.add_tile(0, climate);
    layout.add_tile(0, entity_tile(layout, "sensor.c"));
    layout.add_tile(0, entity_tile(layout, "sensor.d"));
    REQUIRE(problem_of(layout) == "ok");
    // landscape 4x2: a (0,0), b (1,0) two rows tall, c (2,0), d (3,0)
    // portrait 2x4: a (0,0); b turns into 2x1 and takes the row below, (0,1); c (1,0); d (0,2)
    CHECK(at(layout.tiles[0], {0, 0}, {0, 0}));
    CHECK(at(layout.tiles[1], {1, 0}, {0, 1}));
    CHECK(at(layout.tiles[2], {2, 0}, {1, 0}));
    CHECK(at(layout.tiles[3], {3, 0}, {0, 2}));
    CHECK(layout.tiles[1].span == (Span{1, 2}));
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

TEST(layout_rejects_a_tile_that_does_not_fit)
{
    Layout layout; // 4x2 with a 1x2 tile in every column, then one more
    for (const char *entity : {"climate.a", "climate.b", "climate.c", "climate.d", "climate.e"}) {
        layout.add_tile(0, entity_tile(layout, entity));
    }
    CHECK_STR(problem_of(layout), "no free 1x2 block left in the landscape 4x2 grid of its page");

    Layout small;
    small.grid = {1, 1};
    small.add_tile(0, entity_tile(small, "camera.a"));
    CHECK_STR(problem_of(small), "a 2x2 tile does not fit into the landscape 1x1 grid of its page");
}

TEST(layout_portrait_falls_back_to_the_first_cell)
{
    Layout layout;
    layout.grid = {2, 1}; // portrait 1x2: a 2x1 picture has no block there
    Tile picture = entity_tile(layout, "camera.a");
    picture.has_size = true;
    picture.span = {2, 1};
    layout.add_tile(0, picture);
    REQUIRE(problem_of(layout) == "ok");
    CHECK(at(layout.tiles[0], {0, 0}, {0, 0}));
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
    CHECK_STR(problem([](Layout &, Tile &t) { t.decimals = 7; }), "decimals is 0 to 6");
    CHECK_STR(problem([](Layout &, Tile &t) { t.unit = "a very long unit"; }), "the unit is longer than 12 bytes");
    CHECK_STR(problem([](Layout &, Tile &t) { t.icon = "nope"; }), "unknown icon 'nope'");
    CHECK_STR(problem([](Layout &, Tile &t) { t.name = std::string(33, 'x').c_str(); }), "the name is longer than 32 bytes");
    CHECK_STR(problem([](Layout &, Tile &t) { t.has_size = true; t.span = {1, 2}; }),
              "size is only for dimmer lights, climate, weather and picture tiles");
    CHECK_STR(problem([](Layout &l, Tile &t) { t.entity = l.intern("sensor.A"); }),
              "'sensor.A' is not an entity id (domain.name: lowercase letters, digits and _)");
    CHECK_STR(problem([](Layout &, Tile &t) { t.kind = TileKind::Light; }), "sensor.a does not make a light tile");
    CHECK_STR(problem([](Layout &, Tile &t) { t.icon = "none"; t.decimals = 2; t.unit = "hPa"; }), "ok");
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
    CHECK_STR(problem("light.a", [](Tile &t) { t.has_size = true; }), "a light has a size only with dimmer: true");
    CHECK_STR(problem("light.a", [](Tile &t) { t.dimmer = true; t.has_size = true; t.span = {2, 1}; }),
              "a light tile is 1x1 or 1x2");
    CHECK_STR(problem("light.a", [](Tile &t) { t.step = 60; }), "the step of a light is 1 to 50 percent");
    CHECK_STR(problem("climate.a", [](Tile &t) { t.step = 0.05f; }), "the step of a climate tile is 0.1 to 5 degrees");
    CHECK_STR(problem("climate.a", [](Tile &t) { t.step = 0.1f; }), "ok");
    CHECK_STR(problem("camera.a", [](Tile &t) { t.has_size = true; t.span = {3, 1}; }),
              "a picture tile is 1x1, 1x2, 2x1 or 2x2");
    CHECK_STR(problem("camera.a", [](Tile &t) { t.refresh_s = 4000; }), "refresh is 1 to 3600 seconds");
    CHECK_STR(problem("light.a", [](Tile &t) { t.dimmer = true; }), "ok");
    CHECK_STR(problem("weather.a", [](Tile &t) { t.has_size = true; t.span = {1, 2}; }), "ok");
    CHECK_STR(problem("weather.a", [](Tile &t) { t.has_size = true; t.span = {2, 1}; }), "ok");
    CHECK_STR(problem("weather.a", [](Tile &t) { t.has_size = true; t.span = {2, 2}; }), "a weather tile is 1x1, 1x2 or 2x1");
    CHECK_STR(problem("weather.a", [](Tile &t) { t.unit = "C"; }), "unit is only for sensors");
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

// four 1x1 switches and more on a 4x2 page: a..d in the first row
Layout switches(int count)
{
    Layout layout;
    const char *names[] = {"switch.a", "switch.b", "switch.c", "switch.d", "switch.e", "switch.f", "switch.g", "switch.h"};
    for (int i = 0; i < count; i++) {
        layout.add_tile(0, entity_tile(layout, names[i]));
    }
    return layout;
}

} // namespace

TEST(layout_places_tiles_with_a_cell_first)
{
    Layout layout = switches(3);
    layout.tiles[1].has_landscape = true;
    layout.tiles[1].landscape = {0, 0};
    REQUIRE(problem_of(layout) == "ok");
    // landscape: b takes its cell, a and c fill the free cells in list order; portrait is automatic
    CHECK(at(layout.tiles[0], {1, 0}, {0, 0}));
    CHECK(at(layout.tiles[1], {0, 0}, {1, 0}));
    CHECK(at(layout.tiles[2], {2, 0}, {0, 1}));
}

TEST(layout_checks_the_cells)
{
    const auto problem = [](Cell cell, bool portrait = false, bool area = false) {
        Layout layout;
        PageIndex page = 0;
        if (area) {
            page = layout.add_page(0, layout.add_tile(0, area_tile("A")));
        }
        Tile tile = entity_tile(layout, "climate.a"); // 1x2, 2x1 in portrait
        (portrait ? tile.has_portrait : tile.has_landscape) = true;
        (portrait ? tile.portrait : tile.landscape) = cell;
        layout.add_tile(page, tile);
        Tile other = entity_tile(layout, "switch.b");
        other.has_landscape = other.has_portrait = true;
        other.landscape = {3, 1};
        other.portrait = {1, 3};
        layout.add_tile(page, other);
        return problem_of(layout);
    };
    CHECK_STR(problem({0, 0}), "ok");
    CHECK_STR(problem({0, 1}), "the landscape cell 0,1 of a 1x2 tile lies outside the 4x2 grid of its page");
    CHECK_STR(problem({1, 3}, true), "the portrait cell 1,3 of a 2x1 tile lies outside the 2x4 grid of its page");
    CHECK_STR(problem({3, 0}), "the landscape cell 3,1 overlaps another tile");
    CHECK_STR(problem({0, 0}, false, true), "the landscape cell 0,0 of an area page is its back tile");
}

TEST(layout_moves_a_tile_and_pushes_others_aside)
{
    Layout layout = switches(5); // a b c d / e
    REQUIRE(problem_of(layout) == "ok");
    REQUIRE(move_tile(layout, 4, Orientation::Landscape, {0, 0}));
    // e takes a's cell; a takes the first free cell from there on: the one e left
    CHECK(layout.tiles[4].landscape == (Cell{0, 0}));
    CHECK(layout.tiles[0].landscape == (Cell{0, 1}));
    CHECK(layout.tiles[1].landscape == (Cell{1, 0}));
    // the page is arranged in landscape now, portrait stays automatic
    for (const Tile &tile : layout.tiles) {
        CHECK(tile.has_landscape && !tile.has_portrait);
    }
    // a free cell: nothing is pushed
    REQUIRE(move_tile(layout, 1, Orientation::Landscape, {3, 1}));
    CHECK(layout.tiles[1].landscape == (Cell{3, 1}) && layout.tiles[2].landscape == (Cell{2, 0}));
}

TEST(layout_pushes_in_reading_order_and_wraps)
{
    Layout layout = switches(6); // a b c d / e f, free: (2,1) (3,1)
    REQUIRE(problem_of(layout) == "ok");
    // a onto d: d takes the first free cell after (3,0) in reading order, (2,1), not the one a left
    REQUIRE(move_tile(layout, 0, Orientation::Landscape, {3, 0}));
    CHECK(layout.tiles[0].landscape == (Cell{3, 0}));
    CHECK(layout.tiles[3].landscape == (Cell{2, 1}));

    Layout full = switches(8); // a b c d / e f g h
    REQUIRE(problem_of(full) == "ok");
    // d onto h: nothing is free after (3,1), so h wraps to the start and finds the cell d left
    REQUIRE(move_tile(full, 3, Orientation::Landscape, {3, 1}));
    CHECK(full.tiles[3].landscape == (Cell{3, 1}) && full.tiles[7].landscape == (Cell{3, 0}));
}

TEST(layout_refuses_a_move_that_does_not_fit)
{
    Layout full = switches(8);
    REQUIRE(problem_of(full) == "ok");
    const Layout before = full;
    CHECK(!move_tile(full, 0, Orientation::Landscape, {4, 0}));  // outside
    CHECK(full.tiles[0].landscape == before.tiles[0].landscape && !full.tiles[0].has_landscape);

    Layout pictures;
    pictures.grid = {3, 2};
    Tile picture = entity_tile(pictures, "camera.a"); // 2x2
    pictures.add_tile(0, picture);
    pictures.add_tile(0, entity_tile(pictures, "switch.b"));
    pictures.add_tile(0, entity_tile(pictures, "switch.c"));
    REQUIRE(problem_of(pictures) == "ok"); // picture (0,0), b (2,0), c (2,1)
    // the picture onto (1,0) covers b and c; from (1,0) on b finds (0,1), c wraps to (0,0)
    REQUIRE(move_tile(pictures, 0, Orientation::Landscape, {1, 0}));
    CHECK(pictures.tiles[1].landscape == (Cell{0, 1}) && pictures.tiles[2].landscape == (Cell{0, 0}));

    // 2x2 with a 1x2 climate in the left column: a switch onto it leaves the climate one free cell
    Layout tight;
    tight.grid = {2, 2};
    tight.add_tile(0, entity_tile(tight, "climate.a"));
    tight.add_tile(0, entity_tile(tight, "switch.b"));
    tight.add_tile(0, entity_tile(tight, "switch.c"));
    REQUIRE(problem_of(tight) == "ok");
    CHECK(!move_tile(tight, 1, Orientation::Landscape, {0, 0}));
    CHECK(tight.tiles[1].landscape == (Cell{1, 0}) && !tight.tiles[1].has_landscape);

    Layout area;
    const PageIndex page = area.add_page(0, area.add_tile(0, area_tile("A")));
    area.add_tile(page, entity_tile(area, "switch.a"));
    REQUIRE(problem_of(area) == "ok");
    CHECK(!move_tile(area, 1, Orientation::Landscape, {0, 0})); // the back tile's cell
}

TEST(layout_moves_in_portrait_only_there)
{
    Layout layout = switches(3); // portrait 2x4: a (0,0) b (1,0) c (0,1)
    REQUIRE(problem_of(layout) == "ok");
    REQUIRE(move_tile(layout, 2, Orientation::Portrait, {1, 3}));
    CHECK(layout.tiles[2].portrait == (Cell{1, 3}) && layout.tiles[2].has_portrait);
    CHECK(!layout.tiles[2].has_landscape && layout.tiles[2].landscape == (Cell{2, 0}));
}

TEST(layout_resets_an_arrangement)
{
    Layout layout = switches(3);
    REQUIRE(problem_of(layout) == "ok");
    REQUIRE(move_tile(layout, 2, Orientation::Landscape, {0, 1}));
    REQUIRE(reset_arrangement(layout, 0, Orientation::Landscape).has_value());
    CHECK(!layout.tiles[2].has_landscape && layout.tiles[2].landscape == (Cell{2, 0}));

    // 2x2: a, b, then a 1x2 climate fits only when arranged
    Layout tight;
    tight.grid = {2, 2};
    tight.add_tile(0, entity_tile(tight, "switch.a"));
    tight.add_tile(0, entity_tile(tight, "switch.b"));
    tight.add_tile(0, entity_tile(tight, "climate.c"));
    const Cell cells[] = {{0, 0}, {0, 1}, {1, 0}};
    for (TileIndex i = 0; i < 3; i++) {
        tight.tiles[i].has_landscape = true;
        tight.tiles[i].landscape = cells[i];
    }
    REQUIRE(problem_of(tight) == "ok");
    const auto reset = reset_arrangement(tight, 0, Orientation::Landscape);
    REQUIRE(!reset.has_value());
    CHECK_STR(std::string(reset.error().message), "no free 1x2 block left in the landscape 2x2 grid of its page");
    CHECK(tight.tiles[2].has_landscape); // unchanged
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
    CHECK(without_rooms->tiles[1].landscape == (Cell{1, 0}));

    const auto without_inside = remove_tile(layout, 2);
    REQUIRE(without_inside.has_value());
    CHECK(without_inside->pages.size() == 3 && without_inside->tiles.size() == 5);
    CHECK(without_inside->tiles[2].opens == 2 && without_inside->pages[2].parent == 1);

    const auto empty = remove_tile(layout, 4); // desk: Office has no tile left
    REQUIRE(!empty.has_value());
    CHECK_STR(std::string(empty.error().message), "the area has no tiles");
}
