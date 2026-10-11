// components/hass_protocol: the template of a page, the decoding of its results, the messages.
// components/core/src/entity.cpp: the store and the display texts.

#include <memory>
#include <string>

#include "cJSON.h"
#include "check.hpp"
#include "hc/core/entity.hpp"
#include "hc/core/pending.hpp"
#include "hc/hass_protocol/protocol.hpp"

using namespace hc;
using namespace hc::core;
using namespace hc::hass_protocol;

namespace {

struct JsonDeleter {
    void operator()(cJSON *json) const
    {
        cJSON_Delete(json);
    }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;

Tile entity_tile(Layout &layout, const char *entity)
{
    Tile tile;
    tile.entity = layout.intern(entity);
    tile.kind = kind_of_entity(entity);
    tile.name = "Named";
    tile.icon = "power";
    return tile;
}

// a page with a dimmer, a climate, a sensor and the dimmer a second time
struct Sample {
    Layout layout;
    TileIndex dimmer, climate, sensor, again;

    Sample()
    {
        Tile light = entity_tile(layout, "light.aurora");
        light.dimmer = true;
        dimmer = layout.add_tile(0, light);
        climate = layout.add_tile(0, entity_tile(layout, "climate.hearth"));
        Tile temperature = entity_tile(layout, "sensor.moon_temperature");
        temperature.name.clear();
        temperature.icon.clear();
        sensor = layout.add_tile(0, temperature);
        Tile spacer;
        spacer.kind = TileKind::Spacer;
        layout.add_tile(0, spacer);
        again = layout.add_tile(0, entity_tile(layout, "light.aurora"));
    }
};

} // namespace

TEST(protocol_fields_follow_the_tile)
{
    Layout layout;
    Tile plain = entity_tile(layout, "light.lamp");
    CHECK(fields_of(plain, false) == bit(Field::State));
    plain.dimmer = true;
    CHECK(fields_of(plain, false) == (bit(Field::State) | bit(Field::Brightness) | bit(Field::ColorModes)));
    CHECK((fields_of(plain, true) & bit(Field::EffectList)) != 0);

    Tile sensor = entity_tile(layout, "sensor.power");
    CHECK(fields_of(sensor, false) == (bit(Field::State) | bit(Field::DeviceClass) | bit(Field::Unit) | bit(Field::Rounded)));
    sensor.unit = "W";
    sensor.decimals = 0;
    CHECK(fields_of(sensor, false) == (bit(Field::State) | bit(Field::DeviceClass)));
    sensor.name.clear();
    sensor.icon.clear();
    CHECK((fields_of(sensor, false) & (bit(Field::Name) | bit(Field::Icon))) == (bit(Field::Name) | bit(Field::Icon)));

    Tile climate = entity_tile(layout, "climate.hall");
    CHECK((fields_of(climate, false) & bit(Field::Target)) != 0);
    CHECK((fields_of(climate, false) & bit(Field::PresetModes)) == 0);
    CHECK((fields_of(climate, true) & bit(Field::PresetModes)) != 0);

    Tile spacer;
    spacer.kind = TileKind::Spacer;
    CHECK(fields_of(spacer, true) == 0);
}

TEST(protocol_page_template)
{
    Sample page;
    const PageSubscription sub = build_page(page.layout, 0, kNone);
    // the second tile of the light shares its entry; the spacer has none
    REQUIRE(sub.entities.size() == 3);
    CHECK(page.layout.entities[sub.entities[0]] == "light.aurora");
    CHECK(page.layout.entities[sub.entities[2]] == "sensor.moon_temperature");
    CHECK_STR(sub.template_text.c_str(),
              "{\"0\":{%set e='light.aurora'%}{\"s\":{{states(e)|tojson}},\"b\":{{state_attr(e,'brightness')|tojson}},"
              "\"cs\":{{state_attr(e,'supported_color_modes')|tojson}}},"
              "\"1\":{%set e='climate.hearth'%}{\"s\":{{states(e)|tojson}},\"t\":{{state_attr(e,'temperature')|tojson}},"
              "\"c\":{{state_attr(e,'current_temperature')|tojson}},\"a\":{{state_attr(e,'hvac_action')|tojson}},"
              "\"tl\":{{state_attr(e,'min_temp')|tojson}},\"th\":{{state_attr(e,'max_temp')|tojson}},"
              "\"ts\":{{state_attr(e,'target_temp_step')|tojson}}},"
              "\"2\":{%set e='sensor.moon_temperature'%}{\"s\":{{states(e)|tojson}},\"r\":{{states(e,rounded=True)|tojson}},"
              "\"n\":{{state_attr(e,'friendly_name')|tojson}},\"i\":{{state_attr(e,'icon')|tojson}},"
              "\"d\":{{state_attr(e,'device_class')|tojson}},\"u\":{{state_attr(e,'unit_of_measurement')|tojson}}}}");

    // Jinja would read "{{%" as an expression
    CHECK(sub.template_text.find("{{%") == std::string::npos);

    // the open panel adds its fields to the entity of its tile
    const PageSubscription panel = build_page(page.layout, 0, page.again);
    CHECK((panel.fields[0] & bit(Field::EffectList)) != 0);
    CHECK((panel.fields[1] & bit(Field::PresetModes)) == 0);
    CHECK(panel.template_text.find("'effect_list'") != std::string::npos);

    CHECK(build_page(page.layout, 7, kNone).empty());
}

TEST(protocol_decode_object_and_text)
{
    Sample page;
    const PageSubscription sub = build_page(page.layout, 0, page.dimmer);
    const char *text = R"({"0":{"s":"on","b":180,"cs":["hs","color_temp"],"cm":"hs","e":null,"el":["Rainbow","Fire, warm"],)"
                       R"("k":null,"kl":2000,"kh":6535,"h":[30.5,80]},)"
                       R"("1":{"s":"heat","t":21.5,"c":"20.8","a":"heating","tl":7,"th":35,"ts":null},)"
                       R"("2":{"s":"12.345","r":"12.3","n":"Moon","i":null,"d":"temperature","u":"°C"},"9":{"s":"x"}})";
    Json object(cJSON_Parse(text));
    Json as_text(cJSON_CreateString(text));
    const cJSON *results[] = {object.get(), as_text.get()};
    for (const cJSON *result : results) {
        hc::memory::Vector<EntityUpdate> updates;
        hc::memory::String error;
        REQUIRE(decode_page(result, sub, updates, error));
        REQUIRE(updates.size() == 3);
        const EntityState &light = updates[0].values;
        CHECK(updates[0].entity == sub.entities[0]);
        CHECK(light.state == "on" && light.brightness == 180);
        CHECK(light.color_modes.size() == 2 && light.color_mode == "hs");
        CHECK(light.effect.empty() && light.effect_list.size() == 2 && light.effect_list[1] == "Fire, warm");
        CHECK(std::isnan(light.color_temp) && light.min_color_temp == 2000 && light.max_color_temp == 6535);
        CHECK(light.hue == 30.5f && light.saturation == 80);
        const EntityState &climate = updates[1].values;
        CHECK(climate.target == 21.5f && climate.current == 20.8f && climate.hvac_action == "heating");
        CHECK(std::isnan(climate.temp_step) && climate.min_temp == 7);
        CHECK((updates[1].fields & bit(Field::TempStep)) != 0);
        const EntityState &sensor = updates[2].values;
        CHECK(sensor.rounded == "12.3" && sensor.unit == "°C" && sensor.name == "Moon" && sensor.icon.empty());
    }

    hc::memory::Vector<EntityUpdate> updates;
    hc::memory::String error;
    Json list(cJSON_Parse("[1,2]"));
    CHECK(!decode_page(list.get(), sub, updates, error));
    Json broken(cJSON_CreateString("{\"0\":"));
    CHECK(!decode_page(broken.get(), sub, updates, error));
    CHECK_STR(error.c_str(), "the template result is not an object");
    // a missing entity or field is left out, not reported as empty
    Json partial(cJSON_Parse(R"({"1":{"s":"off"}})"));
    REQUIRE(decode_page(partial.get(), sub, updates, error));
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].fields == bit(Field::State));
}

TEST(protocol_store_reports_changes)
{
    EntityStore store(2);
    EntityUpdate update;
    update.entity = 1;
    update.fields = bit(Field::State) | bit(Field::Target);
    update.values.state = "heat";
    CHECK(store.apply(update));        // first values
    CHECK(!store.apply(update));       // the same again; an unknown target stays unknown
    update.values.target = 21;
    CHECK(store.apply(update));
    CHECK(store.state(1).target == 21 && store.state(1).has(Field::Target) && !store.state(1).has(Field::Current));
    // a field outside the update is not touched
    update.fields = bit(Field::State);
    update.values.target = 5;
    CHECK(!store.apply(update));
    CHECK(store.state(1).target == 21);
    update.entity = 9;
    CHECK(!store.apply(update));
}

TEST(protocol_store_keeps_last_brightness)
{
    EntityStore store(1);
    EntityUpdate update;
    update.entity = 0;
    update.fields = bit(Field::State) | bit(Field::Brightness);
    update.values.state = "off";
    store.apply(update);
    CHECK(store.state(0).last_brightness == -1); // never seen on
    update.values.state = "on";
    update.values.brightness = 84;
    store.apply(update);
    update.values.state = "off";
    update.values.brightness = -1;
    store.apply(update);
    // off: HA reports no brightness, the level it comes back at stays
    CHECK(store.state(0).brightness == -1 && store.state(0).last_brightness == 84);
}

TEST(protocol_display_texts)
{
    EntityState state;
    state.state = "21.456";
    state.rounded = "21.5";
    state.known = bit(Field::State) | bit(Field::Rounded);
    state.unit = "°C";
    state.name = "Moon";
    Tile tile;
    CHECK_STR(display_value(state, tile).c_str(), "21.5");
    tile.decimals = 2;
    CHECK_STR(display_value(state, tile).c_str(), "21.46");
    tile.decimals = 0;
    CHECK_STR(display_value(state, tile).c_str(), "21");
    state.state = "unavailable";
    CHECK_STR(display_value(state, tile).c_str(), "21.5");
    CHECK(display_unit(state, tile) == "°C");
    tile.unit = "K";
    CHECK(display_unit(state, tile) == "K");
    CHECK(display_name(state, tile, "sensor.moon") == "Moon");
    state.name.clear();
    CHECK(display_name(state, tile, "sensor.moon") == "sensor.moon");
    tile.name = "Mine";
    CHECK(display_name(state, tile, "sensor.moon") == "Mine");
}

TEST(protocol_messages)
{
    CHECK_STR(render_template_message(4, "{\"0\":{{states('a.b')|tojson}}}").c_str(),
              R"({"id":4,"type":"render_template","template":"{\"0\":{{states('a.b')|tojson}}}","report_errors":true})");
    CHECK_STR(unsubscribe_message(5, 4).c_str(), R"({"id":5,"type":"unsubscribe_events","subscription":4})");

    hc::memory::String out, error;
    Intent intent;
    intent.kind = IntentKind::Toggle;
    REQUIRE(call_service_message(6, intent, "switch.anvil_fan", out, error));
    CHECK_STR(out.c_str(), R"({"id":6,"type":"call_service","domain":"switch","service":"toggle","service_data":{},)"
                           R"("target":{"entity_id":"switch.anvil_fan"}})");
    CHECK(!call_service_message(6, intent, "sensor.power", out, error));
    CHECK_STR(error.c_str(), "this action does not fit sensor.power");

    intent.kind = IntentKind::Press;
    REQUIRE(call_service_message(7, intent, "script.close_shutters", out, error));
    CHECK(out.find(R"("domain":"script","service":"turn_on")") != std::string::npos);
    REQUIRE(call_service_message(7, intent, "button.restart", out, error));
    CHECK(out.find(R"("domain":"button","service":"press")") != std::string::npos);

    intent.kind = IntentKind::SetLevel;
    intent.value = 42.4f;
    REQUIRE(call_service_message(8, intent, "light.forge_lamp", out, error));
    CHECK(out.find(R"("service":"turn_on","service_data":{"brightness_pct":42})") != std::string::npos);
    intent.value = 0;
    REQUIRE(call_service_message(8, intent, "light.forge_lamp", out, error));
    CHECK(out.find(R"("service":"turn_off","service_data":{})") != std::string::npos);
    CHECK(!call_service_message(8, intent, "climate.hearth", out, error));

    intent.kind = IntentKind::SetTemperature;
    intent.value = 21.549f;
    REQUIRE(call_service_message(9, intent, "climate.hearth", out, error));
    CHECK(out.find(R"("service":"set_temperature","service_data":{"temperature":21.5})") != std::string::npos);

    intent.kind = IntentKind::SetPreset;
    intent.text = "eco \"night\"";
    REQUIRE(call_service_message(10, intent, "climate.hearth", out, error));
    const bool escaped = out.find("\"preset_mode\":\"eco \\\"night\\\"\"") != std::string::npos;
    CHECK(escaped);

    intent.kind = IntentKind::SetColor;
    intent.value = 120;
    intent.value2 = 55.55f;
    REQUIRE(call_service_message(11, intent, "light.aurora", out, error));
    CHECK(out.find(R"("hs_color":[120,55.6])") != std::string::npos);

    intent.kind = IntentKind::SetColorTemp;
    intent.value = 2700.4f;
    REQUIRE(call_service_message(12, intent, "light.aurora", out, error));
    CHECK(out.find(R"("color_temp_kelvin":2700)") != std::string::npos);
}

TEST(protocol_picker_messages)
{
    // the search text goes in as a variable: quotes and Jinja need no escaping
    Json list(cJSON_Parse(entity_list_message(3, "light", "K\"üche {{x}}").c_str()));
    REQUIRE(list != nullptr);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(list.get(), "type")), "render_template");
    CHECK(cJSON_GetObjectItem(list.get(), "id")->valueint == 3);
    const cJSON *variables = cJSON_GetObjectItem(list.get(), "variables");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(variables, "d")), "light");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(variables, "q")), "K\"üche {{x}}");
    CHECK(cJSON_GetObjectItem(variables, "n")->valueint == static_cast<int>(kEntityListMax));
    const std::string list_template = cJSON_GetStringValue(cJSON_GetObjectItem(list.get(), "template"));
    CHECK(list_template.find("{{%") == std::string::npos);
    CHECK(list_template.find("|tojson}}") != std::string::npos);

    Json details(cJSON_Parse(entity_details_message(4, "light.aurora").c_str()));
    REQUIRE(details != nullptr);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(details.get(), "variables"), "e")),
              "light.aurora");

    // the result as an object, or as its text (older Home Assistant)
    hc::memory::String out;
    Json object(cJSON_Parse(R"({"more":false,"rows":[["light.a","A","light",null,null]]})"));
    REQUIRE(template_result_json(object.get(), out));
    CHECK_STR(out.c_str(), R"({"more":false,"rows":[["light.a","A","light",null,null]]})");
    Json text(cJSON_CreateString(R"({"id":"x.y","missing":true})"));
    REQUIRE(template_result_json(text.get(), out));
    CHECK_STR(out.c_str(), R"({"id":"x.y","missing":true})");
    Json plain(cJSON_CreateString("not json"));
    CHECK(!template_result_json(plain.get(), out));
    Json number(cJSON_CreateNumber(4));
    CHECK(!template_result_json(number.get(), out));
}

TEST(protocol_pending_actions)
{
    PendingTracker pending;
    Expectation on;
    on.state = "on";
    pending.start(3, on, 1000);
    REQUIRE(pending.find(3) != nullptr);
    CHECK(pending.find(4) == nullptr);

    EntityState state;
    state.state = "off";
    CHECK(!pending.resolve(3, state)); // an update that is not the result yet
    state.state = "on";
    CHECK(pending.resolve(3, state));
    CHECK(pending.empty());

    // a level within HA's rounding, a setpoint
    Expectation level;
    level.brightness = 77;
    pending.start(1, level, 0);
    state.brightness = 76;
    CHECK(pending.resolve(1, state));
    Expectation target;
    target.target = 21.5f;
    pending.start(2, target, 0);
    CHECK(!pending.resolve(2, state)); // the target is unknown
    state.target = 21.5f;
    CHECK(pending.resolve(2, state));

    // the time runs out; a new action replaces the old one of its entity
    pending.start(5, on, 0);
    pending.start(5, on, 3000);
    int ended = 0;
    pending.expire(PendingTracker::kTimeoutMs, [&](EntityIndex) { ended++; });
    CHECK(ended == 0);
    pending.expire(3000 + PendingTracker::kTimeoutMs, [&](EntityIndex entity) { ended += entity; });
    CHECK(ended == 5 && pending.empty());

    pending.start(6, on, 0);
    CHECK(pending.fail(6) && !pending.fail(6));
}

TEST(protocol_pending_light_panel)
{
    PendingTracker pending;
    EntityState state;
    CHECK(!Expectation{}.says_something());

    // a color as HA reports it back: the hue across 0, the saturation rounded
    Expectation color;
    color.hue = 359;
    color.saturation = 80;
    CHECK(color.says_something());
    pending.start(1, color, 0);
    state.hue = 30;
    state.saturation = 80;
    CHECK(!pending.resolve(1, state));
    state.hue = 0.5f;
    state.saturation = 79.2f;
    CHECK(pending.resolve(1, state));

    // kelvin through mireds: 6500 K is 154 mireds is 6494 K
    Expectation warm;
    warm.color_temp = 6500;
    pending.start(2, warm, 0);
    state.color_temp = 3000;
    CHECK(!pending.resolve(2, state));
    state.color_temp = 6494;
    CHECK(pending.resolve(2, state));

    Expectation effect;
    effect.effect = "Rainbow";
    pending.start(3, effect, 0);
    state.effect = "Solid";
    CHECK(!pending.resolve(3, state));
    state.effect = "Rainbow";
    CHECK(pending.resolve(3, state));
}

TEST(protocol_pending_climate_panel)
{
    PendingTracker pending;
    EntityState state;
    state.state = "heat";
    state.preset_mode = "comfort";
    state.fan_mode = "auto";

    Expectation mode;
    mode.state = "cool"; // the hvac mode is the state
    pending.start(1, mode, 0);
    CHECK(!pending.resolve(1, state));
    state.state = "cool";
    CHECK(pending.resolve(1, state));

    Expectation preset;
    preset.preset = "eco";
    CHECK(preset.says_something());
    pending.start(2, preset, 0);
    CHECK(!pending.resolve(2, state));
    state.preset_mode = "eco";
    CHECK(pending.resolve(2, state));

    Expectation fan;
    fan.fan_mode = "on";
    pending.start(3, fan, 0);
    CHECK(!pending.resolve(3, state));
    state.fan_mode = "on";
    CHECK(pending.resolve(3, state));
}

TEST(protocol_history_messages)
{
    char text[32];
    iso_time(0, text, sizeof(text));
    CHECK(std::string(text) == "1970-01-01T00:00:00+00:00");
    iso_time(1791374400, text, sizeof(text)); // a leap year in the window
    CHECK(std::string(text) == "2026-10-07T12:00:00+00:00");
    iso_time(951825600, text, sizeof(text));
    CHECK(std::string(text) == "2000-02-29T12:00:00+00:00");

    const std::string stats(history_message(7, HistoryKind::Statistics, "sensor.t", 1791374400, 1791396000));
    CHECK(stats == R"({"id":7,"type":"recorder/statistics_during_period","start_time":"2026-10-07T12:00:00+00:00",)"
                   R"("end_time":"2026-10-07T18:00:00+00:00","statistic_ids":["sensor.t"],"period":"5minute",)"
                   R"("types":["mean"]})");
    const std::string states(history_message(8, HistoryKind::States, "binary_sensor.m", 0, 60));
    CHECK(states.find(R"("type":"history/history_during_period")") != std::string::npos);
    CHECK(states.find(R"("entity_ids":["binary_sensor.m"],"minimal_response":true)") != std::string::npos);
}

TEST(protocol_history_decode)
{
    memory::Vector<HistoryPoint> points;
    memory::String error;
    cJSON *stats = cJSON_Parse(R"({"sensor.t":[{"start":1791374400000.0,"end":1791374700000.0,"mean":21.25},)"
                               R"({"start":1791374700000.0,"end":1791375000000.0,"mean":null}]})");
    REQUIRE(decode_history(stats, HistoryKind::Statistics, "sensor.t", points, error));
    REQUIRE(points.size() == 2);
    CHECK(points[0].time == 1791374400 && points[0].value == 21.25f);
    CHECK(points[1].time == 1791374700 && std::isnan(points[1].value));
    cJSON_Delete(stats);

    // nothing recorded in the window: no list, no error
    cJSON *empty = cJSON_Parse("{}");
    CHECK(decode_history(empty, HistoryKind::Statistics, "sensor.t", points, error) && points.size() == 2);
    cJSON_Delete(empty);

    points.clear();
    cJSON *states = cJSON_Parse(R"({"binary_sensor.m":[{"s":"off","lu":1791374400.5},)"
                                R"({"s":"on","lu":1791374460.0},{"s":"unavailable","lc":1791374500.0,"lu":1791374520.0}]})");
    REQUIRE(decode_history(states, HistoryKind::States, "binary_sensor.m", points, error));
    REQUIRE(points.size() == 3);
    CHECK(points[0].time == 1791374400 && points[0].value == 0.0f);
    CHECK(points[1].value == 1.0f);
    CHECK(points[2].time == 1791374500 && std::isnan(points[2].value));
    cJSON_Delete(states);

    cJSON *broken = cJSON_Parse(R"({"sensor.t":[{"mean":1}]})");
    CHECK(!decode_history(broken, HistoryKind::Statistics, "sensor.t", points, error) && !error.empty());
    cJSON_Delete(broken);
}

TEST(core_image_cover)
{
    // 1280x720 for a 230x150 tile: 1:4 is 320x180, 1:8 (160x90) would be enlarged
    CHECK(cover_shift(1280, 720, 230, 150) == 2);
    CHECK(cover_shift(640, 480, 640, 480) == 0);
    CHECK(cover_shift(4000, 3000, 100, 100) == 3); // never more than 1:8
    CHECK(scaled_size(721, 3) == 91);

    // full screen 800x480: the streams of the user's cameras (2026-10-08)
    CHECK(stream_shift(800, 480, 800, 480) == 0);
    CHECK(stream_shift(640, 320, 800, 480) == 0);   // smaller: shown whole
    CHECK(stream_shift(704, 576, 800, 480) == 0);   // 83 % shown
    CHECK(stream_shift(1280, 960, 800, 480) == 1);  // 1:1 shows 31 %, 1:2 all
    CHECK(stream_shift(2304, 1296, 800, 480) == 2); // 1:2 shows 51 %
    CHECK(stream_shift(8000, 6000, 100, 100) == 3); // never more than 1:8

    // the PPA's cover of the 800x480 screen (scale in 1/16)
    CoverBlock same = cover_block(800, 480, 800, 480, 16);
    CHECK(same.scale == 16 && same.width == 800 && same.height == 480 && same.out_width == 800 && same.out_x == 0);
    // 640x320 enlarged 1.5 times: 533 columns of the middle, 799 wide
    CoverBlock enlarged = cover_block(640, 320, 800, 480, 16);
    CHECK(enlarged.scale == 24 && enlarged.width == 533 && enlarged.height == 320 && enlarged.x == 53);
    CHECK(enlarged.out_width == 799 && enlarged.out_height == 480 && enlarged.out_x == 0);
    // 1280x960 at 10/16: 800x600 of which the middle 480 lines
    CoverBlock large = cover_block(1280, 960, 800, 480, 16);
    CHECK(large.scale == 10 && large.width == 1280 && large.height == 768 && large.y == 96);
    CHECK(large.out_width == 800 && large.out_height == 480);
    // 704x576: 1.136 rounds up to 19/16, never short of the box by more than the rounding
    CoverBlock odd = cover_block(704, 576, 800, 480, 16);
    CHECK(odd.scale == 19 && odd.out_width <= 800 && odd.out_width >= 798 && odd.out_height <= 480 && odd.out_height >= 478);
    // a tile from a large picture: at least 1/16
    CHECK(cover_block(4000, 3000, 100, 50, 16).scale == 1);
    // the whole of 1280x960 at 10/16: 800x600, nothing cut; twice the screen: 1600x1200
    CoverBlock whole = whole_block(1280, 960, 800, 480, 16);
    CHECK(whole.scale == 10 && whole.x == 0 && whole.width == 1280 && whole.height == 960);
    CHECK(whole.out_width == 800 && whole.out_height == 600 && whole.out_y == 0);
    whole = whole_block(1280, 960, 1600, 960, 16);
    CHECK(whole.scale == 20 && whole.out_width == 1600 && whole.out_height == 1200);
    // at most 2 M pixels: 640x360 for 200 % of 480x800 would be 2880x1620 (72/16), 1880x1057 at 47/16
    whole = whole_block(640, 360, 960, 1600, 16, 2000000);
    CHECK(whole.scale == 47 && whole.out_width == 1880 && whole.out_height == 1057);

    // esp_new_jpeg's scale (multiples of 8) to cover the S3's 480x320
    uint32_t sw = 0;
    uint32_t sh = 0;
    CHECK(cover_size8(800, 480, 480, 320, sw, sh) && sw == 536 && sh == 320);
    CHECK(cover_size8(1280, 960, 480, 320, sw, sh) && sw == 480 && sh == 360);
    CHECK(!cover_size8(640, 320, 480, 320, sw, sh)); // as large as the box in height: as it is
    CHECK(!cover_size8(320, 240, 480, 320, sw, sh)); // smaller: as it is
    CHECK(cover_size8(4000, 3000, 100, 50, sw, sh) && sw == 504 && sh == 376); // at most 1/8
    // a tile of 230x150 from 704x576
    CHECK(cover_size8(704, 576, 230, 150, sw, sh) && sw == 232 && sh == 192);

    // 5x3 in rows of 8 at 1:2: columns 0, 2, 4 of rows 0 and 2 (the sizes round up)
    uint16_t frame[8 * 3] = {};
    for (uint16_t i = 0; i < 8 * 3; i++) {
        frame[i] = i;
    }
    uint16_t half[3 * 2] = {};
    shrink(frame, 5, 3, 8, 1, half);
    CHECK(half[0] == 0 && half[1] == 2 && half[2] == 4);
    CHECK(half[3] == 16 && half[4] == 18 && half[5] == 20);

    // 4x2 into 2x2: the middle two columns
    const uint16_t wide[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint16_t box[4] = {};
    cover_copy(wide, 4, 2, box, 2, 2);
    CHECK(box[0] == 2 && box[1] == 3 && box[2] == 6 && box[3] == 7);

    // 2x2 into 4x4: each pixel twice in both directions
    const uint16_t small[] = {1, 2, 3, 4};
    uint16_t big[16] = {};
    cover_copy(small, 2, 2, big, 4, 4);
    CHECK(big[0] == 1 && big[1] == 1 && big[2] == 2 && big[3] == 2);
    CHECK(big[12] == 3 && big[15] == 4);

    // 2x4 (tall) into 2x2: the middle two rows
    const uint16_t tall[] = {1, 2, 3, 4, 5, 6, 7, 8};
    cover_copy(tall, 2, 4, box, 2, 2);
    CHECK(box[0] == 3 && box[1] == 4 && box[2] == 5 && box[3] == 6);
}

TEST(core_mjpeg_part_header)
{
    size_t length = 0;
    CHECK(mjpeg_content_length("Content-Length: 19993", length) && length == 19993);
    CHECK(mjpeg_content_length("content-length:42\r", length) && length == 42);
    CHECK(!mjpeg_content_length("Content-Type: image/jpeg", length));
    CHECK(!mjpeg_content_length("Content-Length: ", length));
    CHECK(!mjpeg_content_length("Content-Length: 12a", length));
    CHECK(!mjpeg_content_length("--frame", length));
}
