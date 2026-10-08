#pragma once

// The YAML subset of the dashboard import and the backup file (docs/architecture.md section 5,
// "Dashboard configuration"). Plain C++, host tested.
//
// Supported: block maps and lists with any consistent indentation (a list may start at the
// indentation of its key), plain, 'single' and "double" quoted scalars, comments, a list of
// scalars in brackets on one line ([2, 1]), a leading "---".
// Rejected with a message: tabs in the indentation, anchors and aliases, tags, block scalars
// (| and >), flow maps ({...}), duplicate keys, more than one document.
//
// Scalars stay text: what a value means (number, true/false) is up to the reader of the tree.
// The tree lives on the large heap.

#include <cstdint>
#include <expected>
#include <string_view>

#include "hc/memory/large.hpp"

namespace hc::formats::yaml {

enum class Kind : uint8_t {
    Null,   // "key:" with nothing below it, "- " with nothing after it
    Scalar,
    Map,
    List,
};

struct Entry;

struct Node {
    Kind kind = Kind::Null;
    // 1-based line where the node starts; for a value, the line of its key or "- "
    uint32_t line = 0;
    memory::String text;          // Scalar
    memory::Vector<Entry> map;    // Map, in the order of the file
    memory::Vector<Node> list;    // List

    // nullptr when this is not a map or the key is not in it
    const Node *find(std::string_view key) const;
};

struct Entry {
    memory::String key;
    uint32_t line = 0;
    Node value;
};

struct Error {
    uint32_t line = 0;
    memory::String message;
};

std::expected<Node, Error> parse(std::string_view source);

// `text` as a scalar for writing: unchanged when YAML reads it back as the same plain text,
// otherwise in double quotes with escapes.
memory::String quote(std::string_view text);

} // namespace hc::formats::yaml
