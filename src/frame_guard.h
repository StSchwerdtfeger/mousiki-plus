#pragma once
// What may reach the terminal in a frame. Text in a frame comes from files and the web (file and folder names, tags, lyrics): a control character in it
// is acted on by the terminal, not drawn (OSC 52 writes the clipboard, OSC 0 the window title, CSI n / t make the terminal answer into the input).
// Everything a frame legitimately holds is kept: colours and cursor moves (CSI m H f A-G d J K X @ P L M h l ...), the Kitty pictures (ESC _ G ... ESC \)
// and the Sixel pictures (ESC P ... q ... ESC \), newlines. Anything else that is an escape or a control character is dropped.
#include <algorithm>
#include <string>
namespace muisc {
inline bool frame_csi_final_ok(char f) {
    switch (f) {
        case 'm': case 'H': case 'f': case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': case 'G': case 'd':
        case 'J': case 'K': case 'X': case '@': case 'P': case 'L': case 'M': case 'S': case 'T': case 'h': case 'l': case 'r':
            return true;
        default: return false;
    }
}
// A Kitty command whose control data (up to the ';') asks the terminal to read a file or shared memory (t=f / t=t / t=s) or to answer (a=q) is not ours:
// our pictures send their pixels in the command itself (t=d). `at` is the index of the ESC.
inline bool frame_kitty_ok(const std::string& in, size_t at) {
    size_t j = at + 3;
    const size_t end = std::min(in.size(), at + 3 + 400);   // (our control data is short)
    std::string key;
    bool in_val = false; char val_key = 0;
    for (; j < end; ++j) {
        const char c = in[j];
        if (c == ';' || c == '\x1b') return true;   // the payload (or the end): nothing asked for so far
        if (c == ',') { in_val = false; key.clear(); continue; }
        if (!in_val) {
            if (c == '=') { in_val = true; val_key = key.size() == 1 ? key[0] : 0; }
            else key += c;
        } else if ((val_key == 't' && (c == 'f' || c == 't' || c == 's')) || (val_key == 'a' && c == 'q')) return false;
    }
    return true;
}
// A mode switch in a frame: only the cursor (?25) and the synchronized update (?2026). Mouse tracking, the alternate screen, bracketed paste ... are the program's own, set outside the frames.
inline bool frame_mode_ok(const std::string& in, size_t from, size_t final_at) {
    const std::string p = in.substr(from, final_at - from);   // what is between "[" and the final byte
    return p == "?25" || p == "?2026";
}
inline std::string guard_frame(const std::string& in) {
    auto bad = [&](size_t i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        return (c < 0x20 && c != '\n' && c != '\r') || c == 0x7F || (c == 0xC2 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) >= 0x80 && static_cast<unsigned char>(in[i + 1]) <= 0x9F);
    };
    size_t first = 0;
    while (first < in.size() && in[first] != '\x1b' && !bad(first)) ++first;
    if (first == in.size()) return in;   // nothing special in it: the common case costs one pass
    std::string out(in, 0, first);
    out.reserve(in.size());
    for (size_t i = first; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0x1B) {
            const char n = i + 1 < in.size() ? in[i + 1] : '\0';
            if (n == '[') {   // CSI: parameters 0x30-0x3F, intermediates 0x20-0x2F, one final byte 0x40-0x7E
                size_t j = i + 2;
                while (j < in.size() && static_cast<unsigned char>(in[j]) >= 0x20 && static_cast<unsigned char>(in[j]) <= 0x3F) ++j;
                if (j < in.size() && static_cast<unsigned char>(in[j]) >= 0x40 && static_cast<unsigned char>(in[j]) <= 0x7E) {
                    const bool mode = in[j] == 'h' || in[j] == 'l';
                    if (frame_csi_final_ok(in[j]) && (!mode || frame_mode_ok(in, i + 2, j))) out.append(in, i, j - i + 1);
                    i = j;
                } else {
                    i = j > i + 1 ? j - 1 : i + 1;   // not a sequence: dropped up to where it stopped
                }
                continue;
            }
            if (n == '_' && i + 2 < in.size() && in[i + 2] == 'G') { if (frame_kitty_ok(in, i)) out.append("\x1b_G"); i += 2; continue; }   // Kitty graphics
            if (n == 'P') {   // Sixel: ESC P <digits;> q
                size_t j = i + 2;
                while (j < in.size() && ((in[j] >= '0' && in[j] <= '9') || in[j] == ';')) ++j;
                if (j < in.size() && in[j] == 'q') { out.append(in, i, j - i + 1); i = j; continue; }
                ++i; continue;   // some other device control string: its ESC P dropped
            }
            if (n == '\\') { out += "\x1b\\"; ++i; continue; }   // the end of a picture
            continue;   // any other escape: the ESC alone is dropped (what follows is then plain text)
        }
        if (bad(i)) { if (c == 0xC2) ++i; continue; }
        out += static_cast<char>(c);
    }
    return out;
}
}  // namespace muisc
