// Small text helpers shared by the whole program. Everything here works on bytes: UTF-8 sequences pass through
// unchanged, so they are safe to use on any text (see utf8_util.h for the width and case helpers).
#pragma once
#include <cctype>
#include <string>

namespace muisc {

// True for the ASCII whitespace characters: space, tab, LF, VT, FF, CR. Written out instead of std::isspace, whose answer
// depends on the C locale (some Windows code pages report bytes above 127 as spaces).
inline bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// Strips leading and trailing whitespace (see is_ascii_space). Returns an empty string for blank input.
inline std::string trim_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_ascii_space(s[a])) ++a;
    while (b > a && is_ascii_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

} // namespace muisc
