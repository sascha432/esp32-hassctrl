#include "hc/formats/multipart.hpp"

namespace hc::formats {

namespace {

bool equal_nocase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if ((a[i] | 0x20) != (b[i] | 0x20)) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// The value of `key="..."` (or key=value) in "form-data; name="a"; filename="b"".
std::string_view parameter(std::string_view header, std::string_view key)
{
    size_t from = 0;
    while (from < header.size()) {
        const size_t semicolon = header.find(';', from);
        std::string_view item = trim(header.substr(from, semicolon == std::string_view::npos ? std::string_view::npos : semicolon - from));
        const size_t equals = item.find('=');
        if (equals != std::string_view::npos && equal_nocase(trim(item.substr(0, equals)), key)) {
            std::string_view value = trim(item.substr(equals + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
        if (semicolon == std::string_view::npos) {
            break;
        }
        from = semicolon + 1;
    }
    return {};
}

} // namespace

std::string_view multipart_boundary(std::string_view content_type)
{
    const size_t semicolon = content_type.find(';');
    if (semicolon == std::string_view::npos || !equal_nocase(trim(content_type.substr(0, semicolon)), "multipart/form-data")) {
        return {};
    }
    return parameter(content_type.substr(semicolon + 1), "boundary");
}

std::expected<memory::Vector<Part>, memory::String> parse_multipart(std::string_view body, std::string_view boundary)
{
    const auto bad = [](const char *message) { return std::unexpected(memory::String(message)); };
    if (boundary.empty() || boundary.size() > 200) {
        return bad("the request has no multipart boundary");
    }
    memory::String delimiter("--");
    delimiter.append(boundary.data(), boundary.size());
    memory::String separator("\r\n");
    separator += delimiter;

    // the first delimiter may follow a preamble
    size_t at = body.find(delimiter);
    if (at == std::string_view::npos) {
        return bad("the request body is not multipart");
    }
    at += delimiter.size();

    memory::Vector<Part> parts;
    for (;;) {
        if (body.substr(at, 2) == "--") {
            return parts; // the closing delimiter
        }
        if (body.substr(at, 2) != "\r\n") {
            return bad("the multipart body is malformed");
        }
        at += 2;
        const size_t headers_end = body.find("\r\n\r\n", at);
        if (headers_end == std::string_view::npos) {
            return bad("a part has no header end");
        }
        const std::string_view headers = body.substr(at, headers_end - at);
        Part part;
        size_t line_start = 0;
        while (line_start < headers.size()) {
            size_t line_end = headers.find("\r\n", line_start);
            if (line_end == std::string_view::npos) {
                line_end = headers.size();
            }
            const std::string_view line = headers.substr(line_start, line_end - line_start);
            const size_t colon = line.find(':');
            if (colon != std::string_view::npos && equal_nocase(trim(line.substr(0, colon)), "content-disposition")) {
                const std::string_view value = line.substr(colon + 1);
                part.name = parameter(value, "name");
                part.filename = parameter(value, "filename");
            }
            line_start = line_end + 2;
        }
        const size_t data_start = headers_end + 4;
        const size_t data_end = body.find(separator, data_start);
        if (data_end == std::string_view::npos) {
            return bad("a part has no closing boundary");
        }
        part.data = body.substr(data_start, data_end - data_start);
        parts.push_back(part);
        at = data_end + separator.size();
    }
}

} // namespace hc::formats
