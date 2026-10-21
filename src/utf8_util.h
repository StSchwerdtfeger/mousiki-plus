#pragma once
#include <string>
#include <cstdint>
#include <algorithm>

namespace muisc {

// ---------------------------------------------------------------------------
// ASCII-only case folding.
//
// std::tolower/std::toupper are locale-dependent and defined over single
// bytes. Run over a UTF-8 string they are actively destructive: main() calls
// setlocale(LC_ALL, ""), so on a machine whose locale is a single-byte code
// page, a byte like 0xE4 -- which in UTF-8 is only ever the *lead* byte of a
// three-byte sequence (any CJK character, for instance) -- is classified as a
// letter and case-mapped to a different byte, corrupting the sequence. The
// result is a string that is no longer valid UTF-8, which then renders as
// partial text or replacement characters.
//
// Case-insensitive matching in this codebase only ever needs to fold ASCII
// (hotkey names, file extensions, latin search terms). These leave every byte
// >= 0x80 exactly as it was, so a UTF-8 string round-trips untouched.
inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

inline std::string ascii_lower_str(std::string s) {
    for (char& c : s) c = ascii_lower(c);
    return s;
}

inline std::string ascii_upper_str(std::string s) {
    for (char& c : s) c = ascii_upper(c);
    return s;
}

inline bool ascii_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline int utf8_seq_len(unsigned char lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

inline uint32_t utf8_decode(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int len = utf8_seq_len(c);
    len = static_cast<int>(std::min<size_t>(static_cast<size_t>(len), s.size() - i));
    uint32_t cp;
    if (len <= 1) {
        cp = c;
    } else {
        cp = c & (0xFF >> (len + 1));
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    i += static_cast<size_t>(len);
    return cp;
}

// ---------------------------------------------------------------------------
// Malformed UTF-8.
//
// utf8_decode() above trusts its input: a stray byte >= 0x80, a sequence cut
// short (a tag truncated in the middle of a character), a continuation byte
// that is not one, an overlong form or a UTF-16 surrogate all come out as SOME
// codepoint that the width table counts as one or two columns -- while the
// terminal drops the bytes or draws something else. The row is then measured
// wider than it is drawn and everything right of it, the border included,
// moves left (seen with a CJK title whose tag ended in broken bytes).
// utf8_repair() makes such text well-formed: every byte that is not part of a
// valid sequence becomes U+FFFD "\xEF\xBF\xBD" (one column, drawn by every
// terminal), so what is measured and what is drawn agree again.
// ---------------------------------------------------------------------------

// Length of the valid UTF-8 sequence starting at s[i], or 0 if the bytes there are not one.
inline size_t utf8_valid_seq_len(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    size_t len;
    uint32_t min_cp;
    if (c >= 0xC2 && c <= 0xDF) { len = 2; min_cp = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; min_cp = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; min_cp = 0x10000; }
    else return 0;   // a continuation byte where a character should start, 0xC0/0xC1 (always overlong), 0xF5..0xFF
    if (i + len > s.size()) return 0;   // cut short at the end of the text
    uint32_t cp = c & (0xFF >> (len + 1));
    for (size_t k = 1; k < len; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }
    if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;   // overlong, out of range, surrogate
    return len;
}

inline bool utf8_is_valid(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        const size_t len = utf8_valid_seq_len(s, i);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

// `s` with every byte that is not part of a valid UTF-8 sequence replaced by U+FFFD. Valid text comes back unchanged.
inline std::string utf8_repair(const std::string& s) {
    if (utf8_is_valid(s)) return s;
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        const size_t len = utf8_valid_seq_len(s, i);
        if (len == 0) { out += "\xEF\xBF\xBD"; ++i; }
        else { out.append(s, i, len); i += len; }
    }
    return out;
}

// For a tag value (title / artist / album / year) as read from a file or from ffprobe: utf8_repair(), and control
// characters -- which a terminal does not draw as a character but acts on (a carriage return jumps back to the start
// of the line, ESC starts an escape sequence) -- are removed; tabs and line breaks become one blank.
inline std::string clean_tag_text(const std::string& raw) {
    // A character cut short at the very END is a truncated tag (a fixed-size field, a writer that cut by bytes): the
    // partial character is dropped rather than shown as U+FFFD. Broken bytes anywhere else still become U+FFFD.
    std::string head = raw;
    for (size_t back = 1; back <= 3 && back <= head.size(); ++back) {
        const size_t p = head.size() - back;
        const unsigned char c = static_cast<unsigned char>(head[p]);
        if ((c & 0xC0) == 0x80) continue;                       // continuation byte: keep looking for the lead byte
        const int need = utf8_seq_len(c);
        if (c >= 0xC2 && c <= 0xF4 && static_cast<size_t>(need) > back) head.resize(p);   // lead byte without all its bytes
        break;
    }
    const std::string s = utf8_repair(head);
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\t' || c == '\n' || c == '\r') { if (out.empty() || out.back() != ' ') out += ' '; continue; }
        if (c < 0x20 || c == 0x7F) continue;
        // C1 controls U+0080..U+009F (UTF-8 "\xC2\x80".."\xC2\x9F"); U+009B is a CSI in some terminals
        if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) <= 0x9F) { ++i; continue; }
        out += static_cast<char>(c);
    }
    return out;
}

} // namespace muisc
