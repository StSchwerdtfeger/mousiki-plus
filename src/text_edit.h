#pragma once
// Shared single-line text editing (caret, Shift+arrow marking, Home/End, Delete, Ctrl+C/X/V, painting a field) and the
// typo-tolerant search score. Used by the music player (src/app.cpp) AND the radio (src_radio/): one copy for both.
#include <algorithm>
#include <string>
#include <vector>
#include "terminal_ui.h"
#include "utf8_util.h"

namespace muisc {

// Every text-entry field below (search box, bulk-add link field, color
// hex field, the retry-lyrics title/artist/type fields) used to only
// accept key values 32-126 -- plain printable ASCII. That's not just a
// Windows gap: a typed umlaut, accented letter, or any other non-ASCII
// character arrives as a multi-byte UTF-8 sequence whose individual byte
// values are all >= 0x80 (continuation bytes 0x80-0xBF, lead bytes
// 0xC2-0xF4), every one of which used to fail this check and simply
// never reach the buffer -- on POSIX with a real UTF-8 terminal just as
// much as on Windows. win_poll_key() (Windows) and the POSIX raw-input
// path both hand such a keystroke over one UTF-8 byte per call already;
// this is what actually lets any of those bytes through.
inline bool is_text_key(int key) {
    return (key >= 32 && key < 127) || (key >= 0x80 && key <= 0xFF);
}

// ---------------------------------------------------------------------------
// Single-line text editing: caret, selection, and how a field is painted
//
// A text field in this app is a plain std::string plus two BYTE offsets (see
// edit_caret_/edit_anchor_ in app.h): the caret, and the other end of the
// selection -- equal to the caret when nothing is marked, which makes
// "caret != anchor" the selection test itself. Offsets are in bytes because
// that is what the buffers hold, but every movement and every deletion is
// quantised to the codepoint boundaries below, so a multi-byte character is
// never split in half.
//
// Two editors share this code and are never active at the same time:
// Mode::ColorEdit (the Settings' in-place value editing) and the meta
// editor's field editor. Both own their offsets and pass them in.
// ---------------------------------------------------------------------------

// Every byte offset at which `s` may be split without breaking UTF-8: 0,
// then the start of each codepoint, then s.size() itself.
inline std::vector<size_t> utf8_cuts(const std::string& s) {
    std::vector<size_t> cuts;
    cuts.reserve(s.size() + 1);
    cuts.push_back(0);
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 1;
        if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        i += n;
        if (i > s.size()) i = s.size(); // truncated lead byte: treat the tail as one char
        // Combining marks (a voiced-sound mark after Japanese は = ば, accents, variation selectors, ZWJ ...) have no width
        // of their own: they stay with the character before them, so the caret, the block cursor and DEL treat the pair
        // as one character instead of splitting it.
        while (i < s.size()) {
            const unsigned char d = static_cast<unsigned char>(s[i]);
            size_t m = 1;
            if ((d & 0xE0) == 0xC0) m = 2;
            else if ((d & 0xF0) == 0xE0) m = 3;
            else if ((d & 0xF8) == 0xF0) m = 4;
            if (d < 0x80 || i + m > s.size() || display_width(s.substr(i, m)) != 0) break;
            i += m;
        }
        cuts.push_back(i);
    }
    return cuts;
}

inline size_t utf8_prev_cut(const std::string& s, size_t off) {
    if (off == 0) return 0;
    size_t best = 0;
    for (size_t c : utf8_cuts(s)) {
        if (c >= off) break;
        best = c;
    }
    return best;
}

inline size_t utf8_next_cut(const std::string& s, size_t off) {
    if (off >= s.size()) return s.size();
    for (size_t c : utf8_cuts(s)) if (c > off) return c;
    return s.size();
}

inline void le_clamp(const std::string& s, size_t& caret, size_t& anchor) {
    if (caret > s.size()) caret = s.size();
    if (anchor > s.size()) anchor = s.size();
}

inline void le_range(size_t caret, size_t anchor, size_t& a, size_t& b) {
    a = std::min(caret, anchor);
    b = std::max(caret, anchor);
}

inline bool le_has_sel(size_t caret, size_t anchor) { return caret != anchor; }

inline void le_erase_selection(std::string& s, size_t& caret, size_t& anchor) {
    size_t a = 0, b = 0;
    le_range(caret, anchor, a, b);
    if (a == b) return;
    s.erase(a, b - a);
    caret = anchor = a;
}

// dir: -1 left, +1 right. shift keeps anchor where it was (marking), which
// is exactly the difference between Shift+Left and Left.
inline void le_move(std::string& s, size_t& caret, size_t& anchor, int dir, bool shift) {
    le_clamp(s, caret, anchor);
    caret = (dir < 0) ? utf8_prev_cut(s, caret) : utf8_next_cut(s, caret);
    if (!shift) anchor = caret;
}

inline void le_backspace(std::string& s, size_t& caret, size_t& anchor) {
    le_clamp(s, caret, anchor);
    if (le_has_sel(caret, anchor)) { le_erase_selection(s, caret, anchor); return; }
    size_t p = utf8_prev_cut(s, caret);
    s.erase(p, caret - p);
    caret = anchor = p;
}

inline void le_delete_forward(std::string& s, size_t& caret, size_t& anchor) {
    le_clamp(s, caret, anchor);
    if (le_has_sel(caret, anchor)) { le_erase_selection(s, caret, anchor); return; }
    size_t n = utf8_next_cut(s, caret);
    s.erase(caret, n - caret);
    anchor = caret;
}

// `limit` is the field's maximum byte length -- the same cap the old
// "append at the end" code enforced with its inline size() checks.
inline void le_insert(std::string& s, size_t& caret, size_t& anchor, char c, size_t limit) {
    le_clamp(s, caret, anchor);
    le_erase_selection(s, caret, anchor);
    if (s.size() >= limit) return;
    s.insert(caret, 1, c);
    caret = anchor = caret + 1;
}

// Paste. Clipboard text can be multi-line and arbitrary length: control
// characters are dropped (a newline inside a single-line row would corrupt
// the layout) and whatever still doesn't fit is trimmed, codepoint-aligned,
// against the same limit.
inline void le_paste(std::string& s, size_t& caret, size_t& anchor, const std::string& text, size_t limit) {
    le_clamp(s, caret, anchor);
    le_erase_selection(s, caret, anchor);
    std::string t;
    t.reserve(text.size());
    for (unsigned char c : text) {
        if (c >= 32 && c != 127) t.push_back(static_cast<char>(c));
    }
    if (t.empty()) return;
    const size_t room = (s.size() >= limit) ? 0 : limit - s.size();
    while (t.size() > room && !t.empty()) t.erase(utf8_prev_cut(t, t.size()), std::string::npos);
    if (t.empty()) return;
    s.insert(caret, t);
    caret = anchor = caret + t.size();
}

// One keystroke, every single-line text field in the app.
//
// Caret movement, Shift+arrows marking, Home/End, Ctrl+C/X/V and typing --
// the same key set the Settings' ColorEdit and the meta editor's field editor
// grew first, factored out here so the four search/filter boxes (the main
// UI's "/", the meta editor's Search line, the playlist editor's name and
// library boxes and the playlist list's own search) can offer exactly the
// same thing instead of a fifth hand-rolled copy.
//
// Returns true only when the TEXT really changed, which is what tells the
// caller to re-run whatever filter the box drives; pure caret/clipboard keys
// return false. `status` (nullable) receives the short feedback these keys
// produce ("COPIED"/"CUT"/"PASTED"), because a box with no status line of
// its own should not go silent on a copy.
inline bool edit_text_key(std::string& buf, size_t& caret, size_t& anchor, int key,
                          size_t limit, std::string* status) {
    auto say = [&](const char* s) { if (status) *status = s; };
    // Modifier combinations arrive as their own sentinel values
    // (terminal_ui.h) precisely because the bare ones are already taken: an
    // arrow press collapses to the letter it would otherwise type, and a
    // typed 'c' has to stay a typed 'c' inside a word.
    if (key == kKeyHome) { caret = anchor = 0; return false; }
    if (key == kKeyEnd) { caret = anchor = buf.size(); return false; }
    if (key == kKeyShiftLeft) { le_move(buf, caret, anchor, -1, true); return false; }
    if (key == kKeyShiftRight) { le_move(buf, caret, anchor, +1, true); return false; }
    if (key == kKeyDelete) { le_delete_forward(buf, caret, anchor); return true; }
    if (key == kKeyCtrlC) {
        size_t a = 0, b = 0;
        le_range(caret, anchor, a, b);
        std::string t = (a == b) ? buf : buf.substr(a, b - a);
        clipboard_set(t);
        say(t.empty() ? "nothing selected" : "COPIED");
        return false;
    }
    if (key == kKeyCtrlX) {
        size_t a = 0, b = 0;
        le_range(caret, anchor, a, b);
        if (a == b) return false; // no selection: cut must never empty the field
        clipboard_set(buf.substr(a, b - a));
        le_erase_selection(buf, caret, anchor);
        say("CUT");
        return true;
    }
    if (key == kKeyCtrlV) { le_paste(buf, caret, anchor, clipboard_get(), limit); say("PASTED"); return true; }
    if (key == 127 || key == 8) { le_backspace(buf, caret, anchor); return true; }
    // A genuinely typed A-D goes into the buffer ("C:\" has no way past this
    // check otherwise), while an arrow press moves the caret Left/Right -- and
    // Up/Down is dropped here: every field this is used on is a single line,
    // and the boxes that DO want Up/Down for the list below them claim those
    // keys before calling in.
    if (last_key_was_arrow() && (key == 'A' || key == 'B')) return false;
    if (last_key_was_arrow() && (key == 'C' || key == 'D')) {
        le_move(buf, caret, anchor, key == 'D' ? -1 : +1, false);
        return false;
    }
    if (is_text_key(key)) { le_insert(buf, caret, anchor, static_cast<char>(key), limit); return true; }
    return false;
}

// (App::edit_focus() lives below, outside this anonymous namespace.)

// What one field looks like on screen after all of that.
struct EditPaint {
    std::string s;  // the windowed text, selection wrapped in reverse video
    int cols = 0;   // display columns s really occupies (ANSI escapes not counted)
    int caret = 0;  // 0-based column of the caret inside that window
};

// Paints a field into `w` columns: a window scrolled so the caret stays
// visible (the buffer can be far longer than the field), the selected range
// in reverse video, and -- with `block` -- the caret itself drawn as a █,
// which is how the meta editor shows a cursor. The Settings tabs instead move
// the REAL terminal cursor to `caret` (they already do that today) and pass
// block = false.
//
// `restore` is re-emitted after the selection so the field keeps its own
// colour; it must be exactly the SGR the caller has already put in effect,
// because display_width() cannot see through ANSI escapes -- colouring first
// and measuring afterwards would miscount every row.
inline EditPaint paint_edit_field(const std::string& s, size_t caret, size_t anchor, int w,
                                  const std::string& restore, bool block) {
    EditPaint out;
    if (w < 1) w = 1;
    le_clamp(s, caret, anchor);
    if (anchor > caret) std::swap(anchor, caret);

    const std::vector<size_t> cuts = utf8_cuts(s);
    std::vector<int> col(cuts.size(), 0); // col[i] = display column of cuts[i]
    for (size_t i = 1; i < cuts.size(); ++i) {
        col[i] = col[i - 1] + display_width(s.substr(cuts[i - 1], cuts[i] - cuts[i - 1]));
    }
    auto idx_of = [&](size_t off) -> size_t {
        for (size_t i = 0; i < cuts.size(); ++i) if (cuts[i] == off) return i;
        return cuts.size() - 1;
    };
    const size_t caret_i = idx_of(caret);
    const size_t anchor_i = idx_of(anchor);

    // The leftmost window that still keeps the caret inside it: starting any
    // further left would push the caret past the right edge, starting further
    // right would hide text the caret is sitting on.
    const int caret_col = col[caret_i];
    const int target = caret_col - (w - 1);
    size_t lo_i = 0;
    for (size_t i = 0; i <= caret_i; ++i) {
        if (col[i] >= target) { lo_i = i; break; }
        lo_i = caret_i;
    }
    size_t hi_i = lo_i;
    while (hi_i + 1 < cuts.size() && col[hi_i + 1] - col[lo_i] <= w) ++hi_i;
    if (hi_i < caret_i) hi_i = caret_i;

    out.caret = caret_col - col[lo_i];
    if (out.caret < 0) out.caret = 0;
    if (out.caret > w - 1) out.caret = w - 1;

    auto emit = [&](size_t from, size_t to, bool sel, const char* glyph) {
        std::string body = glyph ? std::string(glyph) : s.substr(from, to - from);
        if (body.empty()) return;
        if (sel) out.s += "\x1b[7m" + body + "\x1b[0m" + restore;
        else out.s += body;
    };
    for (size_t i = lo_i; i < hi_i; ++i) {
        const bool sel = (i >= anchor_i && i < caret_i);
        if (block && i == caret_i) {
            // The block covers as many columns as the character it replaces: a double-width one (Japanese, Chinese,
            // Korean, emoji) drawn as a single block made the row one column short and pushed the right border in.
            const int cw = std::max(1, col[i + 1] - col[i]);
            std::string blk;
            for (int k = 0; k < cw; ++k) blk += "\u2588";
            emit(cuts[i], cuts[i + 1], sel, blk.c_str());
        }
        else emit(cuts[i], cuts[i + 1], sel, nullptr);
    }
    // Caret at (or beyond) the end of the window: there is no character to
    // replace, so the block is appended -- which only fits because the window
    // above reserves the column (col[caret_i] - col[lo_i] <= w-1).
    if (block && caret_i >= hi_i) emit(0, 0, false, "\u2588");

    out.cols = col[hi_i] - col[lo_i];
    if (block && caret_i >= hi_i) out.cols += 1;
    return out;
}


inline std::string lower(std::string s) {
    return ascii_lower_str(std::move(s));
}

// Lowercases AND folds common word-separator punctuation (hyphen,
// underscore, dot, slash) down to plain spaces. Used only for search
// matching (fuzzy_score below), never for display.
//
// Without this, a tag/filename like "X-Files" is one glued-together
// token "x-files" as far as matching is concerned, while a query typed
// as "X Files" is two separate words ["x", "files"]. Tier 1 (exact
// substring) fails because "x-files" never contains the literal text
// "x files". Tier 2 (per-word fuzzy) also fails: comparing whole word
// "x" against whole word "x-files" gives a huge edit distance (adding
// "-files"), well below the 0.55 quality floor -- even though every
// individual word the user typed is actually present. Folding the
// separator to a space first turns "x-files" into "x files" so both
// tiers line up with "X Files" the same way they already would for a
// title that happens to use a plain space.
inline std::string normalize_for_search(std::string s) {
    s = lower(std::move(s));
    for (char& c : s) {
        if (c == '-' || c == '_' || c == '.' || c == '/') c = ' ';
    }
    return s;
}

inline std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) words.push_back(cur);
    return words;
}

// Standard edit distance (single-char insert/delete/substitute cost 1).
// Small strings only (track titles/artists/search words) — the O(n*m)
// DP table is negligible at this scale.
inline int levenshtein(const std::string& a, const std::string& b) {
    size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 0; i <= n; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= m; ++j) dp[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1, dp[i][j - 1] + 1, dp[i - 1][j - 1] + cost});
        }
    }
    return dp[n][m];
}

// Typo-tolerant match score: higher is better, negative means "not a
// match at all" (excluded from results). An exact literal substring
// match always outranks every fuzzy match, regardless of how good the
// fuzzy quality is — so "another love" typed exactly always sits above
// a merely-close fuzzy hit. Within the fuzzy tier, each query word must
// find a reasonably close word somewhere in the target (a query word
// that's wildly different from everything in the target means this
// isn't really a match, even if some OTHER query word happens to fit)
// — this is what lets "anogher lobe" (typo'd "another love") still find
// the track: word-level edit distance tolerates the substituted
// characters that a plain substring or subsequence check would miss
// entirely (neither "anogher" nor "lobe" appears anywhere in "another
// love" as literal text).
inline double fuzzy_score(const std::string& query, const std::string& target) {
    std::string q = normalize_for_search(query), t = normalize_for_search(target);
    if (q.empty()) return 0.0;

    size_t pos = t.find(q);
    if (pos != std::string::npos) {
        return 1000.0 - std::min<double>(static_cast<double>(pos), 900.0); // tier 1: exact substring, earlier position ranks higher
    }

    auto q_words = split_words(q);
    auto t_words = split_words(t);
    if (q_words.empty() || t_words.empty()) return -1.0;

    double total_quality = 0.0;
    for (auto& qw : q_words) {
        double best_quality = -1.0;
        for (auto& tw : t_words) {
            size_t max_len = std::max(qw.size(), tw.size());
            if (max_len == 0) continue;
            double normalized = static_cast<double>(levenshtein(qw, tw)) / static_cast<double>(max_len);
            double quality = 1.0 - normalized;
            if (quality > best_quality) best_quality = quality;
        }
        if (best_quality < 0.55) return -1.0; // this query word doesn't fit anywhere close enough -- not a real match
        total_quality += best_quality;
    }
    return (total_quality / static_cast<double>(q_words.size())) * 100.0; // tier 2: fuzzy, always below tier 1's range
}


} // namespace muisc
