// The Translit. / Latin preview of the Settings screens (player and radio): what the highlighted option changes, as examples with the result
// for each value (the value in use is lit) and a few examples that never change. One implementation, so both previews always show the same.
#pragma once
#include <string>
#include <vector>

namespace muisc {

struct TranslitPreviewIn {
    int kind = 2;                 // 0 Latin Macrons, 1 Latin Wo, 2 Translit. Kanji, 3 Latin Name Lookup (the player only)
    int macrons = 0, wo = 0, hanzi = 0;   // the settings as they are now (hanzi: 0 leave, 1 Chinese, 2 Japanese)
    bool lookup_on = false;
    int cw = 40;                  // the width of the preview box
    double t = 0.0;               // the clock (seconds): rows wider than the box scroll with it
    std::string lit, off;         // the escape codes that light a result / end it
    std::string reader_line;      // the one line about the optional kanji reader (differs between the player and the radio)
};

struct TranslitPreviewOut {
    std::vector<std::string> rows;   // ready for the box (wide rows scrolled)
    std::string caption;
};

TranslitPreviewOut translit_preview(const TranslitPreviewIn& in);

}  // namespace muisc
