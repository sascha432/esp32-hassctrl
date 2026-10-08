// components/formats/src/multipart.cpp: the upload of a set of files.

#include "check.hpp"
#include "hc/formats/multipart.hpp"

using namespace hc::formats;

namespace {

std::string field(const char *name, const char *filename, const char *data)
{
    std::string text = "--XyZ\r\nContent-Disposition: form-data; name=\"" + std::string(name) + "\"";
    if (filename != nullptr) {
        text += "; filename=\"" + std::string(filename) + "\"\r\nContent-Type: text/yaml";
    }
    return text + "\r\n\r\n" + data + "\r\n";
}

} // namespace

TEST(multipart_finds_the_boundary)
{
    CHECK_STR(std::string(multipart_boundary("multipart/form-data; boundary=XyZ")).c_str(), "XyZ");
    CHECK_STR(std::string(multipart_boundary("Multipart/Form-Data; charset=utf-8; boundary=\"a b\"")).c_str(), "a b");
    CHECK(multipart_boundary("application/x-www-form-urlencoded").empty());
    CHECK(multipart_boundary("multipart/form-data").empty());
}

TEST(multipart_reads_files_with_paths_and_binary_safe_data)
{
    const std::string body = field("files", "hass.yaml", "tiles:\r\n  - entity: a.b\r\n") +
                             field("files", "rooms/bath.yaml", "- entity: b.c  # --XyZ in a line\r\n") +
                             field("check", nullptr, "1") + "--XyZ--\r\n";
    const auto parts = parse_multipart(body, "XyZ");
    REQUIRE(parts.has_value());
    REQUIRE(parts->size() == 3);
    CHECK_STR(std::string((*parts)[0].filename).c_str(), "hass.yaml");
    CHECK_STR(std::string((*parts)[0].data).c_str(), "tiles:\r\n  - entity: a.b\r\n");
    CHECK_STR(std::string((*parts)[1].filename).c_str(), "rooms/bath.yaml");
    CHECK_STR(std::string((*parts)[1].data).c_str(), "- entity: b.c  # --XyZ in a line\r\n");
    CHECK_STR(std::string((*parts)[2].name).c_str(), "check");
    CHECK((*parts)[2].filename.empty());
    CHECK_STR(std::string((*parts)[2].data).c_str(), "1");
}

TEST(multipart_allows_a_preamble_and_empty_files)
{
    const std::string body = "ignored preamble\r\n" + field("files", "empty.yaml", "") + "--XyZ--";
    const auto parts = parse_multipart(body, "XyZ");
    REQUIRE(parts.has_value() && parts->size() == 1);
    CHECK((*parts)[0].data.empty());
}

TEST(multipart_reports_a_broken_body)
{
    const auto error = [](const std::string &body) {
        const auto parts = parse_multipart(body, "XyZ");
        return parts ? std::string("ok") : std::string(parts.error());
    };
    CHECK_STR(error("nothing here").c_str(), "the request body is not multipart");
    CHECK_STR(error("--XyZ junk").c_str(), "the multipart body is malformed");
    CHECK_STR(error("--XyZ\r\nContent-Disposition: form-data; name=\"a\"").c_str(), "a part has no header end");
    CHECK_STR(error("--XyZ\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\ndata").c_str(), "a part has no closing boundary");
    CHECK_STR(std::string(parse_multipart("x", "").error()).c_str(), "the request has no multipart boundary");
}
