// "Sharp Corners": the round corner glyphs of the frames (╭ ╮ ╰ ╯) become the sharp ones (┌ ┐ └ ┘). Done on the finished frame, so every
// frame, overlay and preview is covered, also the corners that are written in the code. (The Settings screen is the other way round, see roundify_corners().) All four glyphs are 3 UTF-8 bytes long, before and
// after: the positions in the frame (where the main screen ends and the floating panels begin) stay valid.
#pragma once
#include <string>

namespace muisc {

inline void sharpen_corners(std::string& s) {
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (static_cast<unsigned char>(s[i]) != 0xE2 || static_cast<unsigned char>(s[i + 1]) != 0x95) continue;
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

}  // namespace muisc
