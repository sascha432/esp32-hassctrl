#pragma once

// multipart/form-data (RFC 7578), as a browser or `requests` sends a set of files. The parts
// point into the body, which has to stay alive. Plain C++, host tested.

#include <expected>
#include <string_view>

#include "hc/memory/large.hpp"

namespace hc::formats {

struct Part {
    std::string_view name;     // name="..." of the form field
    std::string_view filename; // filename="..."; empty for a field that is not a file
    std::string_view data;
};

// The boundary of a "Content-Type: multipart/form-data; boundary=..." value; empty when it is
// not multipart or has no boundary.
std::string_view multipart_boundary(std::string_view content_type);

// All parts of `body`. An error is a sentence for the person who sent the request.
std::expected<memory::Vector<Part>, memory::String> parse_multipart(std::string_view body, std::string_view boundary);

} // namespace hc::formats
