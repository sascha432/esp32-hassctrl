#include "hc/formats/yaml.hpp"

#include <cstdio>
#include <cstring>

namespace hc::formats::yaml {

const Node *Node::find(std::string_view key) const
{
    for (const auto &entry : map) {
        if (entry.key == key) {
            return &entry.value;
        }
    }
    return nullptr;
}

namespace {

struct Line {
    uint32_t number;
    uint32_t indent;
    // after the indentation, without trailing spaces; full-line comments and empty lines are
    // not kept at all
    std::string_view text;
};

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

bool is_dash(std::string_view text)
{
    return text == "-" || text.starts_with("- ");
}

// the text after "- "; empty for "-" alone
std::string_view after_dash(std::string_view text)
{
    return text.size() <= 1 ? std::string_view() : trim(text.substr(2));
}

// The position of the ':' that ends the key of "key: value" or "key:", or npos. A ':' inside a
// plain value ("http://h") is not followed by a space and does not count.
size_t key_end(std::string_view text)
{
    if (text.empty() || text.front() == '"' || text.front() == '\'' || text.front() == '#') {
        return std::string_view::npos;
    }
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '#' && i > 0 && text[i - 1] == ' ') {
            return std::string_view::npos;
        }
        if (text[i] == ':' && (i + 1 == text.size() || text[i + 1] == ' ')) {
            return i;
        }
    }
    return std::string_view::npos;
}

// a value that is empty or only a comment ("key:   # note")
bool is_empty_value(std::string_view text)
{
    return text.empty() || text.front() == '#';
}

class Parser {
public:
    explicit Parser(std::string_view source) : _source(source)
    {
    }

    std::expected<Node, Error> run()
    {
        if (!split()) {
            return std::unexpected(_error);
        }
        Node root;
        if (_lines.empty()) {
            return root;
        }
        root.line = _lines[0].number;
        if (_lines[0].indent != 0) {
            fail(_lines[0].number, "the first line must not be indented");
            return std::unexpected(_error);
        }
        if (!block(0, root)) {
            return std::unexpected(_error);
        }
        if (_pos < _lines.size()) {
            fail(_lines[_pos].number, "unexpected indentation");
            return std::unexpected(_error);
        }
        return root;
    }

private:
    bool fail(uint32_t line, const char *message)
    {
        _error.line = line;
        _error.message = message;
        return false;
    }

    bool split()
    {
        std::string_view rest = _source;
        if (rest.starts_with("\xEF\xBB\xBF")) {
            rest.remove_prefix(3); // UTF-8 byte order mark
        }
        uint32_t number = 0;
        bool document_started = false;
        while (!rest.empty()) {
            const size_t end = rest.find('\n');
            std::string_view line = rest.substr(0, end);
            rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
            number++;

            uint32_t indent = 0;
            while (indent < line.size() && line[indent] == ' ') {
                indent++;
            }
            const std::string_view text = trim(line.substr(indent));
            if (text.empty() || text.front() == '#') {
                continue;
            }
            if (indent < line.size() && line[indent] == '\t') {
                return fail(number, "tabs are not allowed for indentation, use spaces");
            }
            if (text == "---" && indent == 0) {
                if (document_started) {
                    return fail(number, "only one document per file");
                }
                document_started = true;
                continue;
            }
            if (text == "..." && indent == 0) {
                return fail(number, "only one document per file");
            }
            document_started = true;
            _lines.push_back({number, indent, text});
        }
        return true;
    }

    // a map, a list or a scalar that starts at the current line, at `indent`
    bool block(uint32_t indent, Node &out)
    {
        const Line &line = _lines[_pos];
        out.line = line.number;
        if (is_dash(line.text)) {
            return list(indent, out);
        }
        if (key_end(line.text) != std::string_view::npos) {
            return map(indent, out);
        }
        // a scalar on a line of its own: only as a list item ("- text" made into a line)
        if (!scalar(line.text, line.number, out)) {
            return false;
        }
        _pos++;
        return true;
    }

    bool list(uint32_t indent, Node &out)
    {
        out.kind = Kind::List;
        while (_pos < _lines.size() && _lines[_pos].indent == indent && is_dash(_lines[_pos].text)) {
            Line &line = _lines[_pos];
            Node item;
            item.line = line.number;
            const std::string_view rest = after_dash(line.text);
            if (is_empty_value(rest)) {
                // "-" alone: the item is the block below it, or null
                _pos++;
                if (_pos < _lines.size() && _lines[_pos].indent > indent) {
                    if (!block(_lines[_pos].indent, item)) {
                        return false;
                    }
                }
            } else {
                // "- key: value" or "- - x": the rest of the line starts a block at its own
                // column, which the following lines continue
                const uint32_t column = indent + static_cast<uint32_t>(rest.data() - line.text.data());
                line.indent = column;
                line.text = rest;
                if (!block(column, item)) {
                    return false;
                }
                item.line = line.number;
            }
            out.list.push_back(std::move(item));
            if (_pos < _lines.size() && _lines[_pos].indent > indent &&
                !(_lines[_pos].indent == indent && is_dash(_lines[_pos].text))) {
                return fail(_lines[_pos].number, "unexpected indentation");
            }
        }
        return true;
    }

    bool map(uint32_t indent, Node &out)
    {
        out.kind = Kind::Map;
        while (_pos < _lines.size() && _lines[_pos].indent == indent) {
            const Line line = _lines[_pos];
            if (is_dash(line.text)) {
                return fail(line.number, "a list item where a key was expected");
            }
            const size_t colon = key_end(line.text);
            if (colon == std::string_view::npos) {
                return fail(line.number, "expected \"key: value\"");
            }
            const std::string_view key = trim(line.text.substr(0, colon));
            if (key.empty()) {
                return fail(line.number, "the key is empty");
            }
            if (out.find(key) != nullptr) {
                return fail(line.number, "the key is in this block twice");
            }
            Entry entry;
            entry.key.assign(key.data(), key.size());
            entry.line = line.number;
            entry.value.line = line.number;
            const std::string_view value = trim(line.text.substr(colon + 1));
            _pos++;
            if (is_empty_value(value)) {
                if (_pos < _lines.size() && _lines[_pos].indent > indent) {
                    if (!block(_lines[_pos].indent, entry.value)) {
                        return false;
                    }
                } else if (_pos < _lines.size() && _lines[_pos].indent == indent && is_dash(_lines[_pos].text)) {
                    // a list may start at the indentation of its key
                    if (!list(indent, entry.value)) {
                        return false;
                    }
                }
            } else if (!scalar(value, line.number, entry.value)) {
                return false;
            }
            out.map.push_back(std::move(entry));
            if (_pos < _lines.size() && _lines[_pos].indent > indent) {
                return fail(_lines[_pos].number, "unexpected indentation");
            }
        }
        return true;
    }

    bool scalar(std::string_view text, uint32_t line, Node &out)
    {
        out.line = line;
        out.kind = Kind::Scalar;
        const char first = text.front();
        if (first == '"' || first == '\'') {
            size_t end = 0;
            if (!quoted(text, line, out.text, end)) {
                return false;
            }
            const std::string_view after = trim(text.substr(end));
            if (!after.empty() && after.front() != '#') {
                return fail(line, "text after the closing quote");
            }
            return true;
        }
        switch (first) {
        case '|':
        case '>':
            return fail(line, "block scalars (| and >) are not supported, write the text on one line");
        case '&':
        case '*':
            return fail(line, "anchors and aliases (& and *) are not supported");
        case '!':
            return fail(line, "tags (!) are not supported");
        case '{':
            return fail(line, "flow maps ({...}) are not supported, write one key per line");
        case '[':
            return flow_list(text, line, out);
        default:
            break;
        }
        // a plain scalar ends at a comment
        size_t end = text.size();
        for (size_t i = 1; i < text.size(); i++) {
            if (text[i] == '#' && (text[i - 1] == ' ' || text[i - 1] == '\t')) {
                end = i;
                break;
            }
        }
        const std::string_view plain = trim(text.substr(0, end));
        out.text.assign(plain.data(), plain.size());
        return true;
    }

    // "text" with escapes or 'text' with '' for a quote; `end` is the index after the closing quote
    bool quoted(std::string_view text, uint32_t line, memory::String &out, size_t &end)
    {
        const char quote = text.front();
        out.clear();
        for (size_t i = 1; i < text.size(); i++) {
            const char c = text[i];
            if (quote == '\'' && c == '\'') {
                if (i + 1 < text.size() && text[i + 1] == '\'') {
                    out.push_back('\'');
                    i++;
                    continue;
                }
                end = i + 1;
                return true;
            }
            if (quote == '"' && c == '"') {
                end = i + 1;
                return true;
            }
            if (quote == '"' && c == '\\') {
                if (++i >= text.size()) {
                    break;
                }
                switch (text[i]) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case '0':
                    return fail(line, "\\0 is not allowed in a text");
                case '"':
                case '\\':
                case '/':
                    out.push_back(text[i]);
                    break;
                case 'u': {
                    if (i + 4 >= text.size()) {
                        return fail(line, "\\u needs four hex digits");
                    }
                    unsigned value = 0;
                    for (size_t k = 1; k <= 4; k++) {
                        const char h = text[i + k];
                        value <<= 4;
                        if (h >= '0' && h <= '9') {
                            value |= static_cast<unsigned>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            value |= static_cast<unsigned>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            value |= static_cast<unsigned>(h - 'A' + 10);
                        } else {
                            return fail(line, "\\u needs four hex digits");
                        }
                    }
                    i += 4;
                    if (value == 0) {
                        return fail(line, "\\0 is not allowed in a text");
                    }
                    // UTF-8 of a code point of the basic plane
                    if (value < 0x80) {
                        out.push_back(static_cast<char>(value));
                    } else if (value < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (value >> 6)));
                        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (value >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                    }
                    break;
                }
                default:
                    return fail(line, "unknown escape after \\");
                }
                continue;
            }
            out.push_back(c);
        }
        return fail(line, "the closing quote is missing");
    }

    // [a, b, "c"]: scalars only, on one line
    bool flow_list(std::string_view text, uint32_t line, Node &out)
    {
        out.kind = Kind::List;
        size_t i = 1;
        for (;;) {
            while (i < text.size() && text[i] == ' ') {
                i++;
            }
            if (i >= text.size()) {
                return fail(line, "the closing ] is missing");
            }
            if (text[i] == ']' && out.list.empty()) {
                i++;
                break;
            }
            Node item;
            item.kind = Kind::Scalar;
            item.line = line;
            if (text[i] == '"' || text[i] == '\'') {
                size_t used = 0;
                if (!quoted(text.substr(i), line, item.text, used)) {
                    return false;
                }
                i += used;
            } else {
                const size_t start = i;
                while (i < text.size() && text[i] != ',' && text[i] != ']') {
                    if (text[i] == '[' || text[i] == '{') {
                        return fail(line, "lists in brackets can only hold plain values");
                    }
                    i++;
                }
                const std::string_view value = trim(text.substr(start, i - start));
                item.text.assign(value.data(), value.size());
            }
            out.list.push_back(std::move(item));
            while (i < text.size() && text[i] == ' ') {
                i++;
            }
            if (i < text.size() && text[i] == ',') {
                i++;
                continue;
            }
            if (i < text.size() && text[i] == ']') {
                i++;
                break;
            }
            return fail(line, "expected , or ] in the list");
        }
        const std::string_view after = trim(text.substr(i));
        if (!after.empty() && after.front() != '#') {
            return fail(line, "text after the closing ]");
        }
        return true;
    }

    std::string_view _source;
    memory::Vector<Line> _lines;
    size_t _pos = 0;
    Error _error;
};

bool needs_quotes(std::string_view text)
{
    if (text.empty() || text.front() == ' ' || text.back() == ' ') {
        return true;
    }
    if (std::strchr("-?:,[]{}#&*!|>'\"%@`", text.front()) != nullptr) {
        return true;
    }
    for (size_t i = 0; i < text.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7F) {
            return true;
        }
        if (c == ':' && (i + 1 == text.size() || text[i + 1] == ' ')) {
            return true;
        }
        if (c == '#' && text[i - 1] == ' ') {
            return true;
        }
    }
    // words other YAML readers take as true, false or null
    static constexpr const char *kWords[] = {"true", "false", "yes", "no", "on", "off", "null", "~"};
    for (const char *word : kWords) {
        if (text.size() == std::strlen(word)) {
            bool same = true;
            for (size_t i = 0; i < text.size() && same; i++) {
                same = (text[i] | 0x20) == word[i];
            }
            if (same) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::expected<Node, Error> parse(std::string_view source)
{
    return Parser(source).run();
}

memory::String quote(std::string_view text)
{
    memory::String out;
    if (!needs_quotes(text)) {
        out.assign(text.data(), text.size());
        return out;
    }
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escape[8];
                std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned char>(c));
                out += escape;
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
    return out;
}

} // namespace hc::formats::yaml
