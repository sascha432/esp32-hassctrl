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
    CHECK(kind_of_entity("lock.front_door") == TileKind::Sensor);
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
    CHECK_STR(problem_of(layout), "no free 1x2 block left in the 4x2 grid of its page");

    Layout small;
    small.grid = {1, 1};
    small.add_tile(0, entity_tile(small, "camera.a"));
    CHECK_STR(problem_of(small), "a 2x2 tile does not fit into the 1x1 grid of its page");
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
              "size is only for dimmer lights, climate and picture tiles");
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
