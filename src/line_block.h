#pragma once
// Whole lines of a multi-line editor (the script overlay): the text of marked lines for the clipboard, and pasting a text of several lines.
// Pure text work, tested without a screen (tests/test_line_block.cpp).
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace muisc::lineblock {

// Line breaks of any kind -> '\n'; control characters are dropped (a tab becomes a space); nothing else is changed.
inline std::string clean_text(const std::string& text) {
    std::string o;
    o.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\r') { o.push_back('\n'); if (i + 1 < text.size() && text[i + 1] == '\n') ++i; }
        else if (c == '\n') o.push_back('\n');
        else if (c == '\t') o.push_back(' ');
        else if (c >= 32 && c != 127) o.push_back(static_cast<char>(c));
    }
    return o;
}

// True when the (cleaned) text has more than one line, or is one whole line (ends with a line break).
inline bool is_multiline(const std::string& text) { return clean_text(text).find('\n') != std::string::npos; }

// Lines first..last (both included, clamped) as clipboard text: every line ends with a line break, so a paste knows these are whole lines.
inline std::string join_lines(const std::vector<std::string>& lines, int first, int last) {
    std::string t;
    if (lines.empty()) return t;
    first = std::max(0, first);
    last = std::min(static_cast<int>(lines.size()) - 1, last);
    for (int i = first; i <= last; ++i) t += lines[static_cast<size_t>(i)] + "\n";
    return t;
}

// Takes lines first..last out (at least one, empty, line is left). Returns how many were taken. `at` is set to where the caret line goes.
inline int erase_lines(std::vector<std::string>& lines, int first, int last, int& at) {
    if (lines.empty()) { lines.push_back(""); at = 0; return 0; }
    first = std::clamp(first, 0, static_cast<int>(lines.size()) - 1);
    last = std::clamp(last, first, static_cast<int>(lines.size()) - 1);
    const int n = last - first + 1;
    lines.erase(lines.begin() + first, lines.begin() + last + 1);
    if (lines.empty()) lines.push_back("");
    at = std::min(first, static_cast<int>(lines.size()) - 1);
    return n;
}

// Pastes a text of several lines (see is_multiline) into `lines`, the caret at line `cy`, byte `cx`.
//  * whole lines (the text ends with a line break; what copying marked lines makes): they are put ABOVE line `cy`; the caret stays on
//    its line (now lower), at the start.
//  * several lines that end in the middle of a line (copied from another program): the first part is added at the caret, the last part
//    goes in front of what was behind the caret; the caret ends behind the pasted text.
// At most `max_lines` lines in all (the rest is left out). Returns the number of lines that were added, or -1 when the text has only one
// line (the caller pastes that into the line itself).
inline int paste_lines(std::vector<std::string>& lines, int& cy, size_t& cx, const std::string& raw, size_t max_lines) {
    const std::string text = clean_text(raw);
    if (text.find('\n') == std::string::npos) return -1;
    std::vector<std::string> parts;
    size_t from = 0;
    for (;;) {
        const size_t nl = text.find('\n', from);
        if (nl == std::string::npos) { parts.push_back(text.substr(from)); break; }
        parts.push_back(text.substr(from, nl - from));
        from = nl + 1;
    }
    const bool whole = text.back() == '\n';
    if (whole) parts.pop_back();
    if (lines.empty()) lines.push_back("");
    cy = std::clamp(cy, 0, static_cast<int>(lines.size()) - 1);
    const size_t room = lines.size() >= max_lines ? 0 : max_lines - lines.size();
    if (whole) {
        if (parts.size() > room) parts.resize(room);
        lines.insert(lines.begin() + cy, parts.begin(), parts.end());
        cy += static_cast<int>(parts.size());
        cx = 0;
        return static_cast<int>(parts.size());
    }
    if (parts.size() - 1 > room) parts.resize(room + 1);
    std::string& cur = lines[static_cast<size_t>(cy)];
    cx = std::min(cx, cur.size());
    const std::string tail = cur.substr(cx);
    cur = cur.substr(0, cx) + parts.front();
    const size_t added = parts.size() - 1;
    std::vector<std::string> more(parts.begin() + 1, parts.end());
    const size_t last_len = more.back().size();
    more.back() += tail;
    lines.insert(lines.begin() + cy + 1, more.begin(), more.end());
    cy += static_cast<int>(added);
    cx = last_len;
    return static_cast<int>(added);
}

} // namespace muisc::lineblock
