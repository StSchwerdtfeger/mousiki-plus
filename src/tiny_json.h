#pragma once
// A deliberately small, self-contained JSON reader/writer. mousiki has
// no JSON dependency anywhere else in the codebase (the two spots that
// touch yt-dlp's JSON lines use a one-key string grep, see
// online_source.cpp) and pulling in a whole library just for one
// flat-ish snapshot file isn't worth it. This supports exactly what
// snapshot.cpp needs: objects, arrays, strings (with the common escapes),
// numbers, and booleans -- nothing fancier (no unicode \uXXXX escapes,
// no comments, no trailing commas). Malformed input is handled by
// returning a Null-ish/empty result rather than throwing; a corrupt or
// half-written snapshot.json should never crash the app, it should just
// fail to restore.
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace muisc {
namespace tinyjson {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj; // insertion-ordered, small N -- linear find() is fine

    static Value make_null() { return Value{}; }
    static Value make_bool(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value make_num(double v) { Value x; x.type = Type::Number; x.num = v; return x; }
    static Value make_str(std::string v) { Value x; x.type = Type::String; x.str = std::move(v); return x; }
    static Value make_arr() { Value x; x.type = Type::Array; return x; }
    static Value make_obj() { Value x; x.type = Type::Object; return x; }

    void set(const std::string& key, Value v) {
        for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(v); return; }
        obj.emplace_back(key, std::move(v));
    }
    const Value* find(const std::string& key) const {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }

    std::string as_string(const std::string& def = "") const { return type == Type::String ? str : def; }
    double as_number(double def = 0.0) const { return type == Type::Number ? num : def; }
    bool as_bool(bool def = false) const { return type == Type::Bool ? b : def; }
};

// ---------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------

// Escapes a string for JSON. Control characters become \u00XX (what the remote page needs).
inline std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Escapes a string for a saved file. Control characters are left out, as the files always did: a control character in a
// title or tag must not come back as a live terminal sequence when the file is read again.
inline std::string escape_for_file(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) out += c;
        }
    }
    return out;
}

inline void dump(const Value& v, std::string& out, int indent) {
    auto pad = [&](int n) { out.append(static_cast<size_t>(n) * 2, ' '); };
    switch (v.type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.b ? "true" : "false"; break;
        case Type::Number: {
            // Integral values print without a trailing ".0" so ints round-trip cleanly.
            if (v.num == static_cast<long long>(v.num)) out += std::to_string(static_cast<long long>(v.num));
            else out += std::to_string(v.num);
            break;
        }
        case Type::String: out += "\""; out += escape_for_file(v.str); out += "\""; break;
        case Type::Array: {
            if (v.arr.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.arr.size(); ++i) {
                pad(indent + 1);
                dump(v.arr[i], out, indent + 1);
                if (i + 1 < v.arr.size()) out += ",";
                out += "\n";
            }
            pad(indent); out += "]";
            break;
        }
        case Type::Object: {
            if (v.obj.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t i = 0; i < v.obj.size(); ++i) {
                pad(indent + 1);
                out += "\""; out += escape_for_file(v.obj[i].first); out += "\": ";
                dump(v.obj[i].second, out, indent + 1);
                if (i + 1 < v.obj.size()) out += ",";
                out += "\n";
            }
            pad(indent); out += "}";
            break;
        }
    }
}

inline std::string write(const Value& v) {
    std::string out;
    dump(v, out, 0);
    out += "\n";
    return out;
}

// ---------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool parse(Value& out) {
        skip_ws();
        if (!parse_value(out)) return false;
        return true; // trailing garbage is tolerated
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skip_ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_; }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

    bool parse_value(Value& out) {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') return parse_string_value(out);
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == 'n') { if (s_.compare(i_, 4, "null") == 0) { i_ += 4; out = Value::make_null(); return true; } return false; }
        return parse_number(out);
    }

    bool parse_object(Value& out) {
        out = Value::make_obj();
        ++i_; // '{'
        skip_ws();
        if (peek() == '}') { ++i_; return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (peek() != '"' || !parse_raw_string(key)) return false;
            skip_ws();
            if (peek() != ':') return false;
            ++i_;
            Value v;
            if (!parse_value(v)) return false;
            out.obj.emplace_back(key, std::move(v));
            skip_ws();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == '}') { ++i_; break; }
            return false;
        }
        return true;
    }

    bool parse_array(Value& out) {
        out = Value::make_arr();
        ++i_; // '['
        skip_ws();
        if (peek() == ']') { ++i_; return true; }
        while (true) {
            Value v;
            if (!parse_value(v)) return false;
            out.arr.push_back(std::move(v));
            skip_ws();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == ']') { ++i_; break; }
            return false;
        }
        return true;
    }

    // Four hex digits at `pos`; false when they are missing or not hex.
    bool read_hex4(size_t pos, uint32_t& out) const {
        if (pos + 4 > s_.size()) return false;
        uint32_t v = 0;
        for (size_t k = 0; k < 4; ++k) {
            const char c = s_[pos + k];
            uint32_t h;
            if (c >= '0' && c <= '9') h = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') h = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') h = static_cast<uint32_t>(c - 'A' + 10);
            else return false;
            v = (v << 4) | h;
        }
        out = v;
        return true;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parse_raw_string(std::string& out) {
        if (peek() != '"') return false;
        ++i_;
        out.clear();
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) {
                char n = s_[i_ + 1];
                switch (n) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'u': {
                        // \uXXXX; a high surrogate followed by a low one is one character outside the BMP (emoji ...).
                        uint32_t cp = 0;
                        if (!read_hex4(i_ + 2, cp)) { out += n; break; }
                        size_t consumed = 6;
                        if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 11 < s_.size() && s_[i_ + 6] == '\\' && s_[i_ + 7] == 'u') {
                            uint32_t low = 0;
                            if (read_hex4(i_ + 8, low) && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                consumed = 12;
                            }
                        }
                        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;   // unpaired surrogate: not valid UTF-8, so a replacement character
                        append_utf8(out, cp);
                        i_ += consumed;
                        continue;
                    }
                    default: out += n; break;
                }
                i_ += 2;
                continue;
            }
            out += c;
            ++i_;
        }
        if (i_ >= s_.size()) return false; // unterminated
        ++i_; // closing quote
        return true;
    }

    bool parse_string_value(Value& out) {
        std::string raw;
        if (!parse_raw_string(raw)) return false;
        out = Value::make_str(std::move(raw));
        return true;
    }

    bool parse_bool(Value& out) {
        if (s_.compare(i_, 4, "true") == 0) { i_ += 4; out = Value::make_bool(true); return true; }
        if (s_.compare(i_, 5, "false") == 0) { i_ += 5; out = Value::make_bool(false); return true; }
        return false;
    }

    bool parse_number(Value& out) {
        size_t start = i_;
        if (peek() == '-' || peek() == '+') ++i_;
        while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                                   s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+')) ++i_;
        if (i_ == start) return false;
        try {
            out = Value::make_num(std::stod(s_.substr(start, i_ - start)));
        } catch (...) {
            return false;
        }
        return true;
    }
};

inline bool parse(const std::string& text, Value& out) {
    Parser p(text);
    return p.parse(out);
}

// The JSON object a helper program printed. Helpers may print diagnostics before their answer, so when the whole text
// is not one object, its last line that starts with '{' is used.
inline bool parse_answer(const std::string& text, Value& out) {
    if (parse(text, out) && out.type == Type::Object) return true;
    std::string last;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '{') last = line;
    }
    return !last.empty() && parse(last, out) && out.type == Type::Object;
}

// Field readers for an object: the default when the key is missing or holds another type.
inline std::string string_field(const Value& o, const std::string& key, const std::string& def = "") {
    const Value* v = o.find(key);
    return v ? v->as_string(def) : def;
}
inline bool bool_field(const Value& o, const std::string& key, bool def = false) {
    const Value* v = o.find(key);
    return v ? v->as_bool(def) : def;
}
inline double number_field(const Value& o, const std::string& key, double def = 0.0) {
    const Value* v = o.find(key);
    return v ? v->as_number(def) : def;
}

} // namespace tinyjson
} // namespace muisc
