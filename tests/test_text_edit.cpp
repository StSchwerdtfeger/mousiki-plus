// Single-line text editing and the typo-tolerant search score (src/text_edit.h), shared by the player and the radio.
#include "test_framework.h"
#include "text_edit.h"

using namespace muisc;

namespace {
struct Field {
    std::string s;
    size_t caret = 0, anchor = 0;
    bool key(int k, size_t limit = 100) { return edit_text_key(s, caret, anchor, k, limit, nullptr); }
    void type(const std::string& text, size_t limit = 100) { for (unsigned char c : text) key(c, limit); }
};
}

TEST(text_edit_typing_and_backspace_utf8) {
    Field f;
    f.type("ab");
    CHECK_EQ(f.s, std::string("ab"));
    f.type("\xC3\xA4");                    // a-umlaut: two bytes, typed one byte at a time
    CHECK_EQ(f.s, std::string("ab\xC3\xA4"));
    CHECK(f.key(127));                      // one backspace removes the whole character, not half of it
    CHECK_EQ(f.s, std::string("ab"));
    CHECK_EQ(f.caret, static_cast<size_t>(2));
}

TEST(text_edit_home_end_insert_in_the_middle) {
    Field f;
    f.type("world");
    f.key(kKeyHome);
    CHECK_EQ(f.caret, static_cast<size_t>(0));
    f.type("hello ");
    CHECK_EQ(f.s, std::string("hello world"));
    f.key(kKeyEnd);
    CHECK_EQ(f.caret, f.s.size());
}

TEST(text_edit_selection_and_delete) {
    Field f;
    f.type("abcdef");
    f.key(kKeyShiftLeft);
    f.key(kKeyShiftLeft);                  // "ef" marked
    CHECK(f.caret != f.anchor);
    CHECK(f.key(kKeyDelete));              // deleting a selection removes exactly it
    CHECK_EQ(f.s, std::string("abcd"));
    CHECK_EQ(f.caret, f.anchor);
}

TEST(text_edit_limit_is_enforced) {
    Field f;
    f.type("abcdefgh", 5);
    CHECK_EQ(f.s, std::string("abcde"));
}

TEST(text_edit_paint_fits_the_width) {
    for (int w : {1, 3, 8, 20}) {
        const EditPaint p = paint_edit_field("the quick brown fox jumps", 25, 25, w, "", true);
        CHECK(p.cols <= w);                // never wider than the field, whatever the text
        CHECK(p.caret >= 0 && p.caret <= w - 1);
    }
    const EditPaint sel = paint_edit_field("hello", 5, 0, 20, "\x1b[0m", false);
    CHECK(sel.s.find("\x1b[7m") != std::string::npos);   // the marked text is in reverse video
}

TEST(text_edit_block_caret_on_a_wide_character_keeps_the_width) {
    // The caret on a double-width character (Japanese ...) must not make the painted row narrower than it is counted.
    const std::string s = "ab \xE3\x83\x91\xE3\x83\xBC\xE3\x83\xA9";   // "ab パーラ"
    for (size_t caret : {size_t(0), size_t(3), size_t(6), size_t(9)}) {
        const EditPaint p = paint_edit_field(s, caret, caret, 20, "", true);
        CHECK_EQ(display_width(p.s), p.cols);
    }
}

TEST(text_edit_combining_mark_stays_with_its_character) {
    // は + U+3099 (a decomposed ば): one character for the caret, the block cursor and backspace.
    const std::string s = "a\xE3\x81\xAF\xE3\x82\x99" "b";
    CHECK_EQ(utf8_cuts(s).size(), static_cast<size_t>(4));   // 0, after a, after the pair, after b
    for (size_t caret : {size_t(1), size_t(7)}) {
        const EditPaint p = paint_edit_field(s, caret, caret, 20, "", true);
        CHECK_EQ(display_width(p.s), p.cols);
    }
    Field f;
    f.s = s;
    f.caret = f.anchor = 8;
    f.key(127);                                              // backspace removes the 'b'
    f.key(127);                                              // ... then the whole pair, not just the mark
    CHECK_EQ(f.s, std::string("a"));
}

TEST(fuzzy_exact_beats_typo) {
    const double exact = fuzzy_score("another love", "Tom Odell - Another Love");
    const double typo = fuzzy_score("anogher lobe", "Tom Odell - Another Love");
    CHECK(exact >= 100.0);
    CHECK(typo > 0.0 && typo < 100.0);
    CHECK(exact > typo);
}

TEST(fuzzy_no_match_and_empty_query) {
    CHECK(fuzzy_score("zzzzqq", "Another Love") < 0.0);
    CHECK_EQ(fuzzy_score("", "anything"), 0.0);
}

TEST(fuzzy_separators_fold_to_spaces) {
    CHECK(fuzzy_score("x files", "X-Files") >= 900.0);          // "X-Files" is found by "x files"
    CHECK(fuzzy_score("a b", "a_b") >= 900.0);
}

TEST(fuzzy_earlier_position_ranks_higher) {
    CHECK(fuzzy_score("love", "love me do") > fuzzy_score("love", "all you need is love"));
}
