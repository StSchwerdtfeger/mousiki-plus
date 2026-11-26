#pragma once
// What may reach the terminal in a frame. Text in a frame comes from files and the web (file and folder names, tags, lyrics): a control character in it
// is acted on by the terminal, not drawn (OSC 52 writes the clipboard, OSC 0 the window title, CSI n / t make the terminal answer into the input).
// Everything a frame legitimately holds is kept: colours and cursor moves (CSI m H f A-G d J K X @ P L M h l ...), the Kitty pictures (ESC _ G ... ESC \)
// and the Sixel pictures (ESC P ... q ... ESC \), newlines. Anything else that is an escape or a control character is dropped.
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
                    if (frame_csi_final_ok(in[j])) out.append(in, i, j - i + 1);
                    i = j;
                } else {
                    i = j > i + 1 ? j - 1 : i + 1;   // not a sequence: dropped up to where it stopped
                }
                continue;
            }
            if (n == '_' && i + 2 < in.size() && in[i + 2] == 'G') { out.append("\x1b_G"); i += 2; continue; }   // Kitty graphics
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
