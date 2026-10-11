#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Black-and-white glyph bitmaps taken from a font that is installed on the
// computer, for the karaoke overlay's big letters. The built-in 5x7 pixel font
// (karaoke_font.h) only knows Latin letters; Japanese, Chinese, Korean (and
// Cyrillic, Greek ...) have far too many characters to ship as bitmaps, so
// their glyphs are rasterised on demand with stb_truetype instead and the
// result is drawn in braille cells exactly like the pixel font.
//
// The font is searched once, the first time a glyph is asked for:
//   1. the file named by the environment variable MOUSIKI_CJK_FONT, if set;
//   2. well-known system fonts (Windows: Yu Gothic / Meiryo / MS Gothic /
//      Microsoft YaHei / Malgun; macOS: Hiragino / PingFang / Apple SD Gothic;
//      Linux and others: Noto Sans CJK, Source Han Sans, WenQuanYi, Droid
//      Sans Fallback, IPA / Takao gothic, Unifont ... found by scanning the
//      font folders). Bold sans faces are preferred: thin strokes disappear
//      when a glyph is only 16 dots high.
// A character that the first font lacks (Hangul in a Japanese-only font, say)
// is looked up in up to two further fonts. Nothing is loaded for text the
// pixel font can draw itself.
// ---------------------------------------------------------------------------

namespace muisc {

struct RasterGlyph {
    int w = 0;                    // cell width in dots (the advance, never 0 for a real character)
    int h = 0;                    // cell height in dots (what was asked for)
    std::vector<uint8_t> dots;    // h * w, 1 = dot set
};

// True when some usable font was found (searches on the first call).
bool glyph_font_available();

// The file the primary font was loaded from ("" when there is none).
std::string glyph_font_path();

// Rasterises `cp` into a cell `h` dots high. False when no installed font has the character.
// Results are cached per (code point, h).
bool glyph_rasterize(char32_t cp, int h, RasterGlyph& out);

}  // namespace muisc
