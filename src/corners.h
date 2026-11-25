// "Sharp Corners": the round corner glyphs of the frames (╭ ╮ ╰ ╯) become the sharp ones (┌ ┐ └ ┘). Done on the finished frame, so every
// frame, overlay and preview is covered, also the corners that are written in the code. (The Settings screen is the other way round, see roundify_corners().) All four glyphs are 3 UTF-8 bytes long, before and
// after: the positions in the frame (where the main screen ends and the floating panels begin) stay valid.
#pragma once
#include <string>

namespace muisc {

// A corner glyph right behind this mark (a colour code that changes nothing) is left as it is by sharpen_corners() / roundify_corners(): the Settings previews
// of Sharp Corners show the corners the option gives, whatever the Settings screen itself is drawn with.
inline constexpr char kKeepCornerMark[] = "\x1b[22;22;22m";
inline bool corner_kept(const std::string& s, size_t i) {
    constexpr size_t n = sizeof(kKeepCornerMark) - 1;
    return i >= n && s.compare(i - n, n, kKeepCornerMark) == 0;
}

inline void sharpen_corners(std::string& s) {
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) != 0xE2 || static_cast<unsigned char>(s[i + 1]) != 0x95) continue;
        if (corner_kept(s, i)) { i += 2; continue; }
        switch (static_cast<unsigned char>(s[i + 2])) {
            case 0xAD: s[i + 1] = static_cast<char>(0x94); s[i + 2] = static_cast<char>(0x8C); break;   // ╭ -> ┌
            case 0xAE: s[i + 1] = static_cast<char>(0x94); s[i + 2] = static_cast<char>(0x90); break;   // ╮ -> ┐
            case 0xB0: s[i + 1] = static_cast<char>(0x94); s[i + 2] = static_cast<char>(0x94); break;   // ╰ -> └
            case 0xAF: s[i + 1] = static_cast<char>(0x94); s[i + 2] = static_cast<char>(0x98); break;   // ╯ -> ┘
            default: break;
        }
        i += 2;
    }
}

// ... and the other way round: the Settings screen is drawn with sharp corners; with Sharp Corners on it gets the round ones (┌ -> ╭ ...).
inline void roundify_corners(std::string& s) {
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) != 0xE2 || static_cast<unsigned char>(s[i + 1]) != 0x94) continue;
        if (corner_kept(s, i)) { i += 2; continue; }
        switch (static_cast<unsigned char>(s[i + 2])) {
            case 0x8C: s[i + 1] = static_cast<char>(0x95); s[i + 2] = static_cast<char>(0xAD); break;   // ┌ -> ╭
            case 0x90: s[i + 1] = static_cast<char>(0x95); s[i + 2] = static_cast<char>(0xAE); break;   // ┐ -> ╮
            case 0x94: s[i + 1] = static_cast<char>(0x95); s[i + 2] = static_cast<char>(0xB0); break;   // └ -> ╰
            case 0x98: s[i + 1] = static_cast<char>(0x95); s[i + 2] = static_cast<char>(0xAF); break;   // ┘ -> ╯
            default: break;
        }
        i += 2;
    }
}

// "Thick Borders" also sets the title of a pane in bold: the words at the start of a box title, up to the first " (", " [", " - ", ", ", two blanks, a quote, a
// colour code (the focus triangle) or a ": " in front of lower case text (what comes after is information or a song title and stays as it is).
inline std::string bold_title(const std::string& label) {
    size_t cut = label.size();
    auto upto = [&](size_t p) { if (p < cut) cut = p; };
    for (const char* sep : {" (", " [", " - ", ", ", "  ", " \"", " *"}) upto(label.find(sep));
    upto(label.find('\x1b'));
    const size_t colon = label.find(": ");
    if (colon != std::string::npos) {
        auto has_lower = [](const std::string& t) { for (char ch : t) if (ch >= 'a' && ch <= 'z') return true; return false; };
        if (!has_lower(label.substr(0, colon)) && has_lower(label.substr(colon + 2))) upto(colon + 1);   // ("MILKDROP FOR THIS TRACK: Song name")
    }
    while (cut > 0 && label[cut - 1] == ' ') --cut;   // (not the blank before the information)
    if (cut == 0) return label;
    return "\x1b[1m" + label.substr(0, cut) + "\x1b[22m" + label.substr(cut);
}

// "Thick Borders": the light line glyphs of the frames (─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼ and the wall stubs ╷ ╵) become heavy ones. Mode 1: the heavy glyphs of
// Unicode (━ ┃ ┏ ┓ ┗ ┛ ┣ ┫ ┳ ┻ ╋ ╻ ╹); there is no heavy round corner, so the round corners ╭ ╮ ╰ ╯ stay thin and Sharp Corners gives heavy sharp ones.
// Mode 2: every one of them, the round corners too, is a private-use code point U+E1C0 ... U+E1D0 of the small font "Mousiki Corners" (packaging/fonts,
// tools/make_corner_font.py) that draws the whole thick round frame itself (a terminal's own box lines never joined well with a font's corners). The code
// points are 3 UTF-8 bytes long like the glyphs they replace; a terminal that cannot find the font shows gaps, so the caller uses mode 2 only when the
// font is in place. Done on the finished frame like the corners above, so every frame, overlay and preview is covered.
inline void thicken_borders(std::string& s, int mode = 1) {
    // index in the font (U+E1C0 + i): 0-3 round ╭ ╮ ╰ ╯, 4-7 sharp ┌ ┐ └ ┘, 8 ─, 9 │, 10-13 ├ ┤ ┬ ┴, 14 ┼, 15 ╷, 16 ╵
    auto pua = [&s](size_t i, int idx) {
        s[i] = static_cast<char>(0xEE);
        s[i + 1] = static_cast<char>(0x87);
        s[i + 2] = static_cast<char>(0x80 + idx);
    };
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) != 0xE2) continue;
        const unsigned char b1 = static_cast<unsigned char>(s[i + 1]), b2 = static_cast<unsigned char>(s[i + 2]);
        if (b1 == 0x94) {
            struct Map { unsigned char from, heavy; int idx; bool two_byte_heavy; };
            static const Map k[] = {
                {0x80, 0x81, 8, false}, {0x82, 0x83, 9, false}, {0x8C, 0x8F, 4, false}, {0x90, 0x93, 5, false}, {0x94, 0x97, 6, false}, {0x98, 0x9B, 7, false},
                {0x9C, 0xA3, 10, false}, {0xA4, 0xAB, 11, false}, {0xAC, 0xB3, 12, false}, {0xB4, 0xBB, 13, false}, {0xBC, 0x8B, 14, true},   // ┼ -> ╋ is U+254B
            };
            for (const Map& e : k) {
                if (e.from != b2) continue;
                if (mode >= 2) pua(i, e.idx);
                else if (e.two_byte_heavy) { s[i + 1] = static_cast<char>(0x95); s[i + 2] = static_cast<char>(e.heavy); }
                else s[i + 2] = static_cast<char>(e.heavy);
                break;
            }
            i += 2;
        } else if (b1 == 0x95) {
            switch (b2) {
                case 0xAD: if (mode >= 2) pua(i, 0); break;   // ╭
                case 0xAE: if (mode >= 2) pua(i, 1); break;   // ╮
                case 0xB0: if (mode >= 2) pua(i, 2); break;   // ╰
                case 0xAF: if (mode >= 2) pua(i, 3); break;   // ╯
                case 0xB7: if (mode >= 2) pua(i, 15); else s[i + 2] = static_cast<char>(0xBB); break;   // ╷ -> ╻
                case 0xB5: if (mode >= 2) pua(i, 16); else s[i + 2] = static_cast<char>(0xB9); break;   // ╵ -> ╹
                default: break;
            }
            i += 2;
        }
    }
}

}  // namespace muisc
