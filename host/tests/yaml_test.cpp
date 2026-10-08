// components/formats/src/yaml.cpp: the YAML subset of the dashboard import and the backup file.

#include "check.hpp"
#include "hc/formats/yaml.hpp"

namespace yaml = hc::formats::yaml;

namespace {

// "line N: message" of a failed parse, or "ok"
std::string error_of(std::string_view source)
{
    const auto parsed = yaml::parse(source);
    if (parsed) {
        return "ok";
    }
    return "line " + std::to_string(parsed.error().line) + ": " + std::string(parsed.error().message);
}

const char *text(const yaml::Node *node)
{
    return node != nullptr && node->kind == yaml::Kind::Scalar ? node->text.c_str() : nullptr;
}

} // namespace

TEST(yaml_reads_maps_lists_and_scalars)
{
    const auto parsed = yaml::parse("defaults:\n"
                                    "  grid: 4x2\n"
                                    "tiles:\n"
                                    "  - entity: light.a   # comment\n"
                                    "    name: 'Kid''s room'\n"
                                    "  - type: area\n"
                                    "    name: \"Rooms \\\"1\\\"\"\n"
                                    "    tiles:\n"
                                    "      - include: rooms/office.yaml\n");
    REQUIRE(parsed.has_value());
    const yaml::Node &root = *parsed;
    REQUIRE(root.kind == yaml::Kind::Map);
    CHECK_STR(text(root.find("defaults")->find("grid")), "4x2");
    const yaml::Node *tiles = root.find("tiles");
    REQUIRE(tiles != nullptr && tiles->kind == yaml::Kind::List);
    REQUIRE(tiles->list.size() == 2);
    CHECK_STR(text(tiles->list[0].find("entity")), "light.a");
    CHECK_STR(text(tiles->list[0].find("name")), "Kid's room");
    CHECK(tiles->list[0].line == 4);
    CHECK_STR(text(tiles->list[1].find("name")), "Rooms \"1\"");
    const yaml::Node *nested = tiles->list[1].find("tiles");
    REQUIRE(nested != nullptr && nested->list.size() == 1);
    CHECK_STR(text(nested->list[0].find("include")), "rooms/office.yaml");
    CHECK(nested->list[0].line == 9);
}

TEST(yaml_accepts_any_consistent_indentation_and_lists_at_key_level)
{
    const auto parsed = yaml::parse("tiles:\n"
                                    "- entity: light.a\n"
                                    "  name: A\n"
                                    "- type: area\n"
                                    "  name: B\n"
                                    "  tiles:\n"
                                    "        - entity: switch.c\n");
    REQUIRE(parsed.has_value());
    const yaml::Node *tiles = parsed->find("tiles");
    REQUIRE(tiles != nullptr && tiles->list.size() == 2);
    CHECK_STR(text(tiles->list[0].find("name")), "A");
    CHECK_STR(text(tiles->list[1].find("tiles")->list[0].find("entity")), "switch.c");
}

TEST(yaml_reads_a_file_that_is_a_list)
{
    const auto parsed = yaml::parse("# rooms.yaml\n"
                                    "- type: area\n"
                                    "  name: Rooms\n"
                                    "  tiles:\n"
                                    "    - include: rooms/office.yaml\n");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->kind == yaml::Kind::List);
    CHECK(parsed->list[0].line == 2);
    CHECK_STR(text(parsed->list[0].find("name")), "Rooms");
}

TEST(yaml_keeps_colons_and_hashes_inside_values)
{
    const auto parsed = yaml::parse("url: http://h:8123/x#y\n"
                                    "name: \"DC Dimmer #1\"\n"
                                    "unit: hPa sea lvl  # comment\n"
                                    "empty:\n"
                                    "note: # only a comment\n");
    REQUIRE(parsed.has_value());
    CHECK_STR(text(parsed->find("url")), "http://h:8123/x#y");
    CHECK_STR(text(parsed->find("name")), "DC Dimmer #1");
    CHECK_STR(text(parsed->find("unit")), "hPa sea lvl");
    CHECK(parsed->find("empty")->kind == yaml::Kind::Null);
    CHECK(parsed->find("note")->kind == yaml::Kind::Null);
}

TEST(yaml_reads_lists_in_brackets_and_escapes)
{
    const auto parsed = yaml::parse("position: [2, 1]\n"
                                    "none: []\n"
                                    "text: \"a\\nb\\u00b0C\"\n");
    REQUIRE(parsed.has_value());
    const yaml::Node *position = parsed->find("position");
    REQUIRE(position->kind == yaml::Kind::List && position->list.size() == 2);
    CHECK_STR(position->list[1].text.c_str(), "1");
    CHECK(parsed->find("none")->list.empty());
    CHECK_STR(text(parsed->find("text")), "a\nb\xC2\xB0" "C");
}

TEST(yaml_skips_bom_crlf_and_a_leading_document_marker)
{
    const auto parsed = yaml::parse("\xEF\xBB\xBF---\r\nname: A\r\n");
    REQUIRE(parsed.has_value());
    CHECK_STR(text(parsed->find("name")), "A");
}

TEST(yaml_reports_what_it_does_not_support)
{
    CHECK_STR(error_of("a:\n\tb: 1\n").c_str(), "line 2: tabs are not allowed for indentation, use spaces");
    CHECK_STR(error_of("a: &x 1\n").c_str(), "line 1: anchors and aliases (& and *) are not supported");
    CHECK_STR(error_of("a: |\n  text\n").c_str(), "line 1: block scalars (| and >) are not supported, write the text on one line");
    CHECK_STR(error_of("a: {b: 1}\n").c_str(), "line 1: flow maps ({...}) are not supported, write one key per line");
    CHECK_STR(error_of("a: !tag 1\n").c_str(), "line 1: tags (!) are not supported");
    CHECK_STR(error_of("a: 1\n---\nb: 2\n").c_str(), "line 2: only one document per file");
}

TEST(yaml_reports_structure_errors_with_the_line)
{
    CHECK_STR(error_of("a: 1\na: 2\n").c_str(), "line 2: the key is in this block twice");
    CHECK_STR(error_of("a: 1\n  b: 2\n").c_str(), "line 2: unexpected indentation");
    CHECK_STR(error_of("a:\n  - x\n  b: 1\n").c_str(), "line 3: unexpected indentation");
    CHECK_STR(error_of("a: 1\nb\n").c_str(), "line 2: expected \"key: value\"");
    CHECK_STR(error_of("a: \"open\n").c_str(), "line 1: the closing quote is missing");
    CHECK_STR(error_of("a: \"x\" y\n").c_str(), "line 1: text after the closing quote");
    CHECK_STR(error_of("  a: 1\n").c_str(), "line 1: the first line must not be indented");
    CHECK_STR(error_of("tiles:\n  - entity: a\n   name: b\n").c_str(), "line 3: unexpected indentation");
}

TEST(yaml_quotes_only_what_needs_it)
{
    CHECK_STR(yaml::quote("Living Room").c_str(), "Living Room");
    CHECK_STR(yaml::quote("http://h:8123").c_str(), "http://h:8123");
    CHECK_STR(yaml::quote("DC Dimmer #1").c_str(), "\"DC Dimmer #1\"");
    CHECK_STR(yaml::quote("a: b").c_str(), "\"a: b\"");
    CHECK_STR(yaml::quote("- x").c_str(), "\"- x\"");
    CHECK_STR(yaml::quote("yes").c_str(), "\"yes\"");
    CHECK_STR(yaml::quote("").c_str(), "\"\"");
    CHECK_STR(yaml::quote("say \"hi\"\n").c_str(), "\"say \\\"hi\\\"\\n\"");
}

TEST(yaml_reads_back_what_quote_wrote)
{
    const char *texts[] = {"Living Room", "DC Dimmer #1", "a: b", "- x", "yes", "", "say \"hi\"\n", "°C", "'single'"};
    for (const char *original : texts) {
        const std::string source = "v: " + std::string(yaml::quote(original)) + "\n";
        const auto parsed = yaml::parse(source);
        REQUIRE(parsed.has_value());
        CHECK_STR(text(parsed->find("v")), original);
    }
}
