#include "app.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "keyboard_layout.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "console_log.h"
#include "karaoke_font.h"
#include "glyph_raster.h"
#include "chiptune.h"
#include "scope_big.h"
#include "scope_window.h"
#include "wizard.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <sys/types.h>
#include <sys/stat.h>   // file_added_time(): stat() / _wstat64()
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace muisc {

namespace {

// ---------------------------------------------------------------------
// Worker-thread exception guard
//
// An exception that escapes a std::thread's function does NOT propagate to
// the thread that spawned it and does not unwind anywhere useful: the
// standard says it calls std::terminate(), which kills the entire process on
// the spot. On Windows that means the app vanishes with no message at all --
// no stack trace, no console output, nothing in the log. Every background
// task in this file (metadata sweep, decode, lyrics fetch, waveform pass,
// search, bulk queue add) touches the filesystem or spawns subprocesses, and
// any of those can throw.
//
// This wrapper is the process-level safety net: whatever a worker throws is
// turned into a log line, and that task alone fails. The encoding bugs fixed
// alongside it were the cause we know about; this makes sure the next one
// (a disappearing USB drive, a permissions change mid-scan) degrades instead
// of detonating.
template <class F>
void run_guarded(const char* what, F&& body) noexcept {
    try {
        body();
    } catch (const std::exception& e) {
        ConsoleLog::instance().log_basic(std::string("internal error in ") + what + ": " + e.what());
    } catch (...) {
        ConsoleLog::instance().log_basic(std::string("internal error in ") + what + " (unknown exception)");
    }
}

// ASCII-only, deliberately: this folds track titles and artist names, which
// are UTF-8. std::tolower over raw bytes mangles multi-byte sequences under
// any single-byte locale -- see ascii_lower() in utf8_util.h.
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
bool is_text_key(int key) {
    return (key >= 32 && key < 127) || (key >= 0x80 && key <= 0xFF);
}

// Removes exactly one full UTF-8 codepoint from the end of a text-entry
// buffer, not just its last byte. A plain pop_back() left a dangling lead
// byte behind for any accented/non-ASCII character (a German umlaut is
// two bytes; CJK is three) -- that lone byte decodes as a replacement
// glyph, and needed a second, confusing Backspace press to actually
// finish clearing what looked like one character.
void pop_utf8_char(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.erase(i);
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
static std::vector<size_t> utf8_cuts(const std::string& s) {
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
        cuts.push_back(i);
    }
    return cuts;
}

static size_t utf8_prev_cut(const std::string& s, size_t off) {
    if (off == 0) return 0;
    size_t best = 0;
    for (size_t c : utf8_cuts(s)) {
        if (c >= off) break;
        best = c;
    }
    return best;
}

static size_t utf8_next_cut(const std::string& s, size_t off) {
    if (off >= s.size()) return s.size();
    for (size_t c : utf8_cuts(s)) if (c > off) return c;
    return s.size();
}

static void le_clamp(const std::string& s, size_t& caret, size_t& anchor) {
    if (caret > s.size()) caret = s.size();
    if (anchor > s.size()) anchor = s.size();
}

static void le_range(size_t caret, size_t anchor, size_t& a, size_t& b) {
    a = std::min(caret, anchor);
    b = std::max(caret, anchor);
}

static bool le_has_sel(size_t caret, size_t anchor) { return caret != anchor; }

static void le_erase_selection(std::string& s, size_t& caret, size_t& anchor) {
    size_t a = 0, b = 0;
    le_range(caret, anchor, a, b);
    if (a == b) return;
    s.erase(a, b - a);
    caret = anchor = a;
}

// dir: -1 left, +1 right. shift keeps anchor where it was (marking), which
// is exactly the difference between Shift+Left and Left.
static void le_move(std::string& s, size_t& caret, size_t& anchor, int dir, bool shift) {
    le_clamp(s, caret, anchor);
    caret = (dir < 0) ? utf8_prev_cut(s, caret) : utf8_next_cut(s, caret);
    if (!shift) anchor = caret;
}

static void le_backspace(std::string& s, size_t& caret, size_t& anchor) {
    le_clamp(s, caret, anchor);
    if (le_has_sel(caret, anchor)) { le_erase_selection(s, caret, anchor); return; }
    size_t p = utf8_prev_cut(s, caret);
    s.erase(p, caret - p);
    caret = anchor = p;
}

static void le_delete_forward(std::string& s, size_t& caret, size_t& anchor) {
    le_clamp(s, caret, anchor);
    if (le_has_sel(caret, anchor)) { le_erase_selection(s, caret, anchor); return; }
    size_t n = utf8_next_cut(s, caret);
    s.erase(caret, n - caret);
    anchor = caret;
}

// `limit` is the field's maximum byte length -- the same cap the old
// "append at the end" code enforced with its inline size() checks.
static void le_insert(std::string& s, size_t& caret, size_t& anchor, char c, size_t limit) {
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
static void le_paste(std::string& s, size_t& caret, size_t& anchor, const std::string& text, size_t limit) {
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
static bool edit_text_key(std::string& buf, size_t& caret, size_t& anchor, int key,
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
static EditPaint paint_edit_field(const std::string& s, size_t caret, size_t anchor, int w,
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
        if (block && i == caret_i) emit(cuts[i], cuts[i + 1], sel, "\u2588");
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

std::string lower(std::string s) {
    return ascii_lower_str(std::move(s));
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
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
std::string normalize_for_search(std::string s) {
    s = lower(std::move(s));
    for (char& c : s) {
        if (c == '-' || c == '_' || c == '.' || c == '/') c = ' ';
    }
    return s;
}

std::vector<std::string> split_words(const std::string& s) {
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
int levenshtein(const std::string& a, const std::string& b) {
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
double fuzzy_score(const std::string& query, const std::string& target) {
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

// ---- the same matching as fuzzy_score(), for a library of tens of thousands of tracks ------------------------------------
// The query is normalised and split once, the targets are normalised once per scan (App::search_idx_), and the edit
// distance stops as soon as a word cannot reach the 0.55 quality floor any more. The results are identical.
struct PreparedQuery {
    std::string q;
    std::vector<std::string> words;
    std::vector<uint64_t> masks;   // per word: which characters it contains (hashed to 64 bits)
};
inline int popcount64(uint64_t x) {   // portable (no compiler intrinsic)
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<int>((x * 0x0101010101010101ULL) >> 56);
}
inline uint64_t char_bit(char c) { return uint64_t{1} << (static_cast<unsigned char>(c) & 63); }
PreparedQuery prepare_query(const std::string& query) {
    PreparedQuery pq;
    pq.q = normalize_for_search(query);
    pq.words = split_words(pq.q);
    for (const auto& w : pq.words) {
        uint64_t m = 0;
        for (char c : w) m |= char_bit(c);
        pq.masks.push_back(m);
    }
    return pq;
}
// Edit distance that gives up above `cutoff`: returns cutoff + 1 then (the exact value is never needed above it).
int levenshtein_capped(std::string_view a, std::string_view b, int cutoff) {
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    if (std::abs(n - m) > cutoff) return cutoff + 1;
    thread_local std::vector<int> prev, cur;
    prev.assign(static_cast<size_t>(m) + 1, 0);
    cur.assign(static_cast<size_t>(m) + 1, 0);
    for (int j = 0; j <= m; ++j) prev[static_cast<size_t>(j)] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int row_min = cur[0];
        for (int j = 1; j <= m; ++j) {
            const int cost = a[static_cast<size_t>(i) - 1] == b[static_cast<size_t>(j) - 1] ? 0 : 1;
            const int v = std::min({prev[static_cast<size_t>(j)] + 1, cur[static_cast<size_t>(j) - 1] + 1, prev[static_cast<size_t>(j) - 1] + cost});
            cur[static_cast<size_t>(j)] = v;
            if (v < row_min) row_min = v;
        }
        if (row_min > cutoff) return cutoff + 1;
        std::swap(prev, cur);
    }
    return prev[static_cast<size_t>(m)];
}
// Tier 1 of fuzzy_score(): exact substring, earlier position ranks higher. -1 = no substring.
double substring_score(const PreparedQuery& pq, const std::string& t_norm) {
    if (pq.q.empty()) return 0.0;
    const size_t pos = t_norm.find(pq.q);
    return pos == std::string::npos ? -1.0 : 1000.0 - std::min<double>(static_cast<double>(pos), 900.0);
}
// Tier 2 of fuzzy_score(): every query word must find a close word in the target. -1 = no match.
double word_fuzzy_score(const PreparedQuery& pq, const std::string& t_norm) {
    if (pq.words.empty() || t_norm.empty()) return -1.0;
    const std::string_view t(t_norm);
    double total_quality = 0.0;
    bool any_word = false;
    for (size_t qi = 0; qi < pq.words.size(); ++qi) {
        const std::string& qw = pq.words[qi];
        const uint64_t qmask = pq.masks[qi];
        double best_quality = -1.0;
        size_t i = 0;
        while (i < t.size()) {   // the target's words, without copying them
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
            const size_t start = i;
            uint64_t tmask = 0;
            while (i < t.size() && t[i] != ' ' && t[i] != '\t') { tmask |= char_bit(t[i]); ++i; }
            if (i == start) break;
            any_word = true;
            const std::string_view tw = t.substr(start, i - start);
            const size_t max_len = std::max(qw.size(), tw.size());
            const int cutoff = static_cast<int>(0.45 * static_cast<double>(max_len)) + 1;
            // Every distinct character of one word that the other lacks costs an edit (collisions of the hash only
            // lower the count), so this is a lower bound of the distance: no need to compute it when it is too big.
            if (std::max(popcount64(qmask & ~tmask), popcount64(tmask & ~qmask)) > cutoff) continue;
            const double quality = 1.0 - static_cast<double>(levenshtein_capped(qw, tw, cutoff)) / static_cast<double>(max_len);
            if (quality > best_quality) best_quality = quality;
        }
        if (!any_word) return -1.0;
        if (best_quality < 0.55) return -1.0;
        total_quality += best_quality;
    }
    return (total_quality / static_cast<double>(pq.words.size())) * 100.0;
}

std::string fmt_mmss(double seconds) {
    if (seconds < 0) return "--:--";
    int total = static_cast<int>(seconds);
    std::ostringstream oss;
    oss.width(2); oss.fill('0'); oss << (total / 60) << ":";
    oss.width(2); oss.fill('0'); oss << (total % 60);
    return oss.str();
}

// --- shared box-drawing helpers ------------------------------------------
// Every panel is built against `total_width` (the FULL visual width of the
// box, borders included) so two boxes placed side by side on the same row
// always sum to exactly the width the caller asked for, and single-box
// rows line up with everything above/below them. Content is always
// total_width-4 (for the "│ X │" pattern), padded with the codepoint-safe
// helpers from terminal_ui.cpp so multi-byte glyphs can't throw off the
// column count the way byte-length padding did before.

// SGR for a "cursor" (hovering) row. If both the configured foreground and
// background resolve to nothing -- e.g. ColorQueueCursorBg is blank in the
// user's config -- the row would look identical to its neighbours and the
// cursor would be invisible, so fall back to reverse video.
std::string cursor_sgr(const std::string& fg, const std::string& bg) {
    std::string s = ansi_for(fg) + bg_ansi_for(bg);
    return s.empty() ? std::string("\x1b[7m") : s;
}

} // namespace

// SGR for search bar input (the "/" box, the meta editor's and playlist
// manager's "Search:" rows): the Colors tab's SEARCH row, where an empty
// cell follows LIST INACTIVE FG/BG -- the colours these bars always had.
static std::string search_sgr(const Settings& s) { // the typed query (behind its text only)
    const std::string& fg = s.search_text_color.empty() ? s.list_color : s.search_text_color;
    const std::string& bg = s.search_bg_color.empty() ? s.list_inactive_bg_color : s.search_bg_color;
    return ansi_for(fg, false) + bg_ansi_for(bg);
}
static std::string search_idle_sgr(const Settings& s) { // everything else in the bar
    // Text follows LIST INACTIVE FG, but the background is NOT inherited: an
    // unset INACTIVE BG leaves the bar plain, so only the query is highlighted.
    const std::string& fg = s.search_inactive_text_color.empty() ? s.list_color : s.search_inactive_text_color;
    return ansi_for(fg, false) + bg_ansi_for(s.search_inactive_bg_color);
}
// `text` in the query colours, then back to the idle ones; nothing when empty
// (an input that was not performed is not highlighted).
static std::string search_run(const Settings& s, const std::string& text, bool nonempty) {
    // The reset matters: an idle colour without a BG would otherwise leave the query's BG running.
    return nonempty ? search_sgr(s) + text + "\x1b[0m" + search_idle_sgr(s) : text;
}

// SGR prefix of the pane focus indicator "◀" (Colors tab FOCUS row, ColorFocus=): bold plus focus_color; an unset
// color degrades to plain bold like the header does.
static std::string focus_sgr(const Settings& s) {
    const std::string params = sgr_params_for(s.focus_color);
    return params.empty() ? "\x1b[1m" : "\x1b[1;" + params + "m";
}

// The small "◀" that marks which pane/field within a panel has the keyboard focus. box_top() wraps the whole title
// string in `border_ansi` (one colour for the whole row), so to make just the triangle read in the focus colour, the
// title text itself switches colour for it and then back to `border_ansi` -- the same colour box_top() already has --
// so the dashes that follow the title stay in the border colour.
// The way back to the border colour is a marker (\x01) that box_top() resolves: it puts the border colour straight in front of
// the first dash after the title, where the smooth border gradient (clamp_output_rows) finds it and gives it the colour of that
// ROW -- a plain colour escape followed by the blank before the dashes would stay the top colour.
static std::string focus_indicator(const Settings& s, const std::string&) {
    return " " + focus_sgr(s) + "◀" + "\x1b[0m" + "\x01";
}

// The text of `s` without its CSI colour sequences (what is really drawn).
static std::string strip_csi(const std::string& s) {
    std::string plain;
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '\x1b' && k + 1 < s.size() && s[k + 1] == '[') {
            k += 2;
            while (k < s.size() && !(s[k] >= 0x40 && s[k] <= 0x7e)) ++k;
            continue;
        }
        plain += s[k];
    }
    return plain;
}

std::string App::box_top(const std::string& label_in, int total_width, const std::string& border_ansi) const {
    // A title wider than the pane used to push the top border past the pane's edge, and the line then broke at the
    // terminal's right edge and shifted everything below it (a long file name plus a 3-digit "206/843" in the meta editor).
    // The text part is cut with "..." instead; a focus indicator at the end of the title (" " + colour + "◀") is kept.
    std::string label = label_in;
    bool back_to_border = false;   // the title ends with a focus indicator: the border colour comes back right before the dashes
    for (size_t at = label.find('\x01'); at != std::string::npos; at = label.find('\x01')) { label.erase(at, 1); back_to_border = true; }
    if (!label.empty()) {
        const int room = total_width - 1 - display_width(settings_.box_upper_left + settings_.box_horizontal) - 2;   // between "╭─ " and " ╮"
        if (display_width(strip_csi(label)) > room && room >= 6) {
            const size_t esc = label.find('\x1b');
            std::string head = esc == std::string::npos ? label : label.substr(0, esc);
            const std::string tail = esc == std::string::npos ? std::string() : label.substr(esc);
            const bool space = !head.empty() && head.back() == ' ' && !tail.empty();   // the blank in front of the indicator
            if (space) head.pop_back();
            const int head_room = room - display_width(strip_csi(tail)) - (space ? 1 : 0);
            if (head_room >= 4) label = truncate_str(head, head_room) + (space ? " " : "") + tail;
            else label = truncate_str(strip_csi(label), room);
        }
    }
    std::string lbl = label.empty() ? "" : (" " + label + " ");
    if (back_to_border && !label.empty()) lbl += border_ansi;
    std::string prefix = settings_.box_upper_left + settings_.box_horizontal + lbl;
    // The label may carry colour escapes (the focus indicator's "◀" in the
    // header colour): display_width() counts their parameter text as visible
    // columns, which shortened the top border by that many cells. Measure
    // the title with the CSI sequences removed.
    std::string plain;
    for (size_t k = 0; k < prefix.size(); ++k) {
        if (prefix[k] == '\x1b' && k + 1 < prefix.size() && prefix[k + 1] == '[') {
            k += 2;
            while (k < prefix.size() && !(prefix[k] >= 0x40 && prefix[k] <= 0x7e)) ++k;
            continue;
        }
        plain += prefix[k];
    }
    int used = display_width(plain);
    int dashes = std::max(0, total_width - used - 1);
    std::string s = prefix;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
    s += settings_.box_upper_right;
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

std::string App::box_bottom(int total_width, const std::string& footer, const std::string& border_ansi) const {
    std::string prefix = footer.empty() ? (settings_.box_lower_left + settings_.box_horizontal) : (settings_.box_lower_left + settings_.box_horizontal + " " + footer + " ");
    int used = display_width(prefix);
    int dashes = std::max(0, total_width - used - 1);
    std::string s = prefix;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
    s += settings_.box_lower_right;
    s = pad_right(s, total_width);
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

std::string App::box_line(const std::string& content, int total_width, const std::string& border_ansi) const {
    int inner = std::max(0, total_width - 4);
    std::string padded = pad_right(truncate_str(content, inner), inner);
    if (border_ansi.empty()) return settings_.box_vertical + " " + padded + " " + settings_.box_vertical;
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    return bar + " " + padded + " " + bar;
}

// box_top() for a label whose tail is a live text field (caret + marked
// range). `field` is already painted and therefore full of reverse-video
// escapes, which display_width() -- UTF-8 aware, ANSI blind -- would count
// as columns; so the dash fill is computed from `field_cols`, the number of
// columns paint_edit_field() says it really occupies.
std::string App::box_top_field(const std::string& prefix, const std::string& field, int field_cols,
                               int total_width, const std::string& border_ansi) const {
    const int dashes = std::max(0, total_width - display_width(prefix) - field_cols - 1);
    std::string s = prefix + field;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal; // a std::string, not a char
    s += settings_.box_upper_right;
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

// The same for box_line(): `prefix` plain, `field` painted, padded to
// box_line()'s own inner width (total_width - 4).
std::string App::box_line_field(const std::string& prefix, const std::string& field, int field_cols,
                                int total_width, const std::string& border_ansi, const std::string& text_ansi) const {
    const int inner = std::max(0, total_width - 4);
    const int fill = std::max(0, inner - display_width(prefix) - field_cols);
    std::string padded = text_ansi + prefix + field + std::string(fill, ' ') + (text_ansi.empty() ? "" : "\x1b[0m");
    if (border_ansi.empty()) return settings_.box_vertical + " " + padded + " " + settings_.box_vertical;
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    return bar + " " + padded + " " + bar;
}

// Points the shared caret/selection (edit_caret_/edit_anchor_) at one
// specific field: keeps the position while that same field is being typed
// into continuously, and jumps to the end whenever a different field -- or
// the same one, freshly focused -- takes over. Called once per key by each
// box, right before edit_text_key() runs on it.
void App::edit_focus(const std::string& owner, const std::string& text) {
    if (edit_owner_ != owner) {
        edit_owner_ = owner;
        edit_caret_ = edit_anchor_ = text.size();
    }
    le_clamp(text, edit_caret_, edit_anchor_);
}

namespace {

// Center-aligns plain (no-ANSI) text within `width` visual columns.
std::string center_pad(const std::string& text, int width) {
    std::string t = truncate_str(text, width);
    int pad = std::max(0, width - display_width(t));
    int left = pad / 2;
    int right = pad - left;
    return std::string(left, ' ') + t + std::string(right, ' ');
}

// Visualizer bars use ONLY this glyph set (per explicit instruction) —
// distinct from WaveformQuantizer's 6-level top/mid/bot triples used by
// the progress bar.
const char* fft_glyph(int level) {
    switch (std::clamp(level, 0, 4)) {
        case 0: return " ";
        case 1: return "\u28C0"; // ⣀
        case 2: return "\u28E4"; // ⣤
        case 3: return "\u28F6"; // ⣶
        default: return "\u28FF"; // ⣿
    }
}

// Word-wraps one lyric line to `width` visual columns (breaking on word
// boundaries, never mid-word), center-aligning each resulting row, and
// bakes in ANSI highlighting for words already "sung" (their timestamp
// <= elapsed) when this is the active line. Width/centering math is done
// on PLAIN text first — color codes are spliced in afterwards, since they
// don't occupy display columns but would otherwise confuse the
// codepoint-counting padding helpers.
// Counts UTF-8 codepoints (not bytes) -- used for the letter-by-letter
// lyrics reveal below. terminal_ui.cpp has an equivalent utf8_take(), but
// it's `static` (file-local) so it isn't reachable from here.


std::vector<std::string> render_lyric_line_wrapped(const LyricLine& line, double elapsed, int width,
                                                     bool is_active, const Settings& settings) {
    struct W { std::string text; double t; bool has_ts; };
    std::vector<W> words;
    if (!line.words.empty()) {
        for (const auto& wt : line.words) words.push_back({wt.second, wt.first, true});
    } else {
        std::istringstream iss(line.full_text);
        std::string w;
        while (iss >> w) words.push_back({w, line.start_time, false});
    }
    if (words.empty() || width <= 0) return {std::string(std::max(0, width), ' ')};

    std::vector<std::vector<W>> rows;
    std::vector<W> cur;
    int cur_len = 0;
    for (auto& w : words) {
        std::string remain = w.text;
        while (!remain.empty()) {
            int r_wlen = display_width(remain);
            int available = cur.empty() ? width : (width - cur_len - 1);
            
            if (r_wlen <= available) {
                cur.push_back({remain, w.t, w.has_ts});
                cur_len += (cur.empty() ? r_wlen : 1 + r_wlen);
                break;
            }
            
            if (!cur.empty()) {
                rows.push_back(cur);
                cur.clear();
                cur_len = 0;
                continue; // retry fitting on a new line
            }
            
            // The word exceeds the full width of a line, must split it.
            std::string chunk = utf8_take(remain, width);
            if (chunk.empty()) {
                // Failsafe: width is too small (e.g., 1) to fit a wide character (width 2).
                // Force-take 2 columns so we at least make progress (1 grapheme cluster).
                chunk = utf8_take(remain, 2);
            }
            
            cur.push_back({chunk, w.t, w.has_ts});
            rows.push_back(cur);
            cur.clear();
            cur_len = 0;
            remain = remain.substr(chunk.size());
        }
    }
    if (!cur.empty()) rows.push_back(cur);

    std::vector<std::string> out;
    for (auto& row : rows) {
        std::vector<std::string> mapped_words;
        int plain_len = 0;
        for (size_t i = 0; i < row.size(); ++i) {
            std::string mapped = apply_font_map(row[i].text, settings.font_map);
            mapped_words.push_back(mapped);
            plain_len += display_width(mapped);
            if (i > 0) plain_len += 1;
        }
        int total_pad = std::max(0, width - plain_len);
        int left_pad, right_pad;
        if (settings.lyrics_alignment == 1) { left_pad = 0; right_pad = total_pad; }           // left
        else if (settings.lyrics_alignment == 2) { left_pad = total_pad; right_pad = 0; }       // right
        else { left_pad = total_pad / 2; right_pad = total_pad - left_pad; }                    // center (default)

        // Word-level karaoke highlight: find the word currently being sung
        // (the last word whose timestamp has passed but the *next* one
        // hasn't) and render just that one underlined on top of the
        // normal active-word color -- gives the highlight a moving
        // "leading edge" instead of every already-sung word looking
        // identical. (A continuous per-frame pulse used to live here too,
        // applied to whole line-synced-only lines with no way to turn it
        // off -- removed; that wasn't requested and had no toggle.)
        int currently_singing = -1;
        for (size_t i = 0; i < row.size(); ++i) {
            if (is_active && row[i].has_ts && row[i].t <= elapsed) currently_singing = static_cast<int>(i);
        }
        const char* kUnderline = "\x1b[4m";

        std::string s(left_pad, ' ');
        std::string word_ansi = ansi_for(settings.active_word_color) + bg_ansi_for(settings.active_word_bg_color);
        std::string active_line_ansi = ansi_for(settings.active_line_color) + bg_ansi_for(settings.active_line_bg_color);
        std::string inactive_ansi = ansi_for(settings.inactive_line_color) + bg_ansi_for(settings.inactive_line_bg_color);
        // Word-by-word / letter-by-letter modes hide not-yet-sung words in
        // the active line entirely (blanked to spaces, same width, so the
        // alignment doesn't jump around) instead of showing them in the
        // active-line color right away -- that's the actual "progressive
        // reveal" that was asked for, as opposed to the old always-fully-
        // visible line with just a moving color highlight.
        bool progressive = is_active && (settings.lyrics_animation == 1 || settings.lyrics_animation == 2);
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) s += ' ';
            bool sung = is_active && row[i].has_ts && row[i].t <= elapsed;
            bool no_word_ts_but_active = is_active && !row[i].has_ts;
            bool is_current = sung && static_cast<int>(i) == currently_singing;
            if (is_current && settings.lyrics_animation == 2) {
                // Letter-by-letter: interpolate how many characters of the
                // CURRENT word are revealed so far. Prefer the next word's
                // timestamp (within this wrapped row) as the end bound; if
                // this is the last word in the row (no next timestamp to
                // interpolate toward), fall back to the previous word's
                // pace instead of just popping the whole word in at once --
                // that "spawns out of nowhere" look was the bug. With no
                // timing context at all (a single-word line), use a
                // reasonable fixed pace.
                std::string full = mapped_words[i];
                int nchars = display_width(full);
                double dur = -1.0;
                if (i + 1 < row.size() && row[i + 1].has_ts && row[i + 1].t > row[i].t) {
                    dur = row[i + 1].t - row[i].t;
                } else if (i > 0 && row[i - 1].has_ts && row[i].t > row[i - 1].t) {
                    dur = row[i].t - row[i - 1].t;
                } else {
                    dur = 0.4;
                }
                double frac = std::clamp((elapsed - row[i].t) / dur, 0.0, 1.0);
                int revealed = static_cast<int>(frac * nchars);
                std::string shown = utf8_take(full, revealed);
                int hidden_cols = display_width(full) - display_width(shown);
                s += word_ansi + kUnderline + shown + "\x1b[0m" + std::string(std::max(0, hidden_cols), ' ');
            } else if (is_current) {
                s += word_ansi + kUnderline + mapped_words[i] + "\x1b[0m";
            } else if (sung) {
                s += word_ansi + mapped_words[i] + "\x1b[0m";
            } else if (progressive && row[i].has_ts) {
                s += std::string(display_width(mapped_words[i]), ' '); // not sung yet -- hidden, not just dimmed
            } else if (no_word_ts_but_active) {
                s += active_line_ansi + mapped_words[i] + "\x1b[0m";
            } else if (is_active) {
                s += active_line_ansi + mapped_words[i] + "\x1b[0m";
            } else {
                s += inactive_ansi + mapped_words[i] + "\x1b[0m";
            }
        }
        s += std::string(right_pad, ' ');
        out.push_back(s);
    }
    return out;
}

// Resolves a filename under scripts/ next to the running binary. Shared by
// the lyrics helper and the fast-search helper -- same candidate list
// (env override, cwd, exe-relative at one/two/three levels up to cover a
// multi-config MSVC build), only the filename differs.
fs::path find_scripts_file(const std::string& filename) {
    if (const char* env = std::getenv("MOUSIKI_SCRIPTS_DIR")) {
        fs::path p = path_from_utf8(env) / filename;
        if (fs::exists(p)) return p;
    }
    fs::path cwd_candidate = fs::path("scripts") / filename;
    if (fs::exists(cwd_candidate)) return cwd_candidate;

#if defined(__APPLE__)
    char exe_buf[4096];
    uint32_t size = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &size) == 0) {
        std::error_code ec;
        fs::path exe_dir = fs::canonical(fs::path(exe_buf), ec).parent_path();
        if (!ec) {
            fs::path p = exe_dir / "scripts" / filename;
            if (fs::exists(p)) return p;
            p = exe_dir.parent_path() / "scripts" / filename;
            if (fs::exists(p)) return p;
        }
    }
#elif defined(_WIN32)
    // No /proc on Windows; GetModuleFileNameW is the direct equivalent.
    // Worth noting the lookup one level up matters more here than on Linux:
    // a multi-config MSVC build puts the exe in build\\Release\\, so
    // scripts/ is two levels above it, and the CMake copy step mirrors
    // scripts/ next to the exe to cover that.
    {
        std::string exe = win_executable_path();
        if (!exe.empty()) {
            fs::path exe_dir = path_from_utf8(exe).parent_path();
            fs::path p = exe_dir / "scripts" / filename;
            if (fs::exists(p)) return p;
            p = exe_dir.parent_path() / "scripts" / filename;
            if (fs::exists(p)) return p;
            p = exe_dir.parent_path().parent_path() / "scripts" / filename;
            if (fs::exists(p)) return p;
        }
    }
#else
    char exe_buf[4096];
    ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
        exe_buf[n] = '\0';
        fs::path exe_dir = fs::path(exe_buf).parent_path();
        fs::path p = exe_dir / "scripts" / filename;
        if (fs::exists(p)) return p;
        p = exe_dir.parent_path() / "scripts" / filename;
        if (fs::exists(p)) return p;
    }
#endif
    return cwd_candidate;
}

fs::path find_lyrics_script() { return find_scripts_file("fetch_lyrics.py"); }

// The InnerTube-based search script (see OnlineSource::search()). Unlike
// the lyrics script, its absence is not an error condition anywhere --
// OnlineSource falls back to yt-dlp's own search whenever this path
// doesn't resolve to a real file, so an empty/missing result here is a
// normal, silent path for anyone who only has the core scripts installed.
fs::path find_fast_search_script() { return find_scripts_file("fast_yt_search.py"); }

// fpcalc is a BUILD product: CMake compiles tools/fpcalc.cpp and copies the
// result next to mousiki.exe and into that exe's scripts/, so it exists only
// beside the executable -- never in a checkout's scripts/ folder, and
// find_scripts_file() can very well hand out exactly that one (the working
// directory wins there on purpose, so edited .py files take effect without a
// rebuild). fetch_meta.py resolves fpcalc next to the script IT was given
// (resolve_fpcalc()), so with those two rules combined a fetch started from a
// checkout's root dies with "fpcalc not found" even though this build has a
// perfectly good helper next to its own exe. Handing that path down through
// MOUSIKI_FPCLC -- the script's own documented override -- keeps both rules
// intact, and never overwrites a path the user exported themselves.
void export_fpcalc_to_scripts() {
    if (std::getenv("MOUSIKI_FPCLC")) return; // an explicit override always wins
#if defined(_WIN32)
    const char* name = "fpcalc.exe";
#else
    const char* name = "fpcalc";
#endif
    fs::path exe_dir;
#if defined(__APPLE__)
    char exe_buf[4096];
    uint32_t size = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &size) == 0) {
        std::error_code ec;
        fs::path resolved = fs::canonical(fs::path(exe_buf), ec);
        if (!ec) exe_dir = resolved.parent_path();
    }
#elif defined(_WIN32)
    std::string exe = win_executable_path();
    if (!exe.empty()) exe_dir = path_from_utf8(exe).parent_path();
#else
    char exe_buf[4096];
    ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
        exe_buf[n] = '\0';
        exe_dir = fs::path(exe_buf).parent_path();
    }
#endif
    if (exe_dir.empty()) return;
    // The same ladder find_scripts_file() walks for scripts/: beside the exe,
    // in its scripts/, then one and two levels up (a multi-config MSVC build
    // puts the exe in build\Release\).
    const fs::path dirs[] = {exe_dir, exe_dir / "scripts",
                             exe_dir.parent_path() / "scripts",
                             exe_dir.parent_path().parent_path() / "scripts"};
    for (const fs::path& dir : dirs) {
        std::error_code ec;
        fs::path candidate = dir / name;
        if (!fs::exists(candidate, ec)) continue;
        std::string value = path_utf8(candidate);
#if defined(_WIN32)
        _putenv_s("MOUSIKI_FPCLC", value.c_str());
#else
        setenv("MOUSIKI_FPCLC", value.c_str(), 1);
#endif
        return;
    }
}

} // namespace

App::App() {
    settings_ = load_settings();
    // Point yt-dlp at the configured folder (if any) before anything can
    // ask where a download would go. Empty config = default cache folder,
    // which is what set_download_dir() restores on its own.
    cache_.set_download_dir(path_from_utf8(settings_.download_folder));
    history_.set_dir(settings_.history_path);
    history_.load(); // listening history: a missing/corrupt file is not fatal (see HistoryStore::load)
    set_emoji_replacement(settings_.replace_emoji);
    player_.set_stereo(settings_.stereo);
    player_.set_normalization(settings_.normalize,
                              static_cast<float>(settings_.normalize_target_lufs),
                              static_cast<float>(settings_.normalize_max_boost_db));
    eq_apply();
    lyrics_script_ = find_lyrics_script();
    fast_search_script_ = find_fast_search_script();
    meta_script_ = find_scripts_file("fetch_meta.py"); // AcoustID helper for Mode::MetaEdit
    translate_script_ = find_scripts_file("translate_lyrics.py"); // lyrics translation of Mode::ReadLyrics
    export_fpcalc_to_scripts(); // ...and the fpcalc it runs (see why there)

    // BUGFIX: the cache-dir injection below used to push_back()
    // unconditionally, every single launch -- and since save_settings()
    // (called on every quit) writes settings_.local_music_paths back to
    // config.txt verbatim, *including* this appended entry, the cache
    // directory accumulated one more duplicate line in config.txt every
    // session. Harmless for correctness -- LocalSource::scan()'s
    // dedup-by-canonical-path already prevents the same file being
    // counted twice -- but wasteful: dozens of redundant directory
    // existence checks and walks on every startup, and a config.txt that
    // silently grows without bound over months of use. Deduping the whole
    // list first (in case manual edits introduced other repeats too),
    // order preserved so the saved file doesn't get needlessly reshuffled.
    {
        std::vector<std::string> deduped;
        deduped.reserve(settings_.local_music_paths.size());
        for (const auto& p : settings_.local_music_paths) {
            if (std::find(deduped.begin(), deduped.end(), p) == deduped.end()) deduped.push_back(p);
        }
        settings_.local_music_paths = std::move(deduped);
    }

    // Inject the cache directory into local music paths so streamed songs
    // automatically appear in the local view for seamless offline playback
    // -- only if it isn't already there (see the dedup note just above).
    std::string cache_dir_str = path_utf8(cache_.cache_dir());
    if (std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), cache_dir_str)
        == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(cache_dir_str);
    }
    // And the configured download folder, for the same reason: whatever
    // yt-dlp writes has to be scannable without a second LocalMusicPath
    // line, which is what the DOWNLOAD PATH setting promises. Skipped
    // while it is still the default -- that IS the cache dir just above.
    std::string download_dir_str = path_utf8(cache_.download_dir());
    if (download_dir_str != cache_dir_str
        && std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), download_dir_str)
               == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(download_dir_str);
    }

    all_local_tracks_ = local_source_.scan(settings_.local_music_paths, &local_scan_diagnostics_);
    local_view_ = all_local_tracks_;
    launch_row_meta_resolver();
    start_device_worker();
}


// Names of the modifier combinations that can be bound (terminal_ui.h has no plain character for them). "ALT+L" is
// the OPTION key on a Mac; the label shown for it follows the platform (pretty_key()).
struct NamedComboKey { const char* name; int code; };
static const NamedComboKey kComboKeys[] = {
    {"ALT+L", kKeyAltL}, {"CTRL+L", kKeyCtrlL}, {"CTRL+SHIFT+U", kKeyCtrlShiftU}, {"CTRL+SHIFT+Z", kKeyCtrlShiftZ},
};

// "" when `key` is not one of the bindable combinations.
static std::string combo_key_name(int key) {
    for (const auto& c : kComboKeys) if (c.code == key) return c.name;
    return "";
}

// What the user typed into a key field, cleaned up: ALT/OPTION spelled either way, any case for the names ("enter",
// "ctrl+l", "up"). Single characters are left exactly as typed (a capital letter means SHIFT+letter).
static std::string normalize_hotkey_string(std::string s) {
    if (s.size() <= 1) return s;
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (s.rfind("OPTION+", 0) == 0) s = "ALT+" + s.substr(7);
    if (s == "UP") return "ARROW_KEY_UP";
    if (s == "DOWN") return "ARROW_KEY_DOWN";
    if (s == "LEFT") return "ARROW_KEY_LEFT";
    if (s == "RIGHT") return "ARROW_KEY_RIGHT";
    if (s == "RETURN") return "ENTER";
    if (s == "ESCAPE") return "ESC";
    return s;
}

int App::hotkey_string_to_key(const std::string& s) {
    if (s == "ARROW_KEY_UP") return 'A';
    if (s == "ARROW_KEY_DOWN") return 'B';
    if (s == "ARROW_KEY_RIGHT") return 'C';
    if (s == "ARROW_KEY_LEFT") return 'D';
    if (s == "ENTER") return '\n';
    if (s == "TAB") return 9;
    if (s == "SPACE") return ' ';
    if (s == "ESC") return 27;
    if (s == "BACKSPACE") return 127;
    for (const auto& c : kComboKeys) if (s == c.name) return c.code;
    if (s.size() == 1) return static_cast<int>(s[0]);
    return 0;
}

bool App::key_is(const char* action, int key) const {
    const auto it = settings_.hotkeys.find(action);
    if (it == settings_.hotkeys.end() || it->second.empty()) return false;
    if (hotkey_string_to_key(it->second) != key) return false;
    // The arrows travel as 'A'..'D' (see resolve_hotkey_action()): a typed capital and an arrow are told apart here.
    if (key >= 'A' && key <= 'D') return last_key_was_arrow() == (it->second.rfind("ARROW_KEY_", 0) == 0);
    return true;
}

std::string App::resolve_hotkey_action(int key) const {
    // The arrow keys travel as the letters 'A'..'D', so a typed SHIFT+a..d is the same int as an arrow. The input
    // layer says which one it was; an arrow only matches ARROW_KEY_* bindings and a typed letter only the others.
    // (Without this, SHIFT+c read as Right and seeked forward, and a binding on "C" fired on the Right arrow.)
    const bool ambiguous = key >= 'A' && key <= 'D';
    const bool was_arrow = ambiguous && last_key_was_arrow();
    for (const auto& [action, key_str] : settings_.hotkeys) {
        if (hotkey_string_to_key(key_str) != key) continue;
        if (ambiguous && was_arrow != (key_str.rfind("ARROW_KEY_", 0) == 0)) continue;
        return action;
    }
    return "";
}

std::string App::hotkey_conflict(const std::string& key_str, const std::string& except_action) const {
    if (key_str.empty()) return "";
    for (const auto& [action, val] : settings_.hotkeys) {
        if (action == except_action) continue;
        if (val == key_str) return action;
    }
    return "";
}

// Pushes a message into both the one-line status area (existing
// behavior) and the persistent console log (both the in-memory buffer
// the Console overlay reads and, via ConsoleLog, the on-disk
// console.log) -- so events are still visible after they've scrolled
// off the status line, after a terminal-session switch redraw wiped
// the screen, or after the session itself has ended.
void App::log_event(const std::string& msg) {
    status_line_ = msg;
    ConsoleLog::instance().log_basic(msg);
}

// ---------------------------------------------------------------------
// Search / list state
// ---------------------------------------------------------------------

// Shared by refresh_local_view() (final, committed query) and the live
// incremental-search preview (whatever's currently typed, before Enter)
// so both behave identically -- what you see while typing is exactly
// what you'll get if you confirm it.
//
// An empty query returns the library sorted per local_sort_mode_ (see
// apply_local_sort()). A real query filters to fuzzy-matching tracks
// only and sorts PURELY by match quality, highest first -- this
// deliberately throws away the sort order entirely rather than using it
// as a tiebreak, so the best match always sits at the top regardless of
// where it happened to fall alphabetically/by-folder.
//
// Matches against filename-derived title, real artist tag, embedded
// title tag, and album tag -- using whatever's already been resolved
// for that row in row_meta_cache_, falling back to the cheap
// parent-folder guess for artist where the tag hasn't been probed yet.
// This is why matching against embedded metadata gets more complete the
// more of the library you've scrolled past / the longer the one-time
// background sweep (launch_row_meta_resolver) has had to run: each row's
// real tags only become searchable once resolved. A file whose tags
// haven't resolved yet is still findable by filename in the meantime.
// Title shown for `path` in the (search-)lists. Normally that is just the
// filename stem the row was built from; with "Show meta data only"
// (settings_.meta_only) on, the embedded title tag wins as soon as one has
// been resolved for that file, so the row shows metadata rather than a
// filename. Files with no title tag (or tags not probed yet) still fall
// back to the filename -- otherwise turning the toggle on would blank out
// an untagged library instead of just re-describing it.
std::string App::list_row_title(const fs::path& path, const std::string& filename_title) const {
    if (!settings_.meta_only) return filename_title;
    std::lock_guard<std::mutex> lk(row_meta_mutex_);
    auto it = row_meta_cache_.find(path_utf8(path));
    if (it != row_meta_cache_.end() && !it->second.title.empty()) return it->second.title;
    return filename_title;
}

std::string App::playlist_row_label(const fs::path& path, const std::string& filename_title) const {
    std::lock_guard<std::mutex> lk(row_meta_mutex_);
    auto it = row_meta_cache_.find(path_utf8(path));
    if (it != row_meta_cache_.end() && !it->second.title.empty() && it->second.title != filename_title) {
        return filename_title + "  \u2014 " + it->second.title; // em dash separator
    }
    return filename_title;
}

std::string App::marquee_or_truncate(const std::string& text, int width, int row_idx,
                                      int& tracked_idx, std::chrono::steady_clock::time_point& since) const {
    if (row_idx != tracked_idx) {
        tracked_idx = row_idx;
        since = std::chrono::steady_clock::now();
    }
    if (display_width(text) <= width || !settings_.marquee) return pad_right(truncate_str(text, width), width);   // fits, or scrolling is off: cut with "..."

    const double hold_secs = 1.2;    // pause on the title's start before scrolling
    const double cols_per_sec = 4.0; // scroll speed
    const std::string gap = "    ";  // seam between one loop and the next
    std::string loop_text = text + gap;
    int period = display_width(loop_text);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    int start_col = 0;
    if (elapsed > hold_secs && period > 0) {
        double scrolled = (elapsed - hold_secs) * cols_per_sec;
        start_col = static_cast<int>(scrolled) % period;
    }
    // Three repeats guarantee a full-width window is always available no
    // matter where start_col lands in the cycle.
    std::string doubled = loop_text + loop_text + loop_text;
    return pad_right(utf8_skip_take(doubled, start_col, width), width);
}

// "/r:" searches the ARTIST only, "/a:" the ALBUM only (the plain "/" search looks at title, artist and album).
// Returns 0 (all fields), 1 (artist) or 2 (album) and the query without the prefix.
static int local_query_scope(const std::string& query, std::string& rest) {
    size_t i = 0;
    while (i < query.size() && query[i] == ' ') ++i;
    rest = query.substr(i);
    int scope = 0;
    if (rest.size() >= 2 && rest[1] == ':') {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(rest[0])));
        scope = c == 'r' ? 1 : c == 'a' ? 2 : 0;
        if (scope) {
            rest.erase(0, 2);
            while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        }
    }
    return scope;
}

std::vector<LocalTrack> App::filter_and_rank_local(const std::string& raw_query) const {
    std::string query;
    const int scope = local_query_scope(raw_query, query);   // 0 everything, 1 artist only, 2 album only
    if (query.empty()) {
        auto result = all_local_tracks_;
        apply_local_sort(result);
        return result;
    }

    // The search index: the normalised strings of every track (built once per scan), the tag fields added as the
    // background sweep resolves them. A keystroke then only compares strings.
    if (search_idx_.size() != all_local_tracks_.size()) {
        search_idx_.clear();
        search_idx_.reserve(all_local_tracks_.size());
        for (const auto& t : all_local_tracks_) {
            SearchIdx e;
            e.key = path_utf8(t.path);
            e.title_n = normalize_for_search(t.title);
            e.folder_artist_n = normalize_for_search(t.folder_artist);
            search_idx_.push_back(std::move(e));
        }
    }
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);   // once for the whole library, not once per track
        for (auto& e : search_idx_) {
            if (e.tags_seen) continue;
            auto it = row_meta_cache_.find(e.key);
            if (it == row_meta_cache_.end()) continue;
            e.tag_title_n = normalize_for_search(it->second.title);
            e.artist_n = normalize_for_search(it->second.artist);
            e.album_n = normalize_for_search(it->second.album);
            e.tags_seen = it->second.tags_resolved;
        }
    }
    const PreparedQuery pq = prepare_query(query);
    std::vector<std::pair<double, const LocalTrack*>> scored;
    scored.reserve(all_local_tracks_.size());
    for (size_t i = 0; i < all_local_tracks_.size(); ++i) {
        const SearchIdx& e = search_idx_[i];
        const std::string& artist = e.artist_n.empty() ? e.folder_artist_n : e.artist_n;
        double score = 0.0;
        if (scope == 1) {
            score = substring_score(pq, artist);
            if (score < 0.0) score = word_fuzzy_score(pq, artist);
        } else if (scope == 2) {
            if (!e.album_n.empty()) {
                score = substring_score(pq, e.album_n);
                if (score < 0.0) score = word_fuzzy_score(pq, e.album_n);
            }
        } else {
            // The best score over the fields. An exact substring (tier 1, 100 .. 1000) always beats a fuzzy one (< 100),
            // so the fuzzy pass only runs when no field contains the query.
            const std::string* fields[4] = {&e.title_n, &artist, &e.tag_title_n, &e.album_n};
            double best = -1.0;
            for (const std::string* f : fields) if (!f->empty()) best = std::max(best, substring_score(pq, *f));
            if (best < 0.0) for (const std::string* f : fields) best = std::max(best, word_fuzzy_score(pq, *f));
            score = best;
        }
        if (score > 0.0) scored.emplace_back(score, &all_local_tracks_[i]);
    }
    std::stable_sort(scored.begin(), scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<LocalTrack> result;
    result.reserve(scored.size());
    for (auto& [score, t] : scored) result.push_back(*t);
    return result;
}

// Mode 1 names what it sorts by, which follows Shift+N (see apply_local_sort()):
// the file name normally, the embedded title tag in metadata-only mode.
const char* App::sort_mode_name(int mode, bool meta_only) {
    switch (mode) {
        case 1: return meta_only ? "title A-Z" : "file name A-Z";
        case 2: return "artist A-Z";
        default: return "folder order";
    }
}

// Applied only when browsing with no active search query -- a fuzzy
// search's relevance ranking always wins over the manual sort mode.
// Duration isn't a sort option (yet): most rows only get a real duration
// once they've been lazily probed for display, so sorting by it up front
// would show mostly-unprobed rows in an arbitrary order until the
// background resolver catches up.
void App::apply_local_sort(std::vector<LocalTrack>& tracks) const {
    if (local_sort_mode_ == 1) {
        // Sort by the title the row actually SHOWS: the filename-derived one,
        // or -- with "Show metadata only" (Shift+N) on -- the embedded title
        // tag where one has been resolved (list_row_title() falls back to the
        // filename otherwise, exactly like the row does). Keys are built once
        // up front instead of taking row_meta_mutex_ on every comparison.
        std::vector<std::pair<std::string, size_t>> keyed;
        keyed.reserve(tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i)
            keyed.emplace_back(lower(list_row_title(tracks[i].path, tracks[i].title)), i);
        std::stable_sort(keyed.begin(), keyed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<LocalTrack> sorted;
        sorted.reserve(tracks.size());
        for (const auto& k : keyed) sorted.push_back(std::move(tracks[k.second]));
        tracks = std::move(sorted);
    } else if (local_sort_mode_ == 2) {
        std::stable_sort(tracks.begin(), tracks.end(), [this](const LocalTrack& a, const LocalTrack& b) {
            auto artist_of = [this](const LocalTrack& t) {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(path_utf8(t.path));
                return (it != row_meta_cache_.end() && !it->second.artist.empty()) ? it->second.artist : t.folder_artist;
            };
            return lower(artist_of(a)) < lower(artist_of(b));
        });
    }
    // mode 0: leave as scanned (folder order) -- no-op
}

// Folder filter (HKeyFilterForFolder) stacks on top of the search/sort
// result rather than replacing it, so filtering-by-folder while a search is
// active narrows to just that folder's matches. Every place that rebuilds
// local_view_ must go through this (or refresh_local_view()): the background
// tag resolver re-filters the list every time new tags land, and a rebuild
// that skipped the folder filter silently undid 'f' a frame or two later.
std::vector<LocalTrack> App::filter_and_rank_local_view(const std::string& query) const {
    std::vector<LocalTrack> view = filter_and_rank_local(query);
    if (folder_filter_.empty()) return view;
    std::vector<LocalTrack> filtered;
    filtered.reserve(view.size());
    for (auto& t : view) {
        if (path_utf8(t.path.parent_path()) == folder_filter_) filtered.push_back(std::move(t));
    }
    return filtered;
}

void App::refresh_local_view() {
    local_view_ = filter_and_rank_local_view(last_local_query_);
    selected_ = 0;
    scroll_ = 0;
}

// Rebuilds the local list after something changed HOW rows are titled or
// ordered (the Shift+N metadata-only toggle), keeping the cursor on the same
// track instead of jumping to the top like refresh_local_view() does.
void App::resort_local_view_keep_selection() {
    if (list_source_ != ListSource::Local) return;
    fs::path prev;
    const bool had = selected_ >= 0 && selected_ < static_cast<int>(local_view_.size());
    if (had) prev = local_view_[static_cast<size_t>(selected_)].path;
    local_view_ = filter_and_rank_local_view(last_local_query_);
    selected_ = 0;
    if (had) {
        for (size_t i = 0; i < local_view_.size(); ++i)
            if (local_view_[i].path == prev) { selected_ = static_cast<int>(i); break; }
    }
    scroll_ = std::max(0, selected_ - list_nav_rows() / 2);
    if (scroll_ > selected_) scroll_ = selected_;
}

void App::settings_open() {
    text_disengage();
    settings_snapshot_ = settings_;
    settings_snapshot_text_ = settings_to_text(settings_);
    settings_dirty_ = false;
    leave_prompt_ = 0;
    theme_ui_ = 0;
    theme_list_ = themes::load_all();
}

void App::settings_update_dirty() {
    settings_dirty_ = settings_to_text(settings_) != settings_snapshot_text_;
}

// `s`: write config.txt and leave. ESC / q: leave and put back everything the screen changed.
void App::settings_close(bool save) {
    if (save) {
        save_settings(settings_);
        status_line_ = "SAVED";
    } else {
        settings_update_dirty();
        if (settings_dirty_) {
            const Settings before = settings_;
            settings_ = settings_snapshot_;
            // what was applied live while the screen was open
            cache_.set_download_dir(path_from_utf8(settings_.download_folder));
            if (before.history_path != settings_.history_path) {
                history_end_current_play();
                history_.save();
                history_.set_dir(settings_.history_path);
                if (history_.file_exists()) history_.load(); else history_.save();
            }
            player_.set_stereo(settings_.stereo);
            player_.set_normalization(settings_.normalize, static_cast<float>(settings_.normalize_target_lufs),
                                      static_cast<float>(settings_.normalize_max_boost_db));
            set_emoji_replacement(settings_.replace_emoji);
            if (before.local_music_paths != settings_.local_music_paths || before.download_folder != settings_.download_folder)
                rescan_library();
            force_redraw_ = true;
        }
        status_line_.clear();
    }
    settings_dirty_ = false;
    theme_ui_ = 0;
    mode_ = Mode::Browse;
}

// Re-runs the local library scan over whatever settings_.local_music_paths
// holds right now, then rebuilds the view on top of the result. This is
// what makes a path edited in the PATHS tab take effect immediately --
// config.txt used to promise "the library is scanned once at startup,
// there's no live rescan", and editing a local path from Settings now IS
// a live rescan (it still needs [S]/quit for the change to be written to
// config.txt, of course).
void App::rescan_library() {
    // Keep the streamed-track cache folder in the list even if the edit
    // happened to drop it, for the same reason the constructor injects it
    // at all: downloaded songs belong in the local view.
    std::string cache_dir_str = path_utf8(cache_.cache_dir());
    if (std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), cache_dir_str)
        == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(cache_dir_str);
    }
    // And the configured download folder, for the same reason: whatever
    // yt-dlp writes has to be scannable without a second LocalMusicPath
    // line, which is what the DOWNLOAD PATH setting promises. Skipped
    // while it is still the default -- that IS the cache dir just above.
    std::string download_dir_str = path_utf8(cache_.download_dir());
    if (download_dir_str != cache_dir_str
        && std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), download_dir_str)
               == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(download_dir_str);
    }

    // A local diagnostics vector, not local_scan_diagnostics_ -- that one
    // is read back once at startup by run() (before any rescan can happen)
    // and never again, so per-scan results go straight to the log instead.
    std::vector<std::string> diagnostics;
    all_local_tracks_ = local_source_.scan(settings_.local_music_paths, &diagnostics);
    search_idx_.clear();
    refresh_local_view(); // applies the active query/sort/filter to the new set
    for (const auto& line : diagnostics) ConsoleLog::instance().log_basic(line);

    // Newly added files need their tags resolved too -- for search and for
    // metadata-only rows. Re-arm the background sweep, which skips every
    // path already present in row_meta_cache_; if a previous sweep is
    // still winding down the two simply overlap, idempotently.
    row_meta_resolver_started_.store(false, std::memory_order_relaxed);
    launch_row_meta_resolver();
}

// Called on every keystroke while typing in the search box, before
// Enter is pressed — this is the "incremental search" behavior: the
// list updates live as you type instead of only after confirming. Local
// queries get filtered+ranked immediately via the same fuzzy logic
// submit_search() will commit on Enter. An "s:" (online search) prefix
// is left alone here — firing a network request on every keystroke
// would be wasteful and slow, so online search still only fires on
// Enter — but the view is reset back to whatever it was before '/' was
// pressed so a stale local preview doesn't linger behind the search box
// while an online query is being typed.
// The online search prefixes: "s:" YouTube, "sc:" SoundCloud, "b:" Bandcamp. Returns the source (0 / 1 / 2) and the
// prefix length, or -1 when `buf` starts with none of them.
static int online_prefix(const std::string& buf, size_t& len) {
    std::string l = buf.substr(0, 3);
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l.compare(0, 3, "sc:") == 0) { len = 3; return 1; }
    if (l.compare(0, 2, "s:") == 0) { len = 2; return 0; }
    if (l.compare(0, 2, "b:") == 0) { len = 2; return 2; }
    return -1;
}

void App::update_live_search_preview() {
    std::string buf = search_buffer_;
    while (!buf.empty() && buf.front() == ' ') buf.erase(buf.begin());
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();

    size_t plen = 0;
    if (online_prefix(buf, plen) >= 0) {   // online: nothing until ENTER
        list_source_ = pre_search_list_source_;
        local_view_ = filter_and_rank_local_view(pre_search_local_query_);
        selected_ = 0;
        scroll_ = 0;
        return;
    }

    // "p:" (playlist search) is local like a plain query -- no network
    // call to defer -- so unlike "s:" above, it's fine to actually run
    // the filter live on every keystroke rather than waiting for Enter.
    if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "p:") {
        std::string q = buf.substr(2);
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        list_source_ = ListSource::Playlist;
        playlist_view_ = filter_playlists(q);
        selected_ = 0;
        scroll_ = 0;
        return;
    }

    // "f:" (folder search) is local too -- filter the folder list live.
    if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "f:") {
        std::string q = buf.substr(2);
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        list_source_ = ListSource::Folder;
        folder_view_ = filter_folders(q);
        selected_ = 0;
        scroll_ = 0;
        return;
    }

    list_source_ = ListSource::Local;
    local_view_ = filter_and_rank_local_view(buf);
    selected_ = 0;
    scroll_ = 0;
}

void App::submit_search() {
    std::string buf = search_buffer_;
    // trim
    while (!buf.empty() && buf.front() == ' ') buf.erase(buf.begin());
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();

    size_t plen = 0;
    if (const int src = online_prefix(buf, plen); src >= 0) {   // s: YouTube, sc: SoundCloud, b: Bandcamp
        std::string query = buf.substr(plen);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_online_query_ = query;
        last_online_source_ = src;
        list_source_ = ListSource::Online;
        if (search_in_progress_.load()) {
            status_line_ = "still searching, hang on ...";
            return;
        }
        launch_search_async(query.empty() ? "music" : query, src);
    } else if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "p:") {
        std::string query = buf.substr(2);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_playlist_query_ = query;
        list_source_ = ListSource::Playlist;
        playlist_view_ = filter_playlists(query);
        selected_ = 0;
        scroll_ = 0;
    } else if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "f:") {
        std::string query = buf.substr(2);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_folder_query_ = query;
        list_source_ = ListSource::Folder;
        folder_view_ = filter_folders(query);
        selected_ = 0;
        scroll_ = 0;
    } else {
        last_local_query_ = buf;
        list_source_ = ListSource::Local;
        refresh_local_view();
    }
}

// "/f:" folder search: every folder that directly holds at least one scanned
// track, filtered by the query. Each whitespace-separated word of the query
// must appear (case-insensitively) in "<parent folder> <folder name>", so
// "beatles abbey" finds Music/The Beatles/Abbey Road. An empty query lists
// every folder. Sorted by folder name.
std::vector<FolderSummary> App::filter_folders(const std::string& query) const {
    std::map<std::string, FolderSummary> by_path;
    for (const auto& t : all_local_tracks_) {
        fs::path dir = t.path.parent_path();
        std::string key = path_utf8(dir);
        auto it = by_path.find(key);
        if (it == by_path.end()) {
            FolderSummary f;
            f.path = key;
            f.name = path_utf8(dir.filename());
            if (f.name.empty()) f.name = key; // drive root etc.
            f.parent = path_utf8(dir.parent_path().filename());
            f.track_count = 1;
            by_path.emplace(key, std::move(f));
        } else {
            ++it->second.track_count;
        }
    }

    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : query) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }

    std::vector<FolderSummary> out;
    out.reserve(by_path.size());
    for (auto& [key, f] : by_path) {
        const std::string hay = f.parent + " " + f.name;
        bool ok = true;
        for (const auto& w : words) if (!contains_ci(hay, w)) { ok = false; break; }
        if (ok) out.push_back(f);
    }
    std::stable_sort(out.begin(), out.end(), [](const FolderSummary& a, const FolderSummary& b) {
        return lower(a.name) < lower(b.name);
    });
    return out;
}

// Enter on a row of the "/f:" list: list all files of that folder in the
// LOCAL AUDIO FILES pane. Same mechanism as the 'f' filter (folder_filter_),
// so [c] clears it again and the pane title shows the folder name. Any
// active local query is dropped so really every file of the folder shows up.
void App::open_selected_folder() {
    if (folder_view_.empty() || selected_ < 0 || selected_ >= static_cast<int>(folder_view_.size())) return;
    folder_filter_ = folder_view_[static_cast<size_t>(selected_)].path;
    last_local_query_.clear();
    local_sort_mode_ = 0; // folder order, like 'f'
    list_source_ = ListSource::Local;
    refresh_local_view();
    log_event("filtered: " + path_utf8(path_from_utf8(folder_filter_).filename()));
}

// Local, case-insensitive substring match on playlist name -- cheap
// enough (just a directory scan) to re-run on every keystroke, same as
// the "p:" live preview above does.
std::vector<PlaylistSummary> App::filter_playlists(const std::string& query) const {
    auto all = playlist_summaries();
    if (query.empty()) return all;
    std::vector<PlaylistSummary> out;
    out.reserve(all.size());
    for (auto& p : all) if (contains_ci(p.name, query)) out.push_back(p);
    return out;
}

// The folder NEW playlists are written to and deleted from: the first
// configured PlaylistsPath (settings_.playlists_paths[0] -- config.txt's
// PlaylistsPath= line, editable from the PATHS tab's PLAYLIST PATH
// list), otherwise settings_.local_music_paths[0]/playlists (~/Music if
// none configured at all, which by the time this runs may itself have
// become the cache folder -- see load_library()'s cache-dir injection).
// Unrelated to HKeyDownloadStream's folder, which comes from the
// separate DOWNLOAD PATH setting (cache_.download_dir()) instead.
// Computed fresh every call, not cached, so it always reflects whatever
// the user currently has set in Settings.
fs::path App::playlists_dir() const {
    if (!settings_.playlists_paths.empty() && !settings_.playlists_paths[0].empty())
        return path_from_utf8(settings_.playlists_paths[0]);
    std::string base;
    if (!settings_.local_music_paths.empty()) {
        base = settings_.local_music_paths[0];
    } else {
        const char* home = std::getenv("HOME");
        base = home ? (std::string(home) + "/Music") : "./Music";
    }
    return path_from_utf8(base) / "playlists";
}

// Every folder playlists are searched in: all of the configured ones, or
// the single folder playlists_dir() resolves to when none is (so every
// caller always has at least one directory to look in). Blanks are
// skipped -- an empty entry is what a "+ new path" line looks like until
// the user has finished typing it -- and duplicates collapse, since
// scanning the same folder twice would just re-yield its playlists.
std::vector<fs::path> App::playlist_dirs() const {
    std::vector<fs::path> out;
    for (const auto& p : settings_.playlists_paths) {
        if (p.empty()) continue;
        fs::path dir = path_from_utf8(p);
        if (std::find(out.begin(), out.end(), dir) == out.end()) out.push_back(dir);
    }
    if (out.empty()) out.push_back(playlists_dir());
    return out;
}

// Playlists across every folder in playlist_dirs(), merged into one
// list: what both the main "/p:" list and the playlist editor's manage
// tab show. De-duplicated by case-insensitive name with the first folder
// containing it winning (so a row always resolves to one concrete file,
// wherever it gets opened from), then re-sorted with exactly the
// case-insensitive order PlaylistManager::list() already guarantees
// within a single folder -- so several configured folders read as one
// alphabetized list rather than as concatenated per-folder blocks.
std::vector<PlaylistSummary> App::playlist_summaries() const {
    std::vector<PlaylistSummary> merged;
    std::unordered_set<std::string> seen;
    for (const fs::path& dir : playlist_dirs()) {
        for (PlaylistSummary& s : PlaylistManager::list(dir)) {
            if (seen.insert(lower(s.name)).second) merged.push_back(std::move(s));
        }
    }
    std::sort(merged.begin(), merged.end(),
              [](const PlaylistSummary& a, const PlaylistSummary& b) { return lower(a.name) < lower(b.name); });
    return merged;
}

// Loads a playlist by name from whichever playlist_dirs() folder has it
// -- first match wins, the same rule playlist_summaries() de-duplicated
// with, so what the list showed always loads. nullopt when no folder
// holds that name (anymore).
std::optional<Playlist> App::load_playlist(const std::string& name) const {
    for (const fs::path& dir : playlist_dirs()) {
        if (auto pl = PlaylistManager::load(dir, name)) return pl;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------
// Playback start
// ---------------------------------------------------------------------

void App::write_load_timing_log(const std::string& title, bool is_local, double t_resolve,
                                 double t_probe, double t_total, const std::string& error) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    fs::path dir = path_from_utf8(home) / ".cache" / "mousiki";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream log(dir / "load_timing.log", std::ios::app);
    if (!log.is_open()) return;

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    log << timebuf << " track=\"" << title << "\" source=" << (is_local ? "local" : "online");
    if (!is_local) log << " resolve=" << t_resolve << "s";
    // This is now "time to first sound", not "time to fully decoded" —
    // decode itself streams in after this point, off the critical path.
    log << " probe=" << t_probe << "s time_to_playback=" << t_total << "s";
    if (!error.empty()) log << " ERROR=\"" << error << "\"";
    log << "\n";
}

void App::launch_load_async(fs::path local_path, std::string title, std::string artist,
                             std::string location_label, bool is_local, std::string video_id) {
    if (load_thread_.joinable()) load_thread_.join(); // previous job already signaled done, safe to reap
    load_in_progress_ = true;
    load_ready_ = false;
    load_stage_ = is_local ? 4 : 1;
    load_started_at_ = std::chrono::steady_clock::now();
    if (!is_local) status_line_ = "resolving \"" + title + "\" ...";

    // This thread ONLY resolves (online) and probes metadata/duration —
    // both fast, no full decode. It publishes a result and returns. Full
    // decode is a SEPARATE, detached thread spawned at the bottom, so
    // this thread (the one the main loop's next launch_load_async call
    // will join()) is never blocked waiting on decode — that's what
    // makes it safe to join from launch_load_async without risking a
    // freeze if the user switches tracks again quickly.
    const bool want_stereo = settings_.stereo; // read here, on the calling thread, rather than from inside the load thread
    const int sid_seconds = settings_.sid_play_length;
    load_thread_ = std::thread([this, local_path, title, artist, location_label, is_local, video_id, want_stereo, sid_seconds]() {
      run_guarded("track load", [&] {
        using clock = std::chrono::steady_clock;
        auto t_start = clock::now();
        auto elapsed_s = [](clock::time_point from) {
            return std::chrono::duration<double>(clock::now() - from).count();
        };

        PendingLoad pl;
        pl.title = title;
        pl.artist = artist;
        pl.location_label = location_label;
        pl.is_local = is_local;
        pl.video_id = video_id;

        double t_resolve = 0.0, t_probe = 0.0;

        fs::path path = local_path;
        if (!is_local) {
            load_stage_ = 1;
            auto t0 = clock::now();
            std::string err;
            auto resolved = youtube_.resolve_by_id(video_id, title, artist, &err);
            t_resolve = elapsed_s(t0);
            if (!resolved) {
                pl.error = "download failed: " + err;
                write_load_timing_log(title, is_local, t_resolve, 0, elapsed_s(t_start), pl.error);
                std::lock_guard<std::mutex> lk(load_mutex_);
                pending_load_ = std::move(pl);
                load_ready_ = true;
                return;
            }
            path = resolved->cached_path;
            pl.title = resolved->title;
            pl.artist = resolved->artist;
        }
        pl.path = path;

        // Tracker modules / game music (ffmpeg via libopenmpt / libgme) and SID tunes (rendered by sidplayfp into a
        // cached WAV, which is then decoded and probed instead of the .sid -- pl.path stays the .sid itself).
        fs::path decode_from = path;
        if (is_local && is_chiptune_file(path)) {
            std::string problem = chiptune_support_problem(path);
            if (problem.empty() && is_sid_file(path)) {
                fs::path wav;
                if (render_sid_to_wav(path, sid_seconds, wav, &problem)) decode_from = wav;
            }
            if (!problem.empty()) {
                pl.error = problem;
                write_load_timing_log(title, is_local, 0, 0, elapsed_s(t_start), pl.error);
                std::lock_guard<std::mutex> lk(load_mutex_);
                pending_load_ = std::move(pl);
                load_ready_ = true;
                return;
            }
        }

        load_stage_ = 4;
        auto t2 = clock::now();
        pl.metadata = probe_metadata(path, pl.title, pl.artist, pl.location_label);
        
        // Ensure lyrics fetch uses the real metadata tags instead of the filename/folder
        if (is_local) {
            if (!pl.metadata.name.empty() && pl.metadata.name != "-") {
                pl.title = pl.metadata.name;
            }
            if (!pl.metadata.artist.empty() && pl.metadata.artist != "-") {
                pl.artist = pl.metadata.artist;
            }
        }
        
        double duration = probe_duration_seconds(decode_from);
        t_probe = elapsed_s(t2);
        pl.total_sec = duration > 0 ? static_cast<size_t>(duration) : 0;

        pl.pcm = std::make_shared<StreamingPcm>();
        // the file's own sample rate: no resampling on the way to the device (it used to be 44.1 kHz for everything)
        pl.pcm->reserve_for_seconds(duration > 0 ? duration : 300.0,
                                    native_sample_rate(decode_from, pl.metadata.sampling, duration), want_stereo ? 2 : 1);
        pl.success = true;

        write_load_timing_log(pl.title, is_local, t_resolve, t_probe, elapsed_s(t_start), "");

        {
            std::lock_guard<std::mutex> lk(load_mutex_);
            pending_load_ = pl;
            load_ready_ = true;
        }

        // Decode continues independently from here — detached because it
        // may still be running when the user switches to a different
        // track, and the StreamingPcm it's filling stays alive via the
        // shared_ptr captured below (and via Player's own reference, if
        // this track is still the one playing) for exactly as long as it
        // needs to. Known tradeoff: if the app quits while a decode is
        // still in flight, that ffmpeg subprocess can be orphaned rather
        // than cleanly killed — worth fixing with real process-group
        // tracking later, not a correctness or crash risk today.
        std::shared_ptr<StreamingPcm> pcm = pl.pcm;
        fs::path decode_path = decode_from;
        std::string wtitle = pl.title, wartist = pl.artist;
        bool waveform_smooth = settings_.waveform_smooth; // captured by value — see below, avoids a cross-thread read of settings_
        std::thread([this, decode_path, pcm, waveform_smooth]() { run_guarded("track decode", [&] {
            if (pcm->windowed) {
                // A long track: held a window at a time (see StreamingPcm). This thread keeps the window filled
                // until nobody holds the track any more; a second one goes through the whole file once for the
                // loudness and the waveform.
                // abandoned: nobody but these decode threads holds the track any more (it was replaced, or the
                // load was superseded) -- they count themselves in internal_refs
                std::weak_ptr<StreamingPcm> watch = pcm;
                StreamingPcm* raw = pcm.get();
                auto abandoned = [watch, raw]() { return watch.use_count() <= raw->internal_refs.load(); };
                raw->internal_refs.store(1);   // this thread's `pcm`
                std::thread([this, decode_path, keep = pcm, waveform_smooth, abandoned]() { run_guarded("track analysis", [&] {
                    analyse_track(decode_path, *keep, abandoned);
                    if (!abandoned() && !keep->env_sumsq.empty()) {
                        auto envelope = WaveformQuantizer::envelope_from_bins(keep->env_sumsq, StreamingPcm::kEnvGran,
                                                                              keep->total_frames.load(), 4096, waveform_smooth);
                        std::lock_guard<std::mutex> lk(waveform_mutex_);
                        pending_waveform_envelope_ = std::move(envelope);
                        waveform_pending_ready_ = true;
                    }
                    keep->internal_refs.fetch_sub(1);   // before `keep` itself goes (never counted low)
                }); }).detach();
                raw->internal_refs.fetch_add(1);   // the analysis thread's `keep` (it exists already: never counted high)
                stream_decode_windowed(decode_path, *pcm, abandoned);
                return;
            }
            stream_decode_ffmpeg(decode_path, *pcm);

            // Deferred mini-waveform pass — only starts once decode is
            // fully done, never gates playback.
            if (!pcm->decode_failed.load()) {
                // PERF: decode is done — pcm->data is no longer being
                // written to, so we pass it directly as a const-ref
                // instead of making a full snapshot copy.  A 5-minute
                // track at 44100 Hz is ~50 MB; that copy was the single
                // biggest reason the waveform appeared so late after
                // playback started, because it doubled the working-set
                // size and stalled the RMS pass behind a large memcpy.
                auto envelope = WaveformQuantizer::generate_high_res_envelope(pcm->data, 4096, waveform_smooth, pcm->channels);
                std::lock_guard<std::mutex> lk(waveform_mutex_);
                pending_waveform_envelope_ = std::move(envelope);
                waveform_pending_ready_ = true;
            }
        }); }).detach();
      });

      // If the guard above swallowed an exception, nothing published a
      // result -- and poll_pending_load() is the only thing that clears
      // load_in_progress_, so the app would refuse to start any track for
      // the rest of the session ("still loading the previous track ...").
      // Publish a failed load instead so the UI recovers and says why.
      if (!load_ready_.load()) {
          std::lock_guard<std::mutex> lk(load_mutex_);
          PendingLoad failed;
          failed.title = title;
          failed.is_local = is_local;
          failed.success = false;
          failed.error = "couldn't load \"" + title + "\" -- see the console log (t)";
          pending_load_ = std::move(failed);
          load_ready_ = true;
      }
    });
}

// Used to block here waiting for the previous track's fetch thread to
// finish (join()) before starting a new one. That's a real main-thread
// stall: whenever lyrics aren't quickly available (still mid network-call
// chain) and the user skips to another track before it resolves,
// switching songs would hang until the abandoned fetch finished.
// Detaching instead, with an epoch guard so a late-arriving stale result
// just gets discarded rather than clobbering the new/retried track's
// lyrics. Shared by the initial per-track fetch (poll_pending_load) and
// the manual retry hotkey (handle_key's 'l' case).
void App::launch_lyrics_fetch(std::string title, std::string artist, fs::path path, bool force_network) {
    lyrics_ready_ = false;
    lyrics_path_ = path;
    int my_epoch = ++lyrics_epoch_;
    const double track_secs = total_sec_;
    std::thread([this, title, artist, path, force_network, my_epoch, track_secs]() { run_guarded("lyrics fetch", [&] {
        LyricsResult r = fetch_synced_lyrics(title, artist, path_utf8(lyrics_script_), path, force_network, track_secs);
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (my_epoch != lyrics_epoch_.load()) return; // a newer/retried fetch has since started — discard
        lyrics_result_ = std::move(r);
        lyrics_ready_ = true;
    }); }).detach();
}

void App::poll_pending_load() {
    if (!load_ready_.load()) return;
    PendingLoad pl;
    {
        std::lock_guard<std::mutex> lk(load_mutex_);
        // BUG FIX #4: clear the flag while still holding load_mutex_ so
        // the load thread cannot race-write pending_load_ again in the
        // window between the copy and the flag reset.
        if (!load_ready_.load()) return; // re-check under lock (spurious wakeup guard)
        pl = std::move(pending_load_);   // moved, not copied: a copy left here kept the track's audio alive
        pending_load_ = PendingLoad{};
        load_ready_ = false;
    }
    load_in_progress_ = false;
    load_stage_ = 0;
    const bool was_advancing = advancing_;
    advancing_ = false;

    if (!pl.success) {
        status_line_ = pl.error;
        // An automatic advance whose next track failed to load: the previous
        // track is over, so stop treating it as current (otherwise its
        // finished flag would immediately trigger another advance).
        if (was_advancing) {
            has_track_ = false;
            // ...and the play it left behind is over too -- nothing took its
            // place, so close the record now (it was heard to the end; the
            // next track simply never arrived). A FAILED manual selection is
            // deliberately not recorded: whatever was playing keeps playing
            // and its play must stay live.
            history_end_current_play();
        }
        return;
    }

    // Player::play() stops whatever it was previously playing as its own
    // first step, so no separate explicit stop() call is needed here —
    // and doing it inside play() (below, off the main thread) is what
    // lets this whole switch never touch the main thread.
    current_pcm_ = pl.pcm;
    total_sec_ = pl.total_sec;
    metadata_ = pl.metadata;
    current_path_ = pl.path;
    current_is_local_ = pl.is_local;
    // Edits left pending by an earlier run belong on this screen too (display_metadata()), not only in the editor.
    if (pl.is_local) meta_ensure_session_loaded();
    current_video_id_ = pl.video_id;
    current_playlist_ = load_playlist_ctx_;
    has_track_ = true;
    // Listening history, in this exact order: the play that just ended is
    // closed BEFORE clear_finished() (its "was it heard to the end?" verdict
    // reads player_.finished(), which the next line resets) and the new one
    // is opened AFTER current_* have been reassigned (they are what
    // history_begin_current_play() records). No history record exists for a
    // failed load -- see the branch above.
    history_end_current_play();
    player_.clear_finished(); // see clear_finished()'s comment — closes the race that caused the double-skip bug
    history_begin_current_play();
    waveform_envelope_.clear();
    waveform_ready_ = false;
    waveform_pending_ready_ = false;
    ++waveform_epoch_; // BUG FIX #5: invalidate any in-flight waveform from the previous track
    last_lyrics_status_.clear();
    fft_.reset(); // don't let the previous track's spectrum tail linger into this one's first frame
    scope_.reset(); // ...and don't let the previous track's waveform linger in the oscilloscope either

    // "Lyrics Engine" (settings_.element_lyrics) used to only hide the
    // panel -- fetch_synced_lyrics() still ran, still spawned Python, and
    // still hit the network for every single track, whether or not
    // anything was ever shown. Toggling it off now actually turns the
    // feature off, matching what the settings label already claimed.
    if (settings_.lyrics_on()) {
        launch_lyrics_fetch(pl.title, pl.artist, pl.path);
    } else {
        // nothing fetched for this track: the lyrics of the last one must not count as its own (the karaoke
        // overlay fetches them itself when it opens, see karaoke_ensure_lyrics())
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        ++lyrics_epoch_;   // a fetch still running for the last track is discarded
        lyrics_path_.clear();
        lyrics_result_ = LyricsResult{};
        lyrics_ready_ = false;
    }

    // This is the whole point of the redesign: play() is handed a
    // StreamingPcm that may have zero frames decoded yet. The audio
    // callback plays silence for anything past what's been decoded and
    // self-corrects the instant more arrives — so sound starts the
    // moment decode produces its first chunk, not after the whole track.
    // Dispatched off the main thread — see launch_device_play_async().
    launch_device_play_async();
    status_line_.clear();
}

void App::launch_device_play_async() {
    int my_gen = ++device_gen_;
    // Raise the handoff guard BEFORE the request is visible to the worker:
    // from here until player_.play() returns, finished_ still describes the
    // old track and must not drive advance_track() (see device_play_pending_gen_).
    device_play_pending_gen_.store(my_gen);
    auto pcm = current_pcm_;
    int vol = player_.volume() > 0 ? player_.volume() : 70;
    // One-shot resume position from a restored snapshot -- consumed
    // here exactly once, then zeroed so every subsequent track change
    // (skip, search-and-play, queue advance, ...) starts at 0 like
    // always. Reading+clearing it up front (still on the main thread,
    // before it's handed to the worker) avoids any race with a second
    // restore attempt -- there isn't one, but this keeps that invariant
    // obvious rather than implicit.
    double start_sec = resume_start_sec_;
    resume_start_sec_ = 0.0;

    // Post to device_worker_loop() rather than spawning a thread here.
    // There's only one request slot, not a queue: if the worker is still
    // busy with an older request when a newer one lands, this simply
    // overwrites it in place before the worker ever reads it, so only the
    // latest survives -- the same "a superseded switch is silently
    // dropped" behaviour the old generation-counter design gave, just
    // enforced by construction instead of by a check the stale thread had
    // to remember to perform.
    {
        std::lock_guard<std::mutex> lk(device_request_mutex_);
        device_request_.pcm = std::move(pcm);
        device_request_.volume = vol;
        device_request_.start_sec = start_sec;
        device_request_.generation = my_gen;
        device_request_ready_ = true;
    }
    device_request_cv_.notify_one();
}

void App::start_device_worker() {
    device_worker_thread_ = std::thread(&App::device_worker_loop, this);
}

void App::stop_device_worker() {
    {
        std::lock_guard<std::mutex> lk(device_request_mutex_);
        device_worker_stop_ = true;
    }
    device_request_cv_.notify_one();
}

void App::device_worker_loop() {
    for (;;) {
        DevicePlayRequest req;
        {
            std::unique_lock<std::mutex> lk(device_request_mutex_);
            device_request_cv_.wait(lk, [this] { return device_request_ready_ || device_worker_stop_; });
            if (device_worker_stop_) return;   // quitting -- don't start one more track on the way out
            req = device_request_;
            device_request_ready_ = false;
        }
        // Defensive only: with a single overwritable slot rather than a
        // real queue, req.generation should already equal device_gen_ by
        // construction every time this fires.
        if (req.generation != device_gen_.load()) continue;
        // Guarded like every other worker: this loop lives for the whole
        // session, and an exception escaping it would take the process with
        // it rather than just failing one track.
        run_guarded("audio device start", [&] {
            std::lock_guard<std::mutex> lk(player_mutex_);
            player_.play(req.pcm, req.start_sec, req.volume, &fft_, &scope_);
        });
        // The new track is in and the old pcm (and its latching finished_
        // flag) is gone, so the main loop may look at finished_ again. Only
        // clear if no newer handoff has been posted meanwhile -- that one is
        // still in flight and owns the guard now.
        int expected = req.generation;
        device_play_pending_gen_.compare_exchange_strong(expected, 0);
    }
}

void App::poll_pending_waveform() {
    if (!waveform_pending_ready_.load()) return;
    std::vector<float> envelope;
    {
        std::lock_guard<std::mutex> lk(waveform_mutex_);
        envelope = std::move(pending_waveform_envelope_);
    }
    waveform_pending_ready_ = false;
    waveform_envelope_ = std::move(envelope);
    waveform_ready_ = true;
    waveform_reveal_start_ = std::chrono::steady_clock::now(); // starts the 700ms left-to-right reveal
}

void App::launch_search_async(const std::string& query, int source) {
    if (search_thread_.joinable()) search_thread_.join();
    search_in_progress_ = true;
    search_ready_ = false;
    static const char* const kWhere[3] = {"YouTube", "SoundCloud", "Bandcamp"};
    status_line_ = std::string("searching ") + kWhere[std::clamp(source, 0, 2)] + " for \"" + query + "\" ...";
    const std::string bc_script = path_utf8(find_scripts_file("bandcamp_search.py"));

    search_thread_ = std::thread([this, query, source, bc_script]() { run_guarded("online search", [&] {
        auto results = source == 1 ? online_.search_soundcloud(query, 15)
                     : source == 2 ? online_.search_bandcamp(query, 15, bc_script)
                     : online_.search(query, /*count=*/15, path_utf8(fast_search_script_));
        std::lock_guard<std::mutex> lk(search_mutex_);
        pending_search_results_ = std::move(results);
    }); 
        // Set outside the guard: poll_pending_search() waits on this flag,
        // so it has to be raised even when the search threw, or the UI sits
        // on "searching ..." forever.
        search_ready_ = true;
    });
}

void App::poll_pending_search() {
    if (!search_ready_.load()) return;
    std::vector<OnlineResult> results;
    {
        std::lock_guard<std::mutex> lk(search_mutex_);
        results = std::move(pending_search_results_);
    }
    search_ready_ = false;
    search_in_progress_ = false;

    online_view_ = std::move(results);
    selected_ = 0;
    scroll_ = 0;
    status_line_ = online_view_.empty() ? "no online results" : "";
}

void App::play_selected() {
    // Playlists aren't "played" directly -- there's no single track to
    // start. Enter on a playlist row queues everything in it instead
    // (see playlist_add_selected_to_queue()), same as the user pressing
    // "a" on it would.
    if (list_source_ == ListSource::Playlist) { playlist_add_selected_to_queue(); return; }
    // Folders aren't played either -- Enter opens the folder's files.
    if (list_source_ == ListSource::Folder) { open_selected_folder(); return; }
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    if (list_source_ == ListSource::Local) start_local_track(local_view_[selected_]);
    else start_online_track(online_view_[selected_]);
}

int App::current_track_list_index() const {
    if (!has_track_) return -1;
    if (list_source_ == ListSource::Playlist || list_source_ == ListSource::Folder) return -1; // no "now playing" identity in a list of playlist/folder names
    if (list_source_ == ListSource::Local) {
        if (!current_is_local_) return -1; // playing an online track while browsing the local list
        for (size_t i = 0; i < local_view_.size(); ++i) {
            if (local_view_[i].path == current_path_) return static_cast<int>(i);
        }
        return -1;
    } else {
        if (current_is_local_) return -1; // playing a local track while browsing online results
        for (size_t i = 0; i < online_view_.size(); ++i) {
            if (online_view_[i].video_id == current_video_id_) return static_cast<int>(i);
        }
        return -1;
    }
}

void App::play_relative(int delta) {
    // "next/previous track" has no meaning while browsing a list of
    // playlist names rather than tracks.
    if (list_source_ == ListSource::Playlist || list_source_ == ListSource::Folder) return;
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0) return;
    // Relative to what's actually *playing*, not wherever the hover
    // cursor happens to be sitting -- falls back to the hover cursor
    // only when there's no sensible "current" position in this list
    // (nothing playing yet, or what's playing is from a different
    // source/isn't in this view at all).
    int base = current_track_list_index();
    if (base < 0) base = selected_;
    selected_ = std::clamp(base + delta, 0, static_cast<int>(list_len) - 1);
    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
    if (selected_ < scroll_) scroll_ = selected_;
    play_selected();
}

void App::play_relative_random() {
    if (list_source_ == ListSource::Playlist || list_source_ == ListSource::Folder) return;
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0) return;
    if (list_len == 1) { selected_ = 0; play_selected(); return; }
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(list_len) - 1);
    int base = current_track_list_index();
    if (base < 0) base = selected_;
    int next;
    do { next = dist(rng); } while (next == base);
    selected_ = next;
    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
    if (selected_ < scroll_) scroll_ = selected_;
    play_selected();
}

void App::queue_reset_lap() {
    for (auto& q : queue_) q.played = false;
}

void App::play_next_from_queue() {
    // Shuffle: pick a random queue item instead of strictly FIFO order.
    // What happens to the item once it is taken depends on the lock ("!"):
    //   * LOCKED (the default): it goes to the END of the queue, so the queue
    //     keeps all its tracks and loops indefinitely instead of draining.
    //   * UNLOCKED: it leaves the queue for good once it is played.
    // Queue-then-stop (play mode 4) on a locked queue plays the queue ONCE:
    // every item that goes to the back is flagged `played`, the next one is
    // the first item not flagged yet, and advance_track() stops playback once
    // none is left (the order is then back to what it was).
    int idx = 0;
    queue_next_run_ = 0; // the head is moving: "a" starts a fresh run
    const bool queue_once = settings_.play_mode == 4 && queue_locked_;
    if (queue_once) {
        auto first = std::find_if(queue_.begin(), queue_.end(), [](const QueueItem& q) { return !q.played; });
        if (first == queue_.end()) {   // a manual skip after the pass was over: start a new pass
            queue_reset_lap();
            first = queue_.begin();
        }
        idx = static_cast<int>(first - queue_.begin());
    } else {
        queue_reset_lap(); // the flags only mean something in queue-then-stop on a locked queue
        if (settings_.play_mode == 2 /*shuffle*/ && queue_.size() > 1) {
            static std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, static_cast<int>(queue_.size()) - 1);
            idx = dist(rng);
        }
    }
    QueueItem item = queue_[idx];
    queue_.erase(queue_.begin() + idx);
    if (queue_locked_) {                       // played track -> end of the list
        QueueItem back = item;
        back.played = queue_once;
        queue_.push_back(std::move(back));
    }
    if (queue_selected_ >= idx && queue_selected_ > 0) --queue_selected_; // index shifted down by the erase
    clamp_queue_selected();
    pending_queue_playlist_ = item.playlist;   // consumed by start_*_track() below; any other start leaves it empty
    if (item.is_local) {
        LocalTrack t{path_utf8(item.local_path.stem()), item.local_path, item.artist};
        start_local_track(t);
    } else {
        OnlineResult r{item.video_id, item.title, item.artist};
        start_online_track(r);
    }
}

void App::advance_track() {
    // has_track_ is deliberately NOT cleared up front. Clearing it here made
    // the metadata panel show "no track loaded" for the gap between one track
    // ending and the next one's load finishing (and made play_relative()
    // measure "next" from the hover cursor instead of from what was actually
    // playing). It's only cleared where playback really ends: Stop mode, or
    // when nothing could be started / the load failed (see below and
    // poll_pending_load()).

    // Repeat: keep replaying whatever just finished -- whether it came
    // from the queue or the library -- without touching the queue or
    // advancing through any list at all. Stop: don't auto-advance into
    // anything, queue or not. Both apply uniformly regardless of the
    // queue's state now -- previously these were only ever consulted
    // once the queue was already empty, which was the other half of the
    // "queue mode won't respect repeat" bug.
    // Sleep timer's "stop after current song": a one-shot, checked before
    // everything else so it beats Repeat and the queue. It is NOT the Stop play
    // mode -- play_mode is left exactly as it was -- it just ends playback the
    // same way Stop mode does and clears itself.
    if (sleep_stop_after_track_) {
        sleep_stop_after_track_ = false;
        has_track_ = false;
        history_end_current_play(); // playback really ends here: nothing follows it
        status_line_ = "sleep timer: stopped after the song";
        return;
    }
    if (settings_.play_mode == 1 /*loop*/) {
        launch_device_play_async(); // same track, already fully decoded, no reload needed
        has_track_ = true;
        // One play of it just ended and looping it starts another one -- the
        // record is closed (verdict still readable: clear_finished() is
        // right below) and a fresh one opened, which is what makes a looped
        // track count as a replay instead of one endless play.
        history_end_current_play();
        player_.clear_finished();
        history_begin_current_play();
        return;
    }
    if (settings_.play_mode == 3 /*stop*/) {
        has_track_ = false; // the only mode where "no track loaded" is the right message
        history_end_current_play(); // playback really ends here: nothing follows it
        return; // no auto-advance -- queue or not
    }

    // Queue then stop (play mode 4): the queue is played through once and
    // playback ends there -- it never falls through to the library. Unlocked,
    // that is simply "the queue is empty" (played tracks leave it); locked,
    // played tracks stay (at the back), so the pass is over once every item
    // is flagged as played.
    if (settings_.play_mode == 4) {
        bool pass_over = queue_.empty();
        if (!pass_over && queue_locked_) {
            pass_over = std::none_of(queue_.begin(), queue_.end(), [](const QueueItem& q) { return !q.played; });
        }
        if (pass_over) {
            queue_reset_lap();  // the next pass starts fresh
            has_track_ = false;
            history_end_current_play(); // playback really ends here: nothing follows it
            log_event("queue finished: stopped");
            return;
        }
    }

    // The queue always takes priority over the library — it's an
    // explicit user-built-up-next list.
    advancing_ = true; // suppress re-entry until poll_pending_load() reports back
    if (!queue_.empty()) {
        play_next_from_queue();
    } else {
        switch (settings_.play_mode) {
            case 2: // shuffle
                play_relative_random();
                break;
            default: // list (sequential); queue-then-stop (4) never gets here,
                     // it stopped above once the queue was used up
                play_relative(1);
                break;
        }
    }
    // Nothing got started (empty list, ...): playback has genuinely ended.
    if (!load_in_progress_.load()) {
        advancing_ = false;
        has_track_ = false;
        history_end_current_play(); // the finished track has no successor: close its record
    }
}

char App::play_mode_letter() const {
    switch (settings_.play_mode) {
        case 1: return 'R';  // repeat (loop current track)
        case 2: return 'S';  // shuffle
        case 3: return 'O';  // stop (play, then stop -- not "S", shuffle already owns that)
        case 4: return 'Q';  // queue, then stop
        default: return 'L'; // list (normal sequential)
    }
}

// "a" -- the hovering track goes in as NEXT: the front of the queue (the head
// is always what plays next, locked or not). Consecutive presses
// keep their order (queue_next_run_), so A, B, C play as A, B, C.
void App::queue_add_selected() { queue_add_selected_impl(false); }

// "e" -- the hovering track goes to the END of the queue.
void App::queue_add_selected_end() { queue_add_selected_impl(true); }

void App::queue_add_selected_impl(bool at_end) {
    // A playlist row queues the whole playlist; that always appends (there is
    // no meaningful "next" for a block of tracks), for "a" and "e" alike.
    if (list_source_ == ListSource::Playlist) { playlist_add_selected_to_queue(); return; }
    if (list_source_ == ListSource::Folder) { status_line_ = "Enter opens the folder -- then add its tracks"; return; }
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    QueueItem item;
    if (list_source_ == ListSource::Local) {
        const auto& t = local_view_[selected_];
        item = {true, t.title, t.folder_artist, t.path, ""};
    } else {
        const auto& r = online_view_[selected_];
        item = {false, r.title, r.uploader, {}, r.video_id};
    }
    if (at_end) {
        queue_.push_back(std::move(item));
    } else {
        const int pos = std::clamp(queue_next_run_, 0, static_cast<int>(queue_.size()));
        queue_.insert(queue_.begin() + pos, std::move(item));
        ++queue_next_run_;
    }
    clamp_queue_selected();
    queue_reveal(at_end ? static_cast<int>(queue_.size()) - 1 : std::clamp(queue_next_run_ - 1, 0, static_cast<int>(queue_.size()) - 1));
}

// Main UI: Enter (or "a") on a playlist row while list_source_==Playlist.
// Loads the playlist from disk and queues every track that's still
// present on disk, skipping (and reporting) any that aren't.
void App::playlist_add_selected_to_queue() {
    if (playlist_view_.empty() || selected_ < 0 || selected_ >= static_cast<int>(playlist_view_.size())) return;
    const auto& summary = playlist_view_[selected_];
    auto pl = load_playlist(summary.name);
    if (!pl) { status_line_ = "could not load \"" + summary.name + "\""; return; }

    int added = 0, skipped = 0;
    for (auto& t : pl->tracks) {
        if (t.missing) { ++skipped; continue; }
        queue_.push_back({true, t.title, t.artist, t.path, "", false, summary.name});
        ++added;
    }
    clamp_queue_selected();
    queue_reveal(static_cast<int>(queue_.size()) - 1);
    status_line_ = "queued " + std::to_string(added) + " track" + (added == 1 ? "" : "s")
                 + " from \"" + summary.name + "\""
                 + (skipped > 0 ? " (" + std::to_string(skipped) + " missing, skipped)" : "");
}

void App::queue_remove_last() {
    if (!queue_.empty()) queue_.pop_back();
    queue_next_run_ = 0;
    clamp_queue_selected();
}

// Moves the queue window so that entry `idx` (just added) is visible -- the new track is the confirmation that "a" / "e" worked.
// The cursor stays where it was; clamp_queue_selected() would pull the window back to it, so this runs after that.
void App::queue_reveal(int idx) {
    if (queue_.empty()) return;
    idx = std::clamp(idx, 0, static_cast<int>(queue_.size()) - 1);
    const int rows = std::max(1, queue_nav_rows());
    if (idx < queue_scroll_) queue_scroll_ = idx;
    else if (idx >= queue_scroll_ + rows) queue_scroll_ = idx - rows + 1;
    queue_scroll_ = std::clamp(queue_scroll_, 0, std::max(0, static_cast<int>(queue_.size()) - rows));
}

void App::clamp_queue_selected() {
    if (queue_.empty()) { queue_selected_ = 0; queue_scroll_ = 0; return; }
    queue_selected_ = std::clamp(queue_selected_, 0, static_cast<int>(queue_.size()) - 1);
    // queue_nav_rows() is the big overlay's row count while it is open, the
    // small pane's otherwise. The window is also kept inside the queue so
    // no empty rows trail the last entry once the queue is long enough.
    const int rows = std::max(1, queue_nav_rows());
    queue_scroll_ = std::clamp(queue_scroll_, 0, std::max(0, static_cast<int>(queue_.size()) - rows));
    if (queue_selected_ >= queue_scroll_ + rows) queue_scroll_ = queue_selected_ - rows + 1;
    if (queue_selected_ < queue_scroll_) queue_scroll_ = queue_selected_;
}

void App::queue_remove_hovering() {
    if (queue_.empty() || queue_selected_ < 0 || queue_selected_ >= static_cast<int>(queue_.size())) return;
    const int idx = queue_selected_;
    queue_.erase(queue_.begin() + idx);
    queue_next_run_ = 0;
    clamp_queue_selected();
}

void App::queue_clear() {
    const size_t n = queue_.size();
    // One level of undo (Ctrl+Shift+Z): what was just cleared.
    if (n > 0) queue_undo_ = queue_;
    queue_.clear();
    queue_next_run_ = 0;
    clamp_queue_selected(); // empty queue -> cursor and scroll back to 0
    const std::string undo_key = hotkey_text("HKeyQueueUndoClear", "CTRL+SHIFT+Z");
    log_event("queue cleared (" + std::to_string(n) + " track" + (n == 1 ? "" : "s") + " removed)"
              + (undo_key == "-" ? std::string() : " -- " + undo_key + " undoes it"));
}

// Ctrl+Shift+Z. Puts the last cleared queue back in FRONT of whatever was
// queued since, then forgets the backup (single level).
void App::queue_undo_clear() {
    if (queue_undo_.empty()) { status_line_ = "nothing to undo -- the queue has not been cleared"; return; }
    const int n = static_cast<int>(queue_undo_.size());
    queue_.insert(queue_.begin(), queue_undo_.begin(), queue_undo_.end());
    queue_undo_.clear();
    queue_next_run_ = 0;
    clamp_queue_selected();
    log_event("queue restored (" + std::to_string(n) + " track" + (n == 1 ? "" : "s") + ")");
}

// "!" -- lock / unlock (locked is the default). Locked: a played track goes to
// the end of the queue, so the queue loops. Unlocked: a played track leaves it.
void App::queue_toggle_lock() {
    queue_locked_ = !queue_locked_;
    queue_next_run_ = 0;
    queue_reset_lap();
    log_event(queue_locked_ ? "queue locked: played tracks move to the end of the queue"
                            : "queue unlocked: played tracks leave the queue");
}

// The head of the queue is always what plays next, so a manual move only has
// to end the current "a" run.
void App::queue_after_move(int /*from*/, int /*to*/) {
    queue_next_run_ = 0;
}

void App::queue_move_hovering(int dir) {
    if (queue_.empty()) return;
    int target = queue_selected_ + dir;
    if (target < 0 || target >= static_cast<int>(queue_.size())) return; // already at an edge
    std::swap(queue_[queue_selected_], queue_[target]);
    queue_after_move(queue_selected_, target);
    queue_selected_ = target;
    clamp_queue_selected();
}

// Shift+4 / Shift+5: the hovering item to the very top / bottom.
void App::queue_move_to_edge(int dir) {
    if (queue_.empty()) return;
    const int from = std::clamp(queue_selected_, 0, static_cast<int>(queue_.size()) - 1);
    const int to = (dir < 0) ? 0 : static_cast<int>(queue_.size()) - 1;
    if (from == to) return; // already there
    QueueItem item = std::move(queue_[from]);
    queue_.erase(queue_.begin() + from);
    queue_.insert(queue_.begin() + to, std::move(item));
    queue_after_move(from, to);
    queue_selected_ = to;
    clamp_queue_selected();
}

// Ctrl+Shift+U: the queue becomes a new playlist in the playlist editor, with
// the name field focused so it only needs a name and HOME. Playlists are
// local-files-only, so streamed (online) queue entries are left out and
// counted in the status line.
void App::queue_to_playlist() {
    if (queue_.empty()) { status_line_ = "the queue is empty -- nothing to save as a playlist"; return; }
    std::vector<PlaylistTrack> tracks;
    int skipped = 0;
    for (const auto& q : queue_) {
        if (!q.is_local) { ++skipped; continue; }
        bool dup = false;
        for (const auto& t : tracks) { if (t.path == q.local_path) { dup = true; break; } }
        if (dup) continue;
        PlaylistTrack pt;
        pt.title = q.title;
        pt.artist = q.artist;
        pt.path = q.local_path;
        pt.missing = false;
        tracks.push_back(std::move(pt));
    }
    if (tracks.empty()) { status_line_ = "no local tracks in the queue (playlists hold local files only)"; return; }
    const int n = static_cast<int>(tracks.size());
    playlist_open_editor();           // fresh editor, tab 0
    playlist_edit_tracks_ = std::move(tracks);
    playlist_edit_track_selected_ = 0;
    playlist_edit_focus_ = 0;         // the name field
    playlist_edit_dirty_ = true;
    playlist_status_ = "queue -> playlist: " + std::to_string(n) + " track" + (n == 1 ? "" : "s")
                     + (skipped > 0 ? " (" + std::to_string(skipped) + " online skipped)" : "")
                     + " -- type a name, Ctrl+S saves";
}

// ---------------------------------------------------------------------
// Autosave / session snapshot
// ---------------------------------------------------------------------

SnapshotData App::build_snapshot() const {
    SnapshotData snap;
    snap.play_mode = settings_.play_mode;
    snap.muted = muted_;
    // Save the *real* volume, not the forced-0 muted value, so unmuting
    // next session restores to what it actually was, not silence.
    snap.volume = muted_ ? pre_mute_volume_ : player_.volume();

    if (has_track_) {
        snap.has_now_playing = true;
        snap.now_playing.is_local = current_is_local_;
        snap.now_playing.path = current_is_local_ ? path_utf8(current_path_) : std::string();
        snap.now_playing.video_id = current_is_local_ ? std::string() : current_video_id_;
        snap.now_playing.title = metadata_.name;
        snap.now_playing.artist = metadata_.artist;
        snap.position_sec = player_.poll_elapsed();
    }

    snap.queue_locked = queue_locked_;
    for (const auto& item : queue_) {
        SnapshotTrack t;
        t.is_local = item.is_local;
        t.path = item.is_local ? path_utf8(item.local_path) : std::string();
        t.video_id = item.is_local ? std::string() : item.video_id;
        t.title = item.title;
        t.artist = item.artist;
        t.playlist = item.playlist;
        snap.queue.push_back(std::move(t));
    }
    return snap;
}

void App::restore_snapshot(const SnapshotData& snap) {
    settings_.play_mode = std::clamp(snap.play_mode, 0, 4);
    // Apply the saved volume first, then re-apply mute on top of it --
    // mirrors what pressing 'x' does at runtime (force 0, remember the
    // real value), just seeded from the snapshot instead of live state.
    pre_mute_volume_ = std::clamp(snap.volume, 0, 100);
    player_.set_volume(pre_mute_volume_);
    if (snap.muted) {
        player_.set_volume(0);
        muted_ = true;
    }

    queue_.clear();
    queue_locked_ = snap.queue_locked;
    queue_next_run_ = 0;
    for (const auto& t : snap.queue) {
        queue_.push_back({t.is_local, t.title, t.artist, t.is_local ? path_from_utf8(t.path) : fs::path(), t.video_id, false, t.playlist});
    }
    clamp_queue_selected();

    if (snap.has_now_playing) {
        resume_start_sec_ = std::max(0.0, snap.position_sec);
        if (snap.now_playing.is_local) {
            fs::path p = path_from_utf8(snap.now_playing.path);
            std::error_code ec;
            if (fs::exists(p, ec)) {
                LocalTrack t{path_utf8(p.stem()), p, snap.now_playing.artist};
                start_local_track(t);
                log_event("resuming: " + t.title);
            } else {
                resume_start_sec_ = 0.0; // file's gone -- nothing to resume into
            }
        } else if (!snap.now_playing.video_id.empty()) {
            OnlineResult r{snap.now_playing.video_id, snap.now_playing.title, snap.now_playing.artist};
            start_online_track(r);
        } else {
            resume_start_sec_ = 0.0;
        }
    }
}

void App::maybe_autosave() {
    if (!settings_.autosave_enabled) return;
    double delay = std::max(1, settings_.autosave_delay_sec);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_autosave_at_).count();
    if (elapsed < delay) return;

    last_autosave_at_ = std::chrono::steady_clock::now();
    save_snapshot(build_snapshot());
    autosave_pulse_active_ = true;
    autosave_pulse_started_at_ = last_autosave_at_;
    ConsoleLog::instance().log_basic("autosaved session snapshot");
}

std::string App::autosave_indicator_glyph() const {
    if (!settings_.autosave_enabled || !settings_.autosave_indicator) return "";

    double t = autosave_pulse_active_
             ? std::chrono::duration<double>(std::chrono::steady_clock::now() - autosave_pulse_started_at_).count()
             : kAutosavePulseSeconds + 1.0; // idle -- well past the pulse window

    std::string glyph = settings_.autosave_chr.empty() ? "\u2022" : settings_.autosave_chr;

    if (settings_.autosave_indicator_type == 0) {
        // blink: appear/disappear twice (4 half-cycles) right after a
        // save, then settle back to hidden until the next one.
        if (t >= kAutosavePulseSeconds) return ""; // idle: hidden between saves
        int half_cycle = static_cast<int>(t / (kAutosavePulseSeconds / 4.0));
        bool visible = (half_cycle % 2) == 0;
        return visible ? glyph : " ";
    }

    // color: always visible, pulses C1 -> C2 -> C1 (heartbeat) right
    // after a save, then settles to a steady C1.
    std::string c1 = ansi_for(settings_.autosave_c1.empty() ? settings_.border_color : settings_.autosave_c1, false);
    std::string c2 = ansi_for(settings_.autosave_c2.empty() ? settings_.visualizer_color : settings_.autosave_c2, false);
    if (t >= kAutosavePulseSeconds) return c1 + glyph + "\x1b[0m"; // idle: steady C1
    // Two full heartbeats across the pulse window: C1->C2->C1->C2->C1.
    double phase = std::fmod(t, kAutosavePulseSeconds / 2.0) / (kAutosavePulseSeconds / 2.0); // 0..1 within each half-beat
    bool towards_c2 = phase < 0.5;
    return (towards_c2 ? c2 : c1) + glyph + "\x1b[0m";
}

// ---------------------------------------------------------------------
// Bulk add (paste a YouTube playlist link while Queue is focused)
// ---------------------------------------------------------------------

void App::launch_bulk_add_async(const std::string& url) {
    if (bulk_add_thread_.joinable()) bulk_add_thread_.join();
    bulk_add_in_progress_ = true;
    bulk_add_ready_ = false;
    {
        // Clear the slot before the worker starts: if the fetch throws, the
        // guard below leaves pending_bulk_add_ untouched, and a stale
        // success from a previous playlist would otherwise be re-committed.
        std::lock_guard<std::mutex> lk(bulk_add_mutex_);
        pending_bulk_add_ = BulkAddResult{};
    }
    bulk_add_thread_ = std::thread([this, url]() { run_guarded("playlist add", [&] {
        BulkAddResult res;
        res.items = online_.list_playlist(url, &res.error);
        res.success = res.error.empty() && !res.items.empty();
        std::lock_guard<std::mutex> lk(bulk_add_mutex_);
        pending_bulk_add_ = std::move(res);
    }); 
        // Same reasoning as launch_search_async(): the poll loop is gated on
        // this flag, so it is raised whether or not the work succeeded.
        bulk_add_ready_ = true;
    });
}

void App::poll_pending_bulk_add() {
    if (!bulk_add_ready_.load()) return;
    BulkAddResult res;
    {
        std::lock_guard<std::mutex> lk(bulk_add_mutex_);
        if (!bulk_add_ready_.load()) return;
        res = std::move(pending_bulk_add_);
        bulk_add_ready_ = false;
    }
    bulk_add_in_progress_ = false;
    if (bulk_add_thread_.joinable()) bulk_add_thread_.join();

    if (!res.success) {
        status_line_ = res.error.empty() ? "couldn't load that playlist" : res.error;
        return; // stay in Mode::BulkAdd, phase 1 -- let the person edit the link and retry
    }

    // Enter phase 2: show the checklist rather than committing
    // immediately. Starts fully de-selected -- "SELECT" is meant for
    // picking your own favorites out of the playlist, so nothing is
    // pre-starred; "ALL" (the "a" key) still adds every fetched track
    // regardless of star state, unaffected by this default.
    pending_bulk_add_ = std::move(res);
    bulk_add_selected_.assign(pending_bulk_add_.items.size(), false);
    bulk_add_cursor_ = 0;
    bulk_add_scroll_ = 0;
    bulk_add_results_ready_ = true;
    status_line_.clear();
}

void App::commit_bulk_add(bool all) {
    int added = 0;
    for (size_t i = 0; i < pending_bulk_add_.items.size(); ++i) {
        if (!all && (i >= bulk_add_selected_.size() || !bulk_add_selected_[i])) continue;
        const auto& item = pending_bulk_add_.items[i];
        queue_.push_back({false, item.title, item.uploader, {}, item.video_id});
        ++added;
    }
    clamp_queue_selected();
    queue_reveal(static_cast<int>(queue_.size()) - 1);
    log_event("added " + std::to_string(added) + " track" + (added == 1 ? "" : "s") + " to queue");

    mode_ = Mode::Browse;
    bulk_add_results_ready_ = false;
    bulk_add_buffer_.clear();
    pending_bulk_add_ = BulkAddResult{};
    bulk_add_selected_.clear();
    bulk_add_cursor_ = 0;
    bulk_add_scroll_ = 0;
}

// Tab layout: 0=Colors, 1=On/Off, 2=Animation, 3=Paths, 4=Reference, 5=About App.
//
// The Paths tab holds the LOCAL PATH / DOWNLOAD PATH / PLAYLIST PATH
// sections (editable path rows -- see build_path_rows(); they used to sit
// on the ON/OFF tab).
//
// The Reference tab opens with a one-line read-only note pointing at the
// cheat sheet ('?') for the full command list, then every rebindable hotkey
// (kRefRows), grouped into categories via the optional `header` field -- set
// only on a category's first row, and rendered as a section title above it --
// and finally the read-only font-mapping table loaded from config.txt. (The
// loudness normalization values used to be editable here as well; they now
// live in the SHIFT+v overlay of the main UI, see build_norm_menu_panel().
// Only the on/off toggle remains on the ON/OFF tab.) This tab
// deliberately does NOT also list the app's literal/non-rebindable key
// commands (ESC, Y/N, the playlist and meta editors' own fixed navigation
// and text-editing keys, and so on): there is nothing to configure for
// those here, and keeping a second copy of them just meant this list and
// the cheat sheet's could quietly drift apart. The cheat sheet ('?') is the
// single, authoritative list of every command, rebindable or not -- hence
// the note. All sections scroll together as one list; see ref_display_row()
// below for how a selectable row (settings_row_) maps to the row it's
// actually drawn on, once the section headers/dividers are accounted for.
//
// IMPORTANT: kRefRows[row].action is looked up in settings_.hotkeys (a
// plain string->string map), so reordering/recategorizing rows here is
// always safe -- rebinding still keys off the action name, never off the
// row's position.
// How a key is shown in the cheat sheet and on the REFERENCE tab: a capital letter is a Shift press, so "T" reads
// "SHIFT+t" (the key stays "T" in config.txt and while it is being edited); literal labels such as "SHIFT+t" or
// "CTRL+SHIFT+z" get the same lower-case letter.
static std::string pretty_key(const std::string& k) {
    if (k == "@SWITCHKEY") return mode_switch_key_label();   // the mode-switch key, named for this keyboard (keyboard_layout.h)
    if (k == "(" || k == ")" || k == "=") return symbol_key_label(k);   // SHIFT+8 / SHIFT+9 / SHIFT+0 on a German keyboard
    if (k == "@KARAOKESIZE") {   // the karaoke size keys * and _, named for this keyboard
        const std::string up = mode_switch_key_label();
        return up + " / " + (up == "*" ? "_" : up == "SHIFT and +" ? "SHIFT and -" : "SHIFT+-");
    }
    if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z') return std::string("SHIFT+") + static_cast<char>(k[0] + 32);
    std::string o = k;
    if (o.rfind("ALT+", 0) == 0) o = std::string(MUISC_ALT_NAME_UC) + o.substr(3);   // ALT, or OPTION on a Mac
    if (o.size() >= 3 && o[o.size() - 2] == '+' && o[o.size() - 1] >= 'A' && o[o.size() - 1] <= 'Z' &&
        (k.rfind("CTRL+", 0) == 0 || k.rfind("ALT+", 0) == 0))
        o[o.size() - 1] = static_cast<char>(o[o.size() - 1] + 32);   // CTRL+L reads CTRL+l, like the SHIFT+x labels
    size_t p = 0;
    while ((p = o.find("SHIFT+", p)) != std::string::npos) {
        p += 6;
        if (p < o.size() && o[p] >= 'A' && o[p] <= 'Z' && (p + 1 >= o.size() || !std::isalpha(static_cast<unsigned char>(o[p + 1]))))
            o[p] = static_cast<char>(o[p] + 32);
    }
    return o;
}

// Word-wraps `text` into lines of at most `width` columns (a longer single word is cut).
static std::vector<std::string> wrap_words(const std::string& text, int width) {
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t j = text.find(' ', i);
        std::string word = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        while (width > 0 && display_width(word) > width) {      // one word longer than the line: cut it
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            const std::string head = truncate_str(word, width);
            if (head.empty()) break;
            out.push_back(head);
            word = word.substr(head.size());
        }
        if (cur.empty()) cur = word;
        else if (display_width(cur) + 1 + display_width(word) <= width) cur += " " + word;
        else { out.push_back(cur); cur = word; }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
}

struct RefHotkeyRow { const char* header; const char* action; const char* label; };
static const RefHotkeyRow kRefRows[] = {
    // --- Playback ---
    {"PLAYBACK", "HKeyPlay", "Play Selected"},
    {nullptr, "HKeyTogglePlayPause", "Toggle Play / Pause"}, // was missing from this tab entirely
    {nullptr, "HKeyPlayNextSong", "Next Track"},
    {nullptr, "HKeyPlayPreviousSong", "Prev Track"},
    {nullptr, "HKeyShuffleNext", "Shuffle Next"},
    {nullptr, "HKeyCyclePlayMode", "Cycle Play Mode"},
    {nullptr, "HKeySeekForward", "Seek Forward"},
    {nullptr, "HKeySeekBackward", "Seek Backward"},
    {nullptr, "HKeyIncreaseVolume", "Volume Up"},
    {nullptr, "HKeyDecreaseVolume", "Volume Down"},
    {nullptr, "HKeyToggleMute", "Toggle Mute"},
    {nullptr, "HKeyToggleNormalize", "Toggle Normalize"},
    {nullptr, "HKeyNormMenu", "Normalization Tuning"}, // on/off, target level, max boost, live
    {nullptr, "HKeySleepTimer", "Sleep Timer"},       // 15/30/60/90/120 min or stop after the current song
    // --- Navigation & View ---
    {"NAVIGATION & VIEW", "HKeyNavigateUp", "Navigate Up"},
    {nullptr, "HKeyNavigateDown", "Navigate Down"},
    {nullptr, "HKeySwitchBetweenCards", "Switch Cards"},
    {nullptr, "HKeyFilterForFolder", "Filter By Folder"},
    {nullptr, "HKeyClearFilter", "Clear Filter"},
    {nullptr, "HKeyCycleSortMode", "Cycle Sort Mode"},
    {nullptr, "HKeyToggleMetaOnly", "Show Metadata Only"}, // list rows: metadata instead of filename
    {nullptr, "HKeyToggleWaveform", "Toggle Waveform"},
    {nullptr, "HKeyFolderOverlay", "Big Folder Overlay"}, // folder tree of the library, `f` shows a folder in the list
    {nullptr, "HKeyConvertOverlay", "File Conversion Overlay"}, // convert high resolution files to MP3 / AAC / Opus
    {nullptr, "HKeyListOverlay", "Big List Overlay"}, // larger LOCAL AUDIO FILES pane floated over the main UI
    {nullptr, "HKeyQueueOverlay", "Big Queue Overlay"}, // larger QUEUE pane floated over the main UI
    {nullptr, "HKeyRefreshUi", "Refresh UI"},
    // --- Lyrics & karaoke ---
    {"LYRICS & KARAOKE", "HKeyToggleLyrics", "Lyrics Cycle Forward"},
    {nullptr, "HKeyToggleLyricsBack", "Lyrics Cycle Backward"},
    {nullptr, "HKeyRetryLyrics", "Retry Lyrics"},
    {nullptr, "HKeyKaraoke", "Karaoke Overlay"},
    {nullptr, "HKeyLyricsTiming", "Lyrics Timing Overlay"},   // shift synced lyrics earlier / later
    {nullptr, "HKeyReadLyrics", "Read Lyrics Overlay"},       // saved lyrics as text, files + translations
    // --- Visualizations ---
    {"VISUALIZATIONS", "HKeyOscMenu", "Oscilloscope Tuning"},  // the SHIFT+o overlay, live
    {nullptr, "HKeySpectroMenu", "Spectrogram Options"},
    {nullptr, "HKeySpectroFull", "Spectrogram Full Screen"},
    {nullptr, "HKeyScopeBig", "Oscilloscope Full Screen"},   // YT left, XY right, grid
    {nullptr, "HKeyScopeWindow", "Scope Window"},     // the oscilloscope in its own (graphics card) window
    {nullptr, "HKeySpectroWindow", "Spectrogram Window"}, // the spectrogram in its own (graphics card) window
    // --- Search ---
    {"SEARCH", "HKeySearch", "Search Local"},   // the prefixes typed in the box (s: sc: b: p: f: r: a:) are fixed, see the cheat sheet
    // --- Queue ---
    {"QUEUE", "HKeyAddHoveringSongToQueue", "Add To Queue (Next)"},
    {nullptr, "HKeyRemoveHoveringSongFromQueue", "Remove From Queue"},
    {nullptr, "HKeyQueueMoveUp", "Queue Move Up"},
    {nullptr, "HKeyQueueMoveDown", "Queue Move Down"},
    {nullptr, "HKeyClearQueue", "Clear Queue"},
    {nullptr, "HKeyQueueAddEnd", "Add To End Of Queue"},
    {nullptr, "HKeyQueueLock", "Lock Queue"},
    {nullptr, "HKeyQueueMoveTop", "Queue Move To Top"},
    {nullptr, "HKeyQueueMoveBottom", "Queue Move To Bottom"},
    {nullptr, "HKeyQueueUndoClear", "Undo Clear Queue"},
    {nullptr, "HKeyQueueToPlaylist", "Save Queue As Playlist"},
    // --- Playlists ---
    {"PLAYLISTS", "HKeyPlaylist", "Open Playlist Editor"}, // opens the playlist create/manage screen
    // --- Meta editor ---
    {"META EDITOR", "HKeyMetaEditor", "Open Meta Editor"}, // edit file names/tags, AcoustID fetch
    {nullptr, "HKeyFetchTags", "Fetch Missing Tags"}, // the hovered title; also inside the meta editor
    // --- Listening history ---
    {"HISTORY", "HKeyHistory", "Open Listening History"}, // HISTORY / TOP TRACKS / TOP PLAYLISTS / HABITS overlay
    // --- Equalizer ---
    {"EQUALIZER", "HKeyEqualizer", "Open Equalizer"}, // 10-band EQ with presets
    // --- Downloads ---
    {"DOWNLOADS", "HKeyDownloadStream", "Download Stream"},
    // --- System ---
    {"LIBRARY", "HKeyRescan", "Rescan Library"},   // pick up files that were added while the app runs
    {"SYSTEM", "HKeySetting", "Open Settings"},
    {nullptr, "HKeyConsole", "Console / Logs"},
    {nullptr, "HKeyWizard", "Help Wizard"},
    {nullptr, "HKeyCheatsheet", "Cheatsheet"},
    {nullptr, "HKeyQuit", "Quit Application"},
};
static constexpr int kRefRowCount = sizeof(kRefRows) / sizeof(kRefRows[0]);

// The one-line, read-only note drawn first on the tab (with a blank spacer
// line above it, same as a header) and before the hotkeys start. Not
// selectable -- it costs a display line, same as a header, but no entry in
// the row-index space below.
static const char* const kRefNote = "KEYS NOT LISTED HERE ARE FIXED. SEE CHEAT SHEET FOR THE FULL LIST OF COMMANDS, `?`";

// Selectable row range of this tab, in order:
//
//     kRefStart .. kRefEnd-1          rebindable hotkeys (kRefRows)
//     kRefEnd ..                      font-map rows (read-only)
//
// Every row-index translation on this tab (ref_display_row(), the scroll
// window, settings_max_row(), the Enter-to-edit guard) has to go through
// these two constants.
static constexpr int kRefStart = 1;   // selectable row 0 is the "Reset All Keys To Default" line, right under the note
static constexpr int kRefEnd = kRefStart + kRefRowCount;

// The ON/OFF tab (tab 1) and the ANIMA/VIZ tab (tab 2) are laid out as categories: a header line, then its rows. Every
// selectable row has an id (SetId, the same numbers on both tabs); settings_row_ counts the selectable rows of the tab
// only, so settings_item_id() turns it into the id that settings_get_value() / settings_options_for() /
// settings_commit_edit() / settings_cycle() and the renderer work with. Adding a row = one id + one line in a layout.
enum SetId {
    kSidVizField, kSidDisk, kSidDummy, kSidQueue, kSidWave, kSidLyricsEngine, kSidLyricViz, kSidVisualizer, kSidNoLyricsViz,
    kSidStereo, kSidNormalize, kSidMetaOnly, kSidEmoji, kSidMarquee, kSidUseLyrics, kSidUseOsci, kSidUseSpectro, kSidUseSphere,
    kSidOrder1, kSidOrder2, kSidOrder3, kSidOrder4,
    kSidPlayback, kSidStartMode,
    kSidVisFluidity, kSidWaveStyle, kSidDiskSpeed, kSidVisDegr, kSidVisVisc, kSidLyrAlign, kSidLyrAnim,
};
struct SetLine { bool header; int id; const char* text; };   // a category header, or a row (id + label)
static const std::vector<SetLine>& settings_layout(int tab) {
    static const std::vector<SetLine> on_off = {
        {true, -1, "MODES"},
        {false, kSidPlayback, "Playback"}, {false, kSidStartMode, "Start Mode"},
        {true, -1, ""},
        {true, -1, "ON/OFF"},
        {false, kSidDisk, "Eliment Disk"}, {false, kSidDummy, "Dummy Buttons"}, {false, kSidQueue, "Queue Display"},
        {false, kSidWave, "Progress Waveform"}, {false, kSidVisualizer, "Spectrum Bars"},   // the braille waveform in the progress bar / the bars under the meta data
        {false, kSidStereo, "Stereo Sound"}, {false, kSidNormalize, "Normalize Volume"},
        {false, kSidEmoji, "Replace Emoji"}, {false, kSidMarquee, "Marquee Scrolling"},
        {true, -1, ""},
        {true, -1, "VIZ/LYRICS FIELD"},   // the field right of the meta data
        {false, kSidVizField, "Viz/Lyrics Field"}, {false, kSidUseLyrics, "Use Lyrics Engine"},
        {false, kSidUseOsci, "Use Oscilloscope"},                                            // false: left out entirely
        {false, kSidUseSpectro, "Use Spectrogram"}, {false, kSidUseSphere, "Use Sphere"},    // (not in the "." cycle)
    };
    // A header with an empty text is the blank line before the next header (every header but the first has one).
    static const std::vector<SetLine> anima = {
        {true, -1, "LYRICS"},
        {false, kSidLyrAlign, "Lyrics Alignment"}, {false, kSidLyrAnim, "Lyrics Animation"},
        {true, -1, ""},
        {true, -1, "DISK"},
        {false, kSidDiskSpeed, "Disk Speed"},
        {true, -1, ""},
        {true, -1, "SPECTRUM BARS"},   // the bars under the meta data
        {false, kSidVisFluidity, "Bars Fluidity"}, {false, kSidVisDegr, "Bars Degradation"}, {false, kSidVisVisc, "Bars Viscosity"},
        {true, -1, ""},
        {true, -1, "PROGRESS WAVEFORM"},   // the braille waveform in the progress bar
        {false, kSidWaveStyle, "Waveform Style"},
        {true, -1, ""},
        {true, -1, "WHAT THE FIELD SHOWS"},
        {false, kSidLyricViz, "Lyrics/Viz Default"}, {false, kSidNoLyricsViz, "No-Lyrics Viz"},
        {true, -1, ""},
        {true, -1, "VIZ/LYRICS ORDER"},
        {false, kSidOrder1, "Viz Order 1"}, {false, kSidOrder2, "Viz Order 2"},
        {false, kSidOrder3, "Viz Order 3"}, {false, kSidOrder4, "Viz Order 4"},
    };
    return tab == 1 ? on_off : anima;
}
static int settings_item_count(int tab) {
    int n = 0;
    for (const auto& l : settings_layout(tab)) if (!l.header) ++n;
    return n;
}
// The id of the row-th selectable row of tab 1 / 2 (-1 when there is none).
static int settings_item_id(int tab, int row) {
    int n = 0;
    for (const auto& l : settings_layout(tab)) if (!l.header && n++ == row) return l.id;
    return -1;
}
// The line (headers counted) the row-th selectable row is drawn on.
static int settings_item_line(int tab, int row) {
    int n = 0, line = 0;
    for (const auto& l : settings_layout(tab)) { if (!l.header && n++ == row) return line; ++line; }
    return 0;
}
static const char* const kVizNames[4] = {"sphere", "osci", "spectro", "lyrics"};   // viz_order values, row values

// Rebuilds the row list of the PATHS tab: a "LOCAL PATH" header +
// one row per configured local music path + a "+ new path" row, then the
// single DOWNLOAD PATH row, then the same three for the playlist paths.
// Headers are display-only (sel stays -1); everything else gets the next
// selectable index in order, starting at 0.
//
// A section whose vector is still empty still shows one (empty) row: an
// unset path has to be an editable blank field, not a missing one,
// otherwise there'd be nowhere to type the very first path.
std::vector<App::PathRow> App::build_path_rows() const {
    std::vector<PathRow> rows;
    rows.reserve(10);

    int sel = 0;

    auto add_path_section = [&](const char* header, bool playlist) {
        PathRow h;
        h.kind = PathRow::Kind::Header;
        h.label = header;
        rows.push_back(h);

        const std::vector<std::string>& paths = playlist ? settings_.playlists_paths
                                                         : settings_.local_music_paths;
        int count = std::max(1, static_cast<int>(paths.size()));
        for (int i = 0; i < count; ++i) {
            PathRow p;
            p.kind = PathRow::Kind::Path;
            p.sel = sel++;
            p.path_index = i;
            p.playlist_path = playlist;
            rows.push_back(p);
        }

        PathRow add;
        add.kind = PathRow::Kind::AddPath;
        add.sel = sel++;
        add.playlist_path = playlist;
        add.label = "+ new path";
        rows.push_back(add);
    };

    add_path_section("LOCAL PATH", false);

    // DOWNLOAD PATH: yt-dlp's single output folder, deliberately NOT run
    // through add_path_section() -- there is only ever one place downloads
    // land, so there is nothing to list and no "+ new path" to add. Reads
    // and writes settings_.download_folder (see settings_get_value() /
    // settings_commit_edit()) and is auto-injected into the local paths at
    // load/rescan, so setting it is all it takes for downloads to appear
    // in the library.
    {
        PathRow d;
        d.label = "DOWNLOAD PATH";
        d.kind = PathRow::Kind::Path;
        d.sel = sel++;
        d.path_index = -1; // no vector index: d.download_folder says who owns it
        d.download_folder = true;
        rows.push_back(d);
    }

    // PLAYLIST PATH: ONE folder (stored as playlists_paths[0]), like the download folder: no list, no "+ new path".
    {
        PathRow p;
        p.label = "PLAYLIST PATH";
        p.kind = PathRow::Kind::Path;
        p.sel = sel++;
        p.path_index = 0;
        p.playlist_path = true;
        rows.push_back(p);

        PathRow n;
        n.kind = PathRow::Kind::Note;
        n.label = "Changing it copies the existing playlists to the new folder.";
        rows.push_back(n);
    }

    // PLAYLIST EXPORT PATH: ONE folder (settings_.playlist_export_path) that 'e' on the Saved Playlists tab exports to.
    {
        PathRow p;
        p.label = "PLAYLIST EXPORT PATH";
        p.kind = PathRow::Kind::Path;
        p.sel = sel++;
        p.path_index = -1;
        p.export_folder = true;
        rows.push_back(p);

        PathRow n;
        n.kind = PathRow::Kind::Note;
        n.label = "Where [e] on the Saved Playlists tab exports M3U8 / M3U files (empty = the playlist folder).";
        rows.push_back(n);
    }

    // CONVERSION EXPORT PATH: ONE folder (settings_.conversion_export_path) the conversion overlay (SHIFT+c) writes to.
    {
        PathRow p;
        p.label = "CONVERSION EXPORT PATH";
        p.kind = PathRow::Kind::Path;
        p.sel = sel++;
        p.path_index = -1;
        p.conversion_folder = true;
        rows.push_back(p);

        PathRow n;
        n.kind = PathRow::Kind::Note;
        n.label = "Where the file conversion (SHIFT+c) saves converted files (empty = \"conversion-export\" in the download folder).";
        rows.push_back(n);
    }

    // HISTORY PATH: ONE folder (settings_.history_path) that holds history.json.
    {
        PathRow p;
        p.label = "HISTORY PATH";
        p.kind = PathRow::Kind::Path;
        p.sel = sel++;
        p.path_index = -1;
        p.history_folder = true;
        rows.push_back(p);

        PathRow n;
        n.kind = PathRow::Kind::Note;
        n.label = "An existing history.json in the new folder is used, otherwise the current one is copied there.";
        rows.push_back(n);
    }

    return rows;
}

App::PathRow App::path_row(int selectable_row) const {
    PathRow none; // sel == -1 == "no such row"
    if (selectable_row < 0) return none;
    for (const PathRow& r : build_path_rows()) if (r.sel == selectable_row) return r;
    return none;
}

int App::path_row_count() const {
    int n = 0;
    for (const PathRow& r : build_path_rows()) if (r.sel >= 0) ++n;
    return n;
}

bool App::path_row_is_text(int selectable_row) const {
    PathRow r = path_row(selectable_row);
    return r.sel >= 0 && r.kind == PathRow::Kind::Path;
}

// Maps a selectable PATHS row to the line it is drawn on, once the section
// headers (a blank spacer above the title plus the title itself, i.e. two
// lines each) are accounted for. Used by both the render block and the
// ColorEdit cursor placement, so the two always agree on where a given
// row lands.
// Number of display lines of the whole PATHS list (headers 2, notes and rows 1).
int App::path_display_total() const {
    int disp = 0;
    for (const PathRow& r : build_path_rows()) disp += (r.kind == PathRow::Kind::Header || (r.kind == PathRow::Kind::Path && (r.label && *r.label))) ? 2 : 1;
    return disp;
}

int App::path_display_row(int selectable_row) const {
    int disp = 0;
    for (const PathRow& r : build_path_rows()) {
        if (r.kind == PathRow::Kind::Header) { disp += 2; continue; }
        if (r.kind == PathRow::Kind::Note) { disp++; continue; }
        if (r.sel == selectable_row) return disp + (r.kind == PathRow::Kind::Path && (r.label && *r.label) ? 1 : 0);
        disp += (r.kind == PathRow::Kind::Path && (r.label && *r.label)) ? 2 : 1;
    }
    return disp; // out of range: one past the end
}

// Maps a selectable REFERENCE row -- kRefStart..kRefEnd-1 for the hotkeys,
// then the font-map letters -- to the row it's actually drawn on, once the
// section header/divider lines inserted along the way (one for the
// read-only note at the top, one above each hotkey category, one above the
// font map) are accounted for. Every header contributes two display rows
// (a blank spacer above it plus the title itself). Used by both the render
// block and the ColorEdit cursor placement below.
int App::ref_display_row(int selectable_row) const {
    int headers = 0;
    for (int i = 0; i <= selectable_row; ++i) {
        if (i < kRefEnd) {
            if (i == 0) headers += 2;            // the read-only note (blank + text), just before the reset line
            if (i >= kRefStart && kRefRows[i - kRefStart].header) headers += 2;
        } else {
            if (i == kRefEnd) headers += 2;      // "FONT / CHARACTER MAP"
        }
    }
    return selectable_row + headers;
}

static std::string* color_cell(Settings& s, int row, int col) {
    switch (row) {
        case 0: return col == 0 ? &s.border_color : &s.border_color_bottom;
        case 1: return col == 0 ? &s.disk_color : &s.disk_color_end;
        case 2: return col == 0 ? &s.meta_key_color : &s.meta_val_color;
        case 3: return col == 0 ? &s.visualizer_color : &s.visualizer_color_end;
        case 4: return col == 0 ? &s.progress_played_color : &s.progress_remaining_color;
        case 5: return col == 0 ? &s.search_text_color : &s.search_bg_color; // SEARCH: search bar input
        case 6: return col == 0 ? &s.search_inactive_text_color : &s.search_inactive_bg_color; // SEARCH INACTIVE: the idle bar
        case 7: return col == 0 ? &s.list_color : &s.list_inactive_bg_color;
        case 8: return col == 0 ? &s.list_playing_color : &s.list_playing_bg_color;
        case 9: return col == 0 ? &s.list_cursor_color : &s.list_cursor_bg_color;
        case 10: return col == 0 ? &s.queue_color : &s.queue_inactive_bg_color;
        case 11: return col == 0 ? &s.queue_playing_color : &s.queue_playing_bg_color;
        case 12: return col == 0 ? &s.queue_cursor_color : &s.queue_cursor_bg_color;
        case 13: return col == 0 ? &s.inactive_line_color : &s.inactive_line_bg_color;
        case 14: return col == 0 ? &s.active_line_color : &s.active_line_bg_color;
        case 15: return col == 0 ? &s.active_word_color : &s.active_word_bg_color;
        case 16: return col == 0 ? &s.header_color : nullptr; // HEADER: text color only, no background cell
        case 17: return col == 0 ? &s.legend_color : nullptr; // LEGEND: text color only, no background cell
        case 18: return col == 0 ? &s.tab_current_color : &s.tab_other_color; // TAB NAMES: current / other (both text colours)
        case 19: return col == 0 ? &s.change_text_color : &s.change_bg_color; // CHANGE: a field actively being typed into
        case 20: return col == 0 ? &s.focus_color : nullptr; // FOCUS: the pane focus indicator, text color only
        default: return nullptr;
    }
}

// The Colors tab, top to bottom: row 0 = THEME | CREATE THEME, row 1 = BORDER_COLOR, row 2 = SMOOTHNESS (rough /
// smooth), rows 3.. = the other colour rows. color_cell() and the theme creator count the colour rows alone (0..20).
static int color_row_of_tab(int t) { return t == 1 ? 0 : (t >= 3 ? t - 2 : -1); }
static int tab_row_of_color(int c) { return c == 0 ? 1 : c + 2; }

std::string* App::color_field_ptr(int row, int col) {
    const int c = color_row_of_tab(row);
    return c < 0 ? nullptr : color_cell(settings_, c, col);
}

// ---- colour themes: the Settings fields a theme sets, keyed like config.txt ------------------------------
namespace {
struct ThemeKey { const char* key; std::string Settings::* p; };
const ThemeKey kThemeKeys[] = {
    {"ColorBorderTop", &Settings::border_color},            {"ColorBorderBottom", &Settings::border_color_bottom},
    {"ColorDiskTop", &Settings::disk_color},                {"ColorDiskBottom", &Settings::disk_color_end},
    {"ColorMetadataKey", &Settings::meta_key_color},        {"ColorMetadataVal", &Settings::meta_val_color},
    {"ColorVizLeft", &Settings::visualizer_color},          {"ColorVizRight", &Settings::visualizer_color_end},
    {"ColorProgressBarPlayed", &Settings::progress_played_color}, {"ColorProgressBarPending", &Settings::progress_remaining_color},
    {"ColorSearchFg", &Settings::search_text_color},        {"ColorSearchBg", &Settings::search_bg_color},
    {"ColorSearchInactiveFg", &Settings::search_inactive_text_color}, {"ColorSearchInactiveBg", &Settings::search_inactive_bg_color},
    {"ColorListInactiveFg", &Settings::list_color},         {"ColorListInactiveBg", &Settings::list_inactive_bg_color},
    {"ColorListPlayingFg", &Settings::list_playing_color},  {"ColorListPlayingBg", &Settings::list_playing_bg_color},
    {"ColorListCursorFg", &Settings::list_cursor_color},    {"ColorListCursorBg", &Settings::list_cursor_bg_color},
    {"ColorQueueInactiveFg", &Settings::queue_color},       {"ColorQueueInactiveBg", &Settings::queue_inactive_bg_color},
    {"ColorQueuePlayingFg", &Settings::queue_playing_color}, {"ColorQueuePlayingBg", &Settings::queue_playing_bg_color},
    {"ColorQueueCursorFg", &Settings::queue_cursor_color},  {"ColorQueueCursorBg", &Settings::queue_cursor_bg_color},
    {"ColorLyricsInactiveFg", &Settings::inactive_line_color}, {"ColorLyricsInactiveBg", &Settings::inactive_line_bg_color},
    {"ColorLyricsActiveLineFg", &Settings::active_line_color}, {"ColorLyricsActiveLineBg", &Settings::active_line_bg_color},
    {"ColorLyricsActiveWordFg", &Settings::active_word_color}, {"ColorLyricsActiveWordBg", &Settings::active_word_bg_color},
    {"ColorHeader", &Settings::header_color},               {"ColorLegend", &Settings::legend_color},
    {"ColorTabCurrent", &Settings::tab_current_color},      {"ColorTabOther", &Settings::tab_other_color},
    {"ColorChangeFg", &Settings::change_text_color},        {"ColorChangeBg", &Settings::change_bg_color},
    {"ColorFocus", &Settings::focus_color},
};

void theme_apply(Settings& s, const themes::Theme& t) {
    for (const ThemeKey& k : kThemeKeys) if (const std::string* v = t.get(k.key, false)) s.*(k.p) = *v;
    // a theme written before the focus indicator had a color of its own: it gets the theme's header color
    if (!t.get("ColorFocus", false) && t.get("ColorHeader", false)) s.focus_color = s.header_color;
}

themes::Theme theme_capture(const Settings& s, themes::Theme base) {
    base.builtin = false;
    for (const ThemeKey& k : kThemeKeys) base.set(k.key, s.*(k.p));
    return base;
}

bool theme_matches(const Settings& s, const themes::Theme& t) {
    int known = 0;
    for (const ThemeKey& k : kThemeKeys) {
        const std::string* v = t.get(k.key, false);
        if (!v) continue;
        ++known;
        if (*v != s.*(k.p)) return false;
    }
    return known > 0;
}
} // namespace

int App::theme_current_index() const {
    for (size_t i = 0; i < theme_list_.size(); ++i) if (theme_matches(settings_, theme_list_[i])) return static_cast<int>(i);
    return -1;
}

void App::theme_open_picker() {
    theme_list_ = themes::load_all();
    theme_sel_ = std::max(0, theme_current_index());
    theme_ui_ = 1;
    force_redraw_ = true;
}

// edit_current (key [e]): the creator opens on the colours in use. If they are a theme of your own, its name is already in the
// NAME field and saving overwrites it (after a question); a built-in theme (or colours that match no theme) needs a new name.
void App::theme_open_creator(bool edit_current) {
    theme_list_ = themes::load_all();
    const int cur = theme_current_index();
    theme_base_ = theme_list_[static_cast<size_t>(cur >= 0 ? cur : 0)];
    theme_draft_ = settings_;
    theme_draft_text_ = settings_to_text(theme_draft_);
    leave_prompt_ = 0;
    theme_name_.clear();
    status_line_.clear();
    if (edit_current) {
        if (cur >= 0 && !theme_list_[static_cast<size_t>(cur)].builtin) theme_name_ = theme_list_[static_cast<size_t>(cur)].name;
        else status_line_ = cur >= 0 ? "\"" + theme_list_[static_cast<size_t>(cur)].name + "\" is built in and cannot be overwritten: give your edit a new name"
                                     : "these colors are not a saved theme: give them a name";
    }
    theme_name_initial_ = theme_name_;
    theme_row_ = -1;
    theme_col_ = 0;
    theme_ui_ = 2;
    force_redraw_ = true;
}

// [s] in the creator: a name is needed, a built-in theme cannot be overwritten, and overwriting a theme of your own is asked first
// (leave_prompt_ 3; [y] then calls theme_save_new()).
void App::theme_try_save() {
    const std::string name = themes::sanitize_name(theme_name_);
    if (name.empty()) { status_line_ = "give the theme a name first (NAME row), then [s]"; force_redraw_ = true; return; }
    const std::vector<themes::Theme> all = themes::load_all();
    const int at = themes::find_name(all, name);
    if (at >= 0) {
        const themes::Theme& t = all[static_cast<size_t>(at)];
        if (t.builtin) { status_line_ = "\"" + t.name + "\" is a built-in theme and cannot be overwritten -- pick another name"; force_redraw_ = true; return; }
        leave_prompt_ = 3;
        force_redraw_ = true;
        return;
    }
    theme_save_new();
}

void App::theme_delete_selected() {
    if (theme_sel_ < 0 || theme_sel_ >= static_cast<int>(theme_list_.size())) return;
    const std::string name = theme_list_[static_cast<size_t>(theme_sel_)].name;
    std::string err;
    if (!themes::delete_theme(name, err)) { status_line_ = err; return; }
    theme_list_ = themes::load_all();
    theme_sel_ = std::clamp(theme_sel_, 0, std::max(0, static_cast<int>(theme_list_.size()) - 1));
    status_line_ = "THEME \"" + name + "\" DELETED";
}

void App::theme_save_new() {
    themes::Theme t = theme_capture(theme_draft_, theme_base_);
    t.name = themes::sanitize_name(theme_name_);
    std::string err;
    if (!themes::save_theme(t, err)) { status_line_ = err; return; }
    theme_list_ = themes::load_all();
    theme_apply(settings_, t);
    theme_ui_ = 0;
    status_line_ = "THEME \"" + t.name + "\" SAVED AND APPLIED (s saves the settings)";
    force_redraw_ = true;
}

static std::filesystem::path theme_dir_path(std::string dir) {
    if (!dir.empty() && dir[0] == '~' && (dir.size() == 1 || dir[1] == '/' || dir[1] == '\\')) {
        const char* home = std::getenv("HOME");
        if (!home || !*home) home = std::getenv("USERPROFILE");
        if (home) dir = std::string(home) + dir.substr(1);
    }
    return path_from_utf8(dir);
}

void App::theme_open_export() {
    theme_list_ = themes::load_all();
    const int cur = theme_current_index();
    theme_io_name_ = cur >= 0 ? theme_list_[static_cast<size_t>(cur)].name : std::string("My Theme");
    if (theme_io_dir_.empty()) theme_io_dir_ = themes::exchange_dir().string();
    theme_io_row_ = 0;
    theme_ui_ = 4;
    force_redraw_ = true;
}

void App::theme_io_rescan() {
    theme_io_files_ = themes::list_theme_files(theme_dir_path(theme_io_dir_));
    theme_io_sel_ = std::clamp(theme_io_sel_, 0, std::max(0, static_cast<int>(theme_io_files_.size()) - 1));
}

void App::theme_open_import() {
    theme_list_ = themes::load_all();
    if (theme_io_dir_.empty()) theme_io_dir_ = themes::exchange_dir().string();
    theme_io_sel_ = 0;
    theme_io_rescan();
    theme_io_row_ = theme_io_files_.empty() ? 0 : 1;
    theme_ui_ = 5;
    force_redraw_ = true;
}

void App::theme_import_files(const std::vector<std::filesystem::path>& files) {
    int n = 0;
    std::string last, err;
    for (const auto& f : files)
        for (themes::Theme& t : themes::read_theme_file(f)) {
            const std::string name = themes::import_theme(t, err);
            if (!name.empty()) { ++n; last = name; }
        }
    theme_list_ = themes::load_all();
    theme_ui_ = 0;
    status_line_ = n == 0 ? (err.empty() ? std::string("no theme found in that file") : err)
                          : "IMPORTED " + std::to_string(n) + " THEME" + (n == 1 ? "" : "S") + (n == 1 ? ": " + last : std::string()) + " -- pick it in the THEME row";
    force_redraw_ = true;
}

void App::handle_theme_key(int key) {
    const int n = static_cast<int>(theme_list_.size());
    if (theme_ui_ == 4) { // export the current colours as a theme file
        if (key == 27 || key == 'q' || key == 'Q') { theme_ui_ = 0; status_line_.clear(); force_redraw_ = true; return; }
        if (key == 'A') { theme_io_row_ = 0; return; }
        if (key == 'B') { theme_io_row_ = 1; return; }
        if (key == '\r' || key == '\n') {
            color_edit_buffer_ = theme_io_row_ == 0 ? theme_io_name_ : theme_io_dir_;
            edit_caret_ = edit_anchor_ = color_edit_buffer_.size();
            edit_owner_ = "settings";
            mode_ = Mode::ColorEdit;
            return;
        }
        if (key == 's' || key == 'S' || key == kKeyCtrlS) {
            const std::string name = themes::sanitize_name(theme_io_name_);
            if (name.empty()) { status_line_ = "give the theme a name first"; return; }
            const int cur = theme_current_index();
            themes::Theme t = theme_capture(settings_, theme_list_[static_cast<size_t>(cur >= 0 ? cur : 0)]);
            t.name = name;
            const std::filesystem::path file = theme_dir_path(theme_io_dir_) / themes::file_name_for(name);
            std::string err;
            if (!themes::write_theme_file(file, t, err)) { status_line_ = err; return; }
            theme_ui_ = 0;
            status_line_ = "EXPORTED " + file.string();
            force_redraw_ = true;
        }
        return;
    }
    if (theme_ui_ == 5) { // import theme files from a folder
        const int nf = static_cast<int>(theme_io_files_.size());
        if (key == 27 || key == 'q' || key == 'Q') { theme_ui_ = 0; status_line_.clear(); force_redraw_ = true; return; }
        if (key == 'A') { if (theme_io_row_ == 1 && theme_io_sel_ > 0) --theme_io_sel_; else theme_io_row_ = 0; return; }
        if (key == 'B') {
            if (theme_io_row_ == 0) { if (nf > 0) theme_io_row_ = 1; }
            else if (theme_io_sel_ < nf - 1) ++theme_io_sel_;
            return;
        }
        if (key == '\r' || key == '\n') {
            if (theme_io_row_ == 0) {
                color_edit_buffer_ = theme_io_dir_;
                edit_caret_ = edit_anchor_ = color_edit_buffer_.size();
                edit_owner_ = "settings";
                mode_ = Mode::ColorEdit;
            } else if (theme_io_sel_ >= 0 && theme_io_sel_ < nf) {
                theme_import_files({theme_io_files_[static_cast<size_t>(theme_io_sel_)]});
            }
            return;
        }
        if (key == 'a' || key == kKeyCtrlS) { if (nf > 0) theme_import_files(theme_io_files_); return; }
        return;
    }
    if (theme_ui_ == 1 || theme_ui_ == 3) {
        if (key == 27 || key == 'q' || key == 'Q') { theme_ui_ = (theme_ui_ == 3) ? 2 : 0; return; }
        if (key == 'A') { if (theme_sel_ > 0) --theme_sel_; return; }
        if (key == 'B') { if (theme_sel_ < n - 1) ++theme_sel_; return; }
        if (key == kKeyHome) { theme_sel_ = 0; return; }
        if (key == kKeyEnd) { theme_sel_ = std::max(0, n - 1); return; }
        if (theme_ui_ == 1 && key == 'D' && !last_key_was_arrow() && theme_sel_ >= 0 && theme_sel_ < n) {   // SHIFT+d: delete a theme of your own
            const themes::Theme& t = theme_list_[static_cast<size_t>(theme_sel_)];
            if (t.builtin) status_line_ = "\"" + t.name + "\" is a built-in theme and cannot be deleted";
            else leave_prompt_ = 4;   // "Do you want to delete theme?" ([y] calls theme_delete_selected())
            force_redraw_ = true;
            return;
        }
        if ((key == '\r' || key == '\n') && theme_sel_ >= 0 && theme_sel_ < n) {
            const themes::Theme& t = theme_list_[static_cast<size_t>(theme_sel_)];
            if (theme_ui_ == 1) {
                theme_apply(settings_, t);
                theme_ui_ = 0;
                status_line_ = "THEME: " + t.name + " (s saves the settings)";
            } else {
                theme_apply(theme_draft_, t);
                theme_base_ = t;
                theme_ui_ = 2;
                status_line_ = "template: " + t.name;
            }
            force_redraw_ = true;
        }
        return;
    }
    // --- the CREATE THEME overlay ---
    if (key == 27 || key == 'q' || key == 'Q') {
        // anything made here is lost on ESC: ask first (the theme is only kept by [s])
        if (theme_name_ != theme_name_initial_ || settings_to_text(theme_draft_) != theme_draft_text_) { leave_prompt_ = 2; force_redraw_ = true; return; }
        theme_ui_ = 0; status_line_.clear(); force_redraw_ = true; return;
    }
    if (key == 's' || key == 'S' || key == kKeyCtrlS) { theme_try_save(); return; }
    if (key == 'A') {
        if (theme_row_ >= 0) { --theme_row_; if (theme_row_ < 0) theme_col_ = 0; }
        return;
    }
    if (key == 'B') {
        if (theme_row_ < 20) { ++theme_row_; if (theme_col_ == 1 && !color_cell(theme_draft_, theme_row_, 1)) theme_col_ = 0; }
        return;
    }
    if (key == 'C') {
        if (theme_row_ < 0) theme_col_ = 1;
        else if (color_cell(theme_draft_, theme_row_, 1)) theme_col_ = 1;
        return;
    }
    if (key == 'D') { theme_col_ = 0; return; }
    if (key == '\r' || key == '\n') {
        if (theme_row_ < 0 && theme_col_ == 1) { theme_ui_ = 3; theme_sel_ = 0;
            for (int i = 0; i < n; ++i) if (theme_list_[static_cast<size_t>(i)].name == theme_base_.name) theme_sel_ = i;
            return; }
        if (theme_row_ < 0) color_edit_buffer_ = theme_name_;
        else { std::string* c = color_cell(theme_draft_, theme_row_, theme_col_); color_edit_buffer_ = c ? *c : ""; }
        edit_caret_ = edit_anchor_ = color_edit_buffer_.size();
        edit_owner_ = "settings";
        mode_ = Mode::ColorEdit;
        return;
    }
}

// The note at the bottom of the ANIMA/VIZ tab: where the oscilloscope and the spectrogram are tuned and opened.
std::vector<std::string> App::settings_anima_note(int W) const {
    const std::string text = "The oscilloscope/spectrogram are tuned live via an option overlay: [" + hotkey_text("HKeyOscMenu", "O") + "] oscilloscope [" +
        hotkey_text("HKeySpectroMenu", "I") + "] spectrogram. [" + [&] {   // "SHIFT+u/0": the two keys of the big overlays
            const std::string a = hotkey_text("HKeySpectroFull", "U"), b = hotkey_text("HKeyScopeBig", "=");
            return a + "/" + (a.rfind("SHIFT+", 0) == 0 && b.rfind("SHIFT+", 0) == 0 ? b.substr(6) : b);
        }() + "] opens a big overlay of both; " +
        hotkey_text("HKeyScopeWindow", ")") + " and " + hotkey_text("HKeySpectroWindow", "(") + " open an extra window.";
    return wrap_lines(text, std::max(20, W - 10), 4);
}

// Tabs 1 and 2 are a list of category headers and rows that can be longer than the panel: the first line shown is
// chosen so that the selected row is in the middle (and its category header stays visible when it is right below it).
int App::settings_tab_first_line(int W, int visible) const {
    const auto& lay = settings_layout(settings_tab_);
    const int total = static_cast<int>(lay.size());
    const int cur = settings_item_line(settings_tab_, settings_row_);
    int first = std::clamp(cur - visible / 2, 0, std::max(0, total - visible));
    const int need = (cur > 0 && lay[static_cast<size_t>(cur - 1)].header) ? cur - 1 : cur;
    if (first > need) first = need;
    return std::max(0, first);
}

int App::settings_max_row() const {
    // Matches get_max_row(): SCHEMA.size() - 1 for each tab, extended for
    // the Reference tab's appended font-map rows and the About tab's
    // scrollable text (both computed dynamically, not hardcoded, so they
    // track the actual font_map/about_app_text() content).
    switch (settings_tab_) {
        case 0: return 22; // THEME row + 21 colour rows + the SMOOTHNESS row (the extras are SEARCH, SEARCH INACTIVE, HEADER, LEGEND, TAB NAMES, CHANGE and FOCUS)
        case 1: case 2: return settings_item_count(settings_tab_) - 1; // ON/OFF / ANIMA/VIZ: the rows of the layout
        case 3: return path_row_count() - 1; // PATHS: every selectable path/"+ new path" row
        case 4: {
            int letters = 0;
            for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) ++letters;
            return kRefEnd + letters - 1; // hotkeys + N font-map rows
        }
        case 5: {
            int MAX_Y = std::max(term_rows_ - 2, 11);
            int visible = std::max(1, MAX_Y - 3);
            int total = static_cast<int>(about_app_text().size());
            return std::max(0, total - visible); // scroll range, not a field cursor
        }
        default: return 0;
    }
}

std::string App::settings_get_value(int row, int col) const {
    // Matches getVal(k): config value if set, else the hardcoded default
    // for the 7 keys that have one, else "___" (unset). Colors format
    // their own "___"-equivalent as "none" at render time instead.
    if (settings_tab_ == 0) {
        if (row == 2) return col == 0 ? (settings_.border_smooth ? "smooth" : "rough") : "";
        std::string* p = const_cast<App*>(this)->color_field_ptr(row, col);
        return p ? *p : "";
    }
    if (settings_tab_ == 1 || settings_tab_ == 2) {
        // The toggle rows read straight off their bool; the pick rows return the name of their choice.
        bool v = false;
        switch (settings_item_id(settings_tab_, row)) {
            case kSidDisk: v = settings_.element_disk; break;
            case kSidDummy: v = settings_.element_dummy_buttons; break;
            case kSidQueue: v = settings_.element_queue; break;
            case kSidWave: v = settings_.element_waveform; break;
            case kSidVizField: v = settings_.element_vizfield; break;
            case kSidLyricsEngine: v = settings_.element_lyrics; break;
            // "Lyrics/Viz Default" isn't a bool: it cycles sphere / osci / spectro / lyrics (see settings_options_for() and settings_commit_edit()).
            case kSidLyricViz: return settings_.viz_default == 4 ? "last used" : kVizNames[std::clamp(settings_.viz_default, 0, 3)];
            case kSidVisualizer: v = settings_.element_visualizer; break;
            case kSidNoLyricsViz: return settings_.nolyrics_viz == 1 ? "osci" : settings_.nolyrics_viz == 2 ? "spectro" : settings_.nolyrics_viz == 3 ? "none" : "sphere";
            case kSidStereo: v = settings_.stereo; break;
            case kSidNormalize: v = settings_.normalize; break;
            case kSidMetaOnly: v = settings_.meta_only; break;
            case kSidEmoji: v = settings_.replace_emoji; break;
            case kSidMarquee: v = settings_.marquee; break;
            case kSidUseLyrics: v = settings_.use_lyrics; break;
            case kSidUseOsci: v = settings_.use_osci; break;
            case kSidUseSpectro: v = settings_.use_spectro; break;
            case kSidUseSphere: v = settings_.use_sphere; break;
            case kSidOrder1: case kSidOrder2: case kSidOrder3: case kSidOrder4:   // which view comes at that place of the "." cycle
                return kVizNames[std::clamp(settings_.viz_order[settings_item_id(settings_tab_, row) - kSidOrder1], 0, 3)];
            case kSidPlayback: {
                static const char* names[] = {"list", "loop", "shuffle", "stop", "queue then stop"};
                return names[std::clamp(settings_.play_mode, 0, 4)];
            }
            case kSidStartMode: return settings_.start_mode == "radio" ? "radio" : "player";
            case kSidVisFluidity: return std::to_string(settings_.visualizer_fluidity);
            case kSidWaveStyle: return settings_.waveform_smooth ? "smooth" : "raw";
            case kSidDiskSpeed: return std::to_string(settings_.disk_rotation_speed).substr(0, 4);
            case kSidVisDegr: return std::to_string(settings_.visualizer_degradation_speed);
            case kSidVisVisc: return std::to_string(settings_.visualizer_viscosity);
            case kSidLyrAlign: return settings_.lyrics_alignment == 1 ? "left" : settings_.lyrics_alignment == 2 ? "right" : "center";
            case kSidLyrAnim: {
                static const char* names[] = {"full", "word by word", "letter by letter", "active line only", "active word only", "line by line"};
                return names[std::clamp(settings_.lyrics_animation, 0, 5)];
            }
            default: return "";
        }
        return v ? "true" : "false";
    }
    if (settings_tab_ == 4 && row == 0) return "[ENTER] reset";
    if (settings_tab_ == 4 && row >= kRefStart && row < kRefEnd) {
        auto it = settings_.hotkeys.find(kRefRows[row - kRefStart].action);
        return it != settings_.hotkeys.end() ? it->second : "";
    }
    if (settings_tab_ == 3) {
        // The path rows (LOCAL PATH / DOWNLOAD PATH / PLAYLIST PATH): the
        // string at their index in the owning vector, which is "" both for
        // a not-yet-set path and for a placeholder row shown while the
        // vector is still empty.
        PathRow r = path_row(row);
        if (r.sel < 0) return "";
        if (r.kind == PathRow::Kind::Path) {
            if (r.history_folder) return settings_.history_path.empty() ? history_.effective_dir() : settings_.history_path;
            if (r.export_folder) return settings_.playlist_export_path.empty() ? path_utf8(playlists_dir()) : settings_.playlist_export_path;
            if (r.conversion_folder) return conversion_export_dir();
            if (r.download_folder) {
                // Show what is actually in effect: while nothing has been
                // configured that is the default cache folder, and the point
                // of this row is that you can see -- and change -- where
                // downloads land, not that you face a blank field.
                return settings_.download_folder.empty() ? path_utf8(cache_.cache_dir())
                                                         : settings_.download_folder;
            }
            const std::vector<std::string>& paths = r.playlist_path ? settings_.playlists_paths
                                                                    : settings_.local_music_paths;
            if (r.path_index >= 0 && r.path_index < static_cast<int>(paths.size()))
                return paths[r.path_index];
            if (r.playlist_path) return path_utf8(playlists_dir());   // nothing configured: show the folder in effect
            return "";
        }
        return ""; // AddPath: the row IS the button, it has no value
    }
    return "";
}

// Matches g_options: the fixed value lists that Left/Right cycles
// through. Empty return = not cyclable (Colors and Reference rows,
// exactly like the reference's g_options map has no entries for those).
std::vector<std::string> App::settings_options_for(int tab, int row) const {
    if (tab == 1 || tab == 2) {
        // Bool toggles cycle true / false; the pick rows have lists of their own.
        switch (settings_item_id(tab, row)) {
            case -1: return {};
            case kSidLyricViz: return {"sphere", "osci", "spectro", "lyrics", "last used"};   // the view the field starts with, not a bool
            case kSidNoLyricsViz: return {"sphere", "osci", "spectro", "none"};
            case kSidOrder1: case kSidOrder2: case kSidOrder3: case kSidOrder4: return {"sphere", "osci", "spectro", "lyrics"};
            case kSidPlayback: return {"list", "loop", "shuffle", "stop", "queue then stop"};
            case kSidStartMode: return {"player", "radio"};
            case kSidVisFluidity: case kSidVisDegr: case kSidVisVisc: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case kSidWaveStyle: return {"raw", "smooth"};
            case kSidDiskSpeed: return {"0.01", "0.05", "0.10", "0.17", "0.25", "0.50", "0.75", "1.00"};
            case kSidLyrAlign: return {"left", "center", "right"};
            case kSidLyrAnim: return {"full", "word by word", "line by line", "letter by letter", "active line only", "active word only"};
            default: return {"true", "false"};
        }
    }
    return {};
}

// Matches g_config[k] = edit_buffer -- stored close to verbatim, no
// clamping. A bool/enum field that doesn't recognize the typed text
// just leaves the setting unchanged, since there's no way to store
// arbitrary text in a typed field the way the reference's string map can.
// Every change on the ON/OFF and ANIMA/VIZ tabs goes through here: the value is stored, then the switches that depend on
// each other are brought in line (enforce_visual_switches()), and the lyrics are fetched when they have just come back.
void App::settings_commit_edit() {
    const bool rows_tab = settings_tab_ == 1 || settings_tab_ == 2;
    const int id = rows_tab ? settings_item_id(settings_tab_, settings_row_) : -1;
    const bool lyrics_before = settings_.lyrics_on();
    settings_commit_edit_impl();
    if (!rows_tab) return;
    settings_note_ = enforce_visual_switches(id);
    if (!lyrics_before && settings_.lyrics_on()) lyrics_view_nudge();   // lyrics switched on: fetch them for the track that plays now
}

void App::settings_commit_edit_impl() {
    const std::string& buf = color_edit_buffer_;
    auto to_lower = [](std::string v) { for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return v; };

    if (theme_ui_ == 4) { if (theme_io_row_ == 0) theme_io_name_ = themes::sanitize_name(buf); else theme_io_dir_ = buf; return; }
    if (theme_ui_ == 5) { theme_io_dir_ = buf; theme_io_rescan(); theme_io_sel_ = 0; theme_io_row_ = theme_io_files_.empty() ? 0 : 1; return; }
    if (theme_ui_ == 2) { // the CREATE THEME overlay edits its own draft
        if (theme_row_ < 0) theme_name_ = themes::sanitize_name(buf);
        else if (std::string* c = color_cell(theme_draft_, theme_row_, theme_col_)) *c = buf;
        return;
    }
    if (settings_tab_ == 0) {
        std::string* p = color_field_ptr(settings_row_, settings_col_);
        if (p) *p = buf;
    } else if (settings_tab_ == 1 || settings_tab_ == 2) {
        const int id = settings_item_id(settings_tab_, settings_row_);
        std::string v = to_lower(buf);
        // "Lyrics/Viz Default" and "No-Lyrics Viz" are picks among views (sphere / osci / spectro, plus lyrics or none), not
        // bools: they have to be handled BEFORE the true/false gate below, or their values would bounce off it and the row
        // would never change.
        if (id == kSidLyricViz || id == kSidNoLyricsViz) {
            int& pick = id == kSidLyricViz ? settings_.viz_default : settings_.nolyrics_viz;
            if (v == "sphere") pick = 0;
            else if (v == "osci" || v == "oscilloscope") pick = 1;
            else if (v == "spectro" || v == "spectrogram") pick = 2;
            else if (v == "none" && id == kSidNoLyricsViz) pick = 3;
            else if (v == "lyrics" && id == kSidLyricViz) pick = 3;
            else if (v == "last used" && id == kSidLyricViz) pick = 4;   // starts with the view it was left on
            if (id == kSidLyricViz && settings_.viz_default <= 3) {   // the default is also the view shown now ("last used" changes nothing)
                settings_.element_lyrics = settings_.viz_default == 3;
                if (settings_.viz_default < 3) settings_.lyric_viz = settings_.viz_default;
            }
            return; // anything unrecognized leaves the pick as it was
        }
        // "Viz Order 1..4": naming a view puts it at this place; the view that was there takes the place it left
        // (so the order stays a permutation of the four).
        if (id >= kSidOrder1 && id <= kSidOrder4) {
            int want = -1;
            for (int i = 0; i < 4; ++i) if (v == kVizNames[i]) want = i;
            if (v == "oscilloscope") want = 1;
            if (v == "spectrogram") want = 2;
            if (want < 0) return;
            const int place = id - kSidOrder1;
            for (int i = 0; i < 4; ++i)
                if (settings_.viz_order[i] == want) { std::swap(settings_.viz_order[i], settings_.viz_order[place]); break; }
            return;
        }
        switch (id) {   // the rows that are neither bools nor views
            case kSidPlayback:
                settings_.play_mode = (v == "loop") ? 1 : (v == "shuffle") ? 2 : (v == "stop") ? 3 : (v == "queue then stop") ? 4 : 0;
                queue_reset_lap();
                return;
            case kSidStartMode: settings_.start_mode = (v == "radio") ? "radio" : "player"; return;
            case kSidVisFluidity: try { settings_.visualizer_fluidity = std::stoi(buf); } catch (...) {} return;
            case kSidWaveStyle: settings_.waveform_smooth = (v == "smooth"); return;
            case kSidDiskSpeed: try { settings_.disk_rotation_speed = std::stod(buf); } catch (...) {} return;
            case kSidVisDegr: try { settings_.visualizer_degradation_speed = std::stoi(buf); } catch (...) {} return;
            case kSidVisVisc: try { settings_.visualizer_viscosity = std::stoi(buf); } catch (...) {} return;
            case kSidLyrAlign: settings_.lyrics_alignment = (v == "left") ? 1 : (v == "right") ? 2 : 0; return;
            case kSidLyrAnim:
                if (v == "word by word") settings_.lyrics_animation = 1;
                else if (v == "letter by letter") settings_.lyrics_animation = 2;
                else if (v == "active line only") settings_.lyrics_animation = 3;
                else if (v == "active word only") settings_.lyrics_animation = 4;
                else if (v == "line by line") settings_.lyrics_animation = 5;
                else settings_.lyrics_animation = 0;
                return;
            default: break;
        }
        bool is_true = (v == "true"), is_false = (v == "false");
        if (!is_true && !is_false) return;
        switch (id) {
            case kSidDisk: settings_.element_disk = is_true; break;
            case kSidDummy: settings_.element_dummy_buttons = is_true; break;
            case kSidQueue: settings_.element_queue = is_true; break;
            case kSidWave: settings_.element_waveform = is_true; break;
            case kSidVizField: settings_.element_vizfield = is_true; break;
            case kSidLyricsEngine: settings_.element_lyrics = is_true; break;
            case kSidVisualizer: settings_.element_visualizer = is_true; break;
            case kSidStereo:
                settings_.stereo = is_true;
                player_.set_stereo(is_true); // audible immediately for a stereo-decoded track
                break;
            case kSidNormalize:
                settings_.normalize = is_true;
                player_.set_normalization(settings_.normalize,
                                          static_cast<float>(settings_.normalize_target_lufs),
                                          static_cast<float>(settings_.normalize_max_boost_db));
                break;
            case kSidMetaOnly: settings_.meta_only = is_true; break;
            case kSidEmoji:
                settings_.replace_emoji = is_true;
                set_emoji_replacement(is_true); // every width calculation follows from the next frame on
                break;
            case kSidMarquee: settings_.marquee = is_true; break;
            case kSidUseLyrics: settings_.use_lyrics = is_true; break;
            case kSidUseOsci: settings_.use_osci = is_true; break;
            case kSidUseSpectro: settings_.use_spectro = is_true; break;
            case kSidUseSphere: settings_.use_sphere = is_true; break;
            default: break;
        }
    } else if (settings_tab_ == 4 && settings_row_ >= kRefStart && settings_row_ < kRefEnd) {
        settings_.hotkeys[kRefRows[settings_row_ - kRefStart].action] = buf;
    } else if (settings_tab_ == 3) {
        PathRow r = path_row(settings_row_);
        if (r.kind == PathRow::Kind::Path && r.sel >= 0) {
            // A path row commits a string instead of a bool. Trim the stray
            // spaces a careless Enter can leave behind (a trailing one would
            // otherwise point at a directory that doesn't exist) and expand
            // a leading ~ exactly the way load_settings() does, so
            // "~/Music" means the same thing whether it was typed into this
            // field or written into config.txt by hand. Committing an empty
            // value deletes the line rather than storing a blank
            // LocalMusicPath=/PlaylistsPath= -- which is also what keeps
            // the placeholder row a still-empty list shows harmless.
            std::string p = buf;
            while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
            while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
            if (!p.empty() && p[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) p = std::string(home) + p.substr(1);
            }
            if (r.export_folder) {
                // One folder, nothing moves: only where the next export goes (empty = the playlist folder).
                settings_.playlist_export_path = p;
                status_line_ = p.empty() ? "playlist export folder: the playlist folder" : "playlist export folder: " + p;
                return;
            }
            if (r.conversion_folder) {
                // One folder, nothing moves: only where the next conversion writes (empty = conversion-export in the download folder).
                settings_.conversion_export_path = p;
                status_line_ = "conversion export folder: " + conversion_export_dir();
                return;
            }
            if (r.history_folder) {
                // One folder: the history moves with it (an existing history.json there is read, otherwise the current one is written there).
                history_end_current_play();
                history_.save();
                settings_.history_path = p;
                history_.set_dir(p);
                if (history_.file_exists()) history_.load(); else history_.save();
                status_line_ = p.empty() ? "history folder: default" : "history folder: " + p;
                return;
            }
            if (r.download_folder) {
                // One folder, never a list: store it (an emptied field goes
                // back to the default cache folder), point yt-dlp at it right
                // away, and -- when one is set -- add it to the local paths so
                // the rescan below picks it up. That injection is the whole
                // "no extra LocalMusicPath line needed" half of this setting.
                settings_.download_folder = p;
                const fs::path old_archive = cache_.download_archive_path();
                cache_.set_download_dir(path_from_utf8(p));
                {   // the shared yt-dlp archive (history/downloaded_history.txt) is COPIED along, never moved; one that
                    // already exists in the new folder is left alone
                    std::error_code aec;
                    const fs::path new_archive = cache_.download_archive_path();
                    if (old_archive != new_archive && fs::exists(old_archive, aec) && !fs::exists(new_archive, aec)) {
                        fs::create_directories(new_archive.parent_path(), aec);
                        fs::copy_file(old_archive, new_archive, aec);
                    }
                }
                if (p.empty()) {
                    status_line_ = "download folder: default (cache dir)";
                } else {
                    if (std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), p)
                        == settings_.local_music_paths.end())
                        settings_.local_music_paths.push_back(p);
                    status_line_ = "download folder: " + p;
                }
                rescan_library(); // a path row that does not rescan is a path row you have to restart for
                return;
            }
            std::vector<std::string>& paths = r.playlist_path ? settings_.playlists_paths
                                                              : settings_.local_music_paths;
            // A changed PLAYLIST path takes its playlists along: the .txt files of the folder it replaces are COPIED to the
            // new one (only ones missing there, nothing is overwritten or deleted). Music and download folders are never copied.
            fs::path copy_from;
            if (r.playlist_path && !p.empty()) {
                if (r.path_index < static_cast<int>(paths.size()) && !paths[r.path_index].empty()) copy_from = path_from_utf8(paths[r.path_index]);
                else if (paths.empty() || r.path_index == 0) copy_from = playlists_dir();   // the default folder it replaces
            }
            if (static_cast<int>(paths.size()) <= r.path_index) paths.resize(r.path_index + 1);
            if (p.empty()) {
                paths.erase(paths.begin() + r.path_index);
                status_line_ = r.playlist_path ? "playlist path removed" : "local path removed";
            } else {
                paths[r.path_index] = p;
                status_line_ = std::string(r.playlist_path ? "playlist path: " : "local path: ") + p;
                if (!copy_from.empty()) {
                    const fs::path to = path_from_utf8(p);
                    std::error_code ec;
                    int copied = 0;
                    if (fs::is_directory(copy_from, ec) && copy_from != to) {
                        fs::create_directories(to, ec);
                        for (const auto& e : fs::directory_iterator(copy_from, ec)) {
                            if (!e.is_regular_file(ec) || ascii_lower_str(path_utf8(e.path().extension())) != ".txt") continue;
                            const fs::path dst = to / e.path().filename();
                            if (fs::exists(dst, ec)) continue;
                            std::error_code cec;
                            fs::copy_file(e.path(), dst, cec);
                            if (!cec) ++copied;
                        }
                    }
                    if (copied > 0) status_line_ += "  (" + std::to_string(copied) + " playlist" + (copied == 1 ? "" : "s") + " copied from the old folder)";
                }
            }
            if (!r.playlist_path) rescan_library(); // the whole point: paths apply without a restart
            settings_row_ = std::min(settings_row_, settings_max_row()); // the list may have shrunk by one
            return;
        }
    }
}

// Matches cycle_option(): find the current value's index in its options
// list and step by `dir`, wrapping. No-op if this row has no options
// list at all.
void App::settings_cycle(int dir) {
    auto opts = settings_options_for(settings_tab_, settings_row_);
    if (opts.empty()) return;
    const int id = (settings_tab_ == 1 || settings_tab_ == 2) ? settings_item_id(settings_tab_, settings_row_) : -1;   // the row, as its SetId
    std::string cur = settings_get_value(settings_row_, 0);
    int idx = -1;
    for (size_t i = 0; i < opts.size(); ++i) if (opts[i] == cur) { idx = static_cast<int>(i); break; }
    idx = (idx == -1) ? 0 : (idx + dir + static_cast<int>(opts.size())) % static_cast<int>(opts.size());
    color_edit_buffer_ = opts[idx];
    edit_caret_ = edit_anchor_ = color_edit_buffer_.size(); // the buffer was replaced, not edited
    settings_commit_edit();
    status_line_ = "TOGGLED -> " + opts[idx];
    if (id == kSidWaveStyle) recompute_waveform_for_current_track();
    const std::string note = settings_note_.empty() ? std::string() : "  (" + settings_note_ + ")";
    if (id == kSidLyricViz) {
        status_line_ = settings_.viz_default == 4
            ? "starts with: the view it was left on" + note
            : std::string("starts with: ") + kVizNames[std::clamp(settings_.viz_default, 0, 3)] + " (the \".\" key changes the view while Mousiki runs)" + note;
    }
    if (id == kSidNoLyricsViz) {
        static const char* const names[4] = {"sphere", "oscilloscope", "spectrogram", "nothing"};
        status_line_ = std::string("no lyrics: ") + names[std::clamp(settings_.nolyrics_viz, 0, 3)] + " is shown" + note;
    }
    if (id == kSidVizField) {
        status_line_ = settings_.element_vizfield ? "viz/lyrics field: on" + note
                                                  : "viz/lyrics field: off -- the meta data take its place, no lyrics are fetched for it";
    }
    if (id == kSidUseLyrics) {
        status_line_ = settings_.use_lyrics ? "lyrics engine: on (lyrics are fetched when the field shows them)" + note
                                            : "lyrics engine: off, no lyrics are fetched" + note;
    }
    if (id == kSidStereo) {
        // A track that was decoded as mono can't become stereo without being
        // decoded again, so switching stereo ON only applies from the next
        // track. Switching it OFF is immediate.
        if (settings_.stereo && has_track_ && current_pcm_ && current_pcm_->channels == 1)
            status_line_ = "stereo: on -- applies from the next track";
        else
            status_line_ = settings_.stereo ? "stereo: on" : "stereo: off";
    }
    if (id == kSidNormalize) {
        status_line_ = settings_.normalize ? "normalize: on" : "normalize: off";
    }
    if (id == kSidMetaOnly) {
        status_line_ = settings_.meta_only ? "lists: metadata only (no filename)"
                                           : "lists: filename + metadata";
        resort_local_view_keep_selection();
    }
    if (id == kSidMarquee) {
        status_line_ = settings_.marquee ? "marquee: long titles scroll" : "marquee: off, long titles are cut with ...";
    }
    if (id == kSidEmoji) {
        status_line_ = settings_.replace_emoji ? "emoji: drawn as a single ? (layout stays aligned)"
                                               : "emoji: drawn as they are (alignment depends on your terminal)";
    }
    if (id >= kSidUseLyrics && id <= kSidUseSphere) {
        static const char* const names[4] = {"lyrics", "oscilloscope", "spectrogram", "sphere"};
        const int r = id - kSidUseLyrics;
        const bool on = id == kSidUseLyrics ? settings_.use_lyrics : id == kSidUseOsci ? settings_.use_osci : id == kSidUseSpectro ? settings_.use_spectro : settings_.use_sphere;
        status_line_ = std::string(names[r]) + (on ? ": in the \".\" cycle" : ": switched off (not in the \".\" cycle; full screen and the windows " +
                              hotkey_text("HKeySpectroWindow", "(") + " / " + hotkey_text("HKeyScopeWindow", ")") + " still work)") + note;
    }
    if (id >= kSidOrder1 && id <= kSidOrder4) {
        std::string o = "\".\" cycle: ";
        for (int i = 0; i < 4; ++i) o += std::string(i ? " > " : "") + kVizNames[std::clamp(settings_.viz_order[i], 0, 3)];
        status_line_ = o;
    }
    if (id == kSidPlayback) status_line_ = std::string("playback: ") + play_mode_name(settings_.play_mode);
    if (id == kSidStartMode) status_line_ = settings_.start_mode == "radio" ? "Mousiki starts in the radio (the music player is one SHIFT+* away)"
                                                                            : "Mousiki starts with the music player";
}

// ---- REFERENCE tab: reset to default + undo (the radio settings work the same way) ----
void App::ref_undo_push(int row) {
    ref_undo_.push_back({settings_.hotkeys, row});
    if (ref_undo_.size() > 5) ref_undo_.erase(ref_undo_.begin());
}

void App::ref_reset_keys(int row) {
    Settings defaults;
    apply_default_hotkeys(defaults);
    ref_undo_push(row);
    const auto before = settings_.hotkeys;
    for (int i = 0; i < kRefRowCount; ++i) {
        if (row != 0 && kRefStart + i != row) continue;
        auto it = defaults.hotkeys.find(kRefRows[i].action);
        if (it != defaults.hotkeys.end()) settings_.hotkeys[kRefRows[i].action] = it->second;
    }
    if (settings_.hotkeys == before) {
        ref_undo_.pop_back();
        status_line_ = row == 0 ? "all keys already have their default" : "this key already has its default";
        return;
    }
    status_line_ = row == 0 ? "ALL KEYS RESET TO DEFAULT (Ctrl+Shift+U undoes it)"
                            : std::string("default key restored for ") + kRefRows[row - kRefStart].label;
}

void App::ref_undo_pop() {
    if (ref_undo_.empty()) { status_line_ = "nothing to undo"; return; }
    const RefUndo u = ref_undo_.back();
    ref_undo_.pop_back();
    int where = u.row;
    if (where == 0) {   // a full reset: go to the first key that comes back
        for (int i = 0; i < kRefRowCount; ++i) {
            auto a = u.hotkeys.find(kRefRows[i].action);
            auto b = settings_.hotkeys.find(kRefRows[i].action);
            const std::string va = a == u.hotkeys.end() ? "" : a->second, vb = b == settings_.hotkeys.end() ? "" : b->second;
            if (va != vb) { where = kRefStart + i; break; }
        }
    }
    settings_.hotkeys = u.hotkeys;
    settings_tab_ = 4;
    settings_col_ = 0;
    settings_row_ = std::clamp(where, 0, kRefEnd - 1);
    status_line_ = u.row == 0 ? "UNDONE: reset of all keys"
                              : std::string("UNDONE: ") + kRefRows[u.row - kRefStart].label + " is now \"" + settings_get_value(u.row, 0) + "\"";
    force_redraw_ = true;
}

void App::handle_settings_key(int key) {
    if (leave_prompt_ != 0 && mode_ != Mode::ColorEdit) {
        // "unsaved changes" question: [s] / ENTER save, [d] discard, anything else (ESC) goes back to editing
        const int kind = leave_prompt_;
        leave_prompt_ = 0;
        force_redraw_ = true;
        const bool save = key == 's' || key == 'S' || key == '\r' || key == '\n' || key == kKeyCtrlS;
        const bool discard = key == 'd' || key == 'D';
        if (kind == 3) { if (key == 'y' || key == 'Y') theme_save_new(); return; }
        if (kind == 4) { if (key == 'y' || key == 'Y') theme_delete_selected(); return; }   // "delete the theme?": only y says yes   // "overwrite the theme?": only y says yes
        if (kind == 1) { if (save) settings_close(true); else if (discard) settings_close(false); }
        else if (save) theme_try_save();   // may ask once more (overwriting a theme of your own)
        else if (discard) { theme_ui_ = 0; status_line_.clear(); }
        return;
    }
    if (theme_ui_ != 0 && mode_ != Mode::ColorEdit) { handle_theme_key(key); return; }
    if (mode_ == Mode::ColorEdit) {
        // The caret can be stale by one keypress after the buffer was
        // replaced wholesale (an option cycle, a rejected hotkey) -- clamp
        // before anything reads it, exactly like the meta editor does.
        le_clamp(color_edit_buffer_, edit_caret_, edit_anchor_);
        if (key == 27) { mode_ = Mode::Settings; return; } // cancel, discard buffer
        // A modifier combination (ALT+L, CTRL+L, CTRL+SHIFT+U / Z) cannot be typed as a character: pressing it in a
        // key field of the REFERENCE tab writes its name into the field.
        if (settings_tab_ == 4 && settings_row_ >= kRefStart && settings_row_ < kRefEnd) {
            const std::string cn = combo_key_name(key);
            if (!cn.empty()) { color_edit_buffer_ = cn; edit_caret_ = edit_anchor_ = cn.size(); return; }
        }
        if (key == '\r' || key == '\n') {
            std::string key_name = (settings_tab_ == 4 && settings_row_ >= kRefStart && settings_row_ < kRefEnd)
                                  ? kRefRows[settings_row_ - kRefStart].action : "";
            // Hotkey overlap fix: if this is a Reference-tab hotkey being
            // rebound and the typed key is already owned by a different
            // action, reject the commit instead of silently creating a
            // collision where two actions fire on the same key. Clear the
            // buffer, force a redraw, and stay in ColorEdit mode so the
            // user can just repeat entry with a different key -- same
            // flow as a normal edit, just not accepted yet.
            if (!key_name.empty()) {
                // What was typed has to be something the key reader can produce: one character, or one of the names
                // (hotkey_string_to_key()). Empty = unbound. "*" and ESC stay reserved: the first switches to the
                // radio mode and the second is the "back to the home view" key, both caught before any binding.
                color_edit_buffer_ = normalize_hotkey_string(color_edit_buffer_);
                edit_caret_ = edit_anchor_ = color_edit_buffer_.size();
                const std::string reject =
                    (!color_edit_buffer_.empty() && hotkey_string_to_key(color_edit_buffer_) == 0)
                        ? std::string("NOT A KEY -- one character, or ENTER TAB SPACE BACKSPACE ARROW_KEY_UP/DOWN/LEFT/RIGHT; "
                                      "press the combination itself for ") + pretty_key("ALT+L") + " / CTRL+l / CTRL+SHIFT+u / z"
                    : (color_edit_buffer_ == "*" || color_edit_buffer_ == "ESC")
                        ? std::string("\"") + color_edit_buffer_ + "\" IS RESERVED (" + (color_edit_buffer_ == "*" ? "switches to the radio mode" : "back to the home view") + ") -- try another key"
                        : std::string();
                if (!reject.empty()) {
                    status_line_ = reject;
                    color_edit_buffer_.clear();
                    edit_caret_ = edit_anchor_ = 0;
                    force_redraw_ = true;
                    return;
                }
                std::string conflict = hotkey_conflict(color_edit_buffer_, key_name);
                if (!conflict.empty()) {
                    std::string who = conflict;   // shown by its label on this tab ("Read Lyrics Overlay"), the action name otherwise
                    for (int i = 0; i < kRefRowCount; ++i) if (conflict == kRefRows[i].action) who = kRefRows[i].label;
                    status_line_ = "KEY \"" + pretty_key(color_edit_buffer_) + "\" ALREADY USED BY \"" + who + "\" -- try another key";
                    color_edit_buffer_.clear();
                    edit_caret_ = edit_anchor_ = 0;
                    force_redraw_ = true;
                    return; // stay in ColorEdit: redraw and repeat
                }
            }
            // Path commits write their own status (which path changed, that
            // the library was rescanned) -- don't bury it under the generic
            // "UPDATED".
            bool wrote_own_status = (settings_tab_ == 3 && path_row_is_text(settings_row_));
            if (!key_name.empty()) ref_undo_push(settings_row_);
            const auto hk_before = settings_.hotkeys;
            settings_commit_edit();
            if (!key_name.empty() && settings_.hotkeys == hk_before && !ref_undo_.empty()) ref_undo_.pop_back();   // nothing changed
            if (!wrote_own_status) {
                std::string what = key_name;   // the row's label, not the action name
                for (int i = 0; i < kRefRowCount; ++i) if (key_name == kRefRows[i].action) what = kRefRows[i].label;
                status_line_ = key_name.empty() ? "UPDATED" : ("UPDATED " + what);
            }
            mode_ = Mode::Settings;
            return;
        }
        // --- caret, selection, clipboard ---------------------------------
        // Shift+arrows mark, Ctrl+C/X/V copy/cut/paste, Home/End jump. These
        // are modifier combinations that arrive as their own sentinel values
        // (terminal_ui.h) precisely because the bare ones are already taken:
        // an arrow press collapses to the letter it would otherwise type, and
        // a typed 'c' has to stay a typed 'c'.
        if (key == kKeyHome) { edit_caret_ = edit_anchor_ = 0; return; }
        if (key == kKeyEnd) { edit_caret_ = edit_anchor_ = color_edit_buffer_.size(); return; }
        if (key == kKeyShiftLeft) { le_move(color_edit_buffer_, edit_caret_, edit_anchor_, -1, true); return; }
        if (key == kKeyShiftRight) { le_move(color_edit_buffer_, edit_caret_, edit_anchor_, +1, true); return; }
        if (key == kKeyDelete) { le_delete_forward(color_edit_buffer_, edit_caret_, edit_anchor_); return; }
        if (key == kKeyCtrlC) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            std::string t = (a == b) ? color_edit_buffer_ : color_edit_buffer_.substr(a, b - a);
            clipboard_set(t);
            status_line_ = t.empty() ? "nothing selected" : "COPIED";
            return;
        }
        if (key == kKeyCtrlX) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            if (a == b) return; // no selection: cut must never empty the field
            clipboard_set(color_edit_buffer_.substr(a, b - a));
            le_erase_selection(color_edit_buffer_, edit_caret_, edit_anchor_);
            status_line_ = "CUT";
            return;
        }
        if (key == kKeyCtrlV) {
            // A path gets far more room than a color/hotkey field does --
            // the same cap typing enforces below.
            const int limit = ((theme_ui_ == 2 && theme_row_ < 0) || (theme_ui_ == 4 && theme_io_row_ == 0)) ? themes::kMaxNameLen
                              : (theme_ui_ == 4 || theme_ui_ == 5) ? 240
                              : (settings_tab_ == 3 && path_row_is_text(settings_row_)) ? 240 : 18;
            le_paste(color_edit_buffer_, edit_caret_, edit_anchor_, clipboard_get(), static_cast<size_t>(limit));
            status_line_ = "PASTED";
            return;
        }
        if (key == 127 || key == 8) { le_backspace(color_edit_buffer_, edit_caret_, edit_anchor_); return; }
        // Arrow keys collapse to the letters 'A'-'D' (same collision as
        // Mode::Search below), which sit inside 32-126 and would otherwise
        // be typed as literal letters -- for a hotkey rebind that would
        // mean an arrow press could silently become the new binding. The
        // value on its own can't tell a real capital A from an Up press,
        // so ask the input layer instead: a genuinely typed A-D goes into
        // the buffer now (without it a Windows path could never be
        // entered -- "C:\" has no way past this check), while an arrow
        // press moves the caret instead (Left/Right) or is dropped (Up/Down
        // -- a single line has nowhere to go vertically).
        if (last_key_was_arrow() && (key == 'A' || key == 'B')) return;
        if (last_key_was_arrow() && (key == 'C' || key == 'D')) {
            le_move(color_edit_buffer_, edit_caret_, edit_anchor_, key == 'D' ? -1 : +1, false);
            return;
        }
        // A path needs far more room than a color/hotkey field does.
        const int edit_limit = ((theme_ui_ == 2 && theme_row_ < 0) || (theme_ui_ == 4 && theme_io_row_ == 0)) ? themes::kMaxNameLen
                             : (theme_ui_ == 4 || theme_ui_ == 5) ? 240
                             : (settings_tab_ == 3 && path_row_is_text(settings_row_)) ? 240 : 18;
        if (is_text_key(key)) {
            le_insert(color_edit_buffer_, edit_caret_, edit_anchor_, static_cast<char>(key),
                      static_cast<size_t>(edit_limit));
        }
        return;
    }

    if (key == kKeyCtrlShiftU) { ref_undo_pop(); return; }   // undo the last key change (up to 5, newest first)
    if (key == kKeyDelete && settings_tab_ == 4 && settings_row_ >= kRefStart && settings_row_ < kRefEnd) {
        ref_reset_keys(settings_row_);                           // the default key of this row
        return;
    }
    if (key == 9) { // Tab
        settings_tab_ = (settings_tab_ + 1) % kSettingsTabCount;
        settings_row_ = 0;
        settings_col_ = 0;
        return;
    }
    // Explicit spec from the user, overriding the reference's own
    // key semantics for this exact case (reference's 's' saves without
    // closing; here 's' saves AND exits, Esc/q just exits without saving).
    if (key == 's' || key == 'S') { settings_close(true); return; }
    if (key == 27 || key == 'q' || key == 'Q') {
        settings_update_dirty();
        if (settings_dirty_) { leave_prompt_ = 1; force_redraw_ = true; return; }   // unsaved changes: ask first
        settings_close(false);
        return;
    }
    if (settings_tab_ == 5) {
        // About App: no fields to edit, but Up/Down still scroll the text.
        if (key == 'A') { if (settings_row_ > 0) --settings_row_; return; }
        if (key == 'B') { if (settings_row_ < settings_max_row()) ++settings_row_; return; }
        return;
    }

    if (settings_tab_ == 0 && !last_key_was_arrow()) {   // [e] edit the theme in use, SHIFT+e / SHIFT+i: export / import a theme
        if (key == 'e') { theme_open_creator(true); return; }
        if (key == 'E') { theme_open_export(); return; }
        if (key == 'I') { theme_open_import(); return; }
    }
    if (key == '\r' || key == '\n') {
        // Reference tab: the hotkeys (kRefStart..kRefEnd-1) are editable;
        // the note above them and the font-map rows after them are
        // read-only display and never enter ColorEdit at all.
        if (settings_tab_ == 4 && settings_row_ == 0) { ref_reset_keys(0); return; }
        if (settings_tab_ == 4 && settings_row_ >= kRefEnd) {
            return;
        }
        // PATHS tab: every path row edits in place; "+ new path" adds a
        // line first.
        if (settings_tab_ == 3) {
            PathRow r = path_row(settings_row_);
            if (r.kind == PathRow::Kind::AddPath && r.sel >= 0) {
                // "+ new path": append an empty line to the list this row
                // belongs to and go straight into editing it. No row
                // bookkeeping needed -- build_path_rows() gives the new
                // entry exactly the selectable index this AddPath row just
                // vacated (every row after it shifts down by one), so
                // settings_row_ already points at the new empty field.
                std::vector<std::string>& paths = r.playlist_path ? settings_.playlists_paths
                                                                  : settings_.local_music_paths;
                paths.push_back("");
                color_edit_buffer_.clear();
                edit_caret_ = edit_anchor_ = 0;
                edit_owner_ = "settings"; // claim the shared caret -- see edit_focus()
                mode_ = Mode::ColorEdit;
                status_line_ = r.playlist_path
                    ? "new playlist path -- type it, ENTER to confirm"
                    : "new local music path -- type it, ENTER to confirm";
                force_redraw_ = true;
                return;
            }
        }
        if (settings_tab_ == 0 && settings_row_ == 2) { settings_.border_smooth = !settings_.border_smooth; return; }   // SMOOTHNESS
        if (settings_tab_ == 0 && settings_row_ == 0) { // THEME | CREATE THEME
            if (settings_col_ == 0) theme_open_picker(); else theme_open_creator();
            return;
        }
        color_edit_buffer_ = settings_get_value(settings_row_, settings_col_);
        edit_caret_ = edit_anchor_ = color_edit_buffer_.size(); // caret starts at the end, as usual
        edit_owner_ = "settings"; // claim the shared caret -- see edit_focus()
        mode_ = Mode::ColorEdit;
        return;
    }
    if (key == 'A') { if (settings_row_ > 0) --settings_row_; return; }
    if (key == 'B') { if (settings_row_ < settings_max_row()) ++settings_row_; return; }
    if (key == 'C') { // right
        if (settings_tab_ == 0) {
            if (settings_row_ == 2) settings_.border_smooth = !settings_.border_smooth;
            else if (color_field_ptr(settings_row_, 1) || settings_row_ == 0) settings_col_ = 1;
        }
        else settings_cycle(1);
        return;
    }
    if (key == 'D') { // left
        if (settings_tab_ == 0) {
            if (settings_row_ == 2) settings_.border_smooth = !settings_.border_smooth;
            else settings_col_ = 0;
        }
        else settings_cycle(-1);
        return;
    }
}

void App::start_local_track(const LocalTrack& track) {
    const std::string playlist_ctx = std::move(pending_queue_playlist_);
    pending_queue_playlist_.clear();
    if (load_in_progress_.load()) { status_line_ = "still loading the previous track ..."; return; }
    load_playlist_ctx_ = playlist_ctx;
    fs::path parent = track.path.parent_path().filename();
    // folder_artist is only a cheap guess for the browse-list Artist column
    // (see LocalTrack's comment in local_source.h) -- it's the same string
    // as the Location field below (both are just the parent folder name),
    // so feeding it in here as the fallback artist made an untagged file's
    // metadata panel show the folder path twice, once as "Artist" and once
    // as "Location". Pass "" instead: probe_metadata() already falls back
    // to "-" when there's no real artist tag, same as start_online_track
    // does for YouTube tracks below.
    launch_load_async(track.path, track.title, "",
                       path_utf8(parent) + "/", /*is_local=*/true, /*video_id=*/"");
}

void App::start_online_track(const OnlineResult& result) {
    const std::string playlist_ctx = std::move(pending_queue_playlist_);
    pending_queue_playlist_.clear();
    if (load_in_progress_.load()) { status_line_ = "still loading the previous track ..."; return; }
    load_playlist_ctx_ = playlist_ctx;
    // Pass "" for artist so the lyrics search queries just the YouTube video title
    // (which usually contains "Artist - Song Name" perfectly), instead of appending the channel name.
    // where it comes from (the meta data panel's Location; also what the download key looks at)
    const std::string& id = result.video_id;
    const char* where = id.find("soundcloud.com") != std::string::npos ? "soundcloud"
                      : id.find("bandcamp.com") != std::string::npos ? "bandcamp" : "youtube";
    launch_load_async({}, result.title, "", where, /*is_local=*/false, result.video_id);
}

// ---------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------

void App::handle_key(int key) {
    if (key == 0) return;

    // While the pending edits are written to the files nothing else may happen; ESC stops after the current file.
    if (meta_save_.active) {
        if (key == 27) meta_save_.cancel = true;
        return;
    }

    // A meta-editor confirmation -- Shift+B's AcoustID disclaimer (raised
    // here in Browse) or Ctrl+Shift+S/D raised from the menu -- swallows every
    // key until it's answered, so nothing underneath it can be triggered by
    // accident. Checked before the mode dispatch on purpose: the prompt can
    // outlive the key that raised it in either of two modes.
    if (handle_meta_prompt_key(key)) return;

    // The help wizard opens from every menu and overlay that is not taking typed text (the main screen has its own
    // route further down); closing it brings the menu back.
    if (wizard::kEnabled && mode_ != Mode::Wizard && mode_ != Mode::Browse && key_is("HKeyWizard", key) && wizard_allowed_over_mode()) {
        wizard_open();
        return;
    }

    if (mode_ == Mode::ColorEdit || mode_ == Mode::Settings) {
        handle_settings_key(key);
        return;
    }

    if (mode_ == Mode::Console) {
        // ESC, or the console hotkey again, closes it. Anything else is
        // ignored -- this is a read-only log view.
        if (key == 27 || key == 't' || key == 'T') mode_ = Mode::Browse;
        return;
    }

    if (mode_ == Mode::Playlist) {
        handle_playlist_key(key);
        return;
    }

    if (mode_ == Mode::MetaEdit) {
        handle_meta_key(key);
        return;
    }

    if (mode_ == Mode::History) {
        handle_history_key(key);
        return;
    }

    if (mode_ == Mode::ReadLyrics) {
        handle_readlyrics_key(key);
        return;
    }

    if (mode_ == Mode::ClearQueue) {
        // "Want to clear queue?" -- Left/Right/TAB move between Yes and No,
        // Enter takes the highlighted one, y / n answer directly, ESC is No.
        // Any OTHER key also cancels (nothing is cleared): this mode must never
        // be a place where keys vanish silently, or a popup that is hard to
        // see would look like a frozen UI. Cancelling is always the safe side.
        const bool arrow = last_key_was_arrow();
        if (arrow && (key == 'C' || key == 'D')) { clear_queue_choice_ = (key == 'D') ? 0 : 1; return; } // Left=Yes, Right=No
        if (!arrow && key == 9) { clear_queue_choice_ ^= 1; return; }
        if (!arrow && (key == 'y' || key == 'Y')) { mode_ = Mode::Browse; queue_clear(); return; }
        if (!arrow && (key == '\r' || key == '\n')) {
            mode_ = Mode::Browse;
            if (clear_queue_choice_ == 0) queue_clear();
            return;
        }
        mode_ = Mode::Browse; // n / N / ESC / anything else: No
        status_line_.clear();
        return;
    }

    if (mode_ == Mode::SpectroMenu) {
        // Spectrogram options (SHIFT+i). Like the oscilloscope overlay: playback keeps running, only these keys work.
        const bool arrow = last_key_was_arrow();
        // rows kSrStyle .. the last (no "Display" row: what the lyrics area shows is the "." key's job)
        if (arrow && key == 'A') { spectro_menu_row_ = spectro_menu_row_ <= kSrStyle ? kSpectroRowCount - 1 : spectro_menu_row_ - 1; return; }
        if (arrow && key == 'B') { spectro_menu_row_ = spectro_menu_row_ >= kSpectroRowCount - 1 ? kSrStyle : spectro_menu_row_ + 1; return; }
        if (arrow && key == 'D') { spectro_menu_adjust(-1); return; }
        if (arrow && key == 'C') { spectro_menu_adjust(+1); return; }
        if (!arrow && (key == 'r' || key == 'R')) { spectro_reset(settings_.spectro); return; }
        std::string act = resolve_hotkey_action(key);
        if (act.empty() && key >= 'a' && key <= 'z') act = resolve_hotkey_action(key - 32);
        if (act.empty() && key >= 'A' && key <= 'Z') act = resolve_hotkey_action(key + 32);
        if (!arrow && act == "HKeySpectroFull") {
            spectro_full_ = !spectro_full_; if (spectro_full_) scope_big_ = false; force_redraw_ = true; return;
        }
        if (!arrow && act == "HKeyScopeWindow") { scope_window_toggle(); return; }     // the windows: always
        if (!arrow && act == "HKeySpectroWindow") { spectro_window_toggle(); return; }
        if (!arrow && (key == 27 || act == "HKeySpectroMenu")) {
            mode_ = Mode::Browse;
            save_settings(settings_);
        }
        return;
    }

    if (spectro_full_ && mode_ == Mode::Browse && key == 27 && !last_key_was_arrow()) {   // ESC leaves the full screen
        spectro_full_ = false;
        force_redraw_ = true;
        return;
    }

    if (scope_big_ && mode_ == Mode::Browse && scope_big_key(key)) return;   // SHIFT+0 overlay: ESC, SHIFT+a, SHIFT+c, the colour prompt

    if (mode_ == Mode::OsciMenu) {
        // Oscilloscope tuning. Playback and the scope keep running underneath;
        // only this overlay takes keys. Unknown keys are ignored (the footer
        // lists every key), ESC or the overlay hotkey again closes and saves.
        const bool arrow = last_key_was_arrow();
        const int osci_n = static_cast<int>(osci_visible_rows(settings_).size());
        if (arrow && key == 'A') { osci_menu_row_ = (osci_menu_row_ + osci_n - 1) % osci_n; return; } // up
        if (arrow && key == 'B') { osci_menu_row_ = (osci_menu_row_ + 1) % osci_n; return; }          // down
        if (arrow && key == 'D') { osci_menu_adjust(-1); return; }                      // left
        if (arrow && key == 'C') { osci_menu_adjust(+1); return; }                      // right
        if (!arrow && (key == 'r' || key == 'R')) {
            osci_reset(settings_);
            return;
        }
        std::string osc_action = resolve_hotkey_action(key);
        if (osc_action.empty() && key >= 'a' && key <= 'z') osc_action = resolve_hotkey_action(key - 32);
        if (osc_action.empty() && key >= 'A' && key <= 'Z') osc_action = resolve_hotkey_action(key + 32);
        if (!arrow && osc_action == "HKeyScopeWindow") { scope_window_toggle(); return; }     // the windows: always
        if (!arrow && osc_action == "HKeySpectroWindow") { spectro_window_toggle(); return; }
        if (!arrow && osc_action == "HKeyScopeBig") { scope_big_ = !scope_big_; spectro_full_ = false; scope_big_color_edit_ = false; scope_metrics_open_ = false; force_redraw_ = true; return; }
        if (!arrow && (key == 27 || osc_action == "HKeyOscMenu")) {
            mode_ = Mode::Browse;
            save_settings(settings_);
        }
        return;
    }

    if (mode_ == Mode::NormMenu) {
        // Loudness normalisation. Playback keeps running underneath and the
        // level follows every change immediately; only this overlay takes
        // keys. Unknown keys are ignored (the footer lists every key), ESC or
        // the overlay hotkey again closes and saves.
        const bool arrow = last_key_was_arrow();
        if (arrow && key == 'A') { norm_menu_row_ = (norm_menu_row_ + 2) % 3; return; } // up
        if (arrow && key == 'B') { norm_menu_row_ = (norm_menu_row_ + 1) % 3; return; } // down
        if (arrow && key == 'D') { norm_menu_adjust(-1); return; }                      // left
        if (arrow && key == 'C') { norm_menu_adjust(+1); return; }                      // right
        if (!arrow && key == ' ') {
            settings_.normalize = !settings_.normalize;
            norm_apply();
            return;
        }
        if (!arrow && (key == 'r' || key == 'R')) { // target + boost back to the defaults; on/off is left alone
            const Settings defaults{};
            settings_.normalize_target_lufs = defaults.normalize_target_lufs;
            settings_.normalize_max_boost_db = defaults.normalize_max_boost_db;
            norm_apply();
            return;
        }
        std::string norm_action = resolve_hotkey_action(key);
        if (norm_action.empty() && key >= 'a' && key <= 'z') norm_action = resolve_hotkey_action(key - 32);
        if (norm_action.empty() && key >= 'A' && key <= 'Z') norm_action = resolve_hotkey_action(key + 32);
        if (!arrow && (key == 27 || norm_action == "HKeyNormMenu")) {
            mode_ = Mode::Browse;
            save_settings(settings_);
        } else if (!arrow && norm_action == "HKeyToggleNormalize") { // the on/off key works in here too
            settings_.normalize = !settings_.normalize;
            norm_apply();
        }
        return;
    }

    if (mode_ == Mode::LyricsEdit) {
        // Lyrics timing. Playback keeps running underneath; only this overlay
        // takes keys. Unknown keys are ignored (the footer lists every key).
        const bool arrow = last_key_was_arrow();
        if (arrow && key == 'C') { lyrics_edit_adjust(+0.1); return; } // right: lyrics later
        if (arrow && key == 'D') { lyrics_edit_adjust(-0.1); return; } // left: lyrics earlier
        if (arrow && key == 'A') { lyrics_edit_adjust(+0.5); return; } // up
        if (arrow && key == 'B') { lyrics_edit_adjust(-0.5); return; } // down
        // SHIFT+Left/Right seek the running track (5 s, like the normal seek keys) so a line can be heard again
        // while it is being re-timed.
        if (key == kKeyShiftLeft)  { if (has_track_) player_.seek_relative(-5.0); return; }
        if (key == kKeyShiftRight) { if (has_track_) player_.seek_relative(5.0);  return; }
        if (arrow) return;
        if (key == 'r' || key == 'R') {
            std::lock_guard<std::mutex> lk(lyrics_mutex_);
            lyrics_result_.delay = 0.0;
            return;
        }
        if (key == '\r' || key == '\n' || key == 's' || key == 'S') {
            double d = 0.0;
            { std::lock_guard<std::mutex> lk(lyrics_mutex_); d = lyrics_result_.delay; }
            char num[24];
            std::snprintf(num, sizeof num, "%+.1f s", d);
            if (save_lyrics_delay(lyrics_path_, d)) status_line_ = std::string("lyrics timing saved: ") + num;
            else status_line_ = std::string("lyrics timing ") + num + " applied, but could not be saved (no lyrics file for this track)";
            lyrics_edit_close();
            return;
        }
        if (key == 27 || key_is("HKeyLyricsTiming", key)) { // cancel: the value from before the overlay opened comes back
            { std::lock_guard<std::mutex> lk(lyrics_mutex_); lyrics_result_.delay = lyrics_edit_orig_delay_; }
            lyrics_edit_close();
        }
        return;
    }

    if (mode_ == Mode::SleepTimer) {
        // Sleep timer. Playback keeps running underneath; only this overlay takes
        // keys. Unknown keys are ignored (the footer lists every key).
        const int rows = 8; // 15 / 30 / 60 / 90 / 120 min, stop after song, fade out, off
        const bool arrow = last_key_was_arrow();
        if (arrow && key == 'A') { sleep_menu_row_ = (sleep_menu_row_ + rows - 1) % rows; return; } // up
        if (arrow && key == 'B') { sleep_menu_row_ = (sleep_menu_row_ + 1) % rows; return; }        // down
        if (!arrow && (key == '\r' || key == '\n') && sleep_menu_row_ == 6) {   // Fade out: a toggle, the overlay stays open
            settings_.sleep_fade = !settings_.sleep_fade;
            if (!settings_.sleep_fade) player_.set_fade(1.0f);
            save_settings(settings_);
            return;
        }
        if (!arrow && (key == '\r' || key == '\n')) {
            sleep_timer_apply(sleep_menu_row_);
            mode_ = Mode::Browse;
            return;
        }
        std::string sl_action = resolve_hotkey_action(key);
        if (sl_action.empty() && key >= 'a' && key <= 'z') sl_action = resolve_hotkey_action(key - 32);
        if (sl_action.empty() && key >= 'A' && key <= 'Z') sl_action = resolve_hotkey_action(key + 32);
        if (!arrow && (key == 27 || sl_action == "HKeySleepTimer")) mode_ = Mode::Browse;
        return;
    }

    if (mode_ == Mode::Equalizer) {
        // Equaliser. Playback keeps running underneath and the sound follows
        // every change immediately; only this overlay takes keys. Unknown keys
        // are ignored (the footer lists every key). ESC or the overlay hotkey
        // closes and saves.
        const bool arrow = last_key_was_arrow();

        // The "Save as:" prompt owns every key until ENTER or ESC: typing,
        // caret, marking and clipboard come from the shared single-line editor.
        if (eq_naming_) {
            if (!arrow && key == 27) { // cancel the prompt only, the overlay stays open
                eq_naming_ = false;
                eq_name_buf_.clear();
                eq_status_ = "Save cancelled";
                return;
            }
            if (!arrow && (key == '\r' || key == '\n')) { eq_commit_name(); return; }
            edit_focus("eq-name", eq_name_buf_);
            edit_text_key(eq_name_buf_, edit_caret_, edit_anchor_, key, kEqNameMaxBytes, nullptr);
            return;
        }

        const bool delete_was_armed = eq_delete_armed_; // a second DEL / X right after the first confirms
        eq_delete_armed_ = false;
        eq_status_.clear();

        if (arrow && key == 'C') { eq_band_ = (eq_band_ + 1) % kEqBands; return; }               // right
        if (arrow && key == 'D') { eq_band_ = (eq_band_ + kEqBands - 1) % kEqBands; return; }    // left
        if (arrow && key == 'A') { eq_set_gain(eq_band_, settings_.eq_gains[eq_band_] + 1.0f); return; } // up
        if (arrow && key == 'B') { eq_set_gain(eq_band_, settings_.eq_gains[eq_band_] - 1.0f); return; } // down
        if (!arrow && (key == ',' || key == '<')) { eq_select_preset(-1); return; }
        if (!arrow && (key == '.' || key == '>' || key == 9)) { eq_select_preset(+1); return; }
        if (!arrow && key == '0') { eq_set_gain(eq_band_, 0.0f); return; }
        if (!arrow && key == ' ') {
            settings_.eq_enabled = !settings_.eq_enabled;
            eq_apply();
            return;
        }
        if (!arrow && (key == 'r' || key == 'R')) { // back to Flat; the on/off state is left alone
            settings_.eq_gains = kEqPresets[0].gains;
            eq_last_preset_ = 0;
            eq_apply();
            return;
        }
        std::string eq_action = resolve_hotkey_action(key);
        if (eq_action.empty() && key >= 'a' && key <= 'z') eq_action = resolve_hotkey_action(key - 32);
        if (eq_action.empty() && key >= 'A' && key <= 'Z') eq_action = resolve_hotkey_action(key + 32);
        const bool eq_close_key = !arrow && (key == 27 || eq_action == "HKeyEqualizer");
        if (!eq_close_key && !arrow && (key == 's' || key == 'S')) { eq_begin_naming(); return; }
        if (!eq_close_key && !arrow && (key == kKeyDelete || key == 'x' || key == 'X')) {
            eq_delete_custom(delete_was_armed);
            return;
        }
        if (eq_close_key) {
            mode_ = Mode::Browse;
            save_settings(settings_);
        }
        return;
    }

    if (mode_ == Mode::Karaoke) {
        // ESC or the karaoke key close it; the playback keys (play/pause,
        // next/previous, seek, volume, mute, ...) work as on the main screen
        // and are simply handed to it; everything else is ignored.
        const bool arrow = last_key_was_arrow();
        auto karaoke_close = [this]() {
            mode_ = Mode::Browse;
            if (karaoke_size_dirty_) { save_settings(settings_); karaoke_size_dirty_ = false; }   // the lyrics size sticks
        };
        if (key == 27) { karaoke_close(); return; }
        // Alt+L: the lyrics timing overlay, over the karaoke screen. Judging an
        // offset is easiest against the big lyrics, so it opens from here as
        // well; lyrics_edit_from_karaoke_ brings karaoke back when it closes.
        if (key_is("HKeyLyricsTiming", key)) {
            const Mode before = mode_;
            mode_ = Mode::Browse;        // lyrics_edit_open() sets Mode::LyricsEdit itself
            lyrics_edit_open();
            if (mode_ == Mode::LyricsEdit) lyrics_edit_from_karaoke_ = true;
            else mode_ = before;         // no synced lyrics: stay in karaoke, the status line says why
            return;
        }
        if (arrow && (key == 'A' || key == 'B')) return;
        // SHIFT and + / SHIFT and - (the characters '*' and '_'): lyrics size 1..5. Fixed keys -- on a US keyboard
        // '*' is SHIFT+8. Only the lyrics change size, the pictures stay as they are.
        if (!arrow && (key == '*' || key == '_')) {
            const int before = settings_.karaoke_lyrics_size;
            settings_.karaoke_lyrics_size = std::clamp(before + (key == '*' ? 1 : -1), 1, 5);
            if (settings_.karaoke_lyrics_size != before) karaoke_size_dirty_ = true;
            karaoke_flash_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            return;
        }
        std::string act;
        if (!arrow) {
            const int lk = (key == '\r') ? '\n' : key;
            act = resolve_hotkey_action(lk);
            if (act.empty() && lk >= 'a' && lk <= 'z') act = resolve_hotkey_action(lk - 32);
            if (act.empty() && lk >= 'A' && lk <= 'Z') act = resolve_hotkey_action(lk + 32);
        }
        if (act == "HKeyKaraoke") { karaoke_close(); return; }
        static const char* const kPassThrough[] = {
            "HKeyTogglePlayPause", "HKeyPlayNextSong", "HKeyPlayPreviousSong", "HKeyShuffleNext",
            "HKeyIncreaseVolume", "HKeyDecreaseVolume", "HKeyToggleMute", "HKeyCyclePlayMode", "HKeyToggleNormalize",
        };
        bool pass = arrow && (key == 'C' || key == 'D');   // Left/Right: seek
        for (const char* a : kPassThrough) if (act == a) pass = true;
        if (pass) {
            mode_ = Mode::Browse;
            handle_key(key);
            if (mode_ == Mode::Browse) mode_ = Mode::Karaoke;
        }
        return;
    }

    if (mode_ == Mode::Cheatsheet) {
        if (key == 27 || key_is("HKeyCheatsheet", key)) mode_ = Mode::Browse;
        else if (key == 'A') --cheatsheet_scroll_; // up -- the table is longer than the screen
        else if (key == 'B') ++cheatsheet_scroll_; // down (clamped while rendering)
        return;
    }

    if (mode_ == Mode::Wizard) {
        // The wizard keeps its own page / part / contents state (src/wizard.cpp); here the key only has to be named.
        // Nothing reaches the player underneath: the wizard is a full-screen view like the cheat sheet, and playback
        // simply keeps running.
        const bool arrow = last_key_was_arrow();
        wizard::Key wk = wizard::Key::None;
        if (arrow) {
            const bool shifted = last_key_was_shifted();
            if (key == 'C') wk = wizard::Key::Right;
            else if (key == 'D') wk = wizard::Key::Left;
            else if (key == 'A') wk = shifted ? wizard::Key::PageUp : wizard::Key::Up;
            else if (key == 'B') wk = shifted ? wizard::Key::PageDown : wizard::Key::Down;
        } else if (key == 27 || key == 'q') wk = wizard::Key::Close;
        else if (key == '\r' || key == '\n') wk = wizard::Key::Enter;
        else if (key == '\b' || key == 127) wk = wizard::Key::Back;           // BACKSPACE and [t]: the table of content
        else if (key == 't' || key == 'T') wk = wizard::Key::Contents;
        else if (key_is("HKeyWizard", key)) wk = wizard::Key::Close;
        if (wizard::handle_key(wizard_st_, wk)) wizard_close();
        return;
    }

    if (mode_ == Mode::BulkAdd) {
        if (key == 27) { // cancel entirely -- discard buffer, results, and selection state
            mode_ = Mode::Browse;
            status_line_.clear();
            bulk_add_buffer_.clear();
            bulk_add_results_ready_ = false;
            pending_bulk_add_ = BulkAddResult{};
            bulk_add_selected_.clear();
            return;
        }

        if (bulk_add_results_ready_) {
            // --- Phase 2: navigate/star the fetched checklist. ---
            int total = static_cast<int>(pending_bulk_add_.items.size());
            if (key == 'A' && total > 0) { // up
                bulk_add_cursor_ = std::max(0, bulk_add_cursor_ - 1);
                if (bulk_add_cursor_ < bulk_add_scroll_) bulk_add_scroll_ = bulk_add_cursor_;
                return;
            }
            if (key == 'B' && total > 0) { // down
                bulk_add_cursor_ = std::min(total - 1, bulk_add_cursor_ + 1);
                if (bulk_add_cursor_ >= bulk_add_scroll_ + kBulkAddVisibleRows) bulk_add_scroll_ = bulk_add_cursor_ - kBulkAddVisibleRows + 1;
                return;
            }
            if (key == ' ' && total > 0) { // toggle star on the hovered row
                if (bulk_add_cursor_ < static_cast<int>(bulk_add_selected_.size())) {
                    bulk_add_selected_[bulk_add_cursor_] = !bulk_add_selected_[bulk_add_cursor_];
                }
                return;
            }
            if (key == 'a') { commit_bulk_add(/*all=*/true); return; }        // "ALL"
            if (key == '\r' || key == '\n') { commit_bulk_add(/*all=*/false); return; } // "[SELECT]"
            return;
        }

        // --- Phase 1: typing the link. ---
        if (key == '\r' || key == '\n') {
            if (!bulk_add_buffer_.empty() && !bulk_add_in_progress_.load()) {
                status_line_ = "fetching playlist ...";
                launch_bulk_add_async(bulk_add_buffer_);
            }
            return; // stays open -- poll_pending_bulk_add() moves to phase 2 once the fetch resolves
        }
        if (key == 127 || key == 8) { pop_utf8_char(bulk_add_buffer_); return; }
        // Same bug as Mode::Search below: arrow keys collapse to 'A'-'D',
        // which sit inside 32-126 and would otherwise get typed as literal
        // letters into the link being entered.
        if ((key == 'A' || key == 'B' || key == 'C' || key == 'D')) return;
        if (is_text_key(key) && bulk_add_buffer_.size() < 200) bulk_add_buffer_ += static_cast<char>(key);
        return;
    }

    if (mode_ == Mode::RetryLyrics) {
        if (rl_confirm_delete_) {   // "delete these lyrics for good?" -- only y confirms, anything else backs out
            if (key == 'y' || key == 'Y') { rl_confirm_delete_ = false; rl_delete_version(); }
            else rl_confirm_delete_ = false;
            return;
        }
        if (key == 27) { mode_ = Mode::Browse; return; } // cancel entirely, nothing submitted

        std::vector<RLField> fields = rl_visible_fields();
        auto cur_pos = std::find(fields.begin(), fields.end(), rl_focus_);
        int idx = (cur_pos != fields.end()) ? static_cast<int>(cur_pos - fields.begin()) : 0;

        const int nver = static_cast<int>(rl_versions_.size());
        if (rl_focus_ == RLField::Source) {   // the version list: up / down walk the rows, SPACE picks one, d deletes one
            if (key == 'B' && last_key_was_arrow() && rl_src_sel_ < nver - 1) { ++rl_src_sel_; rl_fix_scroll(); return; }
            if (key == 'A' && last_key_was_arrow() && rl_src_sel_ > 0) { --rl_src_sel_; rl_fix_scroll(); return; }
            if (key == ' ') { rl_choose_version(); return; }
            if (key == 'd' && !last_key_was_arrow()) {
                if (rl_src_sel_ < 0 || rl_src_sel_ >= nver) return;
                if (rl_versions_[static_cast<size_t>(rl_src_sel_)].kind == LyricsVersion::Kind::Embedded) {
                    status_line_ = "lyrics embedded in the file cannot be deleted here";
                    return;
                }
                rl_confirm_delete_ = true;
                return;
            }
        }
        if (key == 9 || key == 'B') { // Tab / down -- next field
            idx = (idx + 1) % static_cast<int>(fields.size());
            rl_focus_ = fields[idx];
            if (rl_focus_ == RLField::Source) { rl_src_sel_ = 0; rl_fix_scroll(); }
            return;
        }
        if (key == 'A') { // up -- previous field
            idx = (idx - 1 + static_cast<int>(fields.size())) % static_cast<int>(fields.size());
            rl_focus_ = fields[idx];
            if (rl_focus_ == RLField::Source) { rl_src_sel_ = std::max(0, nver - 1); rl_fix_scroll(); }   // arrived from below: the last row
            return;
        }
        if (key == '\r' || key == '\n') { rl_submit(); return; } // [ENTER] Refetch, works from any field (the old copy is kept)
        if (rl_focus_ == RLField::Source) return;                // nothing to type into the list

        if (bool* b = rl_bool_ptr(rl_focus_)) {
            if (key == ' ') { // toggle -- checkboxes are the only thing Space does anything to
                bool new_val = !*b;
                // Enforce the radio group: turning one of
                // slowed/ultra-slowed/spedup on clears the other two.
                // Reverb/remix/other stay independent of this and of
                // each other.
                if (rl_focus_ == RLField::TypeSlowed || rl_focus_ == RLField::TypeUltraSlowed || rl_focus_ == RLField::TypeSpedup) {
                    rl_slowed_ = rl_ultra_slowed_ = rl_spedup_ = false;
                }
                *b = new_val;
                // Turning RemixText/OtherText off (unchecking) drops
                // them from rl_visible_fields() -- if focus was sitting
                // on one of those now-hidden text fields, land back on
                // the checkbox that just hid it instead of a field that
                // no longer exists in the nav order.
                return;
            }
            return; // typing/backspace do nothing on a checkbox field
        }

        if (std::string* t = rl_text_ptr(rl_focus_)) {
            if (key == 127 || key == 8) { pop_utf8_char(*t); return; }
            // 'A'/'B' (up/down) are already intercepted above for field
            // navigation, but 'C'/'D' (right/left) fall through to here
            // uncaught -- same bug as Mode::Search below, where an arrow
            // code inside the printable range gets typed as a literal
            // letter instead of being recognized as an arrow key.
            if (key == 'C' || key == 'D') return;
            if (is_text_key(key) && t->size() < 200) *t += static_cast<char>(key);
            return;
        }
        return;
    }

    if (mode_ == Mode::Search) {
        if (key == 27) {
            // Cancel: put the view back exactly as it was before '/' was
            // pressed, discarding whatever the live preview below was
            // showing.
            list_source_ = pre_search_list_source_;
            last_local_query_ = pre_search_local_query_;
            refresh_local_view();
            mode_ = Mode::Browse;
            return;
        }
        if (key == '\r' || key == '\n') {
            // Moved through the results with Up/Down while typing: ENTER plays the highlighted entry right away (the
            // search is kept as the list filter, the list keeps the focus and the highlight). Without that it only
            // commits the search, as before.
            const bool picked = search_nav_moved_;
            const int sel = selected_, scr = scroll_;
            const ListSource src = list_source_;
            submit_search();
            mode_ = Mode::Browse;
            const size_t len = (list_source_ == ListSource::Local) ? local_view_.size()
                             : (list_source_ == ListSource::Folder) ? folder_view_.size()
                             : (list_source_ == ListSource::Playlist) ? playlist_view_.size() : 0;
            if (picked && list_source_ == src && sel >= 0 && static_cast<size_t>(sel) < len) {
                selected_ = sel;
                scroll_ = std::clamp(scr, std::max(0, sel - list_nav_rows() + 1), sel);
                queue_focus_ = false;
                play_selected();
            }
            search_nav_moved_ = false;
            return;
        }
        // Up/Down navigate the live preview below the box -- claimed before
        // the caret keys because they are exactly the two arrow letters
        // edit_text_key() would otherwise drop ("nowhere to go vertically"),
        // and navigating the list you are filtering is worth more here than
        // a second way to do nothing. Left/Right used to seek by 5 seconds
        // from inside this box too; they are the caret keys now, since a
        // text field needs them more and seek still works from Browse.
        if (last_key_was_arrow() && (key == 'A' || key == 'B')) { // up / down
            search_nav_moved_ = true;
            // SHIFT+Up/Down: whole page at a time while the big list overlay is open.
            if (list_overlay_active() && last_key_was_shifted()) { list_overlay_page(key == 'A' ? -1 : 1); return; }
            if (key == 'A') {
                if (selected_ > 0) --selected_;
                if (selected_ < scroll_) scroll_ = selected_;
                return;
            }
            // While actively typing "p:...", the live preview below is
            // already showing playlist_view_ (see update_live_search_preview())
            // -- navigate that instead of local_view_ in that case, same
            // as Enter/submit_search() would commit to.
            size_t list_len = (list_source_ == ListSource::Playlist) ? playlist_view_.size()
                            : (list_source_ == ListSource::Folder) ? folder_view_.size()
                            : local_view_.size();
            if (list_len > 0 && selected_ < static_cast<int>(list_len) - 1) ++selected_;
            if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
            return;
        }
        edit_focus("browse-search", search_buffer_);
        const bool search_changed = edit_text_key(search_buffer_, edit_caret_, edit_anchor_, key, 120, &status_line_);
        if (search_changed) { update_live_search_preview(); search_nav_moved_ = false; }
        return;
    }

    // Mode::Browse
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size()
                     : (list_source_ == ListSource::Online) ? online_view_.size()
                     : (list_source_ == ListSource::Folder) ? folder_view_.size()
                     : playlist_view_.size();

    // Big list overlay (SHIFT+l): SHIFT+Up/Down turn a whole page, ESC closes
    // it. Matched on the raw arrow codes + the shift flag, like SHIFT+b below
    // -- the input layer reports Shift+Up/Down as a plain 'A'/'B' arrow with
    // last_key_was_shifted() set, so there is no hotkey string for them. With
    // the overlay closed both keys fall through and behave exactly like plain
    // Up/Down, as they always did.
    // Big folder overlay (SHIFT+f): takes the keys it needs (tree navigation, search, `f`), the playback keys fall through.
    if (folder_overlay_active() && folder_overlay_key(key)) return;
    if (convert_overlay_active() && convert_overlay_key(key)) return;
    if (list_overlay_active()) {
        if (last_key_was_arrow() && last_key_was_shifted() && (key == 'A' || key == 'B')) {
            list_overlay_page(key == 'A' ? -1 : 1);
            return;
        }
        if (key == 27) { list_overlay_close(); return; }
    }
    // Big queue overlay (SHIFT+k): same keys as the list overlay above.
    if (queue_overlay_active()) {
        if (last_key_was_arrow() && last_key_was_shifted() && (key == 'A' || key == 'B')) {
            queue_overlay_page(key == 'A' ? -1 : 1);
            return;
        }
        if (key == 27) { queue_overlay_close(); return; }
    }

    // Ctrl+Shift+U / Ctrl+Shift+Z: queue -> playlist editor / undo the last
    // clear. Sentinel keys (see terminal_ui.h), so they are matched directly.

    // (SHIFT+b, the AcoustID lookup of the hovered title, is the HKeyFetchTags action below. A typed capital B and the
    // Down arrow are the same value; resolve_hotkey_action() tells them apart with last_key_was_arrow().)
    // Hotkeys are resolved to an action name via settings_.hotkeys /
    // resolve_hotkey_action() instead of switching on the raw key
    // directly, so a rebinding in Settings > Reference (or config.txt)
    // actually changes what a keypress does. This closes a gap that
    // exists in the *original* Linux/macOS codebase too, not something
    // the Windows port introduced: resolve_hotkey_action() was already
    // there, fully implemented, but nothing ever called it -- the
    // switch below was hardcoded on literal characters no matter what
    // config.txt or the Settings UI said. Two small normalizations keep
    // every default binding behaving exactly as it did before this
    // change: CR (13, what Windows' _getch() actually sends for Enter)
    // is treated as LF (10, what hotkey_string_to_key("ENTER") maps to)
    // so Enter keeps working regardless of which one a given terminal
    // reports; and a letter that doesn't resolve on its own also tries
    // its opposite case, so rebinding an action to "n" still fires on
    // Shift+N the same way the old hardcoded "case \'n\': case \'N\':"
    // pairs always did, without having to special-case every letter
    // action individually.
    // SHIFT and the + key (the character '*' on a German keyboard): switch to the radio mode. Fixed key. Only here, in the
    // Browse view -- text fields and menus handled their keys above.
    if (key == '*' && !last_key_was_arrow()) { switch_mode_ = true; quit_ = true; return; }
    int lookup_key = (key == '\r') ? '\n' : key;
    std::string action = resolve_hotkey_action(lookup_key);
    if (action.empty() && lookup_key >= 'a' && lookup_key <= 'z') action = resolve_hotkey_action(lookup_key - 32);
    if (action.empty() && lookup_key >= 'A' && lookup_key <= 'Z') action = resolve_hotkey_action(lookup_key + 32);

    if (action == "HKeyRescan") {   // SHIFT+r: new files copied / uploaded while the app runs
        rescan_now();
    } else if (action == "HKeyFetchTags") {   // SHIFT+b: AcoustID lookup (audio fingerprint) of the hovered title
        std::string path;
        if (queue_focus_) {
            if (queue_selected_ >= 0 && queue_selected_ < static_cast<int>(queue_.size()) && queue_[queue_selected_].is_local) {
                path = path_utf8(queue_[queue_selected_].local_path);
            }
        } else if (list_source_ == ListSource::Local && !local_view_.empty() &&
                   selected_ >= 0 && selected_ < static_cast<int>(local_view_.size())) {
            path = path_utf8(local_view_[selected_].path);
        }
        if (path.empty()) {
            status_line_ = "fetch tags: only a local file can be fingerprinted";
        } else {
            meta_prompt_single_fetch(path); // asks the AcoustID disclaimer first
        }
    } else if (action == "HKeyLyricsTiming") {   // lyrics timing overlay
        lyrics_edit_open();
    } else if (action == "HKeyReadLyrics") {     // READ LYRICS
        readlyrics_open();
    } else if (action == "HKeyQueueToPlaylist") {   // the queue -> the playlist editor
        queue_to_playlist();
    } else if (action == "HKeyQueueUndoClear") {    // bring back the queue that was just cleared
        queue_undo_clear();
    } else if (action == "HKeySetting") {
        settings_open();
        mode_ = Mode::Settings;
        settings_tab_ = 0;
        settings_row_ = 0;
        settings_col_ = 0;
    } else if (action == "HKeyPlaylist") {
        playlist_open_editor();
    } else if (action == "HKeyMetaEditor") { // SHIFT+m: edit file names / tags, fetch metadata
        meta_open();
    } else if (action == "HKeyHistory") { // SHIFT+h: listening history (HISTORY / TOP TRACKS / HABITS)
        history_open();
    } else if (action == "HKeySpectroMenu") { // SHIFT+i: spectrogram options
        spectro_menu_row_ = kSrStyle;
        mode_ = Mode::SpectroMenu;
    } else if (action == "HKeyScopeBig") { // SHIFT+0: the big oscilloscope overlay on / off
        scope_big_ = !scope_big_;
        spectro_full_ = false;
        scope_big_color_edit_ = false;
        scope_metrics_open_ = false;
        force_redraw_ = true;
        status_line_ = scope_big_ ? "oscilloscope: full screen (ESC or " + hotkey_text("HKeyScopeBig", "=") + " closes, " + hotkey_text("HKeyOscMenu", "O") + " options)" : "";
    } else if (action == "HKeySpectroFull") { // SHIFT+u: spectrogram full screen on / off
        spectro_full_ = !spectro_full_;
        if (spectro_full_) scope_big_ = false;
        force_redraw_ = true;
        status_line_ = spectro_full_ ? "spectrogram: full screen (ESC or " + hotkey_text("HKeySpectroFull", "U") + " closes, " + hotkey_text("HKeySpectroMenu", "I") + " options)" : "";
    } else if (action == "HKeyScopeWindow") { // SHIFT+9: the oscilloscope in its own window (always, even with it off on ON/OFF)
        scope_window_toggle();
    } else if (action == "HKeySpectroWindow") { // SHIFT+8: the spectrogram in its own window (always)
        spectro_window_toggle();
    } else if (action == "HKeyOscMenu") { // SHIFT+o: oscilloscope tuning overlay
        osci_menu_row_ = 0;
        mode_ = Mode::OsciMenu;
    } else if (action == "HKeyNormMenu") { // SHIFT+v: loudness normalization overlay
        norm_menu_row_ = 0;
        mode_ = Mode::NormMenu;
    } else if (action == "HKeySleepTimer") { // SHIFT+z: sleep timer overlay
        sleep_menu_row_ = sleep_stop_after_track_ ? 5
                        : sleep_timer_active_ ? (sleep_timer_minutes_ == 15 ? 0 : sleep_timer_minutes_ == 30 ? 1
                                               : sleep_timer_minutes_ == 60 ? 2 : sleep_timer_minutes_ == 90 ? 3 : 4)
                        : 0;
        mode_ = Mode::SleepTimer;
    } else if (action == "HKeyEqualizer") { // SHIFT+e: equaliser overlay
        eq_open();
    } else if (action == "HKeyFolderOverlay") { // SHIFT+f: big folder overlay on/off
        if (folder_overlay_open_) folder_overlay_close(); else folder_overlay_open();
    } else if (action == "HKeyConvertOverlay") { // SHIFT+c: file conversion overlay on/off
        if (convert_overlay_open_) convert_overlay_close(); else convert_overlay_open();
    } else if (action == "HKeyListOverlay") { // SHIFT+l: big list overlay on/off
        if (list_overlay_open_) list_overlay_close(); else list_overlay_open();
    } else if (action == "HKeyQueueOverlay") { // SHIFT+k: big queue overlay on/off
        if (queue_overlay_open_) queue_overlay_close(); else queue_overlay_open();
    } else if (action == "HKeySwitchBetweenCards") { // Tab: toggle Up/Down + reorder focus between the list and the queue
        // The queue pane is hidden behind the big list overlay and the queue
        // always has focus inside the big queue overlay, so there is nothing
        // to switch to while either is up.
        if (!list_overlay_active() && !queue_overlay_active()) queue_focus_ = !queue_focus_;
    } else if (action == "HKeyNavigateUp") {
        if (queue_focus_) {
            if (queue_selected_ > 0) --queue_selected_;
            clamp_queue_selected();
        } else {
            if (selected_ > 0) --selected_;
            if (selected_ < scroll_) scroll_ = selected_;
        }
    } else if (action == "HKeyNavigateDown") {
        if (queue_focus_) {
            if (!queue_.empty() && queue_selected_ < static_cast<int>(queue_.size()) - 1) ++queue_selected_;
            clamp_queue_selected();
        } else {
            if (list_len > 0 && selected_ < static_cast<int>(list_len) - 1) ++selected_;
            // BUGFIX: selection could move past the visible window without
            // the window ever following it, leaving the highlighted row
            // invisible below row 8 instead of the list scrolling up.
            if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
        }
    } else if (action == "HKeySeekForward") {
        if (has_track_) player_.seek_relative(5.0);
    } else if (action == "HKeySeekBackward") {
        if (has_track_) player_.seek_relative(-5.0);
    } else if (action == "HKeyQueueMoveUp") { // move the hovering queue item up (only meaningful once you've Tab'd into the queue)
        queue_move_hovering(-1);
    } else if (action == "HKeyQueueMoveDown") { // move the hovering queue item down (only meaningful once you've Tab'd into the queue)
        queue_move_hovering(1);
    } else if (action == "HKeyQueueMoveTop") {    // Shift+4: hovering queue item to the top
        queue_move_to_edge(-1);
    } else if (action == "HKeyQueueMoveBottom") { // Shift+5: hovering queue item to the bottom
        queue_move_to_edge(1);
    } else if (action == "HKeyQueueLock") {       // "!": locked queue keeps played tracks
        queue_toggle_lock();
    } else if (action == "HKeyQueueAddEnd") {     // "e": hovering list track to the end of the queue
        if (queue_focus_) {
            status_line_ = "focus the track list (TAB) to add a track to the end of the queue";
        } else {
            queue_add_selected_end();
            log_event("added to end of queue");
        }
    } else if (action == "HKeyTogglePlayPause") {
        if (has_track_) { if (player_.is_paused()) player_.resume(); else player_.pause(); }
    } else if (action == "HKeyIncreaseVolume") {
        if (has_track_) player_.set_volume(std::min(100, player_.volume() + 5));
    } else if (action == "HKeyDecreaseVolume") {
        if (has_track_) player_.set_volume(std::max(0, player_.volume() - 5));
    } else if (action == "HKeyPlayNextSong") {
        // The queue (if any) takes priority, same as auto-advance-on-
        // finish does, and respects Shuffle/Queue-then-stop via
        // play_next_from_queue() -- a manual skip still always actually
        // skips, though: Repeat/Stop/Queue-then-stop only govern
        // *automatic* advance, not an explicit "next" press.
        if (!queue_.empty()) play_next_from_queue();
        else play_relative(1);
    } else if (action == "HKeyPlayPreviousSong") {
        // Relative to what's actually playing (see
        // current_track_list_index()), not the hover cursor. No queue
        // equivalent: a FIFO queue has no well-defined "previous" once
        // an item's been consumed.
        play_relative(-1);
    } else if (action == "HKeyShuffleNext") {
        // Shuffle to a random next track in the current list -- a
        // manual one-off jump, independent of Play Mode
        // (settings_.play_mode). Reuses the same play_relative_random()
        // the automatic Shuffle play mode already calls on auto-advance;
        // unlike PlayNextSong, this does not consult the queue at all --
        // shuffling picks from the browse list on purpose, since the
        // queue is a deliberately ordered, user-built list and jumping
        // it around at random would defeat the point of it.
        play_relative_random();
    } else if (action == "HKeyToggleLyrics" || action == "HKeyToggleLyricsBack") {   // "." forward, "," backward
        // Cycle the lyrics area without going through Settings > On/Off:
        // sphere -> oscilloscope -> spectrogram -> lyrics ... (the order is Settings -> ON/OFF -> "Viz Order 1..4"). The visuals are
        // the lyrics engine's "off" states (element_lyrics=false, no network
        // fetching) with settings_.lyric_viz saying which one is drawn
        // (0 = sphere, 1 = osci, 2 = spectro), so the cycle is just a walk over those
        // settings. Returning to the lyrics needs a nudge though: the only
        // other place that starts a fetch is track load (poll_pending_load),
        // so without it the lyrics would not appear until the next track.
        // Views switched off on the ON/OFF tab (Use Lyrics / Oscilloscope / Spectrogram / Sphere) are left out.
        const int cur = settings_.element_lyrics ? 3 : settings_.lyric_viz == 1 ? 1 : settings_.lyric_viz == 2 ? 2 : 0;
        const int next = settings_.viz_step(cur, action == "HKeyToggleLyricsBack" ? -1 : 1, false);
        static const char* const view_names[4] = {"sphere", "oscilloscope", "spectrogram", "lyrics"};
        if (!settings_.element_vizfield) {
            status_line_ = "the Viz/Lyrics field is switched off (Settings > ON/OFF > Viz/Lyrics Field)";
        } else if (next == cur) {
            status_line_ = std::string("lyrics area: ") + view_names[cur] + " (the other views are switched off on Settings -> ON/OFF)";
        } else if (next == 3) {                    // -> lyrics
            settings_.element_lyrics = true;
            if (has_track_) {
                std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
                last_lyrics_status_.clear(); // fresh 1.75s caption window, not a leftover from before it was off
                launch_lyrics_fetch(metadata_.name, artist, current_path_);
            }
            status_line_ = "lyrics area: lyrics";
        } else {
            settings_.element_lyrics = false;
            settings_.lyric_viz = next;   // 0 sphere, 1 osci, 2 spectro
            status_line_ = std::string("lyrics area: ") + view_names[next];
        }
    } else if (action == "HKeyAddHoveringSongToQueue") {
        // Add hovering song to queue (List focus) -- or, when the Queue
        // panel itself is focused, there's nothing hovering-in-the-list
        // to add, so it opens the bulk-add panel instead (paste a
        // YouTube playlist link, queue everything in it).
        if (queue_focus_) {
            mode_ = Mode::BulkAdd;
            bulk_add_buffer_.clear();
            bulk_add_results_ready_ = false;
            pending_bulk_add_ = BulkAddResult{};
            bulk_add_selected_.clear();
            bulk_add_cursor_ = 0;
            bulk_add_scroll_ = 0;
            status_line_.clear();
        } else {
            queue_add_selected();
            log_event("added to queue");
        }
    } else if (action == "HKeyClearQueue") { // SHIFT+x: clear the whole queue, after a Yes/No confirmation
        if (queue_.empty()) {
            status_line_ = "queue is already empty";
        } else {
            clear_queue_choice_ = 1; // default to No so a stray Enter never wipes the queue
            mode_ = Mode::ClearQueue;
        }
    } else if (action == "HKeyRemoveHoveringSongFromQueue") {
        queue_remove_hovering();
        log_event("removed from queue");
    } else if (action == "HKeyCyclePlayMode") {
        // Cycle play mode: list -> repeat -> shuffle -> stop -> queue then
        // stop -> list -- one key for all five instead of a separate
        // toggle per mode.
        settings_.play_mode = (settings_.play_mode + 1) % 5;
        queue_reset_lap();
        {
            // Indexed 0=list,1=repeat,2=shuffle,3=stop,4=queue then stop,
            // matching play_mode's own numbering (not cycle order).
            static const char* mode_names[] = {"list", "repeat", "shuffle", "stop", "queue then stop"};
            log_event(std::string("play mode: ") + mode_names[settings_.play_mode]);
        }
    } else if (action == "HKeyKaraoke") {
        // The karaoke overlay: the lyrics of the playing track over the whole screen.
        mode_ = Mode::Karaoke;
    } else if (action == "HKeyRefreshUi") {
        // Force a full redraw, for when a resize or terminal-session
        // switch raced the render loop and left a torn/stale frame on
        // screen. hard_clear is normally only set on a detected width or
        // mode change; this forces it once unconditionally on the very
        // next frame.
        force_redraw_ = true;
        log_event("ui refreshed");
    } else if (action == "HKeyConsole") {
        mode_ = Mode::Console;
    } else if (action == "HKeyToggleNormalize") { // loudness normalisation on/off, for A/B-ing it by ear
        settings_.normalize = !settings_.normalize;
        player_.set_normalization(settings_.normalize,
                                  static_cast<float>(settings_.normalize_target_lufs),
                                  static_cast<float>(settings_.normalize_max_boost_db));
        std::string msg = settings_.normalize ? "normalize: on" : "normalize: off";
        const float lufs = player_.track_lufs();
        if (settings_.normalize && has_track_ && !std::isnan(lufs)) {
            char buf[96];
            std::snprintf(buf, sizeof buf, " (track %.1f LUFS -> target %.0f, %+.1f dB)",
                          lufs, settings_.normalize_target_lufs,
                          static_cast<double>(settings_.normalize_target_lufs) - lufs);
            msg += buf;
        }
        status_line_ = msg;
        log_event(msg);
    } else if (action == "HKeyToggleMetaOnly") { // SHIFT+n: swap the lists between filename+metadata and metadata-only rows
        settings_.meta_only = !settings_.meta_only;
        std::string msg = settings_.meta_only ? "lists: metadata only (no filename)"
                                              : "lists: filename + metadata";
        status_line_ = msg;
        log_event(msg);
        resort_local_view_keep_selection(); // "title A-Z" follows the displayed title
        force_redraw_ = true;
    } else if (action == "HKeyToggleMute") { // force volume to 0 without touching pause state
        if (!muted_) {
            pre_mute_volume_ = player_.volume();
            player_.set_volume(0);
            muted_ = true;
            log_event("muted");
        } else {
            player_.set_volume(pre_mute_volume_);
            muted_ = false;
            log_event("unmuted");
        }
    } else if (action == "HKeyWizard") {
        wizard_open();
    } else if (action == "HKeyCheatsheet") {
        mode_ = Mode::Cheatsheet;
    } else if (action == "HKeyFilterForFolder") {
        if (list_source_ == ListSource::Local && !local_view_.empty() &&
            selected_ >= 0 && selected_ < static_cast<int>(local_view_.size())) {
            folder_filter_ = path_utf8(local_view_[selected_].path.parent_path());
            // Filtering by folder also drops back to folder order (T cycles
            // away from it again), so the pane title reads "sort: folder
            // order" and the files sit in the order they are on disk.
            local_sort_mode_ = 0;
            refresh_local_view();
            log_event("filtered: " + path_utf8(path_from_utf8(folder_filter_).filename()));
        }
    } else if (action == "HKeyClearFilter") {
        if (!folder_filter_.empty()) {
            folder_filter_.clear();
            refresh_local_view();
            log_event("filter cleared");
        }
    } else if (action == "HKeyRetryLyrics") { // opens the manual title/artist override form
        if (!settings_.lyrics_on()) {
            status_line_ = "lyrics are turned off (Settings > ON/OFF > Use Lyrics Engine / Viz/Lyrics Field)";
        } else if (has_track_) {
            rl_open_from_current_track();
            mode_ = Mode::RetryLyrics;
        }
    } else if (action == "HKeyToggleWaveform") { // toggle waveform style (raw/smooth) directly, without going into Settings
        settings_.waveform_smooth = !settings_.waveform_smooth;
        recompute_waveform_for_current_track();
        log_event(settings_.waveform_smooth ? "waveform: smooth" : "waveform: raw");
    } else if (action == "HKeyDownloadStream") { // save cached stream to the configured download folder
        if (has_track_) {
            if (path_utf8(current_path_).find(".cache") != std::string::npos || metadata_.location == "youtube" ||
                metadata_.location == "soundcloud" || metadata_.location == "bandcamp") {
                // Same folder the DOWNLOAD PATH setting promises everywhere
                // else (see load_library()'s cache-dir injection and the
                // Settings > PATHS tab's Download Path field): the
                // configured path, or ~/.cache/mousiki when none is set --
                // never settings_.local_music_paths[0]/$HOME/Music, which
                // this used to fall back to and had nothing to do with the
                // folder the user actually configured for downloads.
                std::string dest_dir = path_utf8(cache_.download_dir());
                std::error_code ec;
                fs::create_directories(path_from_utf8(dest_dir), ec);

                std::string safe_name = metadata_.name;
                for (char& c : safe_name) if (c == '/' || c == '\\') c = '_';
                std::string safe_artist = (metadata_.artist == "-" ? "" : metadata_.artist);
                for (char& c : safe_artist) if (c == '/' || c == '\\') c = '_';

                std::string filename = safe_artist.empty() ? safe_name : safe_name + " - " + safe_artist;
                filename += path_utf8(current_path_.extension());

                fs::path dest_path = path_from_utf8(dest_dir) / path_from_utf8(filename);
                if (fs::exists(dest_path, ec)) {
                    status_line_ = "already saved: " + path_utf8(dest_path.filename());
                } else {
                    fs::copy_file(current_path_, dest_path, fs::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        fs::remove(current_path_, ec);
                        current_path_ = dest_path; // update so sidecar lyrics go to the new folder
                        metadata_.location = dest_dir;
                        status_line_ = "saved to " + path_utf8(dest_path);
                        refresh_local_view();
                    } else {
                        status_line_ = "failed to save: " + ec.message();
                    }
                }
            } else {
                status_line_ = "not a cached stream";
            }
        }
    } else if (action == "HKeyCycleSortMode") { // cycle local-list sort mode (folder order -> title A-Z -> artist A-Z)
        local_sort_mode_ = (local_sort_mode_ + 1) % 3;
        refresh_local_view();
        log_event(std::string("sort: ") + sort_mode_name(local_sort_mode_, settings_.meta_only));
    } else if (action == "HKeyPlay") {
        play_selected();
    } else if (action == "HKeySearch") {
        if (queue_overlay_open_) queue_overlay_close(); // the search filters the list underneath, which the queue overlay covers
        mode_ = Mode::Search;
        search_buffer_.clear();
        search_nav_moved_ = false;
        pre_search_list_source_ = list_source_;
        pre_search_local_query_ = last_local_query_;
    } else if (action == "HKeyQuit") {
        // Only the bound key itself quits: the case fallback above would also make SHIFT+q quit, which is too easy to hit.
        auto qk = settings_.hotkeys.find("HKeyQuit");
        if (qk != settings_.hotkeys.end() && hotkey_string_to_key(qk->second) == lookup_key) quit_ = true;
    } else if (key == 27) {
        // ESC -- back to the home view: full local library, no filter,
        // from the top. Same destination regardless of how buried you
        // are (mid search results, viewing online results, scrolled
        // deep into the list). Not in settings_.hotkeys / kRefRows at all,
        // on purpose: this mirrors the original, which likewise has no
        // HKeyEsc entry -- ESC is a fixed shortcut, not something meant
        // to be rebound (see the SYSTEM (MAIN UI) section of
        // the cheat sheet, '?', which is where fixed keys like this one
        // are documented -- the Reference tab only lists rebindable ones).
        list_source_ = ListSource::Local;
        last_local_query_.clear();
        folder_filter_.clear();
        refresh_local_view();
        status_line_.clear();
    }
}

// ---------------------------------------------------------------------
// Lazy metadata probing for whatever's currently visible in the list
// ---------------------------------------------------------------------

// Tries to fully resolve a row -- duration AND tags -- using nothing but
// in-process header parsing (native_duration.h): no subprocess, safe to
// call from the render thread. For a well-formed tag in a format this
// covers (MP3/ID3v2, FLAC/Ogg/Opus's Vorbis comments, or M4A/MP4/AAC's
// iTunes-style atoms -- covers every format probe_duration_native()
// already parses for duration) this is a complete answer on its own. For
// anything it can't handle (an unsupported/unusual tag layout, or a
// container this doesn't recognize at all), it comes back with whatever
// duration native parsing found, tags_resolved left false -- callers fall
// back to ffprobe for tags in that case.
//
// This is what turns "resolving an 800-track library's metadata" from
// hundreds of ffprobe subprocess spawns (the actual bottleneck -- each one
// costs tens to hundreds of milliseconds, worse under antivirus real-time
// scanning on Windows) into a few pread() syscalls per file for the
// overwhelming majority of a typical library, whatever mix of formats it's in.
RowMeta try_native_row_meta(const fs::path& path) {
    RowMeta rm;
    uint32_t dur = probe_duration_native(path);
    if (dur > 0) rm.duration_sec = static_cast<double>(dur);

    std::string ext = lower(path_utf8(path.extension()));
    NativeId3Tags tags;
    if (ext == ".mp3") tags = probe_id3v2_native(path);
    else if (ext == ".flac") tags = probe_flac_native(path);
    else if (ext == ".ogg" || ext == ".opus") tags = probe_ogg_native(path);
    else if (ext == ".m4a" || ext == ".mp4" || ext == ".aac") tags = probe_mp4_native(path);

    if (tags.resolved) {
        rm.title = tags.title;
        rm.artist = tags.artist;
        rm.album = tags.album;
        rm.year = tags.year; // TYER/TDRC (MP3), DATE (Vorbis), ©day (MP4)
        rm.tags_resolved = true; // fully resolved without ffprobe
    }
    return rm;
}

// This used to call probe_row_meta() here directly — which spawns a real
// ffprobe SUBPROCESS, synchronously, on the render thread, once per
// newly-visible row, every single frame a new row scrolled into view.
// Scroll through a big library quickly (or hit one slow/hanging file —
// weird encode, flaky storage) and the whole UI thread — rendering AND
// input — blocks for however long those subprocess calls take, with no
// way to even press 'q' to get out of it. That's what was crashing the
// whole player.
//
// Fix: use the native in-process binary-header parser (native_duration.h
// — pure pread() syscalls, no subprocess, effectively can't hang) for
// duration AND (for MP3) tags here, directly on the render thread —
// genuinely safe now. Anything that parser can't handle (unsupported
// format, corrupt file, a tag layout probe_id3v2_native() declines) gets
// picked up by the background sweep (launch_row_meta_resolver, started
// once after the initial scan) that walks the whole library and falls
// back to ffprobe there — off the main thread entirely, never gating
// rendering or input.
void App::ensure_visible_row_meta() {
    if (list_source_ != ListSource::Local) return;
    for (int i = scroll_; i < std::min(static_cast<int>(local_view_.size()), scroll_ + list_nav_rows()); ++i) {
        const auto& t = local_view_[i];
        std::string key = path_utf8(t.path);
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            if (row_meta_cache_.count(key)) continue; // already resolved (native path or background sweep)
        }
        RowMeta rm = try_native_row_meta(t.path);
        if (rm.duration_sec > 0 || rm.tags_resolved) {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            // No cap on the cache (about 200 bytes a track): a cap left the tags of every file past it unsearchable.
            row_meta_cache_[key] = rm;
        }
        // A visible row whose tags resolved right here (native, no ffprobe
        // needed) still needs the version bump: this row scrolling into
        // view generally happens well before the background sweep ever
        // gets to it, so without this the same "search doesn't notice
        // metadata that arrived after the filter ran" gap would just
        // reappear for the fast native path instead of the slow ffprobe
        // one. See poll_pending_row_meta_tags().
        if (rm.tags_resolved) row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
        // else: leave unresolved — native parsing is cheap enough to just
        // retry next frame, and the background sweep will fill it in via
        // ffprobe regardless, so there's no real cost to not caching a miss.
    }
}

// Re-runs quantization on the already-decoded PCM for whatever's
// currently playing — used when the smooth/raw toggle changes mid-track,
// so the effect shows up right away instead of only on the next track.
// No re-decode needed: current_pcm_ already holds everything decoded so
// far (streaming decode may still be filling it in, hence the acquire
// load of `available` rather than assuming it's complete).
void App::recompute_waveform_for_current_track() {
    if (!has_track_ || !current_pcm_) return;
    std::shared_ptr<StreamingPcm> pcm = current_pcm_;
    bool smooth = settings_.waveform_smooth;
    // BUG FIX #5: increment epoch before spawning — any already-running
    // waveform thread will see its own epoch is stale and discard its
    // result instead of racing to overwrite pending_waveform_envelope_.
    int my_epoch = ++waveform_epoch_;
    std::thread([this, pcm, smooth, my_epoch]() { run_guarded("waveform pass", [&] {
        if (pcm->windowed) {
            // a long track: from the figures of its analysis pass (the samples are not all held)
            if (!pcm->analysis_done.load(std::memory_order_acquire) || pcm->env_sumsq.empty()) return;
            auto envelope = WaveformQuantizer::envelope_from_bins(pcm->env_sumsq, StreamingPcm::kEnvGran,
                                                                  pcm->total_frames.load(), 4096, smooth);
            std::lock_guard<std::mutex> lk(waveform_mutex_);
            if (my_epoch != waveform_epoch_.load()) return;
            pending_waveform_envelope_ = std::move(envelope);
            waveform_pending_ready_ = true;
            return;
        }
        size_t n = pcm->available.load(std::memory_order_acquire);
        if (n == 0) return;
        // `n` counts frames; the buffer is interleaved, so copy n * channels floats.
        std::vector<float> snapshot(pcm->data.begin(),
                                    pcm->data.begin() + static_cast<long>(n * static_cast<size_t>(pcm->channels)));
        auto envelope = WaveformQuantizer::generate_high_res_envelope(snapshot, 4096, smooth, pcm->channels);
        std::lock_guard<std::mutex> lk(waveform_mutex_);
        if (my_epoch != waveform_epoch_.load()) return; // superseded — discard
        pending_waveform_envelope_ = std::move(envelope);
        waveform_pending_ready_ = true;
    }); }).detach();
}

void App::launch_row_meta_resolver() {
    // One sweep per session -- except that rescan_library() clears the
    // flag when the configured path set changes, which is the one
    // situation where files this sweep has already passed (or never saw)
    // can appear.
    if (row_meta_resolver_started_.exchange(true)) return;
    auto paths = std::make_shared<std::vector<fs::path>>();
    paths->reserve(all_local_tracks_.size());
    for (auto& t : all_local_tracks_) paths->push_back(t.path);

    // BUG FIX #5: this used to be a single thread working through the
    // whole library one ffprobe subprocess at a time. Each call is a real
    // process spawn -- slow, and especially so on Windows (process
    // creation there routinely runs tens of milliseconds even before
    // antivirus real-time scanning gets a look at ffprobe.exe, which adds
    // more on top). Sequentially, that means a library of even a few
    // hundred tracks can take a long time to fully resolve, and a file
    // that only happens to sit later in scan order is simply unresolved
    // -- and therefore not yet findable by its tags -- for that entire
    // stretch. A small fixed pool of worker threads pulling from a shared
    // index lets several ffprobe calls be in flight at once (this is
    // I/O/process-spawn-bound work, not CPU-bound, so a handful of
    // threads is a real speedup and not just contention), cutting the
    // time-to-fully-searchable by roughly the pool size with no change to
    // per-file behavior.
    constexpr int kResolverWorkers = 4;
    auto next_index = std::make_shared<std::atomic<size_t>>(0);

    for (int worker = 0; worker < kResolverWorkers; ++worker) {
        std::thread([this, paths, next_index]() { run_guarded("library metadata sweep", [&] {
            for (;;) {
                size_t i = next_index->fetch_add(1, std::memory_order_relaxed);
                if (i >= paths->size()) return;
                const fs::path& p = (*paths)[i];
                std::string key = path_utf8(p);
                {
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    auto it = row_meta_cache_.find(key);
                    // BUG FIX #2: a cache hit here used to be treated as "this
                    // file is done", which was true back when the cache only
                    // ever held a duration. Now ensure_visible_row_meta() can
                    // populate an entry with JUST duration_sec (from the cheap
                    // native header parser, no tags) before this sweep ever
                    // reaches the file -- for any file with a well-formed
                    // MP3/FLAC/etc. header, that's the common case, since the
                    // render thread visits it first. Skipping here on presence
                    // alone meant that entry's artist/title/album tags -- the
                    // ones search actually needs -- never got fetched, because
                    // this is the only place that runs the ffprobe tag lookup.
                    // Only a real prior probe_row_meta() result (tags_resolved)
                    // means there's genuinely nothing left to fetch.
                    if (it != row_meta_cache_.end() && it->second.tags_resolved) continue;
                }
                RowMeta rm = try_native_row_meta(p);
                // BUG FIX #6 / perf: try the native, no-subprocess ID3v2
                // reader first. For a well-formed MP3 tag -- the common
                // case in any real library -- this resolves the file with
                // a few pread() calls instead of a whole ffprobe process
                // spawn, which is what actually made an 800-track library
                // take minutes: ffprobe's per-call overhead (worse under
                // Windows antivirus real-time scanning) dominates
                // completely once you're spawning hundreds of them. Only
                // fall back to ffprobe for whatever native declined --
                // a non-MP3 format, or an MP3 tag laid out in a way the
                // lightweight reader won't guess at (see native_duration.h).
                if (!rm.tags_resolved) {
                    RowMeta ff = probe_row_meta(p);
                    if (rm.duration_sec <= 0) rm.duration_sec = ff.duration_sec; // keep native's duration if we already had it
                    rm.title = ff.title;
                    rm.artist = ff.artist;
                    rm.album = ff.album;
                    rm.tags_resolved = ff.tags_resolved;
                }
                {
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    // BUG FIX #4: only a brand-new key is subject to the cap --
                    // overwriting a key already present doesn't grow the map,
                    // so it must never be blocked by "already at the cap", or
                    // no file already in the cache could ever be promoted from
                    // a duration-only entry to one with real tags once the
                    // cache first filled up.
                    row_meta_cache_[key] = rm;   // no cap: ~200 bytes a track, and a cap left the tags of every file past it unsearchable
                }
                // BUG FIX #3: tell the main thread real tags for a file just
                // landed. Without this, a search typed (or already showing)
                // before this sweep reached the file stays frozen on whatever
                // it found at the time -- the file becomes searchable by its
                // metadata from this point on, but nothing ever re-runs the
                // filter to notice, so it looks like metadata search "doesn't
                // work" for exactly the files whose tags resolve after the
                // fact. poll_pending_row_meta_tags() (called every frame,
                // same as the other poll_pending_* functions) picks this up.
                if (rm.tags_resolved) row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
            }
        }); }).detach();
    }
}

// Every frame's counterpart to launch_row_meta_resolver(): re-filters the
// currently-shown local list once new tags have arrived in the background,
// so a metadata match (like an artist tag) shows up on its own instead of
// requiring the user to retype the query after the sweep happens to catch
// up. A no-op the vast majority of frames (the version check is a single
// relaxed atomic load), so this is cheap to call unconditionally.
void App::poll_pending_row_meta_tags() {
    uint64_t v = row_meta_tags_version_.load(std::memory_order_relaxed);
    if (v == row_meta_tags_seen_) return;
    // The sweep bumps the version for every file it resolves; with tens of thousands of files, re-filtering the whole
    // library on each of those frames would keep the UI busy. At most four times a second -- the rest waits for the
    // next frame (row_meta_tags_seen_ stays behind, so nothing is lost).
    const auto now = std::chrono::steady_clock::now();
    if (now - row_meta_refilter_at_ < std::chrono::milliseconds(250)) return;
    row_meta_refilter_at_ = now;
    row_meta_tags_seen_ = v;

    if (list_source_ != ListSource::Local) return; // nothing local is even on screen right now

    // Mirror update_live_search_preview()'s "which query is live right now"
    // logic: mid-typing uses search_buffer_, otherwise the last committed
    // query. Either way, leave "s:"/"p:" alone -- those aren't local
    // filters and don't read row_meta_cache_ at all.
    std::string q = (mode_ == Mode::Search) ? search_buffer_ : last_local_query_;
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.size() >= 2) {
        std::string prefix = lower(q.substr(0, 2));
        size_t plen = 0;
        if (online_prefix(q, plen) >= 0 || prefix == "p:") return;
    }

    // Re-filtering can reorder/shrink the list (a track that just became a
    // metadata match can appear anywhere by rank), so re-anchor on the
    // previously-selected track's identity rather than leaving `selected_`
    // pointing at whatever index now happens to sit there.
    fs::path prev_selected_path;
    bool had_selection = selected_ >= 0 && selected_ < static_cast<int>(local_view_.size());
    if (had_selection) prev_selected_path = local_view_[static_cast<size_t>(selected_)].path;

    local_view_ = filter_and_rank_local_view(q);

    if (had_selection) {
        selected_ = 0;
        for (size_t i = 0; i < local_view_.size(); ++i) {
            if (local_view_[i].path == prev_selected_path) { selected_ = static_cast<int>(i); break; }
        }
    }
    if (local_view_.empty()) selected_ = 0;
    else if (selected_ >= static_cast<int>(local_view_.size())) selected_ = static_cast<int>(local_view_.size()) - 1;
    if (scroll_ > selected_) scroll_ = selected_;
    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
}

// ---------------------------------------------------------------------
// Idle-state cassette picture
// ---------------------------------------------------------------------
// Braille cassette shown in the metadata panel's lyrics/sphere column
// whenever no track is loaded (never started, playback stopped in "stop"
// mode, or the file that was playing got deleted). Byte-for-byte copy of
// tape_ascii.txt: 15 rows of 30 braille cells -- deliberately the same
// geometry as the rotating disk (disk_art.cpp), so it fills that column at
// exactly the panel's size and can be tinted with the same top-to-bottom
// disk gradient. See the !has_track_ branch of build_metadata_panel().
//
// The characters are written as \uXXXX escapes so this file stays pure
// ASCII: no editor, copy or checkout can re-encode them, while /utf-8
// still compiles them into exactly the same UTF-8 bytes.
static constexpr const char* kTapeArt[] = {
    "\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2880\u2864\u28c4\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2880\u28c0\u28e4\u28c0\u28c0\u2800\u2800\u2880\u2840\u2814\u2801\u2800\u28b8\u2803\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2820\u2802\u2809\u2809\u2809\u2811\u28b2\u28fe\u2849\u2800\u2800\u2800\u2800\u2808\u2801\u2800\u2800\u28c0\u2814\u2802\u2801\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2847\u2800\u2800\u2800\u2800\u2800\u28b8\u2800\u2809\u28a6\u2800\u2800\u2800\u2800\u2800\u2800\u28b0\u2801\u2800\u2800\u2800\u2880\u28c0\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2810\u28c4\u2800\u2800\u2800\u2800\u2808\u2823\u2804\u280a\u2800\u28c0\u28c0\u28e4\u28e4\u28f4\u28fe\u28f6\u28ff\u28ff\u28ff\u28ff\u28ff\u2847\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2800\u2808\u28f3\u28c4\u28e0\u28e4\u28f4\u28f6\u28f6\u28ff\u28ff\u28bf\u28df\u28ef\u28bf\u28dd\u28af\u2877\u28fb\u28be\u28df\u28ff\u28fb\u28ff\u2800\u2800\u2800\u2800",
    "\u28e4\u28f6\u28f6\u28fe\u28ff\u28bf\u28fb\u289f\u28ef\u28bf\u2875\u28fb\u28ae\u283f\u28ee\u28b7\u28ef\u28fb\u28fd\u286b\u28bf\u289d\u286b\u285b\u28cd\u28bf\u2846\u2800\u2800\u2800",
    "\u28bb\u28fe\u28ff\u28ff\u28ef\u28b7\u28ef\u287b\u28f7\u28eb\u285f\u287d\u28cf\u287b\u2873\u286d\u286a\u28d6\u289c\u28ce\u28b3\u289d\u2854\u286e\u28d5\u283d\u28f7\u2800\u2800\u2800",
    "\u2818\u28ff\u283f\u28de\u289b\u2873\u286d\u28f9\u28a2\u28a7\u2879\u28ea\u28ce\u28de\u283c\u282e\u28be\u287e\u28de\u285b\u281a\u2897\u28dd\u28ae\u286a\u2873\u28fb\u2844\u2800\u2800",
    "\u2800\u28bf\u2857\u2875\u28d9\u288e\u285e\u289c\u2837\u28bb\u28ef\u287b\u28de\u28b7\u28a9\u28a3\u28b9\u287e\u28ef\u2866\u2820\u28ed\u2897\u28b5\u2839\u285c\u287d\u28e7\u2800\u2800",
    "\u2800\u2838\u28df\u28f2\u2809\u28de\u28ba\u28eb\u2840\u28c0\u28bf\u28dd\u28de\u287f\u2874\u2875\u2873\u283b\u280d\u281b\u2899\u2801\u2829\u2800\u2804\u2800\u28b9\u28bf\u2840\u2800",
    "\u2800\u2800\u28ff\u28d5\u289d\u2855\u28c7\u283b\u2832\u281a\u280b\u288a\u2808\u28ec\u28c0\u28e4\u2802\u2880\u2801\u2800\u2804\u2800\u28c2\u28c0\u28ec\u28e4\u28fc\u285d\u2847\u2800",
    "\u2800\u2800\u28b8\u28ef\u280e\u2880\u2800\u28b2\u2866\u2815\u2800\u2804\u2800\u28c3\u28c8\u28ed\u28e4\u28f4\u28f6\u28fe\u28ff\u283f\u283f\u281f\u281b\u280b\u2809\u2809\u2800\u2800",
    "\u2800\u2800\u2800\u28ff\u28f3\u28c0\u28ec\u28e4\u28f4\u28f6\u28fe\u28ff\u283f\u283f\u281f\u281b\u280b\u2809\u2809\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2800\u2839\u283d\u281f\u281b\u280b\u2809\u2809\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
};

static constexpr int kTapeArtRows = 15;   // == rows in tape_ascii.txt
static constexpr int kTapeArtWidth = 30;  // == cells per row == disk width

// ---------------------------------------------------------------------
// Panel builders
// ---------------------------------------------------------------------

// ---- oscilloscope colours / parameters (shared by the braille and the image style) ----
namespace {
void osci_temperature_rgb(float t, int& r, int& g, int& b) {
    static const float kStops[5][4] = {{0.00f, 190, 30, 12}, {0.25f, 255, 120, 20}, {0.50f, 255, 224, 150}, {0.75f, 205, 225, 255}, {1.00f, 90, 140, 255}};
    t = std::clamp(t, 0.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        if (t <= kStops[i + 1][0]) {
            const float k = (t - kStops[i][0]) / (kStops[i + 1][0] - kStops[i][0]);
            r = static_cast<int>(kStops[i][1] + (kStops[i + 1][1] - kStops[i][1]) * k);
            g = static_cast<int>(kStops[i][2] + (kStops[i + 1][2] - kStops[i][2]) * k);
            b = static_cast<int>(kStops[i][3] + (kStops[i + 1][3] - kStops[i][3]) * k);
            return;
        }
    }
    r = 90; g = 140; b = 255;
}
// Colour of palette `id` (kOsciPaletteNames) at t = 0..1. 0 and 1 use the VIZ gradient of the COLORS tab.
void osci_palette_rgb(const Settings& s, int id, float t, int& r, int& g, int& b) {
    static const float kStops[5][5][3] = {
        {{20, 205, 120}, {30, 225, 205}, {60, 160, 255}, {150, 95, 255}, {235, 80, 205}},
        {{95, 35, 150}, {170, 45, 140}, {235, 70, 105}, {255, 150, 55}, {255, 232, 150}},
        {{45, 95, 235}, {60, 170, 255}, {120, 230, 255}, {210, 250, 255}, {255, 255, 255}},
        {{0, 255, 220}, {0, 150, 255}, {130, 65, 255}, {255, 45, 205}, {255, 100, 120}},
        {{255, 70, 70}, {255, 175, 45}, {235, 235, 70}, {60, 225, 120}, {85, 160, 255}}};
    t = std::clamp(t, 0.0f, 1.0f);
    if (id <= 1) {
        const std::string a = gradient_ansi(s.visualizer_color, s.visualizer_color_end, t);
        if (std::sscanf(a.c_str(), "\x1b[38;2;%d;%d;%dm", &r, &g, &b) != 3) r = g = b = 220;
        return;
    }
    if (id == 2) { osci_temperature_rgb(t, r, g, b); return; }
    if (id == 8) { r = 70; g = 255; b = 90; return; }   // classic green: the phosphor of an old scope, brightness alone makes the picture
    const int k = std::clamp(id - 3, 0, 4);
    const float f = t * 4.0f;
    const int i = std::min(3, static_cast<int>(f));
    const float u = f - static_cast<float>(i);
    r = static_cast<int>(kStops[k][i][0] + (kStops[k][i + 1][0] - kStops[k][i][0]) * u);
    g = static_cast<int>(kStops[k][i][1] + (kStops[k][i + 1][1] - kStops[k][i][1]) * u);
    b = static_cast<int>(kStops[k][i][2] + (kStops[k][i + 1][2] - kStops[k][i][2]) * u);
}
// The spectrogram's "gradient" colours: the two ends of the VIZ gradient (Settings -> COLORS), like the oscilloscope's.
void spectro_gradient_from(const Settings& st) {
    int r0 = 255, g0 = 255, b0 = 255, r1 = 255, g1 = 255, b1 = 255;
    osci_palette_rgb(st, 0, 0.0f, r0, g0, b0);
    osci_palette_rgb(st, 0, 1.0f, r1, g1, b1);
    auto c8 = [](int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); };
    spectro_set_gradient({c8(r0), c8(g0), c8(b0)}, {c8(r1), c8(g1), c8(b1)});
}
std::string osci_rgb_seq(int r, int g, int b, float k) {
    auto sc = [k](int v) { return std::clamp(static_cast<int>(static_cast<float>(v) * k), 0, 255); };
    return "\x1b[38;2;" + std::to_string(sc(r)) + ";" + std::to_string(sc(g)) + ";" + std::to_string(sc(b)) + "m";
}
std::string osci_dim_ansi(const std::string& ansi, float k) {
    int r, g, b;
    if (std::sscanf(ansi.c_str(), "\x1b[38;2;%d;%d;%dm", &r, &g, &b) != 3) return ansi;   // palette colour: can't scale
    return osci_rgb_seq(r, g, b, k);
}
OscilloscopeVisualizer::Params osci_scope_params(const Settings& s, double dt, int style = -1) {   // style: 0 braille, 1 image, -1 the one in use
    const OsciSet& o = style < 0 ? s.osci() : s.osci_set[style == 1 ? 1 : 0];
    OscilloscopeVisualizer::Params p;
    // the afterglow is set per frame at 30 fps: scale by the real frame time so another frame rate keeps the same look
    p.decay = std::pow(o.decay, static_cast<float>(std::clamp(dt, 0.004, 0.15) * 30.0));
    p.dot_threshold = o.dot_threshold;
    p.tail_brightness = o.tail;
    p.interpolate = o.interp;
    p.z_axis = o.z;
    p.z_depth = o.z_depth;
    p.z_source = o.z_source;
    p.trace = o.trace;
    p.rotate = o.rotate;
    p.mono_phase = o.mono_phase;
    p.glow = o.glow;
    p.mode = s.osci_mode;
    if (o.music) { p.mono_phase = false; p.fixed_gain = true; }   // oscilloscope music mode: the exact figure
    return p;
}
} // namespace

// Lays one meta-editor entry (typed or fetched, saved or not) over the block
// the main screen shows for the playing file. `cur` is the file's path BEFORE
// any rename, which is how a name that is only the file name (no title tag)
// is recognised. Empty tag values show as "-" like probe_metadata() does.
static void overlay_meta_entry(TrackMetadata& md, const MetaEditEntry& e, const fs::path& cur) {
    auto dash = [](const std::string& v) { return v.empty() ? std::string("-") : v; };
    const int ti = static_cast<int>(MetaField::Title), ar = static_cast<int>(MetaField::Artist);
    const int al = static_cast<int>(MetaField::Album), yr = static_cast<int>(MetaField::Year);
    const int fn = static_cast<int>(MetaField::FileName);
    if (e.edited[ti]) {
        md.name = e.value[ti].empty() ? path_utf8(cur.stem()) : e.value[ti];
    } else if (e.edited[fn] && !e.value[fn].empty() && md.name == path_utf8(cur.stem())) {
        md.name = e.value[fn];   // no title tag: the name IS the file name, and it is being renamed
    }
    if (e.edited[ar]) md.artist = dash(e.value[ar]);
    if (e.edited[al]) md.album = dash(e.value[al]);
    if (e.edited[yr]) md.year = dash(e.value[yr]);
}

// What the main screen shows: the loaded metadata_ with the pending meta-editor
// edits of the playing local file on top, so typing or fetching in the editor
// (SHIFT+n, SHIFT+t, ...) is visible there at once, before CTRL+s writes it.
TrackMetadata App::display_metadata() const {
    TrackMetadata md = metadata_;
    if (!has_track_ || !current_is_local_ || current_path_.empty()) return md;
    if (const MetaEditEntry* e = meta_entry(path_utf8(current_path_))) overlay_meta_entry(md, *e, current_path_);
    return md;
}

std::vector<std::string> App::build_metadata_panel(int total_width) const {
    const int inner = total_width - 4;
    const int disk_w = settings_.element_disk ? disk_.width() : 0;
    const int panel_h = disk_.height();
    
    std::string sep = "  " + settings_.meta_separator + "  ";
    int sep_w = display_width(sep);
    const int fixed_extra = settings_.element_disk ? sep_w : 2;

    int avail = std::max(10, inner - disk_w - fixed_extra);
    // Previously this split only happened when settings_.element_lyrics was
    // true; with it off, lyrics_w stayed 0 and meta_w took the whole
    // panel, so turning the Lyrics Engine off silently also gave up the
    // sphere visualization that normally fills this column while nothing
    // is playing lyrics -- the panel just went from "sphere" to "wide
    // plain metadata" instead of staying visually alive. The split is now
    // unconditional; settings_.element_lyrics only gates whether lyrics
    // are fetched/shown as text (below), not whether this column exists.
    int meta_w = std::min(42, std::max(10, avail - 10));
    meta_w = std::min(meta_w, avail);
    int lyrics_w = std::max(0, avail - meta_w);
    if (!settings_.element_vizfield) { meta_w = avail; lyrics_w = 0; }   // "Viz/Lyrics Field" off: the meta data take the field's place

    std::vector<std::string> disk_frame;
    if (settings_.element_disk) {
        disk_frame = disk_.frame(angle_);
        while (static_cast<int>(disk_frame.size()) < panel_h) disk_frame.emplace_back(std::string(disk_w, ' '));
        for (size_t row_i = 0; row_i < disk_frame.size(); ++row_i) {
            float t = disk_frame.size() > 1 ? static_cast<float>(row_i) / static_cast<float>(disk_frame.size() - 1) : 0.0f;
            std::string disk_end = settings_.disk_color_end;
            std::string row_ansi = gradient_ansi(settings_.disk_color, disk_end, t);
            disk_frame[row_i] = row_ansi + disk_frame[row_i] + "\x1b[0m";
        }
    }

    double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    fft_.set_fluidity(settings_.visualizer_fluidity);
    fft_.set_degradation_speed(settings_.visualizer_degradation_speed);
    fft_.set_viscosity(settings_.visualizer_viscosity);

    // meta content rows
    std::vector<std::string> meta_rows(panel_h, std::string());
    bool viz_rows_colored = false;
    std::vector<int> bars; // computed once below, reused by the sphere visualizer fallback further down
    if (has_track_) {
        int meta_row_cursor_ = 1; // row 0 stays blank: the metadata block starts below the panel's top edge (the idle message is centred in the column instead)
        std::string k_col = settings_.meta_key_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_key_color);
        std::string v_col = settings_.meta_val_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_val_color);
        // Rows that may be used by the metadata block: everything above the
        // visualizer's fixed two bottom rows (all rows when the visualizer is
        // switched off). Nothing below can ever reach past this line, so the
        // spectrum is never overwritten, shifted or pushed -- whatever the
        // tags look like.
        const int last_meta_row = (settings_.element_visualizer ? panel_h - 2 : panel_h) - 1;
        // `marquee_slot` >= 0: the value always takes exactly one row and, when
        // it is wider than the column, scrolls through it (Name / Artist /
        // Album). Otherwise it wraps over up to `max_lines` rows, but only as
        // many as are still free (Location).
        auto kv = [&](const std::string& label, const std::string& value, int max_lines = 1, int marquee_slot = -1) {
            if (meta_row_cursor_ > last_meta_row) return;   // no free row left: drop it, never touch the visualizer rows
            std::string mapped_label = apply_font_map(label, settings_.font_map);
            std::string mapped_val = apply_font_map(value, settings_.font_map);
            int avail_v = std::max(0, meta_w - 12);
            std::string l_pad = pad_right(mapped_label, 10);

            std::vector<std::string> value_lines;
            if (marquee_slot >= 0 && marquee_slot < 3) {
                // The scroll restarts whenever the text changes (new track).
                const int key = static_cast<int>(std::hash<std::string>{}(mapped_val) & 0x7fffffffu);
                value_lines.push_back(marquee_or_truncate(mapped_val, avail_v, key,
                                                          marquee_meta_idx_[marquee_slot], marquee_meta_since_[marquee_slot]));
            } else {
                const int room = std::max(1, last_meta_row - meta_row_cursor_ + 1);
                value_lines = wrap_lines(mapped_val, avail_v, std::min(max_lines, room));
                if (value_lines.empty()) value_lines.push_back(std::string());
            }

            for (size_t li = 0; li < value_lines.size(); ++li) {
                if (meta_row_cursor_ > last_meta_row) break;   // never past the visualizer
                const std::string& v_tr = value_lines[li];
                bool first = (li == 0);
                // Continuation lines repeat the label column as blank
                // space (not the colon) so the wrapped text lines up
                // directly under where the value on line one starts.
                std::string label_col = first ? l_pad : std::string(10, ' ');
                std::string sep_txt = first ? ": " : "  ";
                std::string plain = label_col + sep_txt + v_tr;
                std::string ansi = first
                    ? (k_col + label_col + "\x1b[0m" + sep_txt + v_col + v_tr + "\x1b[0m")
                    : (label_col + sep_txt + v_col + v_tr + "\x1b[0m");
                ansi += std::string(std::max(0, meta_w - display_width(plain)), ' ');
                meta_rows[meta_row_cursor_++] = ansi;
            }
        };
        // Name, Artist and Album are the fields most likely to overrun the
        // column (long titles, multi-artist collabs). They stay on one row
        // each and scroll as a marquee when too long, so the block has a
        // fixed height and the visualizer below is never affected. Location
        // (deep folder paths) may still use a second row if one is free.
        const TrackMetadata shown = display_metadata();   // pending meta-editor edits included
        // The first line follows SHIFT+n (HKeyToggleMetaOnly), like the list rows do: "Title" is the title tag (the
        // file name when the file has none), "File" is the file name -- also with a pending FILE rename applied.
        // Streams have no file name and always show "Title".
        const bool show_file = has_track_ && current_is_local_ && !current_path_.empty() && !settings_.meta_only;
        std::string first_value = shown.name;
        if (show_file) {
            first_value = path_utf8(current_path_.stem());
            if (const MetaEditEntry* me = meta_entry(path_utf8(current_path_))) {
                const int fn = static_cast<int>(MetaField::FileName);
                if (me->edited[fn] && !me->value[fn].empty()) first_value = me->value[fn];
            }
        }
        kv(show_file ? "File" : "Title", first_value, 1, 0);
        kv("Artist", shown.artist, 1, 1);
        kv("Album", shown.album, 1, 2);
        kv("Year", shown.year);
        kv("Sampling", metadata_.sampling);
        kv("Type", metadata_.type);
        kv("Format", metadata_.format);
        kv("File size", metadata_.file_size);
        kv("Location", metadata_.location, 2);
        if (!metadata_.extra_label.empty()) kv(metadata_.extra_label, metadata_.extra_value);

        // Real spectrum visualizer (KISS FFT), not a copy of the progress
        // bar's RMS envelope. Two rows: bottom row is the base level
        // (0-4), top row is whatever's left over above that (0-4) so
        // taller peaks build upward — attached to the panel's bottom row
        // per instruction, with the second row directly above it.
        if (settings_.element_visualizer) {
            int viz_w = std::min(meta_w, settings_.element_vizfield ? 48 : 96);
            bars = fft_.compute_bars(viz_w, viz_dt_);
            std::string viz_top, viz_bottom;
            int nbars = static_cast<int>(bars.size());
            for (int i = 0; i < nbars; ++i) {
                int level = bars[i];
                float t = nbars > 1 ? static_cast<float>(i) / static_cast<float>(nbars - 1) : 0.0f;
                std::string bar_ansi;
                if (!settings_.viz_center_color.empty()) {
                    bar_ansi = multi_stop_gradient_ansi(settings_.viz_left_color, settings_.viz_center_color, settings_.viz_right_color, t);
                } else {
                    bar_ansi = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t);
                }
                viz_bottom += bar_ansi;
                viz_bottom += fft_glyph(std::min(level, 4));
                viz_top += bar_ansi;
                viz_top += fft_glyph(std::max(0, level - 4));
            }
            viz_top += "\x1b[0m";
            viz_bottom += "\x1b[0m";
            if (viz_w < meta_w) {
                std::string tail(meta_w - viz_w, ' ');
                viz_top += tail;
                viz_bottom += tail;
            }
            meta_rows[panel_h - 2] = viz_top;
            meta_rows[panel_h - 1] = viz_bottom;
            viz_rows_colored = true;
        } else {
            bars = fft_.compute_bars(48, viz_dt_);
        }
    } else {
        // Nothing loaded -- never started, playback stopped with no track,
        // or the file that was playing got deleted. The message sits centred
        // (both ways) in the metadata/visualizer column instead of being
        // glued to its first row, where it used to hide behind the disk.
        meta_rows[std::max(0, panel_h / 2)] = center_pad("Currently No Track Loaded", meta_w);
    }
    for (int i = 0; i < static_cast<int>(meta_rows.size()); ++i) {
        if (meta_rows[i].empty()) {
            meta_rows[i] = std::string(meta_w, ' ');
        }
        // Every other row (kv data, visualizer, and the centred idle message
        // above -- center_pad() already pads that one to meta_w) was padded
        // by its own builder, and padding it again would miscount its ANSI
        // color escapes as visible columns, truncating it.
    }

    // lyrics window: word-wrapped, center-aligned, word-level highlight on
    // the active line — windowed so the active line's wrapped block is
    // always vertically centered, blank-padded at the edges.
    std::vector<std::string> lyric_rows(panel_h, std::string(lyrics_w, ' '));
    bool lyrics_avail = false;
    std::vector<LyricLine> lines_copy;
    std::string lyrics_status;
    double lyrics_delay = 0.0; // lyrics timing correction (ALT+L), seconds, + = lyrics later
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (settings_.lyrics_on() && lyrics_ready_) {
            lines_copy = lyrics_result_.lines;
            lyrics_delay = lyrics_result_.delay;
            lyrics_status = lyrics_result_.message;
            lyrics_avail = !lines_copy.empty();
        } else if (settings_.lyrics_on() && has_track_) {
            lyrics_status = "fetching lyrics ...";
        }
        // else: Lyrics Engine is off -- no fetch ever ran, so there's
        // nothing to report. Leaving lyrics_status empty means the sphere
        // below renders with no caption at all, rather than a stale or
        // misleading status line.
    }

    if (!has_track_) {
        // Nothing is playing, so there is no lyric visual and no lyric line to draw
        // and this column would just be empty. Fill it with the braille
        // cassette picture (kTapeArt above -- 15 rows of 30 cells, the disk's
        // own dimensions), centred in the column and tinted with the SAME
        // top-to-bottom disk gradient the rotating disk gets, so the idle
        // panel keeps the colour it has while playing.
        int start = std::max(0, (panel_h - kTapeArtRows) / 2);
        for (int i = 0; i < kTapeArtRows && start + i < panel_h && lyrics_w > 0; ++i) {
            float t = panel_h > 1 ? static_cast<float>(start + i) / (panel_h - 1) : 0.0f;
            int left = (lyrics_w - kTapeArtWidth) / 2;
            std::string body = (left >= 0)
                ? std::string(left, ' ') + kTapeArt[i]
                  + std::string(lyrics_w - left - kTapeArtWidth, ' ')
                : utf8_skip_take(kTapeArt[i], -left, lyrics_w); // column narrower than the art: keep its middle, never wrap braille cells
            lyric_rows[start + i] =
                gradient_ansi(settings_.disk_color, settings_.disk_color_end, t) + body + "\x1b[0m";
        }
    } else if (!lyrics_avail) {
        // A status message ("fetching...", "no lyrics found", etc.) is
        // only shown for the first 1.75s after it appears -- after that
        // the placeholder visual (sphere or osci) gets the whole panel to
        // itself instead of a permanently stuck caption line. Each distinct message content
        // gets its own fresh window (so "fetching..." showing, then
        // later "no lyrics found", each get their moment) rather than
        // one timer for the whole track.
        if (lyrics_status != last_lyrics_status_) {
            last_lyrics_status_ = lyrics_status;
            lyrics_status_shown_at_ = std::chrono::steady_clock::now();
        }
        double status_age = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - lyrics_status_shown_at_).count();
        bool show_caption = status_age < 1.75 && !lyrics_status.empty();

        if (has_track_ && lyrics_w >= 6 && panel_h >= 3) {
            // Fill the panel with the audio-reactive placeholder visual
            // instead of leaving it blank -- which one is picked by
            // Settings -> ON/OFF -> "Lyrics/Viz" / "No-Lyrics Viz":
            //   sphere -> reuses `bars` (already computed for the main
            //             spectrum strip above, same frame) rather than
            //             running a second independent audio analysis.
            //   osci   -> XY scope (L = horizontal, R = vertical) drawn from the
            //             raw waveform ring the audio callback feeds
            //             (OscilloscopeVisualizer).
            // Both are tinted with the VIZ colors (Settings -> Colors ->
            // VIZ): the sphere with the pair's midpoint, the scope with a
            // LEFT -> RIGHT sweep, dimmed per cell by beam brightness.
            // The visual always gets the whole panel; a caption ("fetching lyrics ...") is a small box laid over its
            // bottom row (below), so the picture never changes its size or moves while it shows.
            const int viz_rows_h = panel_h;
            // Lyrics view without lyrics: the "No-Lyrics Viz" pick; otherwise the view the "." cycle is on.
            int area_viz = settings_.element_lyrics ? settings_.nolyrics_viz : settings_.lyric_viz;
            if (area_viz < 3 && !settings_.viz_usable(area_viz)) area_viz = 3;   // nothing is switched on: an empty field
            if (area_viz == 1) {
                // XY scope: render() hands back exactly lyrics_w cells per
                // row (empty ones included), each with a braille pattern and
                // a 0..255 beam brightness. EVERY cell is emitted -- blanks
                // as a plain space -- so each row is exactly lyrics_w
                // columns wide; dropping the blanks (as an earlier version
                // did) made rows shorter than the panel, which pushed the
                // right border around and left stale cells on screen.
                // Colour = the VIZ gradient swept left -> right, dimmed by
                // the cell's brightness so the afterglow fades out.
                const OsciSet& os = settings_.osci();
                const int pal = os.palette;
                OscilloscopeVisualizer::Params osci_params = osci_scope_params(settings_, viz_dt_);
                bool as_image = false;
                if (settings_.osci_style == 1 && gfx_proto_ != GfxProto::None && lyrics_w > 0) {
                    // image style: the terminal draws the picture; the cells stay blank here and the loop emits it
                    gfx_cell_pixels(settings_.cell_pixels, cell_w_, cell_h_);
                    const int cw0 = std::max(4, cell_w_), ch0 = std::max(8, cell_h_);
                    const int cwq = std::clamp(560 / lyrics_w, 4, cw0);
                    GfxFrame& g = gfx_;
                    // Image resolution (SHIFT+o): render 1/scale of it; Kitty scales the picture up itself, Sixel repeats
                    // its pixels up to the real cell size (out_cw x out_ch). Less to render, encode and send per frame.
                    g.scale = std::clamp(settings_.image_scale, 1, 3);
                    g.out_cw = cw0; g.out_ch = ch0;
                    // 1/scale of the real cell pixels, but never more than the width cap above (cwq)
                    const int cws = std::max(2, std::min(cwq, cw0 / g.scale));
                    const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
                    g.cols = lyrics_w; g.rows = viz_rows_h; g.w = lyrics_w * cws; g.h = viz_rows_h * chs;
                    g.col = 2 + disk_w + (settings_.element_disk ? sep_w : 2) + meta_w;
                    g.row = 1;
                    g.id = 0; g.grid.clear(); g.cut_w = 0;   // no grid on the main screen (the big overlay sets one on the same frame object)
                    scope_.render_image(g.w, g.h, osci_params, g.level, g.hue);
                    if (pal == 0) {
                        for (int y = 0; y < g.h; ++y)
                            for (int x = 0; x < g.w; ++x)
                                g.hue[static_cast<size_t>(y) * g.w + x] = static_cast<uint8_t>(x * 255 / std::max(1, g.w - 1));
                    }
                    for (int h = 0; h < 256; ++h) {
                        int r = 255, gr = 255, b = 255;
                        osci_palette_rgb(settings_, pal, static_cast<float>(h) / 255.0f, r, gr, b);
                        g.pal[static_cast<size_t>(h)] = {static_cast<uint8_t>(r), static_cast<uint8_t>(gr), static_cast<uint8_t>(b)};
                    }
                    g.active = true;
                    g.fresh = gfx_due_;
                    as_image = true;
                    // placeholder cells: the main loop writes them as spaces or (Sixel) skips them so the picture stays
                    for (int i = 0; i < panel_h; ++i) lyric_rows[i] = gfx_hole(lyrics_w);
                }
                if (!as_image) {
                auto osci_cells = scope_.render(lyrics_w, viz_rows_h, osci_params);
                for (int i = 0; i < static_cast<int>(osci_cells.size()) && i < panel_h; ++i) {
                    std::string colored, last_ansi;
                    for (int cell = 0; cell < lyrics_w; ++cell) {
                        const auto& c = osci_cells[i][cell];
                        if (c.braille == 0) { colored += ' '; continue; }
                        const float t = lyrics_w > 1
                            ? static_cast<float>(cell) / static_cast<float>(lyrics_w - 1) : 0.0f;
                        // 8 brightness steps keeps the escape traffic small
                        const float lvl = std::round(c.level / 255.0f * 7.0f) / 7.0f;
                        const float k = 0.30f + 0.70f * lvl;
                        std::string ansi;
                        if (pal == 0) ansi = osci_dim_ansi(gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t), k);
                        else { int r = 255, g = 255, b = 255; osci_palette_rgb(settings_, pal, c.hue / 255.0f, r, g, b); ansi = osci_rgb_seq(r, g, b, k); }
                        if (ansi != last_ansi) { colored += ansi; last_ansi = ansi; }
                        const int cp = 0x2800 + c.braille;
                        colored += static_cast<char>(0xE0 | (cp >> 12));
                        colored += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        colored += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    lyric_rows[i] = colored + "\x1b[0m";
                }
                }
            } else if (area_viz == 2) {
                // Spectrogram (like Audacity's): braille, or a picture the terminal draws (tiles sent by the main loop)
                spectro_used_ = true;
                spectro_gradient_from(settings_);
                spectro().update(settings_.spectro);
                if (settings_.spectro.style == 1 && gfx_proto_ != GfxProto::None) {
                    spectro_area_[0] = 1;
                    spectro_area_[1] = 2 + disk_w + (settings_.element_disk ? sep_w : 2) + meta_w;
                    spectro_area_[2] = lyrics_w;
                    spectro_area_[3] = viz_rows_h;
                    for (int i = 0; i < viz_rows_h && i < panel_h; ++i) lyric_rows[i] = gfx_hole(lyrics_w);
                } else {
                    auto rows = spectro().braille(settings_.spectro, lyrics_w, viz_rows_h);
                    for (int i = 0; i < static_cast<int>(rows.size()) && i < panel_h; ++i) lyric_rows[i] = rows[static_cast<size_t>(i)];
                }
            } else if (area_viz == 0) {
                auto sphere_rows = sphere_.render(lyrics_w, viz_rows_h, bars, viz_dt_);
                std::string sphere_ansi = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, 0.5f);
                for (int i = 0; i < static_cast<int>(sphere_rows.size()) && i < panel_h; ++i) {
                    lyric_rows[i] = sphere_ansi + pad_right(sphere_rows[i], lyrics_w) + "\x1b[0m";
                }
            }
            if (show_caption) {
                const std::string border = ansi_for(settings_.border_color, false);
                std::string text = lyrics_status;
                if (display_width(text) > lyrics_w - 4) text = truncate_str(text, std::max(1, lyrics_w - 4));
                const std::string box = border + "[ \x1b[0m" + text + border + " ]\x1b[0m";
                const int bw = display_width(text) + 4;
                const int x = std::max(0, (lyrics_w - bw) / 2);
                const int row = panel_h - 1;
                lyric_rows[row] = overlay_cells(lyric_rows[row], x, box, bw, lyrics_w);
                const int area_col = 2 + disk_w + (settings_.element_disk ? sep_w : 2) + meta_w;
                // where it is on screen (0-based): the picture styles leave these cells free / write it again on top
                caption_rect_[0] = 1 + row; caption_rect_[1] = area_col + x; caption_rect_[2] = bw;
                caption_text_ = box;
            }
        } else if (show_caption) {
            int pad = std::max(0, (lyrics_w - display_width(lyrics_status)) / 2);
            lyric_rows[0] = pad_right(std::string(pad, ' ') + lyrics_status, lyrics_w);
        }
    } else {
        const double lyric_t = elapsed - lyrics_delay; // the time the lyrics are looked up at
        int active = 0;
        for (size_t i = 0; i < lines_copy.size(); ++i) {
            if (lines_copy[i].start_time <= lyric_t) active = static_cast<int>(i);
            else break;
        }

        if (settings_.lyrics_animation == 4) {
            // Only active word: show nothing but whichever single word is
            // currently being sung, centered alone in the panel. Falls
            // back to the whole line if this song has no word-level sync
            // data at all (nothing finer to show).
            const LyricLine& al = lines_copy[active];
            std::string word = al.full_text;
            if (!al.words.empty()) {
                word = al.words.front().second;
                for (const auto& wt : al.words) if (wt.first <= lyric_t) word = wt.second; // last one <= lyric_t
            }
            word = apply_font_map(word, settings_.font_map);
            int pad = std::max(0, (lyrics_w - display_width(word)) / 2);
            std::string line = std::string(pad, ' ') + word;
            std::string colored = ansi_for(settings_.active_word_color) + pad_right(truncate_str(line, lyrics_w), lyrics_w) + "\x1b[0m";
            lyric_rows[panel_h / 2] = colored;
        } else if (settings_.lyrics_animation == 3) {
            // Only active line: same per-word rendering as the default
            // view, just without the scrolling context lines around it.
            auto wrapped = render_lyric_line_wrapped(lines_copy[active], lyric_t, lyrics_w, true, settings_);
            int start_row = std::max(0, (panel_h - static_cast<int>(wrapped.size())) / 2);
            for (size_t i = 0; i < wrapped.size() && start_row + static_cast<int>(i) < panel_h; ++i) {
                lyric_rows[start_row + i] = wrapped[i];
            }
        } else {
            // Full (default) and Word-by-word/Letter-by-letter (which only
            // change render_lyric_line_wrapped's *content*, not this
            // scrolling layout) all share the same multi-line context view.
            //
            // Only wrap lines actually near the visible window — wrapping
            // the whole song every frame would be wasted work.
            int context = 6;
            int lo = std::max(0, active - context);
            int hi = std::min(static_cast<int>(lines_copy.size()) - 1, active + context);

            std::vector<std::string> flat_rows;
            int active_row_start = 0, active_row_count = 1;
            for (int li = lo; li <= hi; ++li) {
                auto wrapped = render_lyric_line_wrapped(lines_copy[li], lyric_t, lyrics_w, li == active, settings_);
                if (li == active) {
                    active_row_start = static_cast<int>(flat_rows.size());
                    active_row_count = static_cast<int>(wrapped.size());
                }
                for (auto& r : wrapped) flat_rows.push_back(std::move(r));
            }

            int active_mid = active_row_start + active_row_count / 2;
            int start = active_mid - panel_h / 2;
            for (int row = 0; row < panel_h; ++row) {
                int idx = start + row;
                if (idx >= 0 && idx < static_cast<int>(flat_rows.size())) lyric_rows[row] = flat_rows[idx];
            }
        }
    }

    // --- assemble bordered block ---
    // NOTE: lyric_rows may contain ANSI color codes (word-highlighting),
    // so this assembles rows by direct concatenation of pre-padded pieces
    // rather than routing through box_line()/pad_right() — those count
    // UTF-8 codepoints for width, and ANSI escape bytes would be
    // miscounted as visible columns, throwing off alignment. Every piece
    // here (disk_frame/meta_rows/lyric_rows) is already padded to its own
    // exact width, so the concatenation is guaranteed to equal `inner`.
    std::vector<std::string> out;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    out.push_back(box_top("", total_width, border_ansi));

    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::string sep_ansi = ansi_for(settings_.border_color, false) + sep + "\x1b[0m";
    for (int row = 0; row < panel_h; ++row) {
        std::string content = "";
        if (settings_.element_disk) {
            content += disk_frame[row] + sep_ansi;
        } else {
            content += "  ";
        }
        content += meta_rows[row];
        // lyrics_w is now always reserved (see the split above), and
        // lyric_rows is always fully padded to it -- whether that's
        // actual synced lyrics, a status caption, or just the sphere --
        // so this no longer needs to be conditional on element_lyrics.
        content += lyric_rows[row];
        out.push_back(bar + " " + content + " " + bar);
    }

    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_progress_panel(int total_width) const {
    const int button_content_w = 9;
    const int button_total_w = button_content_w + 4; // "│ X │"
    int side_panel_w = settings_.element_dummy_buttons ? (button_total_w * 3) : 38;
    int main_total_w = std::max(24, total_width - side_panel_w);
    int wave_w = main_total_w - 4;

    double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    int active_cols = (total_sec_ > 0) ? static_cast<int>((elapsed / static_cast<double>(total_sec_)) * wave_w) : 0;
    active_cols = std::clamp(active_cols, 0, wave_w);

    int reveal_cols = wave_w;
    if (waveform_ready_) {
        double reveal_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - waveform_reveal_start_).count();
        if (reveal_sec < 0.7) {
            reveal_cols = static_cast<int>((reveal_sec / 0.7) * wave_w);
        }
    } else {
        reveal_cols = 0;
    }

    std::string color_played = ansi_for(settings_.progress_played_color);
    std::string color_unplayed = ansi_for(settings_.progress_remaining_color);
    std::string color_reset = "\x1b[0m";

    std::string top_wave, mid_wave, bot_wave;
    std::vector<int> waveform_levels;
    if (!waveform_envelope_.empty()) {
        waveform_levels = WaveformQuantizer::resample_for_ui(waveform_envelope_, wave_w);
    } else {
        waveform_levels.assign(wave_w, 0);
    }

    for (int i = 0; i < wave_w; ++i) {
        bool revealed = i < reveal_cols;
        int level = (revealed && i < static_cast<int>(waveform_levels.size())) ? waveform_levels[i] : 0;
        BrailleColumn col = WaveformQuantizer::get_column(level);
        if (i == 0) {
            std::string c = (i < active_cols) ? color_played : color_unplayed;
            top_wave += c; mid_wave += c; bot_wave += c;
        } else if (i == active_cols) {
            top_wave += color_unplayed; mid_wave += color_unplayed; bot_wave += color_unplayed;
        }
        top_wave += col.top; mid_wave += col.mid; bot_wave += col.bot;
    }
    top_wave += color_reset; mid_wave += color_reset; bot_wave += color_reset;

    // Generic fallback bar for when the waveform element is switched
    // off: a plain "[#####-------]" fill on the middle row, blank above
    // and below it, instead of leaving the panel showing a stray braille
    // waveform on two of its three rows (top_wave/mid_wave used to render
    // unconditionally regardless of this setting -- only the bottom row
    // respected the toggle, which is the "turning off waveform doesn't
    // actually turn it off" bug).
    std::string generic_blank(wave_w, ' ');
    std::string generic_bar;
    if (!settings_.element_waveform) {
        int inner_w = std::max(0, wave_w - 2); // account for the '[' and ']'
        int filled = (total_sec_ > 0) ? static_cast<int>((elapsed / static_cast<double>(total_sec_)) * inner_w) : 0;
        filled = std::clamp(filled, 0, inner_w);
        generic_bar = "[" + color_played + std::string(filled, '#') + color_reset
                     + color_unplayed + std::string(inner_w - filled, '-') + color_reset + "]";
    }

    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    // Buttons (<<< PLAY >>>) and the volume bar now match the border
    // color rather than the separate (and, for these two elements,
    // effectively unused/inert) button_color field.
    std::string button_ansi = border_ansi;

    auto button_mid = [&](const std::string& text) {
        std::string centered = button_ansi + center_pad(text, button_content_w) + "\x1b[0m";
        std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
        return bar + " " + centered + " " + bar;
    };
    std::string play_label = (has_track_ && player_.is_paused()) ? "PLAY" : (has_track_ ? "PAUSE" : "PLAY");

    std::vector<std::string> out;
    if (settings_.element_dummy_buttons) {
        out.push_back(box_top("PROGRESS BAR", main_total_w, border_ansi)
                      + box_top("", button_total_w, border_ansi) + box_top("", button_total_w, border_ansi)
                      + box_top("", button_total_w, border_ansi));
    } else {
        out.push_back(box_top("PROGRESS BAR", main_total_w, border_ansi));
    }
    
    std::string row1_content = settings_.element_waveform ? top_wave : generic_blank;
    std::string row2_content = settings_.element_waveform ? mid_wave : generic_bar;
    std::string row3_content = settings_.element_waveform ? bot_wave : generic_blank;

    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    if (settings_.element_dummy_buttons) {
        out.push_back(bar + " " + row1_content + " " + bar
                      + button_mid("<<<") + button_mid(play_label) + button_mid(">>>"));
        out.push_back(bar + " " + row2_content + " " + bar
                      + box_bottom(button_total_w, "", border_ansi_bottom) + box_bottom(button_total_w, "", border_ansi_bottom)
                      + box_bottom(button_total_w, "", border_ansi_bottom));
    } else {
        out.push_back(bar + " " + row1_content + " " + bar);
        out.push_back(bar + " " + row2_content + " " + bar);
    }

    int vol = player_.volume();
    int vol_hashes = (vol * 20) / 100;
    // "#" (filled) matches the waveform's played color, "-" (empty)
    // matches its unplayed/remaining color -- color_played/color_unplayed
    // are already computed above from progress_played_color/
    // progress_remaining_color for the waveform itself, reused here so
    // the volume bar visually reads as the same kind of fill. The
    // "VOLUME BAR:[...]" label and brackets use the border color, same
    // as the buttons above.
    std::string vol_bar_colored = color_played + std::string(vol_hashes, '#') + "\x1b[0m"
                                 + color_unplayed + std::string(20 - vol_hashes, '-') + "\x1b[0m";
    std::string vol_bar_text = std::string(vol_hashes, '#') + std::string(20 - vol_hashes, '-'); // plain, for width math only
    std::string vol_tail = border_ansi + "VOLUME BAR:[" + "\x1b[0m" + vol_bar_colored
                          + border_ansi + "]" + "\x1b[0m" + " " + std::to_string(vol) + "%";
    int side_w = total_width - main_total_w;
    // pad_left counts raw bytes, so it can't be used once vol_tail carries
    // ANSI bytes -- pad by the *visible* width instead (the volume-bar
    // color fix below is what introduced the mismatch).
    int visible_w = display_width("VOLUME BAR:[" + vol_bar_text + "] " + std::to_string(vol) + "%");
    int gap = std::max(0, side_w - visible_w);

    if (!settings_.autosave_enabled) {
        // AutoSave fully off: no indicator, and the blank gap moves to
        // the *right* of the text instead of sitting between the
        // border and it -- "│VOLUME BAR" touching the border directly,
        // same total line width either way so nothing else has to
        // change to stay aligned.
        if (gap > 0) vol_tail = vol_tail + std::string(gap, ' ');
    } else {
        // AutoSave on: keep the existing gap (│  VOLUME BAR), but let
        // the indicator glyph occupy the first character of it when
        // enabled -- │• VOLUME BAR with room to spare, or │•VOLUME BAR
        // once the gap is down to exactly one column.
        std::string pad(gap, ' ');
        if (gap >= 1) {
            std::string glyph = autosave_indicator_glyph();
            if (!glyph.empty()) pad = glyph + pad.substr(1);
        }
        vol_tail = pad + vol_tail;
    }

    std::string time_plain = "[ " + fmt_mmss(elapsed) + " ]" + settings_.box_horizontal + "[ " + fmt_mmss(static_cast<double>(total_sec_)) + " ]";
    // Manual box-bottom construction (rather than the shared box_bottom()
    // helper) so the timestamp text can carry its own color: box_bottom()
    // measures/pads the footer as plain text, and embedding ANSI bytes
    // into that path would get miscounted as visible columns.
    std::string ts_ansi = ansi_for(settings_.progress_timestamp_color.empty() ? settings_.border_color : settings_.progress_timestamp_color, false);
    std::string time_colored = ts_ansi + time_plain + "\x1b[0m";
    std::string prefix_plain = settings_.box_lower_left + settings_.box_horizontal + " " + time_plain + " ";
    int used = display_width(prefix_plain);
    int dashes_n = std::max(0, main_total_w - used - 1);
    std::string bottom_line = border_ansi_bottom + settings_.box_lower_left + settings_.box_horizontal + " "
                             + time_colored + " "
                             + border_ansi_bottom; // re-apply the border color -- time_colored's own reset above would otherwise leave the rest of this line uncolored.
                                                   // Straight in front of the dashes: the smooth gradient only recolours a border escape that a border glyph follows
    for (int i = 0; i < dashes_n; ++i) bottom_line += settings_.box_horizontal;
    bottom_line += settings_.box_lower_right;
    bottom_line += "\x1b[0m";
    // This row is only main_total_w wide, but the row above it runs the
    // full width (volume bar occupies the right-hand side). Frames are
    // drawn without a screen clear (cursor-home only), so any cell this
    // row does not write keeps whatever was there before -- e.g. the edge
    // of a floating panel (Retry Lyrics / Bulk Add / Clear Queue) that was
    // stamped over it, which then stayed on screen under the volume bar
    // after the panel closed. Pad to the full width so it is overwritten.
    if (side_w > 0) bottom_line += std::string(side_w, ' ');
    out.push_back(bar + " " + row3_content + " " + bar + vol_tail);
    out.push_back(bottom_line);
    return out;
}
std::vector<std::string> App::build_search_bar(int total_width) const {
    std::string label = (list_source_ == ListSource::Online) ? "SEARCH ONLINE"
                       : (list_source_ == ListSource::Playlist) ? "SEARCH PLAYLISTS"
                       : (list_source_ == ListSource::Folder) ? "SEARCH FOLDERS"
                       : "SEARCH LOCAL";
    if (list_source_ == ListSource::Local) {   // "/r:" and "/a:" narrow the search to artists / albums
        std::string rest;
        const int scope = local_query_scope(mode_ == Mode::Search ? search_buffer_ : last_local_query_, rest);
        if (scope == 1) label = "SEARCH ARTISTS";
        else if (scope == 2) label = "SEARCH ALBUMS";
    }

    {
        const std::string sl = sleep_timer_label();
        if (!sl.empty()) label += "  [" + sl + "]";
    }
    // The online resolve/download step (YouTube, SoundCloud, Bandcamp) shows its live note here, in the search box's
    // top border, instead of on an extra row under the list: that row made the frame one line taller than the
    // terminal for a moment, which scrolled the whole UI.
    if (load_in_progress_.load() && load_stage_.load() == 1) {
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - load_started_at_).count();
        const std::string t = std::to_string(static_cast<int>(secs)) + "s)";
        const int room = total_width - 5 - 5;   // search box width minus its corners and label padding
        for (const std::string& note : {"  resolving/downloading... (" + t, "  downloading... (" + t, "  (" + t}) {
            if (display_width(label + note) <= room) { label += note; break; }
        }
    }

    std::string content;
    if (mode_ == Mode::Search) {
        content.clear(); // caret/selection box built lower down
    } else if (list_source_ == ListSource::Online) {
        content = std::string(last_online_source_ == 1 ? "/sc:" : last_online_source_ == 2 ? "/b:" : "/s:") + last_online_query_;
    } else if (list_source_ == ListSource::Playlist) {
        content = "/p:" + last_playlist_query_;
    } else if (list_source_ == ListSource::Folder) {
        content = "/f:" + last_folder_query_;
    } else {
        std::string rest;
        const int scope = local_query_scope(last_local_query_, rest);
        content = scope ? "/" + std::string(1, scope == 1 ? 'r' : 'a') + ":" + rest : "/l:" + last_local_query_;
    }

    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::vector<std::string> out;
    int search_w = total_width - 5;
    if (mode_ == Mode::Search) label += focus_indicator(settings_, border_ansi);   // the search box has the keys
    out.push_back(box_top(label, search_w, border_ansi) + border_ansi + "╭───╮\x1b[0m");
    // Play-mode indicator: L=list, R=repeat, S=shuffle, Q=queue then stop,
    // O=stop -- one letter for whichever of the five settings_.play_mode
    // states is active, cycled with a single "m" press
    // (HKeyCyclePlayMode) rather than a separate toggle per mode.
    std::string mode_letter(1, play_mode_letter());
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::string line;
    if (mode_ == Mode::Search) {
        // Caret + selection + block cursor, where this used to be the query
        // with a hard-coded block appended. This one row cannot go through
        // box_line(): that pads with display_width(), which counts the
        // reverse-video escapes of a marked range as columns, so every
        // highlighted character would over-pad the line -- pad against
        // paint_edit_field column count instead (it returns exactly what
        // those escapes are worth).
        const int inner = std::max(1, search_w - 4); // box_line inner width
        const std::string idle = search_idle_sgr(settings_);
        const bool typed = !search_buffer_.empty();
        EditPaint p = paint_edit_field(search_buffer_, edit_caret_, edit_anchor_, inner - 1, typed ? search_sgr(settings_) : idle, true);
        std::string body = "/" + search_run(settings_, p.s, typed);
        const int fill = std::max(0, inner - (1 + p.cols));
        std::string pad(static_cast<size_t>(fill), ' ');
        line = bar + " " + idle + body + pad + "\x1b[0m " + bar;
    } else {
        const int inner = std::max(0, search_w - 4);
        // "/l:" etc. stay idle; only the query behind it is highlighted.
        const std::string shown = truncate_str(content, inner);
        const size_t colon = shown.find(':');
        const std::string pre = colon == std::string::npos ? shown : shown.substr(0, colon + 1);
        const std::string qry = shown.substr(pre.size());
        const int fill = std::max(0, inner - display_width(shown));
        line = bar + " " + search_idle_sgr(settings_) + pre + search_run(settings_, qry, !qry.empty())
             + std::string(static_cast<size_t>(fill), ' ') + "\x1b[0m " + bar;
    }
    out.push_back(line + border_ansi + settings_.box_vertical
                  + " " + mode_letter + " " + settings_.box_vertical + "\x1b[0m");
    out.push_back(box_bottom(search_w, "", border_ansi_bottom) + border_ansi_bottom + "╰───╯\x1b[0m");
    return out;
}

// "LOCAL AUDIO FILES (sort: ..., folder: <name> [c] clear)". A long folder name would run past the pane's top border
// and break the frame, so the text inside the () is shortened with "..." until the whole title fits.
std::string App::local_pane_label(int total_width) const {
    const std::string head = "LOCAL AUDIO FILES (sort: " + std::string(sort_mode_name(local_sort_mode_, settings_.meta_only));
    if (folder_filter_.empty()) return head + ")";
    const int room = total_width - 5;   // the title sits between "╭─ " and " ──╮"
    const std::string name = path_utf8(path_from_utf8(folder_filter_).filename());
    const std::string pre = ", folder: ", post = " [c] clear)";
    int avail = room - display_width(head) - display_width(pre) - display_width(post);
    if (avail >= 4) return head + pre + truncate_str(name, avail) + post;
    // very narrow pane: drop the "[c] clear" hint first, then the folder name itself
    const std::string post2 = ")";
    avail = room - display_width(head) - display_width(pre) - display_width(post2);
    if (avail >= 4) return head + pre + truncate_str(name, avail) + post2;
    std::string tiny = head + ")";
    return display_width(tiny) <= room ? tiny : truncate_str(tiny, std::max(1, room));
}

std::vector<std::string> App::build_list_panel(int total_width, int height) const {
    bool online = (list_source_ == ListSource::Online);
    bool playlists_mode = (list_source_ == ListSource::Playlist);
    bool folders_mode = (list_source_ == ListSource::Folder);
    std::string label = online ? "ONLINE RESULTS"
                       : playlists_mode ? "SAVED PLAYLISTS (Enter: queue all)"
                       : folders_mode ? "LOCAL AUDIO FOLDERS (Enter: open folder)"
                       : local_pane_label(total_width - 2);   // 2 columns stay free for the focus indicator
    size_t total = online ? online_view_.size() : playlists_mode ? playlist_view_.size()
                 : folders_mode ? folder_view_.size() : local_view_.size();
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);

    std::vector<std::string> out;
    // The list has the keys unless the search box is being typed into or the queue was focused (TAB).
    if (!queue_focus_ && mode_ != Mode::Search) label += focus_indicator(settings_, border_ansi);
    out.push_back(box_top(label, total_width, border_ansi));

    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll_ + row;
        std::string content;
        if (idx < static_cast<int>(total)) {
            if (online) {
                const auto& r = online_view_[idx];
                const int uploader_w = 18;
                int title_w = std::max(5, inner - idx_w - 2 - 2 - uploader_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(r.title, settings_.font_map);
                std::string t_uploader = apply_font_map(r.uploader, settings_.font_map);
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_title, title_w), title_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_uploader, uploader_w), uploader_w);
            } else if (playlists_mode) {
                const auto& p = playlist_view_[idx];
                const int count_w = 10;
                int name_w = std::max(5, inner - idx_w - 2 - 2 - count_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_name = apply_font_map(p.name, settings_.font_map);
                std::string t_count = std::to_string(p.track_count) + (p.track_count == 1 ? " track" : " tracks");
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_name, name_w), name_w) + settings_.list_separator + " "
                        + pad_right(t_count, count_w);
            } else if (folders_mode) {
                const auto& f = folder_view_[idx];
                const int parent_w = 16;
                const int count_w = 10;
                int name_w = std::max(5, inner - idx_w - 2 - 2 - parent_w - 2 - count_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_name = apply_font_map(f.name, settings_.font_map);
                std::string t_parent = apply_font_map(f.parent, settings_.font_map);
                std::string t_count = std::to_string(f.track_count) + (f.track_count == 1 ? " track" : " tracks");
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_name, name_w), name_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_parent, parent_w), parent_w) + settings_.list_separator + " "
                        + pad_right(t_count, count_w);
            } else {
                const auto& t = local_view_[idx];
                const int artist_w = 16;
                const int dur_w = 5;
                int title_w = std::max(5, inner - idx_w - 2 - 2 - artist_w - 2 - dur_w);
                double dur = -1;
                // BUGFIX: this was always the parent-folder name, even
                // though the metadata panel already reads the real ffprobe
                // artist tag for the loaded track — now the list uses that
                // same real tag (probed lazily for visible rows), falling
                // back to the folder guess only until it's been probed.
                std::string artist = t.folder_artist;
                {
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    auto it = row_meta_cache_.find(path_utf8(t.path));
                    if (it != row_meta_cache_.end()) {
                        dur = it->second.duration_sec;
                        if (!it->second.artist.empty()) {
                            artist = it->second.artist;
                        } else if (it->second.tags_resolved) {
                            // A real probe already ran and confirmed this file
                            // has no artist tag -- same distinction the
                            // metadata panel now makes (see start_local_track).
                            // Without this, an untagged file kept showing the
                            // folder-name guess forever instead of "-", which
                            // just relocated the original bug into this list.
                            artist = "-";
                        }
                    }
                }
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(list_row_title(t.path, t.title), settings_.font_map);
                std::string t_artist = apply_font_map(artist, settings_.font_map);
                std::string t_dur = apply_font_map(fmt_mmss(dur), settings_.font_map);

                // Only the hovered row animates, and only when its title
                // is actually too long to fit -- every other row still
                // gets the same static truncate_str() as before, so
                // nothing about the rest of the list changes.
                std::string title_shown = (idx == selected_)
                    ? marquee_or_truncate(t_title, title_w, idx, marquee_row_idx_, marquee_since_)
                    : pad_right(truncate_str(t_title, title_w), title_w);
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(title_shown, title_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_artist, artist_w), artist_w) + settings_.list_separator + " "
                        + t_dur;
            }
        }
        bool sel = (idx == selected_) && idx < static_cast<int>(total);
        bool is_playing_row = has_track_ && list_source_ == ListSource::Local && idx < static_cast<int>(total)
                               && local_view_[idx].path == current_path_;
        std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else if (is_playing_row) {
            std::string playing_ansi = ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color);
            out.push_back(bar + " " + playing_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }

    std::string footer;
    int remaining = static_cast<int>(total) - (scroll_ + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_queue_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;
    // "locked" sits next to "focused" (and shows without focus too, because
    // the lock changes what playback does whether or not the pane has focus).
    // The focus indicator replaces the old "(focused)" note; "locked" shows with or without focus (the lock changes
    // what playback does whether or not the pane has the keys).
    std::string title = queue_locked_ ? "QUEUE (locked)" : "QUEUE";
    if (queue_focus_) title += focus_indicator(settings_, border_ansi);
    out.push_back(box_top(title, total_width, border_ansi));

    if (queue_.empty()) {
        int mid_row = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid_row) {
                std::string text = apply_font_map("ADD TRACKS TO QUEUE", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string queue_ansi = ansi_for(settings_.queue_color, false);
            out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
        }
    } else {
        for (int row = 0; row < height; ++row) {
            int idx = queue_scroll_ + row;
            std::string content;
            bool is_row_playing = false;
            bool is_row_hovering = false;
            if (idx < static_cast<int>(queue_.size())) {
                const auto& q = queue_[idx];
                // The same information as the LOCAL AUDIO pane (title / name and artist), without the list number.
                std::string artist = q.artist;
                if (q.is_local) {   // the real artist tag once the list has probed the file, like the LOCAL AUDIO pane
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    auto it = row_meta_cache_.find(path_utf8(q.local_path));
                    if (it != row_meta_cache_.end()) {
                        if (!it->second.artist.empty()) artist = it->second.artist;
                        else if (it->second.tags_resolved) artist = "-";
                    }
                }
                if (artist.empty()) artist = "-";
                std::string t_title = apply_font_map(q.is_local ? list_row_title(q.local_path, q.title) : q.title, settings_.font_map);   // "Show meta data only" counts here too
                const std::string t_artist = apply_font_map(artist, settings_.font_map);
                const int sep_w = display_width(settings_.list_separator) + 1;
                const int artist_w = inner >= 34 ? 16 : inner >= 24 ? 10 : 0;
                if (artist_w == 0) content = t_title;
                else {
                    const int title_w = std::max(5, inner - artist_w - sep_w);
                    content = pad_right(truncate_str(t_title, title_w), title_w) + settings_.list_separator + " " + truncate_str(t_artist, artist_w);
                }
                is_row_playing = has_track_ && q.is_local && q.local_path == current_path_;
                is_row_hovering = queue_focus_ && (idx == queue_selected_);
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string color_ansi;
            if (is_row_hovering) color_ansi = cursor_sgr(settings_.queue_cursor_color, settings_.queue_cursor_bg_color);
            else if (is_row_playing) color_ansi = ansi_for(settings_.queue_playing_color, true) + bg_ansi_for(settings_.queue_playing_bg_color);
            else color_ansi = ansi_for(settings_.queue_color, false) + bg_ansi_for(settings_.queue_inactive_bg_color);
            out.push_back(bar + " " + color_ansi + padded + "\x1b[0m " + bar);
        }
    }

    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

// ---------------------------------------------------------------------
// Playlist editor overlay (Mode::Playlist, HKeyPlaylist)
// ---------------------------------------------------------------------

void App::playlist_refresh_lib_view() {
    playlist_edit_lib_view_ = filter_and_rank_local(playlist_edit_lib_query_);
    playlist_edit_lib_selected_ = std::clamp(playlist_edit_lib_selected_, 0,
        std::max(0, static_cast<int>(playlist_edit_lib_view_.size()) - 1));
}

void App::playlist_refresh_manage_view() {
    playlist_manage_view_ = filter_playlists(playlist_manage_query_);
    playlist_manage_selected_ = std::clamp(playlist_manage_selected_, 0,
        std::max(0, static_cast<int>(playlist_manage_view_.size()) - 1));
}

// HKeyPlaylist entry point -- always starts a fresh, blank playlist on
// tab 0 (name field focused, ready to type). The only way to bring an
// *existing* playlist back into the editor is explicit: tab 1, Enter on
// it (see playlist_load_into_editor()) -- that way reopening this
// overlay never silently discards an unsaved in-progress playlist by
// accident.
void App::playlist_open_editor() {
    mode_ = Mode::Playlist;
    playlist_export_open_ = false;
    text_disengage();
    playlist_tab_ = 0;
    playlist_edit_focus_ = 0;
    playlist_edit_name_.clear();
    playlist_edit_tracks_.clear();
    playlist_edit_lib_query_.clear();
    playlist_edit_lib_selected_ = 0;
    playlist_edit_track_selected_ = 0;
    playlist_status_.clear();
    playlist_edit_dirty_ = false;
    playlist_confirm_exit_ = false;
    playlist_confirm_delete_ = false;
    playlist_manage_query_.clear();
    playlist_manage_focus_ = 1; // land in the list: Enter/DEL work immediately; Tab goes to the search box
    playlist_refresh_lib_view();
    playlist_refresh_manage_view();
}

void App::playlist_load_into_editor(const std::string& name) {
    auto pl = load_playlist(name);
    if (!pl) { playlist_status_ = "could not load \"" + name + "\""; return; }
    playlist_edit_name_ = pl->name;
    playlist_edit_tracks_ = pl->tracks;
    playlist_edit_track_selected_ = 0;
    playlist_tab_ = 0;
    playlist_edit_focus_ = 1;
    text_disengage();
    playlist_edit_dirty_ = false; // freshly loaded from disk -- matches what's saved, nothing to lose yet
    playlist_status_ = "editing \"" + pl->name + "\" (" + std::to_string(pl->tracks.size()) + " tracks)";
}

static std::string header_sgr(const Settings& s); // defined further down (Settings panel section)
static std::string legend_sgr(const Settings& s); // likewise
static std::string focus_indicator(const Settings& s, const std::string& border_ansi); // likewise

// ---------------------------------------------------------------------
// Playlist export (M3U8 / M3U)
// ---------------------------------------------------------------------

void App::playlist_export_begin() {
    const int total = static_cast<int>(playlist_manage_view_.size());
    if (playlist_manage_selected_ < 0 || playlist_manage_selected_ >= total) { playlist_status_ = "no playlist selected"; return; }
    playlist_export_name_ = playlist_manage_view_[playlist_manage_selected_].name;
    playlist_export_dir_ = settings_.playlist_export_path.empty() ? path_utf8(playlists_dir()) : settings_.playlist_export_path;
    playlist_export_m3u_ = false;
    playlist_export_field_ = 0;
    playlist_export_status_.clear();
    edit_owner_.clear();   // the folder field takes the caret fresh (at the end)
    playlist_export_open_ = true;
}

void App::playlist_export_run() {
    std::string dir = playlist_export_dir_;
    while (!dir.empty() && (dir.front() == ' ' || dir.front() == '\t')) dir.erase(dir.begin());
    while (!dir.empty() && (dir.back() == ' ' || dir.back() == '\t')) dir.pop_back();
    if (!dir.empty() && dir[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) dir = std::string(home) + dir.substr(1);
    }
    if (dir.empty()) { playlist_export_status_ = "enter a folder first"; playlist_export_field_ = 0; return; }
    auto pl = load_playlist(playlist_export_name_);
    if (!pl) { playlist_export_status_ = "could not load \"" + playlist_export_name_ + "\""; return; }

    const fs::path folder = path_from_utf8(dir);
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (!fs::is_directory(folder, ec)) { playlist_export_status_ = "cannot create the folder " + dir; return; }
    const std::string ext = playlist_export_m3u_ ? ".m3u" : ".m3u8";
    const fs::path file = folder / path_from_utf8(PlaylistManager::sanitize_name(pl->name) + ext);

    // Extended M3U: one #EXTINF line (length unknown = -1, "Artist - Title")
    // and the absolute path per track. Both formats are written as UTF-8 --
    // M3U8 is defined that way, and current players read a plain .m3u as
    // UTF-8 too. Missing files are left out (and counted).
    std::ostringstream out;
    out << "#EXTM3U\n";
    out << "#PLAYLIST:" << pl->name << "\n";
    int written = 0, missing = 0;
    for (const auto& t : pl->tracks) {
        if (!fs::exists(t.path, ec)) { ++missing; continue; }
        std::string title = t.title, artist = t.artist;
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            auto it = row_meta_cache_.find(path_utf8(t.path));
            if (it != row_meta_cache_.end()) {
                if (!it->second.title.empty()) title = it->second.title;
                if (!it->second.artist.empty()) artist = it->second.artist;
            }
        }
        out << "#EXTINF:-1," << (artist.empty() ? title : artist + " - " + title) << "\n";
        out << path_utf8(fs::absolute(t.path, ec)) << "\n";
        ++written;
    }
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) { playlist_export_status_ = "cannot write " + path_utf8(file); return; }
    f << out.str();
    f.close();
    playlist_export_open_ = false;
    playlist_status_ = "exported " + std::to_string(written) + " track" + (written == 1 ? "" : "s") + " to " + path_utf8(file)
                     + (missing > 0 ? " (" + std::to_string(missing) + " missing, left out)" : "");
    log_event(playlist_status_);
}

std::vector<std::string> App::build_playlist_export_panel(int panel_w) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    const int inner = panel_w - 4;
    std::vector<std::string> lines;
    auto row = [&](const std::string& painted, int cols) {
        lines.push_back(bar + " " + painted + std::string(static_cast<size_t>(std::max(0, inner - cols)), ' ') + " " + bar);
    };
    lines.push_back(box_top("EXPORT PLAYLIST \"" + truncate_str(playlist_export_name_, std::max(4, panel_w - 24)) + "\"", panel_w, border));
    row("", 0);
    {   // the folder field, with caret while it has the focus
        const std::string label = "Folder : ";
        const int lw = display_width(label);
        const bool f = playlist_export_field_ == 0;
        std::string painted; int cols;
        if (f) {
            const size_t c = std::min(edit_caret_, playlist_export_dir_.size());
            const size_t a = std::min(edit_anchor_, playlist_export_dir_.size());
            EditPaint p = paint_edit_field(playlist_export_dir_, edit_owner_ == "playlist-export-dir" ? c : playlist_export_dir_.size(),
                                           edit_owner_ == "playlist-export-dir" ? a : playlist_export_dir_.size(),
                                           std::max(4, inner - lw), "", true);
            painted = p.s; cols = p.cols;
        } else {
            painted = truncate_str(playlist_export_dir_, std::max(4, inner - lw));
            cols = display_width(painted);
        }
        row(header_sgr(settings_) + label + R + painted, lw + cols);
    }
    {   // the format row
        const std::string label = "Format : ";
        const bool f = playlist_export_field_ == 1;
        const std::string hi = f ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : "\x1b[1m";
        const std::string m8 = playlist_export_m3u_ ? " M3U8 " : hi + "[M3U8]" + R;
        const std::string m3 = playlist_export_m3u_ ? hi + "[M3U]" + R : " M3U ";
        row(header_sgr(settings_) + label + R + m8 + "  " + m3 + legend_sgr(settings_) + "   (M3U8 = UTF-8, the default)" + R,
            display_width(label) + 6 + 2 + 5 + display_width("   (M3U8 = UTF-8, the default)"));
    }
    {
        const std::string file = PlaylistManager::sanitize_name(playlist_export_name_) + (playlist_export_m3u_ ? ".m3u" : ".m3u8");
        const std::string t = truncate_str("File   : " + file, inner);
        row(t, display_width(t));
    }
    row("", 0);
    {
        const std::string st = truncate_str(playlist_export_status_, inner);
        row("\x1b[32m" + st + R, display_width(st));
    }
    {
        const std::string hint = truncate_str("[TAB/\u2191\u2193] Field | [\u2190\u2192] Cursor / format | [ENTER] Export | [ESC] Cancel", inner);
        row(legend_sgr(settings_) + hint + R, display_width(hint));
    }
    lines.push_back(box_bottom(panel_w, "", border));
    return lines;
}

// ---------------------------------------------------------------------
// Playlist import (M3U8 / M3U)
// ---------------------------------------------------------------------

namespace {

std::string import_trim(std::string d) {
    while (!d.empty() && (d.front() == ' ' || d.front() == '\t')) d.erase(d.begin());
    while (!d.empty() && (d.back() == ' ' || d.back() == '\t')) d.pop_back();
    if (!d.empty() && d[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) d = std::string(home) + d.substr(1);
    }
    return d;
}

std::string import_lower_ext(const fs::path& p) {
    std::string e = path_utf8(p.extension());
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// "%20" etc. of a file:// URL
std::string import_url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else o.push_back(s[i]);
    }
    return o;
}

// Reads one extended-M3U / plain M3U(8) file: the optional "#PLAYLIST:" name and
// the track paths. Relative paths are resolved against the file's own folder;
// stream URLs are skipped (a playlist here is local files only), "file://" URLs
// are unwrapped. Returns false if the file cannot be read.
bool import_parse_m3u(const fs::path& file, std::string* name_out, std::vector<fs::path>* tracks_out, int* skipped_urls) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return false;
    std::string line;
    bool first = true;
    std::error_code ec;
    while (std::getline(in, line)) {
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3); // UTF-8 BOM
        first = false;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (name_out && line.compare(0, 10, "#PLAYLIST:") == 0) {
                std::string n = line.substr(10);
                while (!n.empty() && n.front() == ' ') n.erase(n.begin());
                if (!n.empty()) *name_out = n;
            }
            continue;
        }
        if (line.compare(0, 7, "http://") == 0 || line.compare(0, 8, "https://") == 0) { if (skipped_urls) ++*skipped_urls; continue; }
        if (line.compare(0, 7, "file://") == 0) line = import_url_decode(line.substr(7));
        fs::path p = path_from_utf8(line);
#if !defined(_WIN32)
        if (line.find('\\') != std::string::npos && !fs::exists(p, ec)) { // made on Windows
            std::string alt = line;
            std::replace(alt.begin(), alt.end(), '\\', '/');
            p = path_from_utf8(alt);
        }
#endif
        if (p.is_relative()) p = file.parent_path() / p;
        if (tracks_out) tracks_out->push_back(p.lexically_normal());
    }
    return true;
}

} // namespace

void App::playlist_import_begin() {
    playlist_import_dir_ = settings_.playlist_export_path.empty() ? path_utf8(playlists_dir()) : settings_.playlist_export_path;
    playlist_import_field_ = 0;
    playlist_import_status_.clear();
    edit_owner_.clear();   // the folder field takes the caret fresh (at the end)
    playlist_import_open_ = true;
    playlist_import_scan();
}

void App::playlist_import_scan() {
    playlist_import_files_.clear();
    playlist_import_counts_.clear();
    playlist_import_marked_.clear();
    playlist_import_selected_ = 0;
    const std::string dir = import_trim(playlist_import_dir_);
    if (dir.empty()) { playlist_import_status_ = "enter a folder"; return; }
    std::error_code ec;
    const fs::path folder = path_from_utf8(dir);
    if (!fs::is_directory(folder, ec)) { playlist_import_status_ = "not a folder"; return; }
    std::vector<fs::path> found;
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string ext = import_lower_ext(it->path());
        if (ext == ".m3u" || ext == ".m3u8") found.push_back(it->path());
    }
    std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
        std::string x = path_utf8(a.filename()), y = path_utf8(b.filename());
        for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : y) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return x < y;
    });
    for (const auto& f : found) {
        std::vector<fs::path> tr;
        import_parse_m3u(f, nullptr, &tr, nullptr);
        playlist_import_files_.push_back(path_utf8(f));
        playlist_import_counts_.push_back(static_cast<int>(tr.size()));
    }
    playlist_import_marked_.assign(playlist_import_files_.size(), 0);
    playlist_import_status_ = playlist_import_files_.empty() ? "no .m3u / .m3u8 files in this folder" : "";
}

void App::playlist_import_run() {
    if (playlist_import_files_.empty()) { playlist_import_status_ = "nothing to import in this folder"; return; }
    // the marked files; with none marked, the hovered one
    std::vector<int> pick;
    for (size_t i = 0; i < playlist_import_marked_.size(); ++i) if (playlist_import_marked_[i]) pick.push_back(static_cast<int>(i));
    if (pick.empty()) pick.push_back(std::clamp(playlist_import_selected_, 0, static_cast<int>(playlist_import_files_.size()) - 1));

    const fs::path dir = playlists_dir();
    int imported = 0, tracks_total = 0, missing_total = 0, urls = 0;
    std::string last_name, error;
    std::error_code ec;
    for (int idx : pick) {
        const fs::path file = path_from_utf8(playlist_import_files_[idx]);
        std::string name;
        std::vector<fs::path> paths;
        if (!import_parse_m3u(file, &name, &paths, &urls)) { error = "cannot read " + path_utf8(file.filename()); continue; }
        if (paths.empty()) { error = path_utf8(file.filename()) + " has no tracks"; continue; }
        if (name.empty()) name = path_utf8(file.stem());
        // The editor's name field takes 25 characters; keep imported names inside that, and never
        // overwrite a playlist that already exists -- " (2)", " (3)" ... instead.
        const std::string base = utf8_take(name, 25);
        std::string unique = base;
        for (int n = 2; PlaylistManager::load(dir, unique).has_value() && n < 1000; ++n) {
            const std::string suffix = " (" + std::to_string(n) + ")";
            unique = utf8_take(base, 25 - static_cast<int>(suffix.size())) + suffix;
        }
        Playlist pl;
        pl.name = unique;
        for (const auto& p : paths) {
            PlaylistTrack t;
            t.path = p;
            t.title = path_utf8(p.stem());
            t.artist = path_utf8(p.parent_path().filename());
            t.missing = !fs::exists(p, ec);
            if (t.missing) ++missing_total;
            pl.tracks.push_back(std::move(t));
        }
        std::string err;
        if (!PlaylistManager::save(dir, pl, &err)) { error = "save failed: " + err; continue; }
        ++imported;
        tracks_total += static_cast<int>(pl.tracks.size());
        last_name = unique;
    }
    if (imported == 0) { playlist_import_status_ = error.empty() ? "nothing imported" : error; return; }

    playlist_import_open_ = false;
    playlist_refresh_manage_view();
    if (list_source_ == ListSource::Playlist) playlist_view_ = filter_playlists(last_playlist_query_);
    playlist_status_ = "imported " + std::to_string(imported) + " playlist" + (imported == 1 ? "" : "s")
                     + (imported == 1 ? " \"" + last_name + "\"" : std::string())
                     + " (" + std::to_string(tracks_total) + " track" + (tracks_total == 1 ? "" : "s") + ")"
                     + (missing_total > 0 ? ", " + std::to_string(missing_total) + " file(s) not found (kept, skipped when queued)" : std::string())
                     + (urls > 0 ? ", " + std::to_string(urls) + " stream URL(s) left out" : std::string())
                     + (error.empty() ? std::string() : "; " + error);
    log_event(playlist_status_);
}

std::vector<std::string> App::build_playlist_import_panel(int panel_w) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    const int inner = panel_w - 4;
    std::vector<std::string> lines;
    auto row = [&](const std::string& painted, int cols) {
        lines.push_back(bar + " " + painted + std::string(static_cast<size_t>(std::max(0, inner - cols)), ' ') + " " + bar);
    };
    lines.push_back(box_top("IMPORT PLAYLISTS", panel_w, border));
    row("", 0);
    {   // the folder field, with caret while it has the focus
        const std::string label = "Folder : ";
        const int lw = display_width(label);
        std::string painted; int cols;
        if (playlist_import_field_ == 0) {
            const bool mine = edit_owner_ == "playlist-import-dir";
            const size_t c = std::min(edit_caret_, playlist_import_dir_.size());
            const size_t a = std::min(edit_anchor_, playlist_import_dir_.size());
            EditPaint p = paint_edit_field(playlist_import_dir_, mine ? c : playlist_import_dir_.size(),
                                           mine ? a : playlist_import_dir_.size(), std::max(4, inner - lw), "", true);
            painted = p.s; cols = p.cols;
        } else {
            painted = truncate_str(playlist_import_dir_, std::max(4, inner - lw));
            cols = display_width(painted);
        }
        row(header_sgr(settings_) + label + R + painted, lw + cols);
    }
    row("", 0);
    {
        const std::string t = "Playlists found (" + std::to_string(playlist_import_files_.size()) + ")"
                            + (playlist_import_files_.empty() ? std::string() : "   [x] = marked");
        row(header_sgr(settings_) + truncate_str(t, inner) + R, display_width(truncate_str(t, inner)));
    }
    constexpr int kRows = 8;
    const int total = static_cast<int>(playlist_import_files_.size());
    const int sel = std::clamp(playlist_import_selected_, 0, std::max(0, total - 1));
    const int scroll = std::clamp(sel - kRows / 2, 0, std::max(0, total - kRows));
    for (int r = 0; r < kRows; ++r) {
        const int i = scroll + r;
        if (i >= total) { row("", 0); continue; }
        const bool marked = i < static_cast<int>(playlist_import_marked_.size()) && playlist_import_marked_[i];
        const std::string cnt = std::to_string(playlist_import_counts_[i]) + (playlist_import_counts_[i] == 1 ? " track" : " tracks");
        const std::string fname = path_utf8(path_from_utf8(playlist_import_files_[i]).filename());
        const int name_w = std::max(5, inner - 4 - 2 - static_cast<int>(cnt.size()));
        std::string text = std::string(marked ? "[x] " : "[ ] ") + pad_right(truncate_str(fname, name_w), name_w) + "  " + cnt;
        text = pad_right(truncate_str(text, inner), inner);
        const bool hov = i == sel;
        const std::string ansi = hov ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color)
                                     : ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
        lines.push_back(bar + " " + ansi + text + R + " " + bar);
    }
    row("", 0);
    {
        const std::string st = truncate_str(playlist_import_status_, inner);
        row("\x1b[32m" + st + R, display_width(st));
    }
    {
        const std::string hint = truncate_str(playlist_import_field_ == 0
            ? "[TAB] List | [↑↓] Select | [ENTER] Import | [ESC] Cancel"
            : "[TAB] Folder | [↑↓] Select | [SPACE] Mark | [a] All | [ENTER] Import | [ESC] Cancel", inner);
        row(legend_sgr(settings_) + hint + R, display_width(hint));
    }
    lines.push_back(box_bottom(panel_w, "", border));
    return lines;
}

void App::playlist_add_hovering_to_edit() {
    if (playlist_edit_lib_selected_ < 0
        || playlist_edit_lib_selected_ >= static_cast<int>(playlist_edit_lib_view_.size())) return;
    const auto& t = playlist_edit_lib_view_[playlist_edit_lib_selected_];
    for (auto& existing : playlist_edit_tracks_) {
        if (existing.path == t.path) { playlist_status_ = "already in the playlist"; return; }
    }
    PlaylistTrack pt;
    pt.title = t.title;
    pt.artist = t.folder_artist;
    pt.path = t.path;
    pt.missing = false;
    playlist_edit_tracks_.push_back(std::move(pt));
    playlist_edit_dirty_ = true;
    playlist_status_.clear();
}

void App::playlist_remove_hovering_track() {
    if (playlist_edit_track_selected_ < 0
        || playlist_edit_track_selected_ >= static_cast<int>(playlist_edit_tracks_.size())) return;
    const std::string removed_title = playlist_edit_tracks_[playlist_edit_track_selected_].title;
    playlist_edit_tracks_.erase(playlist_edit_tracks_.begin() + playlist_edit_track_selected_);
    playlist_status_ = "removed \"" + removed_title + "\"";
    playlist_edit_track_selected_ = std::clamp(playlist_edit_track_selected_, 0,
        std::max(0, static_cast<int>(playlist_edit_tracks_.size()) - 1));
    playlist_edit_dirty_ = true;
}

// Keys 4/5 in the track list (tab 0, playlist_edit_focus_==2) -- same
// swap-with-neighbor approach as queue_move_hovering(), just against
// playlist_edit_tracks_ instead of queue_. Reordering counts as a
// change like add/remove, so it sets the dirty flag too.
void App::playlist_move_hovering_track(int dir) {
    if (playlist_edit_tracks_.empty()) return;
    int target = playlist_edit_track_selected_ + dir;
    if (target < 0 || target >= static_cast<int>(playlist_edit_tracks_.size())) return; // already at an edge
    std::swap(playlist_edit_tracks_[playlist_edit_track_selected_], playlist_edit_tracks_[target]);
    playlist_edit_track_selected_ = target;
    playlist_edit_dirty_ = true;
}

// Tab 1's DEL, fired only after playlist_confirm_delete_ has been
// confirmed with Y -- see handle_playlist_key().
void App::playlist_delete_selected() {
    if (playlist_manage_selected_ < 0
        || playlist_manage_selected_ >= static_cast<int>(playlist_manage_view_.size())) return;
    std::string name = playlist_manage_view_[playlist_manage_selected_].name;
    std::string error;
    if (!PlaylistManager::remove(playlists_dir(), name, &error)) {
        playlist_status_ = "delete failed: " + error;
        return;
    }
    playlist_status_ = "deleted \"" + name + "\"";
    playlist_refresh_manage_view();
    // Same reasoning as playlist_save_current(): keep the main UI's "/p:"
    // browse view in sync if it's currently showing playlists.
    if (list_source_ == ListSource::Playlist) playlist_view_ = filter_playlists(last_playlist_query_);
}

// HOME -- saves and STAYS in the playlist menu (ESC leaves it), so the
// confirmation shows in the menu's own status row. Only the "Save before
// exiting?" prompt passes leave=true: there the save is part of leaving.
// Deliberately not a plain letter (an earlier version used "S", which meant
// typing an "s" into the name field or the library search saved and kicked
// you out mid-keystroke); HOME can never appear inside typed text.
void App::playlist_save_current(bool leave) {
    std::string name = playlist_edit_name_;
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) {
        playlist_status_ = "enter a name first";
        playlist_edit_focus_ = 0;
        return;
    }
    Playlist pl;
    pl.name = name;
    pl.tracks = playlist_edit_tracks_;
    std::string error;
    if (!PlaylistManager::save(playlists_dir(), pl, &error)) {
        playlist_status_ = "save failed: " + error;
        return;
    }
    status_line_ = "saved playlist \"" + name + "\" (" + std::to_string(pl.tracks.size()) + " tracks)";
    playlist_status_ = status_line_; // the menu stays open now, so say it here too
    // If the main UI is currently browsing "/p:" results, refresh them
    // so a newly-saved (or renamed) playlist shows up immediately.
    if (list_source_ == ListSource::Playlist) playlist_view_ = filter_playlists(last_playlist_query_);
    playlist_edit_dirty_ = false;
    if (leave) mode_ = Mode::Browse;
}

void App::handle_playlist_key(int key) {
    // "Save before exiting?" prompt -- shown instead of the hint line
    // when ESC is pressed on tab 0 with unsaved changes (see below).
    // Swallows every key except the three it cares about so nothing
    // gets edited underneath it by accident.
    if (playlist_confirm_exit_) {
        if (key == 'y' || key == 'Y') { playlist_confirm_exit_ = false; playlist_save_current(true); return; }
        if (key == 'n' || key == 'N') { playlist_confirm_exit_ = false; mode_ = Mode::Browse; return; }
        if (key == 27) { playlist_confirm_exit_ = false; } // cancel the prompt, keep editing
        return;
    }

    // "Really delete this playlist?" prompt -- shown instead of the hint
    // line when DEL is pressed on tab 1 (see below). Same swallow-every-
    // other-key shape as playlist_confirm_exit_ above, so nothing on tab
    // 1 can be triggered by accident while it's up.
    if (playlist_confirm_delete_) {
        if (key == 'y' || key == 'Y') { playlist_confirm_delete_ = false; playlist_delete_selected(); return; }
        if (key == 'n' || key == 'N') { playlist_confirm_delete_ = false; return; }
        if (key == 27) { playlist_confirm_delete_ = false; } // cancel the prompt
        return;
    }

    // Import overlay ('i' on the Saved Playlists tab): owns every key while open.
    if (playlist_import_open_) {
        const bool arrow = last_key_was_arrow();
        const int total = static_cast<int>(playlist_import_files_.size());
        if (key == 27) { playlist_import_open_ = false; return; }
        if (key == '\r' || key == '\n' || key == kKeyCtrlS) { playlist_import_run(); return; }
        if (key == 9) { playlist_import_field_ ^= 1; return; }
        if (arrow && key == 'A') { if (playlist_import_selected_ > 0) --playlist_import_selected_; return; }
        if (arrow && key == 'B') { if (total > 0 && playlist_import_selected_ < total - 1) ++playlist_import_selected_; return; }
        if (playlist_import_field_ == 1) { // the list: SPACE marks, 'a' marks / unmarks all
            if (key == ' ' && total > 0) {
                const int i = std::clamp(playlist_import_selected_, 0, total - 1);
                playlist_import_marked_[i] = !playlist_import_marked_[i];
            } else if (key == 'a' || key == 'A') {
                const bool any_unmarked = std::find(playlist_import_marked_.begin(), playlist_import_marked_.end(), 0) != playlist_import_marked_.end();
                std::fill(playlist_import_marked_.begin(), playlist_import_marked_.end(), any_unmarked ? 1 : 0);
            }
            return;
        }
        edit_focus("playlist-import-dir", playlist_import_dir_);
        if (edit_text_key(playlist_import_dir_, edit_caret_, edit_anchor_, key, 240, &playlist_import_status_)) playlist_import_scan();
        return;
    }

    // Export overlay ('e' on the Saved Playlists tab): owns every key while open.
    if (playlist_export_open_) {
        const bool arrow = last_key_was_arrow();
        if (key == 27) { playlist_export_open_ = false; return; }
        if (key == '\r' || key == '\n') { playlist_export_run(); return; }
        if (key == kKeyCtrlS) { playlist_export_run(); return; }
        if (key == 9 || (arrow && (key == 'A' || key == 'B'))) { playlist_export_field_ ^= 1; return; }
        if (playlist_export_field_ == 1) {   // format row: Left/Right or SPACE flip M3U8 <-> M3U
            if ((arrow && (key == 'C' || key == 'D')) || key == ' ') playlist_export_m3u_ = !playlist_export_m3u_;
            return;
        }
        edit_focus("playlist-export-dir", playlist_export_dir_);
        edit_text_key(playlist_export_dir_, edit_caret_, edit_anchor_, key, 240, &playlist_export_status_);
        return;
    }

    // Which text box (if any) has the keyboard right now -- the same owner
    // names edit_focus() uses. Empty on the two plain lists.
    const char* field = playlist_tab_ == 1 ? (playlist_manage_focus_ == 0 ? "playlist-manage-search" : "")
                      : playlist_edit_focus_ == 0 ? "playlist-name"
                      : playlist_edit_focus_ == 1 ? "playlist-lib-search" : "";

    if (key == 27) { // ESC
        // First ESC after typing: leave the field (Left/Right switch tabs
        // again, the text stays). Only an ESC outside a field leaves the menu.
        if (*field && text_engaged(field)) { text_disengage(); return; }
        if (playlist_tab_ == 0 && playlist_edit_dirty_) { playlist_confirm_exit_ = true; return; }
        mode_ = Mode::Browse;
        return;
    }
    if (key == kKeyCtrlS) {
        if (playlist_tab_ == 0) playlist_save_current();
        else playlist_status_ = "switch to Create/Edit (\u2190\u2192) to save a playlist";
        return;
    }
    // Arrows collapse to 'A'..'D' app-wide; last_key_was_arrow() is what
    // tells a real arrow from a typed capital.
    const bool arrow = last_key_was_arrow();
    // Tab switch: plain Left/Right, as in the meta editor and Settings --
    // EXCEPT while a text box is in use. A box only counts as in use once
    // something was typed / edited in it (text_engage()); until then the
    // arrows switch tabs, afterwards they move the caret, and ESC hands
    // them back to the tabs. Alt+Left/Right keeps working from everywhere.
    const bool lr = arrow && (key == 'C' || key == 'D');
    if (key == kKeyAltLeft || key == kKeyAltRight || (lr && !(*field && text_engaged(field)))) {
        playlist_tab_ = (playlist_tab_ + 1) % 2;
        text_disengage();
        if (playlist_tab_ == 1) playlist_refresh_manage_view();
        return;
    }

    if (playlist_tab_ == 1) {
        // --- Tab 1: browse/manage saved playlists ---
        int total = static_cast<int>(playlist_manage_view_.size());
        if (key == 9) { // Tab -- search box <-> list
            playlist_manage_focus_ = (playlist_manage_focus_ == 0) ? 1 : 0;
            text_disengage();
            return;
        }
        if (playlist_manage_focus_ == 0) { // search box -- filter, caret, marking, clipboard
            // Up/Down keep walking the filtered list while the box is
            // focused, same as the main UI's "/" search does.
            if (arrow && (key == 'A' || key == 'B')) {
                if (key == 'A') {
                    if (playlist_manage_selected_ > 0) --playlist_manage_selected_;
                    return;
                }
                if (total > 0 && playlist_manage_selected_ < total - 1) ++playlist_manage_selected_;
                return;
            }
            if (key == '\r' || key == '\n') { playlist_manage_focus_ = 1; text_disengage(); return; } // Enter: leave the box, take the list
            edit_focus("playlist-manage-search", playlist_manage_query_);
            text_engage("playlist-manage-search");
            if (edit_text_key(playlist_manage_query_, edit_caret_, edit_anchor_, key, 80, &playlist_status_))
                playlist_refresh_manage_view();
            return;
        }
        if (arrow && key == 'A') { // up
            if (playlist_manage_selected_ > 0) --playlist_manage_selected_;
            return;
        }
        if (arrow && key == 'B') { // down
            if (total > 0 && playlist_manage_selected_ < total - 1) ++playlist_manage_selected_;
            return;
        }
        if (key == '\r' || key == '\n') {
            if (total > 0 && playlist_manage_selected_ < total) {
                playlist_load_into_editor(playlist_manage_view_[playlist_manage_selected_].name);
            }
            return;
        }
        if (key == kKeyDelete && total > 0 && playlist_manage_selected_ < total) {
            playlist_confirm_delete_ = true;
            return;
        }
        if (key == 'e' || key == 'E') { playlist_export_begin(); return; }   // export as M3U8 / M3U
        if (key == 'i' || key == 'I') { playlist_import_begin(); return; }   // import M3U8 / M3U files
        return;
    }

    // --- Tab 0: create/edit ---
    if (key == 9) { // Tab -- cycle focus: name field -> library picker -> track list -> ...
        playlist_edit_focus_ = (playlist_edit_focus_ + 1) % 3;
        text_disengage();
        return;
    }

    if (playlist_edit_focus_ == 0) { // name field
        if (key == '\r' || key == '\n') { playlist_edit_focus_ = 1; text_disengage(); return; } // confirm name, jump to picking tracks
        if (arrow && (key == 'A' || key == 'B')) return; // single line, nothing to move to
        edit_focus("playlist-name", playlist_edit_name_);
        text_engage("playlist-name");
        // Caret, marking, clipboard and typing in one call -- Up/Down
        // arrows included (edit_text_key drops them, this box has no
        // vertical anything to navigate), where the old code had to
        // blacklist 'A'/'B' so they could not be typed either.
        if (edit_text_key(playlist_edit_name_, edit_caret_, edit_anchor_, key, 25, &playlist_status_))
            playlist_edit_dirty_ = true;
        return;
    }

    if (playlist_edit_focus_ == 1) { // library picker -- typing filters live, same as the main search box
        int total = static_cast<int>(playlist_edit_lib_view_.size());
        if (arrow && key == 'A') {
            if (playlist_edit_lib_selected_ > 0) --playlist_edit_lib_selected_;
            return;
        }
        if (arrow && key == 'B') {
            if (total > 0 && playlist_edit_lib_selected_ < total - 1) ++playlist_edit_lib_selected_;
            return;
        }
        if (key == '\r' || key == '\n') { playlist_add_hovering_to_edit(); return; }
        edit_focus("playlist-lib-search", playlist_edit_lib_query_);
        text_engage("playlist-lib-search");
        if (edit_text_key(playlist_edit_lib_query_, edit_caret_, edit_anchor_, key, 120, &playlist_status_))
            playlist_refresh_lib_view();
        return;
    }

    // playlist_edit_focus_ == 2: the in-progress playlist's track list
    {
        int total = static_cast<int>(playlist_edit_tracks_.size());
        if (arrow && key == 'A') {
            if (playlist_edit_track_selected_ > 0) --playlist_edit_track_selected_;
            return;
        }
        if (arrow && key == 'B') {
            if (total > 0 && playlist_edit_track_selected_ < total - 1) ++playlist_edit_track_selected_;
            return;
        }
        // DEL, Backspace, or 'd' (lowercase only -- a typed capital C/D
        // used to be intercepted above as the Left/Right arrow and now
        // only reaches here as a genuine arrow, which does nothing).
        if (key == kKeyDelete || key == 127 || key == 8 || key == 'd') { playlist_remove_hovering_track(); return; }
        // 4/5 move the hovering track up/down -- same keys as the main
        // queue's HKeyQueueMoveUp/Down, kept as literal codes (like the
        // rest of this function) rather than routed through
        // resolve_hotkey_action() since this whole handler already
        // works in raw arrow-collapsed key codes, not configurable
        // hotkeys.
        if (key == '4') { playlist_move_hovering_track(-1); return; }
        if (key == '5') { playlist_move_hovering_track(1); return; }
        return;
    }
}

static std::string header_sgr(const Settings& s); // defined further down (Settings panel section)
static std::string legend_sgr(const Settings& s); // likewise
static std::string focus_indicator(const Settings& s, const std::string& border_ansi); // likewise

// SEARCH LIBRARY: the library filter's own boxed pane (title row, input row,
// bottom row), coloured with the Colors tab's SEARCH row like every other
// search bar. The input is live while the LIBRARY pane has focus -- the
// caret/selection shows then, a placeholder when it is empty and idle.
std::vector<std::string> App::build_playlist_search_panel(int total_width) const {
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::vector<std::string> out;
    out.push_back(box_top(std::string("SEARCH LIBRARY") + (playlist_edit_focus_ == 1 ? focus_indicator(settings_, border_ansi) : std::string()), total_width, border_ansi));

    const std::string prefix = "Search: ";
    const int field_w = std::max(1, total_width - 4 - static_cast<int>(prefix.size()));
    std::string field;
    int field_cols = 0;
    if (playlist_edit_focus_ == 1) { // keys go to the box: caret + block cursor
        const bool typed = !playlist_edit_lib_query_.empty();
        EditPaint p = paint_edit_field(playlist_edit_lib_query_, edit_caret_, edit_anchor_, field_w, typed ? search_sgr(settings_) : search_idle_sgr(settings_), true);
        field = search_run(settings_, p.s, typed);
        field_cols = p.cols;
    } else {
        const bool typed = !playlist_edit_lib_query_.empty();
        const std::string plain = truncate_str(typed ? playlist_edit_lib_query_ : std::string("(type to filter)"), field_w);
        field = search_run(settings_, plain, typed);
        field_cols = static_cast<int>(display_width(plain));
    }
    out.push_back(box_line_field(prefix, field, field_cols, total_width, border_ansi, search_idle_sgr(settings_)));
    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_playlist_library_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // The query has its own SEARCH LIBRARY pane above (build_playlist_search_panel());
    // typing still goes to it while this pane has focus, so the focus triangle
    // sits here, where Up/Down and ENTER act.
    out.push_back(box_top(std::string("LIBRARY") + (playlist_edit_focus_ == 1 ? focus_indicator(settings_, border_ansi) : std::string()),
                          total_width, border_ansi));

    // Three equal columns -- Filename | Title | Artist -- separated by
    // " | ", with a header row of their own right under the title row.
    // Filename is the file's stem, Title/Artist come from the resolved
    // tags (row_meta_cache_) with the same folder-name fallback the main
    // list uses for Artist; an untagged Title shows "-". No index column:
    // the header promises exactly these three and each gets a third.
    const std::string col_sep = " | ";
    const int sep_w = static_cast<int>(col_sep.size());
    const int avail = std::max(3, inner - 2 * sep_w);
    const int col1_w = avail / 3;
    const int col2_w = avail / 3;
    const int col3_w = avail - col1_w - col2_w;
    {
        std::string header = pad_right(truncate_str("Filename", col1_w), col1_w) + col_sep
                           + pad_right(truncate_str("Title", col2_w), col2_w) + col_sep
                           + pad_right(truncate_str("Artist", col3_w), col3_w);
        out.push_back(bar + " " + header_sgr(settings_)
                      + pad_right(truncate_str(header, inner), inner) + "\x1b[0m " + bar);
    }

    int total = static_cast<int>(playlist_edit_lib_view_.size());
    int scroll = std::clamp(playlist_edit_lib_selected_ - height / 2, 0, std::max(0, total - height));
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& t = playlist_edit_lib_view_[idx];
            std::string tag_title, tag_artist;
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(path_utf8(t.path));
                if (it != row_meta_cache_.end()) {
                    tag_title = it->second.title;
                    tag_artist = it->second.artist;
                }
            }
            if (tag_title.empty()) tag_title = "-";
            if (tag_artist.empty()) tag_artist = t.folder_artist.empty() ? std::string("-") : t.folder_artist;
            std::string c_file = apply_font_map(t.title, settings_.font_map);
            std::string c_title = apply_font_map(tag_title, settings_.font_map);
            std::string c_artist = apply_font_map(tag_artist, settings_.font_map);
            // Every column padded to its own fixed width *before*
            // concatenating (rather than truncating the assembled whole
            // afterward) -- matches build_list_panel()'s row construction,
            // so one title's display_width() landing a column short can't
            // shift every border to its right. The hovering row's
            // filename scrolls (marquee) when it doesn't fit.
            bool row_focused_sel = (playlist_edit_focus_ == 1) && (idx == playlist_edit_lib_selected_);
            std::string file_shown = row_focused_sel
                ? marquee_or_truncate(c_file, col1_w, idx, marquee_pl_lib_row_idx_, marquee_pl_lib_since_)
                : pad_right(truncate_str(c_file, col1_w), col1_w);
            content = file_shown + col_sep
                    + pad_right(truncate_str(c_title, col2_w), col2_w) + col_sep
                    + pad_right(truncate_str(c_artist, col3_w), col3_w);
        }
        bool sel = (playlist_edit_focus_ == 1) && (idx == playlist_edit_lib_selected_) && idx < total;
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_playlist_tracks_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // When focused, also show WHICH row the cursor is on ("3/12"), so the
    // selection is readable even on a terminal/colour scheme where the
    // highlighted row is hard to see.
    std::string sel_pos;
    if (playlist_edit_focus_ == 2 && !playlist_edit_tracks_.empty()) {
        sel_pos = "  " + std::to_string(playlist_edit_track_selected_ + 1) + "/"
                + std::to_string(playlist_edit_tracks_.size());
    }
    std::string label = "TRACKS (" + std::to_string(playlist_edit_tracks_.size()) + ")" + sel_pos
                       + (playlist_edit_focus_ == 2 ? focus_indicator(settings_, border_ansi) : ""); // filled triangle: focus indicator,
                                                                        // same purpose as LIBRARY's "\u2588" text
                                                                        // cursor but this panel has no text field
                                                                        // of its own to blink a cursor in
    out.push_back(box_top(label, total_width, border_ansi));

    int total = static_cast<int>(playlist_edit_tracks_.size());
    if (total == 0) {
        int mid = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid) {
                std::string text = apply_font_map("ENTER ON A LIBRARY TRACK TO ADD IT", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            // Highlighted even with nothing to select, same cursor-color
            // treatment a real row gets below -- otherwise an empty,
            // focused panel is visually identical to an unfocused one.
            if (playlist_edit_focus_ == 2) {
                std::string cursor_ansi = cursor_sgr(settings_.queue_cursor_color, settings_.queue_cursor_bg_color);
                out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
            } else {
                std::string queue_ansi = ansi_for(settings_.queue_color, false);
                out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
            }
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    // Same three equal columns as the LIBRARY pane above (Filename | Title |
    // Artist, identical widths so they line up under its header) -- but no
    // header row of its own, and no index column.
    const std::string col_sep = " | ";
    const int sep_w = static_cast<int>(col_sep.size());
    const int avail = std::max(3, inner - 2 * sep_w);
    const int col1_w = avail / 3;
    const int col2_w = avail / 3;
    const int col3_w = avail - col1_w - col2_w;

    int scroll = std::clamp(playlist_edit_track_selected_ - height / 2, 0, std::max(0, total - height));
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& t = playlist_edit_tracks_[idx];
            std::string tag_title, tag_artist;
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(path_utf8(t.path));
                if (it != row_meta_cache_.end()) {
                    tag_title = it->second.title;
                    tag_artist = it->second.artist;
                }
            }
            if (tag_title.empty()) tag_title = "-";
            if (tag_artist.empty()) {
                std::string folder = path_utf8(t.path.parent_path().filename());
                tag_artist = folder.empty() ? std::string("-") : folder;
            }
            std::string file_txt = t.missing ? (t.title + " [missing]") : t.title;
            std::string c_file = apply_font_map(file_txt, settings_.font_map);
            std::string c_title = apply_font_map(tag_title, settings_.font_map);
            std::string c_artist = apply_font_map(tag_artist, settings_.font_map);
            bool row_focused_sel = (playlist_edit_focus_ == 2) && (idx == playlist_edit_track_selected_);
            std::string file_shown = row_focused_sel
                ? marquee_or_truncate(c_file, col1_w, idx, marquee_pl_track_row_idx_, marquee_pl_track_since_)
                : pad_right(truncate_str(c_file, col1_w), col1_w);
            content = file_shown + col_sep
                    + pad_right(truncate_str(c_title, col2_w), col2_w) + col_sep
                    + pad_right(truncate_str(c_artist, col3_w), col3_w);
        }
        bool sel = (playlist_edit_focus_ == 2) && (idx == playlist_edit_track_selected_) && idx < total;
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.queue_cursor_color, settings_.queue_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string queue_ansi = ansi_for(settings_.queue_color, false) + bg_ansi_for(settings_.queue_inactive_bg_color);
            out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// SEARCH PLAYLISTS: the saved-playlists filter in a pane of its own, coloured
// like every search bar (query highlighted only as far as it reaches).
std::vector<std::string> App::build_playlist_manage_search_panel(int total_width) const {
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::vector<std::string> out;
    out.push_back(box_top(std::string("SEARCH PLAYLISTS") + (playlist_manage_focus_ == 0 ? focus_indicator(settings_, border_ansi) : std::string()),
                          total_width, border_ansi));
    const std::string prefix = "Search: ";
    std::string field;
    int field_cols = 0;
    const bool typed = !playlist_manage_query_.empty();
    if (playlist_manage_focus_ == 0) { // focus is in the box: caret + block cursor
        EditPaint p = paint_edit_field(playlist_manage_query_, edit_caret_, edit_anchor_,
                                       std::max(1, total_width - 4 - static_cast<int>(prefix.size())),
                                       typed ? search_sgr(settings_) : search_idle_sgr(settings_), true);
        field = search_run(settings_, p.s, typed);
        field_cols = p.cols;
    } else { // focus elsewhere: plain text, with the placeholder hint
        const std::string plain = typed ? playlist_manage_query_ : std::string("(type to filter)");
        field = search_run(settings_, plain, typed);
        field_cols = static_cast<int>(display_width(plain));
    }
    out.push_back(box_line_field(prefix, field, field_cols, total_width, border_ansi, search_idle_sgr(settings_)));
    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_playlist_manage_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;
    // The ◀ marks whichever pane has the keys: this list when the search pane
    // (build_playlist_manage_search_panel(), drawn right above) is not the one.
    out.push_back(box_top(std::string("SAVED PLAYLISTS") + (playlist_manage_focus_ == 1 ? focus_indicator(settings_, border_ansi) : std::string()),
                          total_width, border_ansi));

    const int list_height = std::max(1, height); // the caller already left room for the search pane

    int total = static_cast<int>(playlist_manage_view_.size());
    if (total == 0) {
        int mid = list_height / 2;
        for (int row = 0; row < list_height; ++row) {
            std::string content;
            if (row == mid) {
                std::string text = apply_font_map(playlist_manage_query_.empty() ? "NO SAVED PLAYLISTS YET" : "NO MATCHING PLAYLISTS", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string list_ansi = ansi_for(settings_.list_color, false);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    int scroll = std::clamp(playlist_manage_selected_ - list_height / 2, 0, std::max(0, total - list_height));
    const int idx_w = 3;
    for (int row = 0; row < list_height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& p = playlist_manage_view_[idx];
            const int count_w = 10;
            int name_w = std::max(5, inner - idx_w - 2 - 2 - count_w);
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_name = apply_font_map(p.name, settings_.font_map);
            std::string t_count = std::to_string(p.track_count) + (p.track_count == 1 ? " track" : " tracks");
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_name, name_w), name_w) + settings_.list_separator + " "
                    + pad_right(t_count, count_w);
        }
        bool sel = (idx == playlist_manage_selected_) && idx < total;
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + list_height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// The settings screen's tab strip (two lines) for a pane that carries its own title on the top border:
//   ╭─ TITLE ──────┐  [CURRENT]  ┌──┐  OTHER  ┌───────────...───╮
//   │              └─────────────┘  └─────────┘                 │
// Only the two outer top corners are rounded (the box corner glyphs of the settings); the indents are square.
static std::pair<std::string, std::string> menu_tab_strip(int W, const std::string& title, const std::vector<std::string>& labels,
                                                          int cur, const std::string& top_ansi, const std::string& bot_ansi,
                                                          const std::string& corner_ul, const std::string& corner_ur,
                                                          const std::string& vertical, int min_inner = 0,
                                                          const std::string& line2 = "", int line2_cols = 0,
                                                          const std::string& tab_cur = "", const std::string& tab_oth = "") {
    const std::string R = "\x1b[0m";
    auto rep = [](const std::string& s, int n) { std::string o; for (int i = 0; i < n; ++i) o += s; return o; };
    const std::string Hz = "\u2500";
    const int tw0 = display_width(title);
    const int n0 = static_cast<int>(labels.size());
    int tabs_w = 2;   // exactly one tab carries the two "[ ]" characters
    for (const auto& l : labels) tabs_w += display_width(l) + 4;
    tabs_w += 4 * std::max(0, n0 - 1);
    const int natural = 1 + 1 + tw0 + 1 + std::max(5, 20 - (tw0 + 3));
    // tab names sit at the right end: exactly three "\u2500" remain before the last corner
    const int first_inner = std::max({natural, min_inner, W - 2 - tabs_w - 5});
    const int dashes = first_inner - (1 + 1 + tw0 + 1);
    std::string top = top_ansi + corner_ul + "\u2500 " + title + " " + rep(Hz, dashes) + "\u256e" + R;
    std::string bot = bot_ansi + vertical + R +
                      (line2.empty() ? std::string(static_cast<size_t>(first_inner), ' ')
                                     : line2 + std::string(static_cast<size_t>(std::max(0, first_inner - line2_cols)), ' ')) +
                      bot_ansi + "\u2570" + R;
    int used = first_inner + 2;
    const int n = static_cast<int>(labels.size());
    for (int i = 0; i < n; ++i) {
        const std::string label = i == cur ? "[" + labels[static_cast<size_t>(i)] + "]" : labels[static_cast<size_t>(i)];
        const int tw = display_width(label) + 4;
        top += "  " + (i == cur ? tab_cur : tab_oth) + label + R + "  ";
        bot += bot_ansi + rep(Hz, tw) + R;
        used += tw;
        if (i < n - 1) {
            top += top_ansi + "\u256d\u2500\u2500\u256e" + R;
            bot += bot_ansi + "\u256f  \u2570" + R;
            used += 4;
        } else {
            const int rest = std::max(0, W - used - 2);
            top += top_ansi + "\u256d" + rep(Hz, rest) + corner_ur + R;
            bot += bot_ansi + "\u256f" + R + std::string(static_cast<size_t>(rest), ' ') + bot_ansi + vertical + R;
        }
    }
    return {top, bot};
}

// Assembles the full-screen playlist editor overlay. Structured like the
// Browse view's own stack of boxed panels (a header box, then side-by-
// side boxed sub-panels, then a plain hint/status line) rather than
// Settings' absolute-positioned single mega-box -- same box-drawing
// vocabulary (box_top/box_line/box_bottom), simpler composition.
void App::build_playlist_screen(std::ostringstream& frame, int W, int target_height) const {
    if (W < 60) W = 60;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string HI = "\x1b[7m", R = "\x1b[0m";

    int fixed_rows = 3; // the two strip lines + bottom border
    {   // title on the border line + the settings-style tab strip (no numbers); the pane below stays separate.
        // On the CREATE / EDIT tab the name field sits on the strip's second line, left of the tab indicators
        // (which are pushed to the right so the 25 characters of a name always fit).
        const int name_max = 25;
        const std::string prefix = " Name: ";
        const std::string dirty_mark = playlist_edit_dirty_ ? " *" : "";
        const int mark_w = static_cast<int>(dirty_mark.size());
        const int field_w = name_max + 1;                       // 25 characters + the caret block
        const int inner_w = static_cast<int>(prefix.size()) + field_w + 2 + 2 + 1;   // room for " *" and the " â" focus mark always: the strip never jumps
        std::string line2;
        int line2_cols = 0;
        if (playlist_tab_ == 0) {
            std::string field;
            int field_cols = 0;
            if (playlist_edit_focus_ == 0) {
                EditPaint p = paint_edit_field(playlist_edit_name_, edit_caret_, edit_anchor_, field_w, "", true);
                field = p.s;
                field_cols = p.cols;
            } else {
                field = truncate_str(playlist_edit_name_.empty() ? std::string("(untitled)") : playlist_edit_name_, field_w);
                field_cols = static_cast<int>(display_width(field));
            }
            std::string focus_mark;
            int focus_w = 0;
            if (playlist_edit_focus_ == 0) { focus_mark = " " + focus_sgr(settings_) + "\xe2\x97\x80" + "\x1b[0m"; focus_w = 2; }
            line2 = prefix + field + dirty_mark + "\x1b[0m" + focus_mark;
            line2_cols = static_cast<int>(prefix.size()) + field_cols + mark_w + focus_w;
        }
        const auto strip = menu_tab_strip(W, "PLAYLISTS", {"CREATE / EDIT", "SAVED PLAYLISTS"}, playlist_tab_, border, border,
                                          settings_.box_upper_left, settings_.box_upper_right, settings_.box_vertical,
                                          inner_w, line2, line2_cols,
                                          ansi_for(settings_.tab_current_color, false), ansi_for(settings_.tab_other_color, false));
        frame << strip.first << "\n" << strip.second << "\n";
    }
    frame << box_bottom(W, "", border_bottom) << "\n";

    // Sized to the room the terminal has (via target_height) with an 8-row
    // floor on short terminals. It used to be capped at 22 rows, which left
    // blank lines under the menu once the window was maximised.
    // Sized against BOTH bounds that matter (same reasoning as
    // build_meta_screen()): target_height is player_view_height(), but
    // clamp_output_rows() keeps term_rows_ - 1 lines and a full-width
    // prompt here can wrap, so never lay out taller than the screen.
    int budget = std::min(target_height, term_rows_ - 1);
    // -5: the panel box's own top+bottom border rows (added by
    // build_playlist_*_panel(), which draw a box around the panel_h rows
    // they're handed) plus the TWO legend lines + status line below it. The
    // old "-2" forgot the border rows, which made the frame 1-2 lines too
    // tall and chopped the green status line -- "removed ...", "deleted ...",
    // "added ..." -- off every single frame. Keeping that sum exact is also
    // what stops the second legend row (the text-field keys) from pushing
    // the frame past term_rows_ - 1 and scrolling the terminal.
    int panel_h = std::max(8, budget - fixed_rows - 5); // no upper cap: fills a maximised window
    if (playlist_tab_ == 0) {
        // Stacked, full width: playlist box (above), LIBRARY, then TRACKS.
        // The two panes together take exactly the lines the old side-by-
        // side pair did (panel_h rows + 2 borders), so the whole screen
        // keeps its height. LIBRARY spends 3 lines on chrome (top border,
        // Filename|Title|Artist header, bottom border), TRACKS 2, which
        // leaves panel_h - 3 data rows to split roughly 1:3.
        // The SEARCH LIBRARY pane above them costs 3 more lines (top border,
        // input row, bottom border), which come out of the TRACKS pane.
        int data_rows = std::max(5, panel_h - 6);
        int lib_rows = std::max(2, data_rows / 4);
        int track_rows = data_rows - lib_rows;
        for (auto& l : build_playlist_search_panel(W)) frame << l << "\n";
        for (auto& l : build_playlist_library_panel(W, lib_rows)) frame << l << "\n";
        for (auto& l : build_playlist_tracks_panel(W, track_rows)) frame << l << "\n";
    } else {
        // The SEARCH PLAYLISTS pane takes 3 lines (it used to be one row inside this pane), so the list
        // gives up two rows net and the screen keeps its height.
        for (auto& l : build_playlist_manage_search_panel(W)) frame << l << "\n";
        for (auto& l : build_playlist_manage_panel(W, panel_h - 3)) frame << l << "\n";
    }

    // Footer: a full key legend (gray, "\x1b[90m") plus a status line
    // (green, "\x1b[32m") below it -- exactly the colors and layout
    // Settings' own footer uses (see build_settings_screen()'s
    // "[TAB] Switch | ... " line and its status_line_ line just below).
    if (playlist_confirm_exit_) {
        std::string prompt = "Save changes to \"" + (playlist_edit_name_.empty() ? std::string("(untitled)") : playlist_edit_name_)
                            + "\" before exiting?   [Y]es   [N]o   [ESC] cancel";
        frame << "\x1b[43;30m " << prompt << " \x1b[0m\n";
        frame << "\n";
    } else if (playlist_confirm_delete_) {
        std::string name = (playlist_manage_selected_ >= 0
                          && playlist_manage_selected_ < static_cast<int>(playlist_manage_view_.size()))
                          ? playlist_manage_view_[playlist_manage_selected_].name : std::string();
        std::string prompt = "Delete playlist \"" + name + "\"? This can't be undone.   [Y]es   [N]o   [ESC] cancel";
        frame << "\x1b[41;97m " << prompt << " \x1b[0m\n";
        frame << "\n";
    } else {
        // Left/Right and ESC say what they do RIGHT NOW (see
        // handle_playlist_key()): in a text box that was typed into they move
        // the caret / leave the box, everywhere else they switch the tab /
        // close the editor.
        const char* field = playlist_tab_ == 1 ? (playlist_manage_focus_ == 0 ? "playlist-manage-search" : "")
                          : playlist_edit_focus_ == 0 ? "playlist-name"
                          : playlist_edit_focus_ == 1 ? "playlist-lib-search" : "";
        const bool in_text = *field && text_engaged(field);
        std::string hint = std::string(in_text ? "[\u2190\u2192] Cursor" : "[\u2190\u2192] Switch Tab")
                         + (playlist_tab_ == 1
                            ? " | [TAB] Focus | [\u2191\u2193] Navi. | [ENTER] Load | [DEL] Delete | [e/i] Export/Import"
                            : " | [TAB] Focus | [\u2191\u2193] Navi. | [ENTER] Add/Load | "
                              "[DEL] Remove | [4/5] Move \u2191\u2193 | [CTRL+s] Save");
        frame << legend_sgr(settings_) << hint << "\x1b[0m\n";
        // Row 2: the text-field keys, plus Exit. Kept off row 1 so each row
        // fits comfortably inside a 120-column terminal without wrapping
        // (the meta editor splits its legend the same way) -- panel_h's
        // budget above already accounts for exactly these two rows. [ESC]
        // Exit used to sit at the end of row 1, but that pushed row 1 past
        // the wrap width and cost a spurious third line; it lives here now.
        frame << legend_sgr(settings_) << "[SHIFT+←→] Mark | [Ctrl+C/X/V] Copy/Cut/Paste | "
              << (in_text ? "[ESC] Leave field" : "[ESC] Exit") << "\x1b[0m\n";
        if (!playlist_status_.empty()) frame << "\x1b[32m" << playlist_status_ << "\x1b[0m\n";
        else frame << "\n";
    }

}

// ---------------------------------------------------------------------
// Settings panel
// ---------------------------------------------------------------------

// main_frame_height() used to live here -- it estimated the Browse view's
// total line count (from the fixed kListVisibleRows constant, among
// other things) so the Settings/Console/Cheatsheet/BulkAdd overlays
// could size themselves to "roughly the same height as the player
// view". That was never actually tied to the real terminal size, which
// is exactly what let all of those overlays overflow a short terminal
// and scroll-duplicate just like the Browse view did (see term_rows_'s
// comment in app.h). Every caller now sizes directly off term_rows_
// (the real, current ioctl-reported row count) instead, so this
// function no longer has a reason to exist.

// SGR prefix for the section titles drawn by the Settings panel itself:
// bold plus settings_.header_color -- the Colors tab's HEADER row, a
// palette index of the 256 by default (10, which is exactly the color
// the REFERENCE tab's category titles have always been drawn in, and
// what the PATHS tab's LOCAL PATH / DOWNLOAD PATH / PLAYLIST PATH titles borrow). An
// explicit 0/empty means "no color" here like it does everywhere else,
// which degrades these to plain bold rather than to a stray escape.
static std::string header_sgr(const Settings& s) {
    std::string params = sgr_params_for(s.header_color);
    return params.empty() ? "\x1b[1m" : "\x1b[1;" + params + "m";
}

// The small "◀" that marks which pane/field within a panel has the
// keyboard focus. box_top() wraps the whole title string in `border_ansi`
// (one colour for the whole row), so to make just the triangle read in the
// header colour, the title text itself must switch colour for it and then
// switch back to `border_ansi` -- the same colour box_top() already has --
// so the dashes that follow the title stay in the border colour.

// SGR prefix for the key command legends (the "[ESC] close | [ENTER] ..."
// hint lines of the Settings panel, the big list / queue overlays and the
// playlist / meta / history screens): settings_.legend_color, the Colors
// tab's LEGEND row. The default "8" yields "\x1b[38;5;8m", the grey these lines
// were always drawn in. An explicit 0/empty means "no color" like everywhere
// else, i.e. the terminal's own text color -- an empty prefix, not a stray
// escape. Callers close the run with "\x1b[0m" themselves.
static std::string legend_sgr(const Settings& s) {
    return ansi_for(s.legend_color, false);
}

// SGR prefix for "this field is actively being typed into" -- every
// in-place text edit across the Settings panel (a path row, a hex colour
// value, a hotkey) and, now, the meta editor's tag fields. The Colors
// tab's CHANGE row (settings_.change_text_color / change_bg_color);
// defaults to "7"/"1" (palette white on red) -- the same white-on-red
// this used to be hardcoded as everywhere it appears, before it had a
// name of its own in the Colors tab.
static std::string change_sgr(const Settings& s) {
    return ansi_for(s.change_text_color, false) + bg_ansi_for(s.change_bg_color);
}

// ---------------------------------------------------------------------
// Meta/tag editor overlay (Mode::MetaEdit, HKeyMetaEditor = Shift+M)
//
// A second full-screen overlay modelled on the playlist editor further up --
// same tab strip, same boxed side-by-side panels, same hint/status footer,
// same Y/N prompts in place of the hint line -- but instead of building a
// playlist it edits the FILES themselves: their names, and their
// artist/title/album/year tags.
//
// Three rules shape the whole thing:
//
//  1. Edits are session state, never a side effect. Everything typed or
//     fetched goes into meta_session_ and is written to an autosave backup
//     (meta_editor.h, under ~/.cache/mousiki/meta_session/) after every
//     single change. The audio files stay untouched until Ctrl+Shift+S
//     explicitly applies the session.
//  2. Leaving is always safe. ESC, quitting, crashing between two edits --
//     none of that loses work: the backup is reloaded next run by
//     meta_ensure_session_loaded(), and it is only ever deleted when the
//     session is applied or explicitly discarded with Ctrl+Shift+X.
//  3. What changed stays visible. A field that was touched -- typed OR
//     filled in by AcoustID -- is drawn in header_sgr(), the same bold
//     header colour the Settings section titles use, so "what will
//     Ctrl+Shift+S write?" is answerable at a glance.
// ---------------------------------------------------------------------

// The AcoustID disclaimer shown by the fetch confirmation (in the status line in Browse, in the footer of the menu).
// It says what this particular fetch will do with tags that already have a value.
std::string App::meta_fetch_disclaimer() const {
    const std::string head = "Fetching meta data via AcoustID and MusicBrainz; not always accurate. ";
    if (meta_prompt_opts_ & 1u) return head + "Only empty tags are filled in; existing tags stay as they are. Continue?";
    return head + "Existing " + (meta_prompt_opts_ & 2u ? std::string("tags") : meta_fetch_opt_summary(meta_prompt_opts_) + " tags")
         + " are overwritten. Continue?";
}

void App::meta_open() {
    meta_status_.clear();          // before the loader below, which may set it
    meta_prompt_ = MetaPrompt::None;
    meta_prompt_paths_.clear();
    meta_ensure_session_loaded();  // reopening never loses what's already pending
    mode_ = Mode::MetaEdit;
    text_disengage();
    meta_tab_ = 0;
    meta_focus_ = 0;
    meta_query_.clear();
    meta_filter_ = 0; // a fresh open shows the whole library again
    meta_lib_selected_ = 0;
    meta_field_ = 0;
    meta_field_editing_ = false;
    meta_fetch_selected_ = 0;
    meta_refresh_lib_view();
}

// Loads the autosaved backup exactly once per run. Called from meta_open()
// AND from every session-touching action, because Shift+B reaches the same
// session from Browse mode without ever opening the menu.
void App::meta_ensure_session_loaded() {
    if (meta_session_loaded_) return;
    meta_session_loaded_ = true;
    std::vector<MetaEditEntry> entries;
    std::vector<std::string> fetch_list;
    unsigned fetch_opts = meta_fetch_opts_;
    if (load_meta_session(entries, fetch_list, fetch_opts)) {
        meta_session_ = std::move(entries);
        meta_fetch_list_ = std::move(fetch_list);
        meta_fetch_opts_ = fetch_opts;
        meta_status_ = "restored autosaved session: " + std::to_string(meta_session_.size())
                     + " file" + (meta_session_.size() == 1 ? "" : "s") + " with pending edits";
        log_event("restored meta edit session (" + std::to_string(meta_session_.size()) + " files)");
    }
    meta_refresh_lib_view();
}

void App::meta_refresh_lib_view() {
    // With the 'r' resort on, this rebuild re-orders the pane underneath the
    // cursor, so remember the hovered row by PATH first: clamping the old
    // INDEX would silently point the picker -- and with it the fields panel,
    // which is what the user is editing -- at a different track.
    std::string keep;
    if (meta_resort_edited_ && !meta_lib_view_.empty()) keep = meta_hovering_path();

    meta_lib_view_ = filter_and_rank_local(meta_query_);

    // Missing-tag filter ('x' / Shift+T/A/Y). meta_row_meta() is a native,
    // in-process tag read cached in row_meta_cache_, so the first pass costs
    // one header parse per file and everything after that is a lookup --
    // no ffprobe, no subprocess. A pending session edit counts as the value
    // the file WOULD have, so filtering right after typing (or deleting) a
    // title reflects what is on screen instead of what is on disk.
    if (meta_filter_ != 0) {
        auto value_of = [this](const LocalTrack& t, int field) -> std::string {
            const std::string p = path_utf8(t.path);
            const MetaEditEntry* e = meta_entry(p);
            if (e && e->edited[field] && !e->value[field].empty()) return e->value[field];
            RowMeta rm = meta_row_meta(t.path);
            switch (field) {
                case 1: return rm.artist;
                case 2: return rm.title;
                case 3: return rm.album;
                case 4: return rm.year;
                default: return t.title;
            }
        };
        auto matches = [&](const LocalTrack& t) {
            if (meta_filter_ == 1) { // "no meta data" = every tag field empty
                for (int f = 1; f < kMetaFieldCount; ++f)
                    if (!value_of(t, f).empty()) return false;
                return true;
            }
            const int field = (meta_filter_ == 3) ? 1   // ARTIST
                            : (meta_filter_ == 2) ? 2   // TITLE
                            : (meta_filter_ == 5) ? 3   // ALBUM
                            : 4;                        // YEAR
            return value_of(t, field).empty();
        };
        meta_lib_view_.erase(std::remove_if(meta_lib_view_.begin(), meta_lib_view_.end(),
                                            [&](const LocalTrack& t) { return !matches(t); }),
                             meta_lib_view_.end());
    }

    if (meta_resort_edited_) {
        // Strict "edited before not edited" on a stable sort is a stable
        // partition: every file with a pending edit moves into one block at
        // the top, and both blocks keep the exact order the pane already had
        // -- the normal alphabetical/scan order, or the fuzzy ranking while
        // a search is active (so a query's relevance order is never thrown
        // away). Toggling 'r' off simply rebuilds without this step, i.e.
        // straight back to that order.
        std::stable_sort(meta_lib_view_.begin(), meta_lib_view_.end(),
                         [this](const LocalTrack& a, const LocalTrack& b) {
                             auto edited = [this](const LocalTrack& t) {
                                 const MetaEditEntry* e = meta_entry(path_utf8(t.path));
                                 return e && e->any_edited();
                             };
                             return edited(a) && !edited(b);
                         });
        if (!keep.empty()) {
            for (size_t i = 0; i < meta_lib_view_.size(); ++i) {
                if (path_utf8(meta_lib_view_[i].path) == keep) {
                    meta_lib_selected_ = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    meta_lib_selected_ = std::clamp(meta_lib_selected_, 0,
        std::max(0, static_cast<int>(meta_lib_view_.size()) - 1));
    meta_fetch_selected_ = std::clamp(meta_fetch_selected_, 0,
        std::max(0, static_cast<int>(meta_fetch_list_.size()) - 1));
}

// 'r': flip the library pane between its normal order and "every file with
// pending edits on top". Nothing else about the list changes -- same rows,
// same filter, same selection (meta_refresh_lib_view() follows it by path).
void App::meta_toggle_resort() {
    meta_resort_edited_ = !meta_resort_edited_;
    meta_refresh_lib_view();
    if (meta_resort_edited_) {
        int n = 0;
        for (const auto& e : meta_session_) if (e.any_edited()) ++n;
        meta_status_ = "library resorted: " + std::to_string(n) + " edited file"
                     + (n == 1 ? "" : "s") + " on top";
    } else {
        meta_status_ = "library order: alphabetical again";
    }
}

const char* App::meta_filter_label() const {
    switch (meta_filter_) {
        case 1: return "NO META DATA";
        case 2: return "MISSING TITLE";
        case 3: return "MISSING ARTIST";
        case 4: return "MISSING YEAR";
        case 5: return "MISSING ALBUM";
        default: return "";
    }
}

// 'x' / Shift+T / Shift+A / Shift+Y: narrow the library pane to files a
// given tag is (or, for 'x', every tag is) missing. The same key clears it,
// which is what keeps four keys enough for five states, and the status line
// always says which filter is up and how much of the library survived it --
// the pane's title carries the label too, so an active filter is never
// something you have to remember.
void App::meta_toggle_filter(int filter) {
    meta_filter_ = (meta_filter_ == filter) ? 0 : filter;
    meta_refresh_lib_view();
    const std::string shown = std::to_string(meta_lib_view_.size());
    if (meta_filter_ == 0) meta_status_ = "filter cleared - " + shown + " files";
    else meta_status_ = std::string("filter: ") + meta_filter_label() + " - " + shown + " files (press again to clear)";
}

// The autosave: save whenever there is something to restore, remove the file
// when there isn't. Deliberately NOT tied to any "was it applied" flag --
// leaving the app always ends with whatever the user last edited still on
// disk as a backup, which is the whole point of the session file.
void App::meta_persist() {
    if (!meta_session_loaded_) return;
    if (meta_session_.empty() && meta_fetch_list_.empty()) delete_meta_session();
    else save_meta_session(meta_session_, meta_fetch_list_, meta_fetch_opts_);
}

const MetaEditEntry* App::meta_entry(const std::string& path) const {
    for (const auto& e : meta_session_) if (e.path == path) return &e;
    return nullptr;
}

MetaEditEntry& App::meta_touch_entry(const std::string& path) {
    for (auto& e : meta_session_) if (e.path == path) return e;
    MetaEditEntry e;
    e.path = path;
    meta_session_.push_back(std::move(e));
    return meta_session_.back();
}

void App::meta_set_field(const std::string& path, int field, const std::string& value) {
    meta_ensure_session_loaded();
    MetaEditEntry& e = meta_touch_entry(path);
    e.edited[field] = true;
    e.value[field] = value;
    e.fetched[field] = false;           // typed over: it is the user's own value now, not a fetched suggestion
    e.choices[field].clear();
    e.choice_aux[field].clear();
    meta_persist(); // autosave the backup after every keystroke -- it's a ~2KB file
}

// Current tags of one file for the editor. Uses the same cache the main list
// uses, and the same in-process header parser as a render-thread-safe first
// pass (no subprocess on the frame loop -- see ensure_visible_row_meta()'s
// comment for why a stray ffprobe here used to freeze the whole UI). Files
// that parser declines are finished off by the background sweep's ffprobe
// pass, which now also reads the date tag, so YEAR arrives there too.
RowMeta App::meta_row_meta(const fs::path& path) {
    std::string key = path_utf8(path);
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        auto it = row_meta_cache_.find(key);
        if (it != row_meta_cache_.end() && it->second.tags_resolved) return it->second;
    }
    RowMeta rm = try_native_row_meta(path);
    bool usable = (rm.duration_sec > 0 || rm.tags_resolved);
    bool promote = usable;
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        auto it = row_meta_cache_.find(key);
        if (it != row_meta_cache_.end()) {
            if (it->second.tags_resolved) { promote = false; rm = it->second; } // a better answer is already cached
            else if (!usable) { rm = it->second; }
        }
        if (promote) row_meta_cache_[key] = rm;
    }
    if (promote && rm.tags_resolved) row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
    return rm;
}

// SHIFT+p in the library pane / fetch list: start the hovered track without
// leaving the editor, to check by ear which file is which. Uses the same
// loader a Browse-list ENTER does; the editor stays open and nothing pending
// is touched.
void App::meta_play_hovering() {
    const std::string p = meta_hovering_path();
    if (p.empty()) { meta_status_ = "no track selected"; return; }
    LocalTrack t;
    bool found = false;
    for (const auto& lt : all_local_tracks_) {
        if (path_utf8(lt.path) == p) { t = lt; found = true; break; }
    }
    if (!found) { // a fetch-list entry that is no longer in the scanned library
        t.path = path_from_utf8(p);
        t.title = path_utf8(t.path.stem());
        t.folder_artist = path_utf8(t.path.parent_path().filename());
    }
    start_local_track(t);
    meta_status_ = "playing: " + t.title;
}

std::string App::meta_hovering_path() const {
    if (meta_tab_ == 1) { // the fetch list's rows ARE the titles
        if (meta_fetch_list_.empty()) return {};
        int i = std::clamp(meta_fetch_selected_, 0, static_cast<int>(meta_fetch_list_.size()) - 1);
        return meta_fetch_list_[i];
    }
    if (meta_lib_view_.empty()) return {};
    int i = std::clamp(meta_lib_selected_, 0, static_cast<int>(meta_lib_view_.size()) - 1);
    return path_utf8(meta_lib_view_[i].path);
}

// What the field editor shows for one field: the pending value if the field
// has been edited (that's what the highlight keys off), otherwise whatever
// the file already carries.
std::string App::meta_display_value(const std::string& path, int field, const MetaEditEntry* e) {
    if (e && e->edited[field]) return e->value[field];
    if (field == static_cast<int>(MetaField::FileName)) {
        // The STEM, not the whole name: typing is meant to feel like editing
        // "Alpha Song" rather than "Alpha Song.mp3", and apply_meta_entry()
        // puts the original extension back on (meta_editor.cpp's
        // normalize_file_name()), so a rename can never change the format.
        return path_utf8(path_from_utf8(path).stem());
    }
    RowMeta rm = meta_row_meta(path_from_utf8(path));
    switch (static_cast<MetaField>(field)) {
        case MetaField::Artist: return rm.artist;
        case MetaField::Title:  return rm.title;
        case MetaField::Album:  return rm.album;
        case MetaField::Year:   return rm.year;
        default: return {};
    }
}

// Precomputes the five displayed values for whatever row is hovered, once
// per frame, because resolving them needs the mutable tag cache while
// build_meta_screen() itself is const. Called from render_frame().
void App::meta_refresh_hover_values() {
    meta_hover_values_.fill(std::string());
    meta_hover_resolved_ = false;
    if (mode_ != Mode::MetaEdit) return;
    std::string path = meta_hovering_path();
    if (path.empty()) return;
    const MetaEditEntry* e = meta_entry(path);
    meta_hover_resolved_ = meta_row_meta(path_from_utf8(path)).tags_resolved;
    for (int i = 0; i < kMetaFieldCount; ++i) meta_hover_values_[i] = meta_display_value(path, i, e);
}

// SHIFT+r in the main UI and in the meta data editor.
void App::rescan_now() {
    const size_t before = all_local_tracks_.size();
    rescan_library();
    if (mode_ == Mode::MetaEdit) meta_refresh_lib_view();
    const long d = static_cast<long>(all_local_tracks_.size()) - static_cast<long>(before);
    std::string msg = "library rescanned: " + std::to_string(all_local_tracks_.size()) + " tracks";
    if (d > 0) msg += " (+" + std::to_string(d) + ")"; else if (d < 0) msg += " (" + std::to_string(d) + ")";
    if (mode_ == Mode::MetaEdit) meta_status_ = msg; else status_line_ = msg;
}

// --- the choice overlay -----------------------------------------------------------------------------------------
void App::meta_choice_open() {
    const std::string path = meta_hovering_path();
    const MetaEditEntry* e = meta_entry(path);
    if (!e || meta_field_ < 0 || meta_field_ >= kMetaFieldCount || !e->needs_choice(meta_field_)) return;
    meta_choice_open_ = true;
    meta_choice_field_ = meta_field_;
    meta_choice_sel_ = 0;
    for (size_t k = 0; k < e->choices[meta_field_].size(); ++k) {
        if (e->choices[meta_field_][k] == e->value[meta_field_]) { meta_choice_sel_ = static_cast<int>(k); break; }
    }
}

void App::meta_choice_pick() {
    const std::string path = meta_hovering_path();
    const MetaEditEntry* ce = meta_entry(path);
    const int f = meta_choice_field_;
    if (!ce || f < 1 || f >= kMetaFieldCount || meta_choice_sel_ < 0 ||
        meta_choice_sel_ >= static_cast<int>(ce->choices[f].size())) { meta_choice_open_ = false; return; }
    MetaEditEntry& e = meta_touch_entry(path);
    const size_t k = static_cast<size_t>(meta_choice_sel_);
    e.value[f] = e.choices[f][k];
    e.edited[f] = true;
    // An album carries its year: while the year is still a fetched suggestion, it follows the album that was picked.
    const int ya = static_cast<int>(MetaField::Year);
    if (f == static_cast<int>(MetaField::Album) && k < e.choice_aux[f].size() && !e.choice_aux[f][k].empty() &&
        e.fetched[ya] && e.edited[ya]) {
        e.value[ya] = e.choice_aux[f][k];
        e.choices[ya].clear();
        e.choice_aux[ya].clear();
        e.fetched[ya] = false;
    }
    e.choices[f].clear();
    e.choice_aux[f].clear();
    e.fetched[f] = false;             // decided: from now on it is drawn like any confirmed edit
    meta_choice_open_ = false;
    meta_persist();
    meta_refresh_lib_view();
    meta_status_ = std::string(meta_field_label(f)) + ": " + e.value[f];
}

void App::meta_choice_discard() {
    const std::string path = meta_hovering_path();
    const int f = meta_choice_field_;
    meta_choice_open_ = false;
    if (f < 1 || f >= kMetaFieldCount) return;
    for (size_t i = 0; i < meta_session_.size(); ++i) {
        if (meta_session_[i].path != path) continue;
        meta_session_[i].clear_field(f);      // back to what the file has
        if (!meta_session_[i].any_edited()) meta_session_.erase(meta_session_.begin() + static_cast<long>(i));
        break;
    }
    meta_persist();
    meta_refresh_lib_view();
    meta_status_ = std::string("discarded the fetched ") + meta_field_label(f) + " and its options";
}

bool App::handle_meta_choice_key(int key) {
    if (!meta_choice_open_) return false;
    const MetaEditEntry* e = meta_entry(meta_hovering_path());
    const int n = (e && meta_choice_field_ >= 0 && meta_choice_field_ < kMetaFieldCount)
                      ? static_cast<int>(e->choices[meta_choice_field_].size()) : 0;
    if (n == 0) { meta_choice_open_ = false; return true; }
    const bool arrow = last_key_was_arrow();
    if (key == 27) { meta_choice_open_ = false; return true; }
    if (arrow && key == 'A') { if (meta_choice_sel_ > 0) --meta_choice_sel_; return true; }
    if (arrow && key == 'B') { if (meta_choice_sel_ + 1 < n) ++meta_choice_sel_; return true; }
    if (key == '\r' || key == '\n') { meta_choice_pick(); return true; }
    if (key == kKeyDelete || key == 127 || key == 8 || (key == 'd' && !arrow)) { meta_choice_discard(); return true; }
    return true;   // swallow everything else while the overlay is up
}

std::vector<std::string> App::build_meta_choice_panel() const {
    const int W = kMetaChoicePanelWidth;
    const int inner = W - 2;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    const MetaEditEntry* e = meta_entry(meta_hovering_path());
    std::vector<std::string> lines;
    const int f = std::clamp(meta_choice_field_, 0, kMetaFieldCount - 1);
    const int n = e ? static_cast<int>(e->choices[f].size()) : 0;
    lines.push_back(box_top(std::string("CHOOSE ") + meta_field_label(f) + " (" + std::to_string(n) + " found)", W, border));
    auto text_row = [&](const std::string& sgr, const std::string& text) {
        return obar + sgr + pad_right(truncate_str(" " + text, inner), inner) + "\x1b[0m" + obar;
    };
    lines.push_back(text_row(legend_sgr(settings_), "The first one is in the field now -- pick the right one:"));
    const int shown = std::max(1, std::min(n, 8));
    const int first = std::clamp(meta_choice_sel_ - shown / 2, 0, std::max(0, n - shown));
    for (int r = 0; r < shown; ++r) {
        const int k = first + r;
        if (k >= n || !e) { lines.push_back(obar + std::string(inner, ' ') + obar); continue; }
        std::string value = e->choices[f][static_cast<size_t>(k)];
        std::string aux = (static_cast<size_t>(k) < e->choice_aux[f].size()) ? e->choice_aux[f][static_cast<size_t>(k)] : std::string();
        const bool current = (e->value[f] == value);
        const std::string mark = current ? " (o) " : " ( ) ";
        std::string row;
        if (aux.empty()) {
            row = pad_right(truncate_str(mark + value, inner - 1), inner - 1) + " ";
        } else {
            const int aux_w = static_cast<int>(aux.size()) + 2;
            row = pad_right(truncate_str(mark + value, inner - 1 - aux_w), inner - 1 - aux_w) + aux + "  " + " ";
        }
        if (k == meta_choice_sel_) row = "\x1b[7m" + row + "\x1b[0m";
        lines.push_back(obar + row + obar);
    }
    if (n > shown) lines.push_back(text_row(legend_sgr(settings_), "( " + std::to_string(n - shown) + " more -- keep going with the arrow keys )"));
    lines.push_back(box_bottom(W, "[\u2191\u2193] select  [ENTER] choose  [DEL/d] discard all  [ESC] close", border));
    return lines;
}

void App::meta_add_hovering_to_fetch() {
    meta_ensure_session_loaded();
    std::string path = meta_hovering_path();
    if (path.empty()) { meta_status_ = "no title selected"; return; }
    for (const auto& p : meta_fetch_list_) {
        if (p == path) {
            meta_status_ = "already on the fetch list (" + std::to_string(meta_fetch_list_.size()) + " queued)";
            return;
        }
    }
    meta_fetch_list_.push_back(path);
    meta_fetch_selected_ = static_cast<int>(meta_fetch_list_.size()) - 1;
    meta_persist();
    meta_status_ = "added to fetch list (" + std::to_string(meta_fetch_list_.size()) + " queued)";
}

// SHIFT+f in the library pane: puts every title that pane shows right now -- so the search AND the missing-tag filter
// (x, SHIFT+t/a/l/y) are applied -- on the fetch list. Titles already on it are skipped, the order of the pane is kept.
void App::meta_add_view_to_fetch() {
    meta_ensure_session_loaded();
    if (meta_lib_view_.empty()) { meta_status_ = "nothing to add -- the library pane is empty"; return; }
    std::unordered_set<std::string> queued(meta_fetch_list_.begin(), meta_fetch_list_.end());
    size_t added = 0;
    for (const auto& t : meta_lib_view_) {
        std::string path = path_utf8(t.path);
        if (queued.insert(path).second) { meta_fetch_list_.push_back(std::move(path)); ++added; }
    }
    if (added == 0) {
        meta_status_ = "all " + std::to_string(meta_lib_view_.size()) + " titles are on the fetch list already (" + std::to_string(meta_fetch_list_.size()) + " queued)";
        return;
    }
    meta_persist();
    meta_status_ = "added " + std::to_string(added) + (added == 1 ? " title" : " titles") + " to the fetch list"
                 + (added < meta_lib_view_.size() ? " (" + std::to_string(meta_lib_view_.size() - added) + " already on it)" : std::string())
                 + " -- " + std::to_string(meta_fetch_list_.size()) + " queued";
}

// DEL / 'd' -- same "unqueue" gesture as the main list's removal key.
void App::meta_remove_hovering() {
    meta_ensure_session_loaded();
    std::string path = meta_hovering_path();
    if (path.empty()) return;
    for (size_t i = 0; i < meta_fetch_list_.size(); ++i) {
        if (meta_fetch_list_[i] == path) {
            meta_fetch_list_.erase(meta_fetch_list_.begin() + static_cast<long>(i));
            meta_refresh_lib_view();
            meta_persist();
            meta_status_ = "removed from fetch list (" + std::to_string(meta_fetch_list_.size()) + " queued)";
            return;
        }
    }
    meta_status_ = "not on the fetch list";
}

void App::meta_prompt_single_fetch(const std::string& path, bool all) {
    if (path.empty()) return;
    meta_ensure_session_loaded();
    meta_prompt_ = MetaPrompt::Fetch;
    meta_prompt_opts_ = all ? 2u : 1u;   // SHIFT+b: only the missing tags; SHIFT+n: all tags (the boxes do not apply)
    meta_prompt_paths_.assign(1, path);
    // In the menu the footer renders the disclaimer; in Browse there is no
    // footer, so it goes through the status line instead.
    if (mode_ == Mode::MetaEdit) meta_status_.clear();
    else status_line_ = meta_fetch_disclaimer();
}

void App::meta_prompt_list_fetch() {
    meta_ensure_session_loaded();
    if (meta_fetch_list_.empty()) {
        meta_status_ = "fetch list is empty -- press 'a' on a track to queue it";
        return;
    }
    meta_prompt_ = MetaPrompt::Fetch;
    meta_prompt_opts_ = meta_fetch_opts_;
    meta_prompt_paths_ = meta_fetch_list_;
}

// The tag fields (1..4) a fetch with the ticked FETCH options `opts` may fill in for `path`. "missing only" asks for
// exactly the tags that are empty right now -- a pending edit counts as the value the file will have -- while "all" and
// the single tags ask regardless of what is there (those are the only ways an existing tag gets overwritten).
std::array<bool, kMetaFieldCount> App::meta_want_for(const std::string& path, unsigned opts) {
    std::array<bool, kMetaFieldCount> want{};
    const MetaEditEntry* e = meta_entry(path);
    for (int i = 1; i < kMetaFieldCount; ++i) {
        if (opts & 2u) want[i] = true;                                        // all
        else if (opts & (1u << (i + 1))) want[i] = true;                      // artist .. year ticked (bits 2..5)
        else if ((opts & 1u) && !(opts & 0x3Cu)) want[i] = meta_display_value(path, i, e).empty();   // missing only
    }
    return want;
}

// Ticks / unticks FETCH option i (the rules live in meta_fetch_opt_toggle()).
void App::meta_toggle_fetch_opt(int i) {
    const unsigned before = meta_fetch_opts_;
    meta_fetch_opts_ = meta_fetch_opt_toggle(meta_fetch_opts_, i);
    meta_persist();
    if (meta_fetch_opts_ == before) { meta_status_ = "at least one fetch option has to stay ticked"; return; }
    const bool overwrites = !(meta_fetch_opts_ & 1u);
    meta_status_ = "fetches: " + meta_fetch_opt_summary(meta_fetch_opts_)
                 + (overwrites ? " (existing tags of those kinds are overwritten)" : " (existing tags stay as they are)");
}


// SHIFT+c on the Fetch List tab. The helper is killed; the answers that had already arrived stay in the session as
// pending edits, everything not fetched yet stays on the fetch list (see poll_pending_meta_fetch()).
void App::meta_cancel_fetch() {
    if (!meta_fetch_running_.load()) { meta_status_ = "no fetch is running"; return; }
    meta_fetch_cancel_.request();
    meta_status_ = "AcoustID: cancelling ...";
}

void App::meta_start_fetch() {
    meta_ensure_session_loaded();
    if (meta_prompt_paths_.empty()) return;
    if (meta_fetch_running_.exchange(true)) {
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "an AcoustID fetch is already running";
        return;
    }

    std::vector<MetaFetchRequest> reqs;
    reqs.reserve(meta_prompt_paths_.size());
    meta_fetch_want_.clear();
    int skipped = 0;
    for (size_t pi = 0; pi < meta_prompt_paths_.size(); ++pi) {
        const std::string& path = meta_prompt_paths_[pi];
        MetaFetchRequest r;
        r.path = path;
        r.want = meta_want_for(path, meta_prompt_opts_);
        bool any_wanted = false;
        for (int i = 1; i < kMetaFieldCount; ++i) any_wanted = any_wanted || r.want[i];
        if (!any_wanted) { ++skipped; continue; }   // missing only, and nothing is missing
        meta_fetch_want_[path] = r.want;
        const MetaEditEntry* e = meta_entry(path);
        RowMeta rm = meta_row_meta(path_from_utf8(path));
        r.title = (e && e->edited[static_cast<int>(MetaField::Title)])
                      ? e->value[static_cast<int>(MetaField::Title)]
                      : rm.title;
        if (r.title.empty()) r.title = path_utf8(path_from_utf8(path).stem()); // last resort: the file name
        r.artist = (e && e->edited[static_cast<int>(MetaField::Artist)])
                       ? e->value[static_cast<int>(MetaField::Artist)]
                       : rm.artist;
        reqs.push_back(std::move(r));
    }
    meta_prompt_paths_.clear();
    meta_fetch_ok_paths_.clear();
    if (reqs.empty()) {
        meta_fetch_running_ = false;
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) =
            "nothing to fetch -- no tag is missing ([SHIFT+n] fetches all tags; or tick tags on the FETCH LIST tab)";
        return;
    }
    meta_fetch_skipped_ = skipped;
    meta_fetch_cancel_.reset();
    {
        std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
        meta_fetch_results_.clear();
        meta_fetch_progress_ = "0/" + std::to_string(reqs.size());
        meta_fetch_done_ = false;
        meta_fetch_outcome_ = MetaFetchOutcome{};
    }
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) =
        "AcoustID: fetching " + std::to_string(reqs.size()) + " title" + (reqs.size() == 1 ? "" : "s") + " ...";

    if (meta_fetch_thread_.joinable()) meta_fetch_thread_.join(); // previous batch already finished (running_ was false)
    meta_fetch_thread_ = std::thread([this, reqs]() {
        run_guarded("acoustid fetch", [&] {
            int total = static_cast<int>(reqs.size());
            int done = 0;
            // One python process fingerprints the whole batch (fpcalc) and
            // paces itself to AcoustID's 3-requests-per-second limit, printing
            // a JSON line per lookup, so results are applied to the session as
            // they land instead of only after the last one.
            MetaFetchOutcome out = run_acoustid_fetch(reqs, meta_script_,
                [this, total, &done](const MetaFetchResult& r) {
                    std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
                    meta_fetch_results_.push_back(r);
                    ++done;
                    meta_fetch_progress_ = std::to_string(done) + "/" + std::to_string(total);
                }, &meta_fetch_cancel_);
            std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
            meta_fetch_outcome_ = out;
            meta_fetch_done_ = true;
        });
    });
}

// Called from the render loop: drains whatever the batch produced since the
// last frame (each result becomes pending edits, highlighted exactly like
// typed ones) and finalises it once the worker reports done.
void App::poll_pending_meta_fetch() {
    std::vector<MetaFetchResult> incoming;
    std::string progress;
    bool done = false;
    MetaFetchOutcome outcome;
    {
        std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
        // All pushes happen under this same lock, so reading `done` in the
        // same critical section as the swap means a true `done` implies no
        // further results are coming.
        incoming.swap(meta_fetch_results_);
        progress = meta_fetch_progress_;
        done = meta_fetch_done_;
        if (done) { outcome = meta_fetch_outcome_; meta_fetch_done_ = false; }
    }

    if (!incoming.empty()) {
        int applied = 0;
        for (const auto& r : incoming) {
            if (!r.ok) continue;
            // Only the tags this entry was allowed to fetch are written; the rest of the answer is ignored.
            std::array<bool, kMetaFieldCount> want{};
            want.fill(true);
            auto wit = meta_fetch_want_.find(r.path);
            if (wit != meta_fetch_want_.end()) want = wit->second;
            MetaEditEntry& e = meta_touch_entry(r.path);
            bool any = false;
            for (int i = 1; i < kMetaFieldCount; ++i) { // 0 = file name: AcoustID never renames anything
                if (!r.set[i] || !want[i]) continue;
                e.edited[i] = true;
                e.fetched[i] = true;                    // a fetched suggestion (several candidates -> shown in the legend colour)
                e.value[i] = r.value[i];
                e.choices[i] = r.choices[i];
                e.choice_aux[i] = r.choice_aux[i];
                if (!e.choices[i].empty()) e.value[i] = e.choices[i].front();   // the shown value is the first candidate
                any = true;
            }
            if (!r.note.empty() && meta_fetch_note_.empty()) meta_fetch_note_ = r.note;
            if (any) { ++applied; meta_fetch_ok_paths_.push_back(r.path); }
        }
        if (applied) { meta_persist(); meta_refresh_lib_view(); }
        if (!progress.empty()) {
            (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "AcoustID: " + progress + " ...";
        }
    }
    if (!done) return;

    if (meta_fetch_thread_.joinable()) meta_fetch_thread_.join();
    meta_fetch_running_ = false;

    // Everything that came back is now in the session, so drop it from the
    // fetch list -- what remains is exactly what still needs another try.
    if (!meta_fetch_ok_paths_.empty() && !meta_fetch_list_.empty()) {
        std::vector<std::string> keep;
        for (const auto& p : meta_fetch_list_) {
            if (std::find(meta_fetch_ok_paths_.begin(), meta_fetch_ok_paths_.end(), p) == meta_fetch_ok_paths_.end()) {
                keep.push_back(p);
            }
        }
        meta_fetch_list_ = std::move(keep);
        meta_refresh_lib_view();
    }
    meta_fetch_ok_paths_.clear();
    int need_choice = 0;
    for (const auto& en : meta_session_) for (int i = 0; i < kMetaFieldCount; ++i) if (en.needs_choice(i)) ++need_choice;

    std::string msg;
    if (outcome.cancelled) {
        msg = "AcoustID: cancelled after " + std::to_string(outcome.ok_count) + " ok";
        if (outcome.fail_count > 0) msg += ", " + std::to_string(outcome.fail_count) + " failed";
        msg += " -- the titles not fetched yet stay on the fetch list";
    } else if (outcome.python_missing || outcome.script_missing) {
        msg = "AcoustID: " + outcome.error;
    } else if (outcome.ok_count > 0 || outcome.fail_count > 0) {
        msg = "AcoustID: " + std::to_string(outcome.ok_count) + " ok";
        if (outcome.fail_count > 0) {
            msg += ", " + std::to_string(outcome.fail_count) + " failed";
            if (!outcome.error.empty()) msg += " (" + outcome.error + ")";
        }
    } else {
        msg = "AcoustID: " + (outcome.error.empty() ? std::string("no result") : outcome.error);
    }
    if (meta_fetch_skipped_ > 0) msg += ", " + std::to_string(meta_fetch_skipped_) + " skipped (nothing missing)";
    if (!meta_fetch_note_.empty()) msg += " [" + meta_fetch_note_ + "]";
    if (need_choice > 0) msg += " -- " + std::to_string(need_choice) + " field" + (need_choice == 1 ? " needs" : "s need") + " a choice (ENTER on the tag)";
    meta_fetch_skipped_ = 0;
    meta_fetch_note_.clear();
    meta_fetch_want_.clear();
    meta_persist();
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
    log_event(msg);
}

// Ctrl+Shift+S -> Y. Writes every pending edit to disk, keeps the failures
// in the session so they can be retried, and only clears the backup when
// nothing is left over.
void App::meta_apply_session() {
    meta_ensure_session_loaded();
    if (meta_save_.active) return;
    if (meta_session_.empty()) {
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "nothing to save -- no pending edits";
        return;
    }
    meta_save_ = MetaSaveJob{};
    meta_save_.active = true;
    meta_save_.todo = meta_session_;   // the working copy; meta_session_ itself shrinks as files are written
    meta_save_.started = meta_save_.last_persist = std::chrono::steady_clock::now();
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "saving " + std::to_string(meta_save_.todo.size()) + " file" + (meta_save_.todo.size() == 1 ? "" : "s") + " ...";
    poll_meta_save();   // a few files are done before the next frame
}

// One slice of the save: files are written for about 0.12 s, then the render loop gets control back (it draws the progress
// panel and reads ESC). Called every frame; does nothing without a job.
void App::poll_meta_save() {
    if (!meta_save_.active) return;
    MetaSaveJob& job = meta_save_;
    const auto t0 = std::chrono::steady_clock::now();
    while (job.next < job.todo.size() && !job.cancel &&
           std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(120)) {
        const MetaEditEntry& e = job.todo[job.next++];
        std::string err, new_path;
        if (apply_meta_entry(e, &err, &new_path)) {
            ++job.ok;
            {
                // The tags on disk just changed, but row_meta_cache_ is
                // keyed by path and only ever filled in once per path (see
                // meta_row_meta() and rescan_library()'s background sweep,
                // which explicitly SKIPS any path already cached). Left
                // alone, a title-only edit -- same path, so rescan_library()
                // never re-resolves it -- would keep showing the pre-edit
                // tags (including "missing title") forever, even though the
                // file itself now has the new title. Dropping the cache
                // entry here (old path, and the new one if renamed) is what
                // makes the next read actually hit the file again.
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                row_meta_cache_.erase(e.path);
                search_idx_.clear();   // the tags on disk changed: the index takes them from the cache again
                if (!new_path.empty() && new_path != e.path) row_meta_cache_.erase(new_path);
            }
            // The playing file now carries these values: make them what the
            // main screen's block is loaded with (it is read once per track),
            // so it keeps showing them after the pending edit is gone.
            if (has_track_ && current_is_local_ && path_utf8(current_path_) == e.path) {
                overlay_meta_entry(metadata_, e, current_path_);
            }
            if (!new_path.empty() && new_path != e.path) {
                // The file was renamed: keep every in-app reference pointed
                // at the new name so playback/queue/lyrics don't break.
                if (path_utf8(current_path_) == e.path) current_path_ = path_from_utf8(new_path);   // (metadata_ was already updated above)
                for (auto& q : queue_) {
                    if (q.is_local && path_utf8(q.local_path) == e.path) q.local_path = path_from_utf8(new_path);
                }
                for (auto& p : meta_fetch_list_) if (p == e.path) p = new_path;
            }
        } else {
            if (job.first_error.empty()) job.first_error = err;
            job.failed.push_back(e);
        }
    }
    // The session on screen (and in the backup file) is always: failures so far + what is still to do.
    meta_session_ = job.failed;
    meta_session_.insert(meta_session_.end(), job.todo.begin() + static_cast<long>(job.next), job.todo.end());
    if (job.next >= job.todo.size() || job.cancel) { meta_save_finish(); return; }
    if (std::chrono::steady_clock::now() - job.last_persist > std::chrono::seconds(2)) {   // survives a crash half-way
        meta_persist();
        job.last_persist = std::chrono::steady_clock::now();
    }
    const std::string msg = "saving " + std::to_string(job.next) + " / " + std::to_string(job.todo.size()) + " ...";
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
}

void App::meta_save_finish() {
    MetaSaveJob job = std::move(meta_save_);
    meta_save_ = MetaSaveJob{};   // inactive
    const int failed = static_cast<int>(job.failed.size());
    const int not_done = static_cast<int>(job.todo.size() - job.next);
    meta_persist();
    if (job.ok > 0) {
        // Names and tags changed -- rebuild the lists. rescan_library() only
        // refreshes the app's own views, so the meta menu's LIBRARY panel has
        // to be rebuilt as well: after a rename it would otherwise keep
        // showing (and keep handing out, via meta_hovering_path()) the old
        // path, and every edit made against it from there on would be filed
        // under a file that no longer exists.
        rescan_library();
        meta_refresh_lib_view();
    }
    std::string msg;
    if (failed == 0) {
        msg = "applied " + std::to_string(job.ok) + " pending edit" + (job.ok == 1 ? "" : "s") + " to disk";
    } else {
        msg = "applied " + std::to_string(job.ok) + ", " + std::to_string(failed) + " failed: " + job.first_error;
    }
    if (not_done > 0) msg += " -- stopped, " + std::to_string(not_done) + " not saved yet (still pending)";
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
    log_event(msg);
}

std::vector<std::string> App::build_meta_save_panel() const {
    const int W = kMetaSavePanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    auto centred = [&](const std::string& plain) {
        const int fill = std::max(0, inner - display_width(plain));
        const int left = fill / 2;
        return bar + " " + std::string(static_cast<size_t>(left), ' ') + plain + std::string(static_cast<size_t>(fill - left), ' ') + " " + bar;
    };
    const size_t total = std::max<size_t>(1, meta_save_.todo.size());
    const size_t done = std::min(meta_save_.next, total);
    const int bar_w = inner - 2;
    const int filled = static_cast<int>(static_cast<double>(bar_w) * static_cast<double>(done) / static_cast<double>(total));
    std::string progress = "[";
    for (int i = 0; i < bar_w - 2; ++i) progress += i < filled ? "\u2588" : "\u2591";
    progress += "]";
    std::vector<std::string> lines;
    lines.push_back(box_top("SAVING", W, border));
    lines.push_back(centred(""));
    lines.push_back(centred("Please wait, this may take a few seconds"));
    lines.push_back(centred(std::to_string(done) + " / " + std::to_string(meta_save_.todo.size()) + " files"));
    lines.push_back(centred(progress));
    lines.push_back(centred(""));
    lines.push_back(box_bottom(W, meta_save_.cancel ? "stopping ..." : "[ESC] stop after the current file", border_bottom));
    return lines;
}

// Ctrl+Shift+X -> Y. The audio files were never written to by an edit, so
// discarding only means throwing the pending values (and their backup file)
// away.
void App::meta_discard_session() {
    meta_ensure_session_loaded();
    int had = static_cast<int>(meta_session_.size());
    meta_session_.clear();
    meta_fetch_list_.clear();
    meta_fetch_ok_paths_.clear();
    meta_persist(); // empty -> the autosave backup file goes away
    std::string msg = "editing session discarded";
    if (had > 0) msg += " (" + std::to_string(had) + " file" + (had == 1 ? "" : "s") + " had pending edits)";
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
    log_event(msg);
}

// Shared Y/N/ESC handler for the three confirmations. Returns true whenever
// it consumed the key -- including the "a prompt is up, swallow everything
// else" case -- and false only when there is no prompt at all, which is what
// lets handle_key() call it unconditionally before its mode dispatch.
bool App::handle_meta_prompt_key(int key) {
    if (meta_prompt_ == MetaPrompt::None) return false;
    auto say = [this](const std::string& s) {
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = s;
    };
    if (key == 'y' || key == 'Y') {
        MetaPrompt p = meta_prompt_;
        meta_prompt_ = MetaPrompt::None;
        // Careful with meta_prompt_paths_: the fetch batch is built FROM it,
        // so it may only be cleared by the actions that don't read it -- an
        // eager clear here made meta_start_fetch() see an empty list and
        // return without fetching anything (silently: the confirmation had
        // already been dismissed).
        if (p == MetaPrompt::Save) {
            meta_prompt_paths_.clear();
            meta_apply_session();
        } else if (p == MetaPrompt::Discard) {
            meta_prompt_paths_.clear();
            meta_discard_session();
        } else {
            meta_start_fetch(); // clears meta_prompt_paths_ itself
        }
        return true;
    }
    if (key == 'n' || key == 'N' || key == 27) {
        meta_prompt_ = MetaPrompt::None;
        meta_prompt_paths_.clear();
        say("cancelled");
        return true;
    }
    return true; // swallow everything else while a confirmation is up
}

void App::handle_meta_key(int key) {
    if (handle_meta_prompt_key(key)) return;
    if (handle_meta_choice_key(key)) return;

    if (key == 27) { // ESC
        // Inside a text box ESC first steps out of it: the field editor goes
        // back to the library (like ENTER), a search box that was typed into
        // hands Left/Right back to the tab switch. Only an ESC outside a box
        // leaves the editor -- which always just keeps the autosave backup.
        if (meta_tab_ == 0 && meta_focus_ == 2 && meta_field_editing_) {
            // Actively typing into a field: ESC first just puts the caret
            // away again (back to "highlighted, not editing" -- see
            // meta_field_editing_), same step ENTER takes. A second ESC,
            // below, is what leaves the field for the library.
            meta_field_editing_ = false;
            text_disengage();
            return;
        }
        if (meta_tab_ == 0 && meta_focus_ == 2) {
            meta_focus_ = 1;
            text_disengage();
            if (meta_resort_edited_ && !meta_lib_view_.empty()) meta_refresh_lib_view();
            return;
        }
        // One more step back from the library picker: the search bar,
        // rather than leaving the editor outright. A second ESC from there
        // (below, since the box isn't engaged any more) is what actually
        // exits now.
        if (meta_tab_ == 0 && meta_focus_ == 1) { meta_focus_ = 0; return; }
        if (meta_tab_ == 0 && meta_focus_ == 0 && text_engaged("meta-search")) { text_disengage(); return; }
        mode_ = Mode::Browse;
        return;
    }
    if (key == kKeyCtrlS) {
        if (meta_session_.empty()) { meta_status_ = "nothing to save -- no pending edits"; return; }
        meta_prompt_ = MetaPrompt::Save;
        return;
    }
    if (key == kKeyCtrlShiftX) {
        if (meta_session_.empty() && meta_fetch_list_.empty()) { meta_status_ = "nothing to discard"; return; }
        meta_prompt_ = MetaPrompt::Discard;
        return;
    }
    // SHIFT+b for the hovered title -- raw 'B' with no arrow behind it (see
    // handle_key()'s Browse branch for why this can't be a normal hotkey).
    // Only fires while the current focus has no caret of its own to type
    // into: the library picker (focus 1) and the whole of tab 1's fetch
    // list, neither of which is a text field. On tab 0 with the search box
    // (focus 0) or the field editor (focus 2) focused, a capital B must
    // still be typable, so this hotkey is skipped there and 'B' falls
    // through to the normal text-entry handling below instead.
    if (key_is("HKeyFetchTags", key) && (key > 255 || meta_tab_ == 1 || meta_focus_ == 1)) {
        std::string path = meta_hovering_path();
        if (path.empty()) meta_status_ = "no title selected";
        else meta_prompt_single_fetch(path, false);
        return;
    }
    // SHIFT+c on the Fetch List tab: stop the running fetch.
    if (key == 'C' && !last_key_was_arrow() && meta_tab_ == 1) { meta_cancel_fetch(); return; }

    // SHIFT+n: the same for ALL tags (overwrites the existing ones); same focus rule as SHIFT+b.
    if (key == 'N' && !last_key_was_arrow() && (meta_tab_ == 1 || meta_focus_ == 1)) {
        std::string path = meta_hovering_path();
        if (path.empty()) meta_status_ = "no title selected";
        else meta_prompt_single_fetch(path, true);
        return;
    }

    // SHIFT+r: rescan the library (same rule as SHIFT+b: not while a text field has the caret).
    if (key_is("HKeyRescan", key) && (key > 255 || meta_tab_ == 1 || meta_focus_ == 1)) { rescan_now(); return; }

    // Arrows collapse to 'A'..'D' app-wide; last_key_was_arrow() is what
    // tells a real arrow from a typed capital. Unlike the playlist editor
    // (where the name field only ever sees casual typing) these fields hold
    // real words, so capitals MUST stay typable here -- which is also why
    // the tab switch below only fires for the arrow itself. It is skipped
    // entirely while a field is being edited: Left/Right are the caret keys
    // there (see the field editor at the bottom), and bouncing to the fetch
    // list on every arrow press is exactly what made marking text
    // impossible.
    bool arrow = last_key_was_arrow();

    // The search box follows the playlist editor's rule: Left/Right switch
    // tabs until something is typed into it (text_engage()), then they move
    // the caret until ESC / TAB / ENTER. In the field editor they are always
    // the caret keys -- it is only ever entered on purpose (ENTER / TAB).
    // Alt+Left/Right switch tabs from everywhere.
    const bool in_text = meta_tab_ == 0 && ((meta_focus_ == 2 && meta_field_editing_) || (meta_focus_ == 0 && text_engaged("meta-search")));
    // The tab switch itself stays blocked for the whole time focus is in the
    // field editor, not just while a field is actively being typed into --
    // a merely-highlighted field still uses Left/Right to move the caret
    // once ENTER activates it, so bouncing to the other tab on a stray
    // arrow there would be surprising either way.
    const bool block_tab_switch = (meta_tab_ == 0 && (meta_focus_ == 2 || (meta_focus_ == 0 && text_engaged("meta-search"))))
                               || (meta_tab_ == 1 && meta_fetch_focus_ == 1);   // the option line uses left/right itself
    if (key == kKeyAltLeft || key == kKeyAltRight || (arrow && (key == 'C' || key == 'D') && !block_tab_switch)) { // the only 2 tabs
        meta_tab_ = (meta_tab_ + 1) % 2;
        meta_focus_ = 0;
        meta_fetch_focus_ = 0;
        meta_field_editing_ = false;
        text_disengage();
        meta_refresh_lib_view();
        return;
    }
    if (key == 9) { // Tab: search field -> library picker -> field editor -> ...
        if (meta_tab_ == 0) meta_focus_ = (meta_focus_ + 1) % 3;
        else meta_fetch_focus_ = 1 - meta_fetch_focus_;   // FETCH LIST: the list <-> the option line in the header
        meta_field_editing_ = false;
        text_disengage();
        return;
    }

    if (meta_tab_ == 1) { // --- fetch list ---
        int total = static_cast<int>(meta_fetch_list_.size());
        if (arrow && key == 'A') { if (meta_fetch_selected_ > 0) --meta_fetch_selected_; return; }
        if (arrow && key == 'B') { if (total > 0 && meta_fetch_selected_ < total - 1) ++meta_fetch_selected_; return; }
        if (meta_fetch_focus_ == 1) {   // the option line: left / right move the cursor, SPACE ticks the box under it
            if (arrow && key == 'C') { meta_opt_cursor_ = (meta_opt_cursor_ + 1) % kMetaFetchOptCount; return; }
            if (arrow && key == 'D') { meta_opt_cursor_ = (meta_opt_cursor_ + kMetaFetchOptCount - 1) % kMetaFetchOptCount; return; }
            if (key == ' ') { meta_toggle_fetch_opt(meta_opt_cursor_); return; }
        }
        if (key == '\r' || key == '\n') { meta_prompt_list_fetch(); return; } // "fetch metadata" over the whole list
        if (key == 'P' && !arrow) { meta_play_hovering(); return; } // SHIFT+p: start the hovered track
        if (key == kKeyDelete || key == 127 || key == 'd') { meta_remove_hovering(); return; }
        return;
    }

    if (meta_focus_ == 0) { // --- search field ---
        // Up/Down still walk the library while the box has focus (same
        // convention as the main UI's "/" search); Left/Right are the caret
        // now, which is what makes marking -- and therefore copy/paste --
        // possible in here at all.
        if (arrow && key == 'A') { if (meta_lib_selected_ > 0) --meta_lib_selected_; return; }
        if (arrow && key == 'B') { if (meta_lib_selected_ + 1 < static_cast<int>(meta_lib_view_.size())) ++meta_lib_selected_; return; }
        if (key == '\r' || key == '\n') {
            // The hovered track (Up/Down already moved meta_lib_selected_
            // while this box had focus) goes straight to its fields --
            // no need to land on the library picker first just to press
            // ENTER a second time.
            text_disengage();
            if (meta_lib_view_.empty()) { meta_status_ = "no track selected"; meta_focus_ = 1; return; }
            meta_focus_ = 2;
            meta_field_editing_ = false; // lands highlighted, not yet armed for typing
            return;
        }
        edit_focus("meta-search", meta_query_);
        text_engage("meta-search");
        if (edit_text_key(meta_query_, edit_caret_, edit_anchor_, key, 120, &meta_status_))
            meta_refresh_lib_view();
        return;
    }

    if (meta_focus_ == 1) { // --- library picker ---
        int total = static_cast<int>(meta_lib_view_.size());
        if (arrow && key == 'A') { if (meta_lib_selected_ > 0) --meta_lib_selected_; return; }
        if (arrow && key == 'B') { if (total > 0 && meta_lib_selected_ < total - 1) ++meta_lib_selected_; return; }
        if (key == '\r' || key == '\n') {
            if (total == 0) { meta_status_ = "no track selected"; return; }
            meta_focus_ = 2; // land on the hovered title's fields, highlighted
            meta_field_editing_ = false; // not yet armed for typing -- ENTER arms it
            return;
        }
        if (key == 'a') { meta_add_hovering_to_fetch(); return; } // queue it for AcoustID, like the main queue's 'a'
        if (key == 'F' && !arrow) { meta_add_view_to_fetch(); return; } // SHIFT+f: the whole (filtered) pane
        if (key == 'r') { meta_toggle_resort(); return; } // toggle: edited files on top vs. alphabetical order
        // Missing-tag filters. 'T'/'Y' have no other meaning here; 'A' is
        // only the Up ARROW together with last_key_was_arrow(), which the
        // two navigation keys above already claimed -- so a plain Shift+A
        // typed here reaches this line.
        if (key == 'x') { meta_toggle_filter(1); return; } // only files with no metadata at all
        if (key == 'T') { meta_toggle_filter(2); return; } // SHIFT+t: missing title
        if (key == 'A') { meta_toggle_filter(3); return; } // SHIFT+a: missing artist
        if (key == 'Y') { meta_toggle_filter(4); return; } // SHIFT+y: missing year
        if (key == 'L') { meta_toggle_filter(5); return; } // SHIFT+l: missing album
        if (key == 'P') { meta_play_hovering(); return; } // SHIFT+p: start the hovered track
        if (key == kKeyDelete || key == 127 || key == 'd') { meta_remove_hovering(); return; }
        return; // every other printable key would be search input, and search lives in focus 0
    }

    // --- field editor: highlight first, ENTER arms the caret to type ---
    // Same two-stage model the PATHS tab uses for its folder fields
    // (mode_ == Mode::ColorEdit there; meta_field_editing_ here, since the
    // meta editor isn't a Settings mode): a hovered tag is merely
    // highlighted -- Up/Down move between tags -- until ENTER shows the
    // caret, at which point Up/Down stop navigating (so they don't get
    // typed as literal letters below) and a second ENTER puts the caret
    // away again without leaving the field.
    {
        std::string path = meta_hovering_path();
        if (path.empty()) { meta_focus_ = 1; meta_field_editing_ = false; return; }

        // The caret belongs to one specific (file, field) pair. If either
        // changed since the last keypress -- a field move, another row, the
        // 'r' resort reordering the list -- the offsets now point at a
        // different string, so retarget them to the end of this one instead
        // of letting them be clamped against the wrong text.
        const MetaEditEntry* e = meta_entry(path);
        std::string cur = meta_display_value(path, meta_field_, e);
        const std::string owner = path + "#" + std::to_string(meta_field_);
        if (edit_owner_ != owner) {
            edit_owner_ = owner;
            edit_caret_ = edit_anchor_ = cur.size();
        }
        le_clamp(cur, edit_caret_, edit_anchor_);
        // meta_set_field() marks the file as edited and rewrites the
        // autosave unconditionally, so it is only ever reached when the
        // bytes really changed -- moving the caret must not make a file
        // count as edited.
        const std::string orig = cur;
        auto commit = [&]() { if (cur != orig) meta_set_field(path, meta_field_, cur); };
        const size_t limit = (meta_field_ == static_cast<int>(MetaField::FileName)) ? 200 : 160;

        if (!meta_field_editing_) {
            // Highlighted, not yet armed: Up/Down move between tags, ENTER
            // arms this one (caret starts at the end, same as a fresh
            // PATHS-row edit). Everything else is a no-op -- typing on a
            // merely-highlighted field would be surprising.
            if (arrow && key == 'A') { if (meta_field_ > 0) --meta_field_; return; }
            if (arrow && key == 'B') { if (meta_field_ < kMetaFieldCount - 1) ++meta_field_; return; }
            if (key == '\r' || key == '\n') {
                if (e && e->needs_choice(meta_field_)) { meta_choice_open(); return; }   // several candidates: pick one first
                meta_field_editing_ = true;
                edit_owner_ = owner;
                edit_caret_ = edit_anchor_ = cur.size();
            }
            return;
        }

        // --- armed: caret, selection, clipboard, typing -------------------
        // Up/Down only navigate fields while merely highlighted (above);
        // here they must be dropped rather than falling through to
        // is_text_key() below, where a collapsed arrow would otherwise be
        // typed as a literal 'A'/'B' -- the same guard Mode::ColorEdit uses.
        if (arrow && (key == 'A' || key == 'B')) return;
        if (key == '\r' || key == '\n') {
            meta_field_editing_ = false; // done typing -- back to highlighted, same tag
            // With the 'r' resort on, the file just edited has (re)joined the
            // edited block: move it up now, while the cursor is still on it --
            // meta_refresh_lib_view() follows the row by path.
            if (meta_resort_edited_ && !meta_lib_view_.empty()) meta_refresh_lib_view();
            return;
        }
        // --- caret, selection, clipboard ---------------------------------
        // Shift+arrows mark, Ctrl+C/X/V copy/cut/paste, Home/End jump. They
        // arrive as their own sentinel values (terminal_ui.h) because the
        // bare ones are already taken here: a plain arrow press collapses to
        // the letter it would otherwise type, and a typed 'c' has to stay a
        // typed 'c' inside a word.
        if (key == kKeyHome) { edit_caret_ = edit_anchor_ = 0; return; }
        if (key == kKeyEnd) { edit_caret_ = edit_anchor_ = cur.size(); return; }
        if (key == kKeyShiftLeft) { le_move(cur, edit_caret_, edit_anchor_, -1, true); return; }
        if (key == kKeyShiftRight) { le_move(cur, edit_caret_, edit_anchor_, +1, true); return; }
        if (key == kKeyCtrlC) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            std::string t = (a == b) ? cur : cur.substr(a, b - a);
            clipboard_set(t);
            meta_status_ = t.empty() ? "nothing selected" : "COPIED";
            return;
        }
        if (key == kKeyCtrlX) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            if (a == b) return; // never empty the field by accident
            clipboard_set(cur.substr(a, b - a));
            le_erase_selection(cur, edit_caret_, edit_anchor_);
            commit();
            meta_status_ = "CUT";
            return;
        }
        if (key == kKeyCtrlV) {
            le_paste(cur, edit_caret_, edit_anchor_, clipboard_get(), limit);
            commit();
            meta_status_ = "PASTED";
            return;
        }
        if (key == kKeyDelete) { le_delete_forward(cur, edit_caret_, edit_anchor_); commit(); return; }
        if (key == 127 || key == 8) { le_backspace(cur, edit_caret_, edit_anchor_); commit(); return; }
        if (arrow && (key == 'C' || key == 'D')) { // plain Left/Right: move the caret, leave the tab alone
            le_move(cur, edit_caret_, edit_anchor_, key == 'D' ? -1 : +1, false);
            return;
        }
        if (is_text_key(key)) {
            le_insert(cur, edit_caret_, edit_anchor_, static_cast<char>(key), limit);
            commit();
        }
        return;
    }
}

std::vector<std::string> App::build_meta_library_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // The pane title carries the active missing-tag filter, so a list
    // narrowed by 'x' or Shift+T/A/Y is never mistaken for the whole
    // library (the status line repeats it with a count).
    std::string lib_label = "LIBRARY";
    if (meta_filter_ != 0) lib_label += std::string(" [") + meta_filter_label() + "]";
    if (meta_focus_ == 1) lib_label += focus_indicator(settings_, border_ansi);
    out.push_back(box_top(lib_label, total_width, border_ansi));

    int total = static_cast<int>(meta_lib_view_.size());
    if (total == 0) {
        int mid = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid) {
                std::string plain = (meta_filter_ != 0) ? "NO TRACKS MATCH THE FILTER"
                                    : meta_query_.empty() ? "NO TRACKS FOUND"
                                    : "NO MATCHING TRACK";
                std::string text = apply_font_map(plain, settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string list_ansi = ansi_for(settings_.list_color, false);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    int scroll = std::clamp(meta_lib_selected_ - height / 2, 0, std::max(0, total - height));
    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        bool edited = false;
        bool needs_choice = false;
        if (idx < total) {
            const auto& t = meta_lib_view_[idx];
            int title_w = std::max(5, inner - idx_w - 2);
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_title = apply_font_map(list_row_title(t.path, t.title), settings_.font_map);
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_title, title_w), title_w);
            // Files carrying pending edits are painted in the header colour,
            // the same bold highlight an edited field gets in the FIELDS
            // panel -- so the list answers "which files is that panel talking
            // about?" without having to walk over them one by one.
            const MetaEditEntry* e = meta_entry(path_utf8(t.path));
            edited = e && e->any_edited();
            needs_choice = e && e->any_needs_choice();   // several fetched candidates wait for a decision
        }
        // The selected row stays highlighted in EVERY focus, not just while
        // the pane itself owns focus: arrows scroll the library from the
        // search field too (see handle_meta_key(), focus 0), and without the
        // highlight there was nothing on screen saying which row those arrows
        // were moving. Matches build_list_panel() in Browse, which never gates
        // its cursor on focus either -- the LIBRARY box's "◀" stays the focus
        // marker, so there is still exactly one thing naming the focused pane.
        bool sel = (idx == meta_lib_selected_);
        std::string padded = pad_right(truncate_str(content, inner), inner);
        std::string base;
        if (sel) {
            base = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
        } else {
            base = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
        }
        // Colour AFTER padding: display_width() is UTF-8-aware but not
        // ANSI-aware, so a coloured row would otherwise measure wrong.
        if (edited) {
            out.push_back(bar + " " + base + (needs_choice ? legend_sgr(settings_) : header_sgr(settings_)) + padded
                          + "\x1b[0m" + base + "\x1b[0m " + bar);
        } else {
            out.push_back(bar + " " + base + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    // One number for the highlight above: how many files in the session have
    // pending edits (meta_session_ only ever holds those).
    if (!meta_session_.empty()) {
        std::string tag = "[ " + std::to_string(meta_session_.size()) + " edited ]";
        footer += footer.empty() ? tag : "  " + tag;
    }
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// The five editable fields. This is where the header-colour highlight and
// the block cursor live, and both are built the same way build_list_panel()
// builds a row: plain segments are measured first, then the already-sized
// pieces get wrapped in colour -- display_width() is UTF-8-aware but NOT
// ANSI-aware, so colouring before padding would miscount the row.
std::vector<std::string> App::build_meta_fields_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    std::string path = meta_hovering_path();
    std::string name = path.empty() ? std::string() : path_utf8(path_from_utf8(path).filename());
    std::string sel_pos;
    if (!meta_lib_view_.empty()) {
        sel_pos = "  " + std::to_string(meta_lib_selected_ + 1) + "/" + std::to_string(meta_lib_view_.size());
    }
    std::string label = "EDIT" + sel_pos + (name.empty() ? std::string() : " " + truncate_str(name, 40))
                      + (meta_focus_ == 2 ? focus_indicator(settings_, border_ansi) : std::string());
    out.push_back(box_top(label, total_width, border_ansi));

    if (path.empty()) {
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == height / 2) {
                std::string text = apply_font_map("NO TRACK SELECTED", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    const MetaEditEntry* entry = meta_entry(path);
    const int label_w = 8;
    for (int row = 0; row < height; ++row) {
        if (row >= kMetaFieldCount) { // unused rows stay blank, so the panel height never changes
            std::string padded = pad_right(std::string(), inner);
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + padded + "\x1b[0m " + bar);
            continue;
        }
        int i = row;
        std::string lab = pad_right(meta_field_label(i), label_w);
        std::string val_raw = meta_hover_values_[i];
        if (val_raw.empty()) {
            val_raw = (i == 0) ? std::string("-")
                    : (meta_hover_resolved_ ? "(untagged)" : "(reading ...)");
        }
        // Two states share the "hovered" row, matching the PATHS tab's
        // highlighted-vs-activated folder field: hovered alone just
        // highlights the tag (no caret, Up/Down move between tags);
        // meta_field_editing_ additionally arms the caret and swaps in the
        // CHANGE colours (change_sgr()) -- the same ones an activated PATHS
        // row uses -- so it's visually obvious typing is live.
        bool hovered = (meta_focus_ == 2) && (i == meta_field_);
        bool editing = hovered && meta_field_editing_;
        bool edited = entry && entry->edited[i];
        const int val_w = std::max(4, inner - label_w);

        std::string base = hovered ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color)
                               : (ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color));

        std::string val;
        int used = 0;
        if (editing) {
            // The field being typed into is painted by paint_edit_field():
            // the window scrolls so the caret stays inside the panel, the
            // marked range is drawn in reverse video, and the caret itself
            // is the block glyph this panel uses for a cursor (it renders
            // whole frames, so there is no real terminal cursor to move --
            // which is also why an arrow press used to be free to switch
            // tabs while the text still had nowhere to go).
            // The caret only belongs to this row if the offsets were last
            // set against exactly this (file, field) pair -- see the field
            // editor in handle_meta_key().
            const std::string owner = path + "#" + std::to_string(i);
            size_t c = val_raw.size(), a = val_raw.size();
            if (edit_owner_ == owner) { c = edit_caret_; a = edit_anchor_; }
            const std::string restore = change_sgr(settings_) + (edited ? header_sgr(settings_) : std::string());
            EditPaint p = paint_edit_field(val_raw, c, a, val_w, restore, /*block=*/true);
            // paint_edit_field() only re-emits `restore` after a selection
            // run, so the CHANGE colours have to be switched on explicitly
            // here too -- same as the PATHS/Colors tabs' own activated-field
            // rendering (change_sgr() prefixed before the painted value).
            val = restore + p.s;
            used = display_width(lab) + p.cols; // p.cols already ignores the ANSI codes
        } else {
            val = truncate_str(val_raw, val_w);
            used = display_width(lab) + display_width(val);
        }
        std::string fill(std::max(0, inner - used), ' ');

        std::string row_ansi = base + lab;
        // Header colour: edited by hand or fetched with a single candidate. Legend colour: several candidates --
        // a choice is waiting (ENTER opens it).
        if (edited) row_ansi += (entry && entry->needs_choice(i)) ? legend_sgr(settings_) : header_sgr(settings_);
        row_ansi += val;
        if (edited) row_ansi += "\x1b[0m" + base; // back to the row's own colours for the padding
        row_ansi += fill + "\x1b[0m";
        out.push_back(bar + " " + row_ansi + " " + bar);
    }

    int n_edited = 0, n_choice = 0;
    if (entry) {
        for (bool b : entry->edited) if (b) ++n_edited;
        for (int i = 0; i < kMetaFieldCount; ++i) if (entry->needs_choice(i)) ++n_choice;
    }
    std::string footer = n_edited ? ("( " + std::to_string(n_edited) + " edited, pending )") : "";
    if (n_choice) footer += std::string(footer.empty() ? "( " : " ( ") + std::to_string(n_choice) + " to choose )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_meta_fetch_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    out.push_back(box_top("FETCH LIST (" + std::to_string(meta_fetch_list_.size()) + ")" + focus_indicator(settings_, border_ansi),
                          total_width, border_ansi));

    int total = static_cast<int>(meta_fetch_list_.size());
    if (total == 0) {
        int mid = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid) {
                std::string text = apply_font_map("NOTHING QUEUED -- PRESS 'a' ON A TRACK IN THE EDIT TAB",
                                                  settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    int scroll = std::clamp(meta_fetch_selected_ - height / 2, 0, std::max(0, total - height));
    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const std::string& p = meta_fetch_list_[idx];
            int title_w = std::max(5, inner - idx_w - 2);
            const MetaEditEntry* e = meta_entry(p);
            std::string base = list_row_title(path_from_utf8(p), path_utf8(path_from_utf8(p).stem()));
            std::string shown = (e && e->any_edited()) ? (base + " [edited]") : base;
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_title = apply_font_map(shown, settings_.font_map);
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_title, title_w), title_w);
        }
        bool sel = (idx == meta_fetch_selected_);
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// Full-screen meta editor, assembled exactly like build_playlist_screen():
// header box, tab strip, optional search row, boxed panels, then a footer
// that is ALWAYS three lines (prompt padded to two + status, or legend's
// two rows + status) so switching states never makes the layout jump.
void App::build_meta_screen(std::ostringstream& frame, int W, int target_height) const {
    if (W < 60) W = 60;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string HI = "\x1b[7m", R = "\x1b[0m";

    int fixed_rows = 3; // the two strip lines + bottom border
    {   // title on the border line + the settings-style tab strip (same as the playlist menu). On the EDIT tab the search
        // field sits on the strip's second line, left of the tab indicators (pushed right so 25 characters always fit).
        const int search_max = 25;
        const std::string prefix = " Search: ";
        const int field_w = search_max + 1;                                    // 25 characters + the caret block
        const int inner_w = meta_tab_ == 1 ? 82   // the FETCH LIST option line (" Fetch: " + six check boxes + focus mark)
                          : static_cast<int>(prefix.size()) + field_w + 2 + 1; // + room for the focus mark
        std::string line2;
        int line2_cols = 0;
        if (meta_tab_ == 0) {
            std::string field;
            int field_cols = 0;
            if (meta_focus_ == 0) { // focus is in the box: caret + block cursor
                EditPaint p = paint_edit_field(meta_query_, edit_caret_, edit_anchor_, field_w,
                                               meta_query_.empty() ? search_idle_sgr(settings_) : search_sgr(settings_), true);
                field = search_run(settings_, p.s, !meta_query_.empty());
                field_cols = p.cols;
            } else { // focus elsewhere: plain text, with the placeholder hint
                const std::string plain = truncate_str(meta_query_.empty() ? std::string("(type to filter)") : meta_query_, field_w);
                field = search_run(settings_, plain, !meta_query_.empty());
                field_cols = static_cast<int>(display_width(plain));
            }
            std::string focus_mark;
            int focus_w = 0;
            if (meta_focus_ == 0) { focus_mark = " " + focus_sgr(settings_) + "\xe2\x97\x80" + "\x1b[0m"; focus_w = 2; }
            line2 = search_idle_sgr(settings_) + prefix + field + "\x1b[0m" + focus_mark;
            line2_cols = static_cast<int>(prefix.size()) + field_cols + focus_w;
        }
        if (meta_tab_ == 1) {
            // FETCH LIST: the fetch options as check boxes, where the EDIT tab has its search. Only the ticked tags are
            // fetched, for every title of the list (ENTER). Missing only / all untick everything else and vice versa.
            const std::string prefix = " Fetch: ";
            std::string opts;
            int opts_cols = 0;
            for (int m = 0; m < kMetaFetchOptCount; ++m) {
                const bool on = (meta_fetch_opts_ & (1u << m)) != 0;
                const std::string word = std::string(on ? "[x] " : "[ ] ") + meta_fetch_opt_label(m);
                const bool cursor = (meta_fetch_focus_ == 1 && m == meta_opt_cursor_);
                std::string shown = word;
                if (cursor) shown = "\x1b[7m" + word + "\x1b[0m" + search_idle_sgr(settings_);
                else if (on) shown = header_sgr(settings_) + word + "\x1b[0m" + search_idle_sgr(settings_);
                opts += shown;
                opts_cols += static_cast<int>(word.size());
                if (m + 1 < kMetaFetchOptCount) { opts += "  "; opts_cols += 2; }
            }
            std::string focus_mark;
            int focus_w = 0;
            if (meta_fetch_focus_ == 1) { focus_mark = " " + focus_sgr(settings_) + "\xe2\x97\x80" + "\x1b[0m"; focus_w = 2; }
            line2 = search_idle_sgr(settings_) + prefix + opts + "\x1b[0m" + focus_mark;
            line2_cols = static_cast<int>(prefix.size()) + opts_cols + focus_w;
        }
        const auto strip = menu_tab_strip(W, "META EDITOR", {"EDIT", "FETCH LIST"}, meta_tab_, border, border,
                                          settings_.box_upper_left, settings_.box_upper_right, settings_.box_vertical,
                                          inner_w, line2, line2_cols,
                                          ansi_for(settings_.tab_current_color, false), ansi_for(settings_.tab_other_color, false));
        frame << strip.first << "\n" << strip.second << "\n";
    }
    frame << box_bottom(W, "", border_bottom) << "\n";

    // Sized against BOTH bounds that matter: term_rows_ - 1 is what
    // clamp_output_rows() will actually keep (a footer line chopped off
    // every frame is a status message or half a confirmation prompt gone),
    // while player_view_height() is what the other overlays are sized to --
    // except on a short terminal, where the Browse view's metadata/progress/
    // search panels don't shrink and that value is several rows taller than
    // the screen. total = fixed_rows + (panel_h + 2) + 3 must fit the
    // smaller of the two: 2 for the panel's own border rows, 4 for the
    // footer (three legend rows + status -- see where it's written below).
    int budget = std::min(target_height, term_rows_ - 1);
    int panel_h = std::max(6, budget - fixed_rows - 6); // no upper cap: fills a maximised window (6 = 2 border rows + the 4-row footer)
    if (meta_tab_ == 0) {
        int left_w = W / 2;
        int right_w = W - left_w;
        auto left_lines = build_meta_library_panel(left_w, panel_h);
        auto right_lines = build_meta_fields_panel(right_w, panel_h);
        size_t rows = std::max(left_lines.size(), right_lines.size());
        for (size_t i = 0; i < rows; ++i) {
            std::string l = (i < left_lines.size()) ? left_lines[i] : std::string(left_w, ' ');
            std::string r = (i < right_lines.size()) ? right_lines[i] : std::string(right_w, ' ');
            frame << l << r << "\n";
        }
    } else {
        for (auto& l : build_meta_fetch_panel(W, panel_h)) frame << l << "\n";
    }

    if (meta_prompt_ != MetaPrompt::None) {
        // Wrapped to at most 2 lines: the AcoustID disclaimer is longer
        // than a narrow terminal, and an unwrapped bar would wrap into a
        // third line and push the whole overlay past its own height.
        std::string text;
        std::string colors;
        if (meta_prompt_ == MetaPrompt::Fetch) {
            text = meta_fetch_disclaimer() + "   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[43;30m"; // yellow, same as the playlist editor's save prompt
        } else if (meta_prompt_ == MetaPrompt::Save) {
            int open_choices = 0;
            for (const auto& en : meta_session_) for (int i = 0; i < kMetaFieldCount; ++i) if (en.needs_choice(i)) ++open_choices;
            text = "Want to save? Applies every pending edit to the files."
                 + (open_choices ? " " + std::to_string(open_choices) + " fetched tag" + (open_choices == 1 ? "" : "s")
                                       + " still ha" + (open_choices == 1 ? "s" : "ve") + " several options -- the first one is saved." : std::string())
                 + "   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[43;30m";
        } else {
            text = "Want to discard changes? The files themselves were never touched.   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[41;97m"; // red, same as its delete prompt
        }
        auto lines = wrap_lines(text, std::max(10, W - 2), 2);
        // Padded to the same three rows the legend footer always uses, so a
        // prompt appearing (or going away) can never change the height of
        // what sits above it.
        while (lines.size() < 4) lines.push_back(std::string());
        for (const auto& l : lines) {
            // The trailing row is reserved (so the footer never changes
            // height between a prompt and a hint/status), but a reserved
            // row stays bare: a full-width empty color bar reads as a
            // rendering glitch, not as an empty status line.
            if (l.empty()) frame << "\n";
            else frame << colors << " " << l << " \x1b[0m\n";
        }
    } else {
        // Left/Right and ESC say what they do RIGHT NOW: inside a text box
        // that is in use they move the caret / leave the box, otherwise they
        // switch the tab / close the editor (see handle_meta_key()).
        const bool in_text = meta_tab_ == 0 && ((meta_focus_ == 2 && meta_field_editing_) || (meta_focus_ == 0 && text_engaged("meta-search")));
        const std::string lr = in_text ? "[\u2190\u2192] Cursor" : "[\u2190\u2192] Tab";
        const std::string esc = in_text ? "[ESC] Leave field" : "[ESC] Exit";
        std::string enter_hint = "[ENTER] Edit";
        if (meta_tab_ == 0 && meta_focus_ == 2) {
            const MetaEditEntry* he = meta_entry(meta_hovering_path());
            if (he && meta_field_ >= 0 && he->needs_choice(meta_field_)) enter_hint = "[ENTER] Choose";
        }
        // The legend is three rows of grouped commands, so it reads as an
        // overview instead of one long line that gets cut at 120 columns:
        //   row 1  moving around / working on the hovered title
        //   row 2  fetching (the same three commands on both tabs)
        //   row 3  the library view and the session (save / discard / leave)
        // Every row is far below 120 columns in its widest variant, and the
        // footer always has the same height -- 3 legend rows + the status row
        // -- so switching tabs or a message appearing never moves the panels.
        // (A fetch confirmation replaces all of it; see the prompt branch.)
        std::vector<std::string> hint_lines;
        if (meta_tab_ == 0) {
            hint_lines.push_back(lr + " | [TAB] Focus | [\u2191\u2193] Navi. | " + enter_hint + " | [SHIFT+p] Play | [" + hotkey_text("HKeyRescan", "R") + "] Rescan");
            hint_lines.push_back("[a] To fetch list | [SHIFT+f] Add full list | [" + hotkey_text("HKeyFetchTags", "B") + "] Fetch missing tag | [SHIFT+n] Fetch all tags");
            hint_lines.push_back("[r] Edited first | [x/T/A/L/Y] Missing meta | [CTRL+s] Save | [CTRL+SHIFT+x] Discard | " + esc);
        } else {
            hint_lines.push_back((meta_fetch_focus_ == 1 ? std::string("[\u2190\u2192] Option") : lr) + " | [TAB] Focus | "
                                 + (meta_fetch_focus_ == 1 ? std::string("[SPACE] Tick | ") : std::string())
                                 + "[\u2191\u2193] Navi. | [SHIFT+p] Play | [DEL] Remove | [" + hotkey_text("HKeyRescan", "R") + "] Rescan");
            hint_lines.push_back("[ENTER] Fetch full list | [" + hotkey_text("HKeyFetchTags", "B") + "] Fetch missing tag | [SHIFT+n] Fetch all tags");
            hint_lines.push_back("[SHIFT+c] Cancel fetch | [CTRL+s] Save | [CTRL+SHIFT+x] Discard | " + esc);
        }
        for (const auto& l : hint_lines) frame << legend_sgr(settings_) << truncate_str(l, W) << "\x1b[0m\n";
        if (!meta_status_.empty()) frame << "\x1b[32m" << truncate_str(meta_status_, W) << "\x1b[0m\n";
        else frame << "\n";
    }
}

// ---------------------------------------------------------------------------
// Listening history overlay (Mode::History, HKeyHistory = Shift+H)
//
// Modelled on the two overlays above: same tab strip, same boxed panel, same
// "two legend rows + one status row" footer, so its height never depends on
// what it happens to be showing. Three tabs:
//
//   1 HISTORY     the last 100 plays, newest first, each with a PLAY / done /
//                 skip marker (the live play is the top row while music runs)
//   2 TOP TRACKS  one row per title -- length and play count -- sorted by play
//                 count; 'r' flips between most- and least-played first
//   3 TOP PLAYLISTS  the top 5 saved playlists of the week / month / quarter /
//                 year / overall in one pane (history_playlists_lines())
//   4 HABITS      session / time-per-day / play-behaviour aggregates, every
//                 category under a header_sgr() header, exactly like the
//                 Settings section titles and the cheat sheet's categories
//
// It is a pure viewer: no key in here edits anything, and 'r' only flips an
// in-memory sort order. The data is maintained outside this overlay -- see
// history_end_current_play()/history_begin_current_play() below (called on
// every playback hand-over) and the frame loop's history_.add_listened().
// ---------------------------------------------------------------------------

void App::history_open() {
    mode_ = Mode::History;
    history_tab_ = 0;
    history_selected_ = 0;
    history_scroll_ = 0;
    history_pane_ = 0;
    history_add_sel_ = 0;
    history_status_.clear();
    if (history_.plays().empty())
        history_status_ = "nothing played yet in this installation";
    history_refresh_top();
    force_redraw_ = true; // full-screen takeover, same rule as entering Settings
}

void App::history_refresh_top() {
    history_top_view_ = history_top(history_.plays(), history_most_first_, &history_.archive());
}

// Closes the record of whatever is playing right now. Called on every
// hand-over (a new track replacing it, playback running out, the app
// quitting), never from anywhere that could see a record twice -- the store
// itself refuses to close one that isn't live.
void App::history_end_current_play() {
    if (!history_.live()) return;
    const HistoryPlay* p = history_.live_play();
    const double len = p ? p->len_sec : 0.0;
    const double heard = p ? p->listened_sec : 0.0;
    // "Heard to the end" = the player ran out of track, or the listener got
    // through 90% of it (which also covers seeking close to the end and then
    // letting it run out). Everything else -- a skip, an app closed
    // mid-track -- is not a completion, and that is what the Habits tab's
    // completion rate is built on.
    const bool completed = player_.finished() || (len > 0.0 && heard >= 0.9 * len);
    history_.finish_live(completed);
    history_.save(); // one small JSON write per finished track
}

// Opens a record for current_path_/current_video_id_/metadata_. Must only be
// called once those three already describe the NEW track.
void App::history_begin_current_play() {
    if (!has_track_) return;
    HistoryPlay p;
    p.id = history_track_id(current_is_local_, path_utf8(current_path_), current_video_id_);
    if (p.id.empty()) return; // nothing to aggregate by: record nothing rather than a phantom title
    p.title = metadata_.name;
    if (p.title.empty() || p.title == "-")
        p.title = path_utf8(current_path_.stem()); // untagged local file
    p.artist = (metadata_.artist == "-") ? std::string() : metadata_.artist;
    p.len_sec = static_cast<double>(total_sec_);
    p.started_at = static_cast<long long>(std::time(nullptr));
    p.playlist = current_playlist_;
    history_.begin_play(p);
}

// The smart grid of the ADD SMART HISTORY TO QUEUE pane (Top Tracks tab):
// column titles and the label of every list, [column][row].
static const char* const kSmartColTitles[4] = {"TOP TRACKS", "TOP OF THE ...", "TIME OF DAY", "REDISCOVER"};
static const char* const kSmartLabels[4][4] = {
    {"Top 10 tracks", "Top 25 tracks", "Top 50 tracks", "Top 100 tracks"},
    {"Top 25 of the week", "Top 25 of the month", "Top 25 of the quarter", "Top 25 of the year"},
    {"Morning (7-11 a.m.)", "Day (11 a.m.-6 p.m.)", "Evening (6-10 p.m.)", "Night (10 p.m.-7 a.m.)"},
    {"Last 25 newly added", "Least 25 played", "Least played: month", "Least played: year"},
};

// When a local file arrived in the library: the later of its modification
// time and its creation / status-change time (a file copied or downloaded
// into a music folder keeps an old modification time on many systems, but
// its creation time -- Windows -- or inode change time -- POSIX -- is when it
// arrived). 0 when the file cannot be read.
static long long file_added_time(const fs::path& p) {
#if defined(_WIN32)
    struct _stat64 st;
    if (_wstat64(p.c_str(), &st) != 0) return 0;
#else
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return 0;
#endif
    return static_cast<long long>(std::max(st.st_mtime, st.st_ctime));
}

void App::history_queue_rows(const std::vector<HistoryTopRow>& rows, int take, const std::string& label,
                             const std::string& empty_msg) {
    if (rows.empty()) { history_status_ = empty_msg; return; }
    const int n = std::min(take, static_cast<int>(rows.size()));
    int added = 0, missing = 0;
    for (int i = 0; i < n; ++i) {
        const HistoryTopRow& r = rows[static_cast<size_t>(i)];
        const std::string artist = (r.artist == "-") ? std::string() : r.artist;
        if (r.id.compare(0, 3, "yt:") == 0) {
            queue_.push_back({false, r.title, artist, {}, r.id.substr(3)});
            ++added;
        } else {
            fs::path path = path_from_utf8(r.id);
            std::error_code ec;
            if (!fs::exists(path, ec)) { ++missing; continue; } // moved/deleted since it was played
            const std::string title = r.title.empty() ? path_utf8(path.stem()) : r.title;
            queue_.push_back({true, title, artist, path, ""});
            ++added;
        }
    }
    clamp_queue_selected();
    queue_reveal(static_cast<int>(queue_.size()) - 1);
    std::string msg = "queued " + std::to_string(added) + " tracks: " + label;
    if (n < take) msg += " (only " + std::to_string(n) + " available)";
    if (missing > 0) msg += " (" + std::to_string(missing) + " missing, skipped)";
    history_status_ = msg;
    log_event(msg);
}

void App::history_add_top_to_queue(int n) {
    // Ranked independently of the list above (which 'r' can flip to
    // least-played first): "top N" always means the N most-played titles.
    history_queue_rows(history_top(history_.plays(), /*most_first=*/true, &history_.archive()), n,
                       "Top " + std::to_string(n) + " tracks", "nothing played yet -- nothing to queue");
}

void App::history_queue_smart(int col, int row) {
    col = std::clamp(col, 0, 3);
    row = std::clamp(row, 0, 3);
    const std::string label = kSmartLabels[col][row];
    const long long now = static_cast<long long>(std::time(nullptr));
    const HistoryArchive* arch = &history_.archive();
    if (col == 0) { history_add_top_to_queue(kHistoryAddCounts[row]); return; }
    if (col == 1) {   // top of the last 7 / 30 / 91 / 365 days
        static const int kDays[4] = {7, 30, 91, 365};
        history_queue_rows(history_top_since(history_.plays(), arch, now - kDays[row] * 86400LL, true),
                           kSmartListSize, label, "nothing played in that time -- nothing to queue");
        return;
    }
    if (col == 2) {   // morning / day / evening / night
        history_queue_rows(history_top_tod(history_.plays(), arch, row, true),
                           kSmartListSize, label, "nothing played at that time of day yet -- nothing to queue");
        return;
    }
    if (row == 2 || row == 3) {   // least played among what was played in the last 30 / 365 days
        history_queue_rows(history_top_since(history_.plays(), arch, now - (row == 2 ? 30 : 365) * 86400LL, false),
                           kSmartListSize, label, "nothing played in that time -- nothing to queue");
        return;
    }
    // Rows 0 and 1 work on the local library itself, not on the history.
    if (all_local_tracks_.empty()) { history_status_ = "the local library is empty -- nothing to queue"; return; }
    std::vector<HistoryTopRow> rows;
    if (row == 0) {   // the newest arrivals in the music folders
        std::vector<std::pair<long long, size_t>> by_time;
        by_time.reserve(all_local_tracks_.size());
        for (size_t i = 0; i < all_local_tracks_.size(); ++i)
            by_time.push_back({file_added_time(all_local_tracks_[i].path), i});
        std::stable_sort(by_time.begin(), by_time.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t k = 0; k < by_time.size() && static_cast<int>(rows.size()) < kSmartListSize; ++k) {
            const LocalTrack& t = all_local_tracks_[by_time[k].second];
            HistoryTopRow r;
            r.id = path_utf8(t.path); r.title = t.title; r.artist = t.folder_artist;
            rows.push_back(r);
        }
    } else {          // the whole library, fewest plays first -- never played counts as 0
        std::map<std::string, int> plays;
        for (const auto& r : history_top(history_.plays(), true, arch)) plays[r.id] = r.plays;
        std::vector<std::pair<int, size_t>> by_plays;
        by_plays.reserve(all_local_tracks_.size());
        for (size_t i = 0; i < all_local_tracks_.size(); ++i) {
            auto it = plays.find(path_utf8(all_local_tracks_[i].path));
            by_plays.push_back({it == plays.end() ? 0 : it->second, i});
        }
        // Shuffled first, so tracks with the same count come in a different
        // order every time (otherwise "least played" would always be the
        // same alphabetical handful of never-played files).
        std::shuffle(by_plays.begin(), by_plays.end(), std::mt19937(std::random_device{}()));
        std::stable_sort(by_plays.begin(), by_plays.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t k = 0; k < by_plays.size() && static_cast<int>(rows.size()) < kSmartListSize; ++k) {
            const LocalTrack& t = all_local_tracks_[by_plays[k].second];
            HistoryTopRow r;
            r.id = path_utf8(t.path); r.title = t.title; r.artist = t.folder_artist;
            rows.push_back(r);
        }
    }
    history_queue_rows(rows, kSmartListSize, label, "the local library is empty -- nothing to queue");
}

void App::handle_history_key(int key) {
    if (key == 0) return;
    const bool arrow = last_key_was_arrow();

    // ESC in the smart grid only gives the focus back to the track list.
    if (key == 27 && history_tab_ == 1 && history_pane_ == 1) { history_pane_ = 0; history_status_.clear(); return; }
    // Leaving: ESC, or SHIFT+h again -- the very key that opened this
    // (nothing here is a text field, so an uppercase 'H' is always that key).
    if (key == 27 || key_is("HKeyHistory", key)) { mode_ = Mode::Browse; return; }

    // Top Tracks tab: TAB switches between the two panes (the track list and
    // ADD SMART HISTORY TO QUEUE). It never switches tabs (Left/Right do).
    if (key == 9) {
        if (history_tab_ == 1) { history_pane_ ^= 1; history_status_.clear(); }
        return;
    }

    // Smart grid focused: all four arrows move in the grid (Left/Right do NOT
    // switch tabs here), Enter queues the highlighted list.
    if (history_tab_ == 1 && history_pane_ == 1) {
        if (arrow && key == 'A') { if (history_add_sel_ > 0) --history_add_sel_; return; }
        if (arrow && key == 'B') { if (history_add_sel_ < 3) ++history_add_sel_; return; }
        if (arrow && key == 'D') { if (history_add_col_ > 0) --history_add_col_; return; }
        if (arrow && key == 'C') { if (history_add_col_ < 3) ++history_add_col_; return; }
        if (key == '\r' || key == '\n') { history_queue_smart(history_add_col_, history_add_sel_); return; }
    }

    // Tab strip: Left/Right cycle, 1/2/3/4 jump outright.
    if (arrow && (key == 'C' || key == 'D')) {
        // 'D' is Left, 'C' is Right -- the same convention the seek keys and
        // the caret movement above use (see handle_key()'s "key == 'D' //
        // left = seek back"). Swapped here, every arrow press walked the tab
        // strip backwards, which on a 3-tab strip looks like jumping 1->3.
        const int dir = (key == 'D') ? -1 : 1;
        history_tab_ = (history_tab_ + dir + kHistTabs) % kHistTabs;
        history_selected_ = 0;
        history_scroll_ = 0;
        history_pane_ = 0;
        history_status_.clear();
        return;
    }
    if (key == '1' || key == '2' || key == '3' || key == '4') {
        history_tab_ = key - '1';
        history_selected_ = 0;
        history_scroll_ = 0;
        history_pane_ = 0;
        history_status_.clear();
        return;
    }

    // Enter on a track of the HISTORY / TOP TRACKS list: play it right away (the overlay stays open).
    if (!arrow && (key == '\r' || key == '\n') && history_tab_ != kHistTabPlaylists && history_tab_ != kHistTabHabits) {
        std::string id, title, artist;
        const auto& plays = history_.plays();
        if (history_tab_ == 0) {
            const int n = static_cast<int>(std::min<size_t>(100, plays.size()));
            if (history_selected_ >= 0 && history_selected_ < n) {
                const HistoryPlay& p = plays[static_cast<size_t>(history_selected_)];
                id = p.id; title = p.title; artist = p.artist;
            }
        } else if (history_selected_ >= 0 && history_selected_ < static_cast<int>(history_top_view_.size())) {
            const HistoryTopRow& r = history_top_view_[static_cast<size_t>(history_selected_)];
            id = r.id; title = r.title; artist = r.artist;
        }
        if (id.empty()) return;
        if (artist == "-") artist.clear();
        if (id.compare(0, 3, "yt:") == 0) {
            OnlineResult r{id.substr(3), title, artist};
            start_online_track(r);
            history_status_ = "playing: " + title;
        } else {
            fs::path path = path_from_utf8(id);
            std::error_code ec;
            if (!fs::exists(path, ec)) { history_status_ = "cannot play: the file no longer exists"; return; }
            LocalTrack t{path_utf8(path.stem()), path, artist};
            start_local_track(t);
            history_status_ = "playing: " + (title.empty() ? path_utf8(path.stem()) : title);
        }
        return;
    }

    // Up/Down: a cursor on the two list tabs, plain scrolling on Top Playlists / Habits
    // (whose content has no cursor and may be taller than the panel).
    const bool scroll_tab = (history_tab_ == kHistTabPlaylists || history_tab_ == kHistTabHabits);
    const int total = (history_tab_ == 0)
        ? static_cast<int>(std::min<size_t>(100, history_.plays().size()))
        : static_cast<int>(history_top_view_.size());
    if (arrow && key == 'A') {
        if (scroll_tab) { if (history_scroll_ > 0) --history_scroll_; }
        else if (history_selected_ > 0) --history_selected_;
        return;
    }
    if (arrow && key == 'B') {
        if (scroll_tab) ++history_scroll_; // clamped against the real content while drawing
        else if (history_selected_ + 1 < total) ++history_selected_;
        return;
    }

    // 'r' flips the Top Tracks order: most played on top (the default) and
    // least played on top. Nowhere else to do it, so it is a no-op there
    // rather than a surprise.
    if (key == 'r' || key == 'R') {
        if (history_tab_ != 1) return;
        history_most_first_ = !history_most_first_;
        history_refresh_top();
        if (history_selected_ >= static_cast<int>(history_top_view_.size()))
            history_selected_ = std::max(0, static_cast<int>(history_top_view_.size()) - 1);
        history_status_ = history_most_first_ ? "TOP TRACKS: most played first"
                                              : "TOP TRACKS: least played first";
        return;
    }
    if (key == 'q' || key == 'Q') mode_ = Mode::Browse;
}

void App::build_history_screen(std::ostringstream& frame, int W, int target_height) {
    // Rebuilt every frame: a track can finish (and another start) while this
    // overlay is on screen -- playback never pauses for it.
    history_refresh_top();

    if (W < 60) W = 60;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string HI = "\x1b[7m", R = "\x1b[0m";

    {   // one fused pane: LISTENING HISTORY rests on the top border line, the settings-style tab strip below it (no numbers)
        const auto strip = menu_tab_strip(W, "LISTENING HISTORY", {"HISTORY", "TOP TRACKS", "TOP PLAYLISTS", "HABITS"}, history_tab_, border, border,
                                          settings_.box_upper_left, settings_.box_upper_right, settings_.box_vertical, 0, "", 0,
                                          ansi_for(settings_.tab_current_color, false), ansi_for(settings_.tab_other_color, false));
        frame << strip.first << "\n" << strip.second << "\n";
    }

    // Chrome around the rows: the two strip lines + the bottom border = 3 (the panel adds its bottom border itself,
    // so only 1 is counted here besides the "- 2" below).
    int fixed_rows = 1;

    // Footer first, because the panel takes exactly the rows it leaves: the
    // legend (as many rows as it needs at this width -- one on a 120-column
    // terminal) plus a status row ONLY while a message is showing. Nothing is
    // reserved to sit blank any more (there used to be three fixed rows, two
    // of them empty on a normal terminal); while a message is up the list
    // simply gives up one row for it, the way Browse does for its prompt.
    const std::string hint = (history_tab_ == 1 && history_pane_ == 1)
        ? "[\u2191\u2193\u2190\u2192] Pick a list | [ENTER] Add it to the queue | [TAB] Track list | [ESC] Leave the pane"
        : (history_tab_ == 1)
        ? "[\u2190\u2192] Switch Tab | [1-4] Tab | [TAB] Smart history pane | [\u2191\u2193] Move | [ENTER] Play | [r] Flip sort | [ESC] Exit"
        : (history_tab_ == kHistTabPlaylists || history_tab_ == kHistTabHabits)
        ? "[\u2190\u2192] Switch Tab | [1-4] Tab | [\u2191\u2193] Scroll | [ESC] Exit"
        : "[\u2190\u2192] Switch Tab | [1-4] Tab | [\u2191\u2193] Move | [ENTER] Play | [ESC] Exit";
    std::vector<std::string> hint_lines(1);
    size_t pos = 0;
    for (;;) {
        size_t sep = hint.find(" | ", pos);
        std::string entry = (sep == std::string::npos) ? hint.substr(pos) : hint.substr(pos, sep - pos);
        std::string& cur = hint_lines.back();
        std::string joined = cur.empty() ? entry : cur + " | " + entry;
        if (cur.empty() || display_width(joined) <= W || hint_lines.size() >= 2) cur = joined;
        else hint_lines.push_back(entry);
        if (sep == std::string::npos) break;
        pos = sep + 3;
    }
    const int legend_rows = static_cast<int>(hint_lines.size());
    const int status_rows = history_status_.empty() ? 0 : 1;

    // Same two bounds build_meta_screen() sizes against: term_rows_ - 1 is
    // what clamp_output_rows() keeps, target_height is what the other
    // overlays use. 2 = the panel's own border rows.
    int budget = std::min(target_height, term_rows_ - 1);
    // Top Tracks carries a second pane (ADD SMART HISTORY TO QUEUE: a title
    // row + four option rows + its own two border rows) below the list, so the list gives those
    // rows up -- the whole tab still adds up to `budget` lines (29 on a
    // 30-line terminal). No upper cap on the list: it fills a maximised window.
    const bool top_tab = (history_tab_ == 1);
    const int add_body = 5;                       // column titles + 4 rows of lists
    const int add_rows = top_tab ? add_body + 2 : 0;
    int panel_h = std::max(top_tab ? 3 : 6, budget - fixed_rows - 2 - legend_rows - status_rows - add_rows);
    for (const auto& l : build_history_panel(W, panel_h)) frame << l << "\n";
    if (top_tab)
        for (const auto& l : build_history_add_panel(W, add_body)) frame << l << "\n";

    for (const auto& l : hint_lines)
        frame << legend_sgr(settings_) << truncate_str(l, W) << "\x1b[0m\n";
    if (status_rows) frame << "\x1b[32m" << truncate_str(history_status_, W) << "\x1b[0m\n";
}

std::vector<std::string> App::build_history_panel(int total_width, int height) {
    const int inner = std::max(20, total_width - 4);
    const int body = std::max(1, height); // rows between box_top() and box_bottom()
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    std::vector<std::string> out;

    // The info that sits on the bottom border line.
    std::string info;
    if (history_tab_ == 0) info = std::to_string(history_.plays().size()) + " plays (newest 100 shown)";
    else if (history_tab_ == 1) info = history_most_first_ ? "most played first" : "least played first";
    else if (history_tab_ == kHistTabPlaylists) info = "top 5 each";

    // Column helpers. Both are only ever fed PLAIN text: display_width()
    // counts escape bytes as columns, so a decorated string must be padded
    // first and coloured after (same rule every other panel here follows).
    auto L = [](const std::string& s, int n) { // right-align a value (by display width: the PLAYS arrow is 3 bytes, 1 column)
        const int w = display_width(s);
        if (w >= n) return truncate_str(s, n);
        return std::string(static_cast<size_t>(n - w), ' ') + s;
    };
    auto T = [&](const std::string& s, int n) { // left-align, truncated at display width
        std::string t = truncate_str(s, n);
        const int fill = n - display_width(t);
        return t + (fill > 0 ? std::string(static_cast<size_t>(fill), ' ') : std::string());
    };
    auto row = [&](const std::string& plain, const std::string& sgr) {
        out.push_back(bar + " " + sgr + pad_right(truncate_str(plain, inner), inner) + R + " " + bar);
    };
    // A section header: only the title carries the colour, like every other
    // header in the app -- a full-width bar would read as a selected row.
    auto head = [&](const std::string& plain) {
        std::string t = truncate_str(plain, inner);
        out.push_back(bar + " " + header_sgr(settings_) + t + R +
                      std::string(static_cast<size_t>(std::max(0, inner - display_width(t))), ' ') + " " + bar);
    };
    auto blank = [&]() { out.push_back(bar + " " + std::string(static_cast<size_t>(inner), ' ') + " " + bar); };

    // text + style: 0 plain, 1 section header, 2 cursor row
    std::vector<std::pair<std::string, int>> lines;
    int cursor = -1; // index of the selected line, for the two list tabs

    if (history_tab_ == 0) {
        const auto& plays = history_.plays();
        const int total = static_cast<int>(std::min<size_t>(100, plays.size()));
        const int when_w = 11, len_w = 7, state_w = 5;
        const int title_w = std::max(8, inner - when_w - 2 - len_w - 2 - state_w);
        lines.push_back({T("WHEN", when_w) + "  " + T("TITLE", title_w) + L("LEN", len_w) + "  " + T("STATE", state_w), 1});
        if (total == 0) {
            lines.push_back({"nothing played yet -- plays show up here as soon as music runs", 0});
        } else {
            for (int i = 0; i < total; ++i) {
                const HistoryPlay& p = plays[i];
                std::string t = (p.artist.empty() || p.artist == "-") ? p.title
                                                                     : p.artist + " - " + p.title;
                if (t.empty() || t == "-") t = p.id;
                const bool live = (i == 0 && history_.live());
                lines.push_back({T(format_when(p.started_at), when_w) + "  " + T(t, title_w) +
                                 L(format_mmss(p.len_sec), len_w) + "  " +
                                 T(live ? "PLAY" : (p.finished ? "done" : "skip"), state_w),
                                 0});
            }
            cursor = history_selected_ + 1; // +1 for the column header above the rows
        }
    } else if (history_tab_ == 1) {
        const int len_w = 7, plays_w = 7;
        const int title_w = std::max(8, inner - len_w - 2 - plays_w);
        lines.push_back({T("TITLE", title_w) + L("LEN", len_w) + "  " + std::string(static_cast<size_t>(std::max(0, plays_w - 7)), ' ') + "PLAYS " + (history_most_first_ ? "\u25bc" : "\u25b2"), 1});
        { std::string rule; for (int i = 0; i < inner; ++i) rule += "\u2500"; lines.push_back({rule, 3}); }
        if (history_top_view_.empty()) {
            lines.push_back({"nothing played yet -- plays show up here as soon as music runs", 0});
        } else {
            for (int i = 0; i < static_cast<int>(history_top_view_.size()); ++i) {
                const HistoryTopRow& r = history_top_view_[static_cast<size_t>(i)];
                std::string t = r.title.empty() ? r.id : r.title;
                lines.push_back({T(t, title_w) + L(format_mmss(r.len_sec), len_w) + "  " +
                                 L(std::to_string(r.plays), plays_w), 0});
            }
            cursor = history_selected_ + 2; // +2 for the column header and the rule above the rows
        }
    } else if (history_tab_ == kHistTabPlaylists) {
        history_playlists_lines(inner, lines);
        cursor = -1; // no cursor: this tab scrolls like Habits
    } else {
        // --- Habits: three categories, each under a highlighted header ---
        const HistoryStats s = history_stats(history_.plays(), &history_.archive());
        const int lbl_w = std::max(20, inner - 20);
        auto num1 = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return std::string(b); };
        auto pct = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.0f", v * 100.0); return std::string(b); };
        auto cat = [&](const char* name) { if (!lines.empty()) lines.push_back({std::string(), 0}); lines.push_back({std::string(name), 1}); };
        auto item = [&](const std::string& label, const std::string& value) {
            lines.push_back({T(label, lbl_w) + value, 0});
        };
        cat("SESSIONS");
        item("Sessions", std::to_string(s.sessions));
        item("Average session length", format_len(s.avg_session_sec));
        item("Tracks per session", num1(s.tracks_per_session));
        cat("TIME PLAYED PER DAY");
        item("Played today", format_len(s.today_sec));
        item("Average day with music", format_len(s.avg_day_sec));
        item("Days with music", std::to_string(s.days));
        item("Total listened", format_len(s.total_sec));
        cat("PLAY BEHAVIOUR");
        item("Tracks played", std::to_string(s.plays));
        item("Finished (played to the end)", std::to_string(s.finished));
        item("Skipped", std::to_string(s.skipped));
        item("Replays", std::to_string(s.replays));
        item("Completion rate", pct(s.completion) + "%");
        cat("LISTENING BY HOUR OF THE DAY");
        {
            static const char* lv[9] = {" ", "\u2581", "\u2582", "\u2583", "\u2584", "\u2585", "\u2586", "\u2587", "\u2588"};
            double mx = 0;
            for (double v : s.hours) mx = std::max(mx, v);
            std::string bars = "  ", labs = "  ";
            for (int h = 0; h < 24; ++h) {
                const int l = mx > 0 ? static_cast<int>(std::ceil(s.hours[static_cast<size_t>(h)] / mx * 8.0 - 1e-9)) : 0;
                const std::string g = lv[std::clamp(l, 0, 8)];
                bars += " " + g + g + " ";
                char b[8]; std::snprintf(b, sizeof b, " %02d ", h);
                labs += b;
            }
            lines.push_back({bars, 0});
            lines.push_back({labs, 0});
        }
        cat("LISTENING BY WEEKDAY");
        {
            static const char* wd[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
            double mx = 0;
            for (double v : s.weekdays) mx = std::max(mx, v);
            for (int d = 0; d < 7; ++d) {
                const double v = s.weekdays[static_cast<size_t>(d)];
                const int len = mx > 0 ? static_cast<int>(std::lround(v / mx * 40.0)) : 0;
                std::string bar40;
                for (int i = 0; i < len; ++i) bar40 += "\u2588";
                lines.push_back({"  " + T(wd[d], 11) + bar40 + std::string(static_cast<size_t>(40 - len), ' ') + "  " + format_len(v), 0});
            }
        }
        cursor = -1; // no cursor: this tab scrolls
    }

    // Window over the content: around the cursor on the two list tabs,
    // from history_scroll_ on Habits (whose scroll offset is clamped here,
    // where the real content height is finally known).
    int start = 0;
    if (cursor >= 0) {
        // Same behaviour as the file list: the column header stays put and the window only moves when the
        // cursor leaves it (no re-centering on every step).
        const int hdr = cursor - history_selected_;                      // fixed lines above the rows
        const int rows_total = static_cast<int>(lines.size()) - hdr;
        const int vis = std::max(1, body - hdr);
        int top = history_view_top_;
        if (history_selected_ < top) top = history_selected_;
        if (history_selected_ >= top + vis) top = history_selected_ - vis + 1;
        top = std::clamp(top, 0, std::max(0, rows_total - vis));
        history_view_top_ = top;
        lines.erase(lines.begin() + hdr, lines.begin() + hdr + top);     // scroll the rows, keep the header
        cursor -= top;
        start = 0;
    } else {
        const int max_scroll = std::max(0, static_cast<int>(lines.size()) - body);
        history_scroll_ = std::clamp(history_scroll_, 0, max_scroll);
        start = std::min(history_scroll_, max_scroll);
    }

    int shown = 0;
    for (int k = start; k < static_cast<int>(lines.size()) && shown < body; ++k, ++shown) {
        const std::string& text = lines[static_cast<size_t>(k)].first;
        const int style = lines[static_cast<size_t>(k)].second;
        if (style == 3) out.push_back(bar + " " + legend_sgr(settings_) + text + R + " " + bar);
        else if (style == 4) out.push_back(bar + " " + text + " " + bar);
        else if (style == 1) head(text);
        else if (style == 2 || (k == cursor && !(history_tab_ == 1 && history_pane_ == 1)))
            row(text, cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color));
        else row(text, "");
    }
    while (shown < body) { blank(); ++shown; }

    out.push_back(box_bottom(total_width, info, border_ansi_bottom));
    return out;
}

// TOP PLAYLISTS tab: the top 5 saved playlists of the week, month, quarter, year and overall in ONE pane. Wide
// terminals get two columns (week / month / quarter on the left, year / overall / summary on the right), narrow
// ones one column that scrolls like Habits. Style per line: 0 plain, 1 section header, 3 legend-coloured rule.
void App::history_playlists_lines(int inner, std::vector<std::pair<std::string, int>>& lines) const {
    struct Period { const char* title; long long days; };   // 0 days = overall
    static const Period kPeriods[5] = {
        {"TOP PLAYLISTS OF THE WEEK", 7}, {"TOP PLAYLISTS OF THE MONTH", 30}, {"TOP PLAYLISTS OF THE QUARTER", 91},
        {"TOP PLAYLISTS OF THE YEAR", 365}, {"TOP PLAYLIST OVERALL", 0}};
    const long long now = static_cast<long long>(std::time(nullptr));
    const bool two = inner >= 84;
    const std::string sep_txt = " " + (settings_.list_separator.empty() ? std::string("|") : settings_.list_separator) + " ";
    const int sep_w = display_width(sep_txt);
    const int col_w = two ? (inner - sep_w) / 2 : inner;
    const int plays_w = 9, time_w = 8;
    const int name_w = std::max(6, col_w - 3 - plays_w - time_w - 2);
    auto T = [](const std::string& s, int n) {
        std::string t = truncate_str(s, n);
        const int fill = n - display_width(t);
        return t + (fill > 0 ? std::string(static_cast<size_t>(fill), ' ') : std::string());
    };
    auto R = [](const std::string& s, int n) {
        const int w = display_width(s);
        return w >= n ? truncate_str(s, n) : std::string(static_cast<size_t>(n - w), ' ') + s;
    };
    auto section = [&](int p) {
        std::vector<std::pair<std::string, int>> out;
        out.push_back({T(kPeriods[p].title, col_w), 1});
        const auto rows = history_top_playlists(history_.plays(), &history_.archive(),
                                                kPeriods[p].days > 0 ? now - kPeriods[p].days * 86400LL : 0, 5);
        for (size_t i = 0; i < 5; ++i) {
            if (i < rows.size()) {
                const auto& r = rows[i];
                out.push_back({std::to_string(i + 1) + ". " + T(r.name, name_w) + "  " +
                               R(std::to_string(r.plays) + (r.plays == 1 ? " play" : " plays"), plays_w) +
                               R(format_len(r.listened_sec), time_w), 0});
            } else {
                out.push_back({std::to_string(i + 1) + ". " + (i == 0 ? "-- no playlist played in this period --" : "-"), 3});
            }
        }
        return out;
    };
    std::vector<std::vector<std::pair<std::string, int>>> sec;
    for (int p = 0; p < 5; ++p) sec.push_back(section(p));

    // Summary block (fills the free spot of the right column).
    int n_playlists = 0;
    long long n_plays = 0;
    history_playlist_totals(history_.plays(), &history_.archive(), n_playlists, n_plays);
    const HistoryStats st = history_stats(history_.plays(), &history_.archive());
    std::vector<std::pair<std::string, int>> sum;
    sum.push_back({T("SUMMARY", col_w), 1});
    sum.push_back({T("Playlists played", col_w - 8) + R(std::to_string(n_playlists), 8), 0});
    sum.push_back({T("Plays from playlists", col_w - 8) + R(std::to_string(n_plays), 8), 0});
    sum.push_back({T("Share of all plays", col_w - 8) +
                   R(st.plays > 0 ? std::to_string(std::min(100, static_cast<int>(100.0 * static_cast<double>(n_plays) / st.plays + 0.5))) + "%" : "-", 8), 0});
    if (n_plays == 0) {
        sum.push_back({T("Queue a saved playlist (Enter on a playlist row) -- its plays are counted from then on.", col_w), 3});
    }

    auto append_col = [&](std::vector<std::pair<std::string, int>>& dst, const std::vector<std::vector<std::pair<std::string, int>>>& parts) {
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) dst.push_back({std::string(), 0});
            for (const auto& l : parts[i]) dst.push_back(l);
        }
    };
    if (!two) {
        append_col(lines, {sec[0], sec[1], sec[2], sec[3], sec[4], sum});
        return;
    }
    std::vector<std::pair<std::string, int>> left, right;
    append_col(left, {sec[0], sec[1], sec[2]});
    append_col(right, {sec[3], sec[4], sum});
    const size_t n = std::max(left.size(), right.size());
    const std::string Z = "\x1b[0m";
    const std::string sep = ansi_for(settings_.border_color, false) + sep_txt + Z;
    auto paint = [&](const std::pair<std::string, int>& l, int w) {
        const std::string t = pad_right(truncate_str(l.first, w), w);
        if (l.second == 1) return header_sgr(settings_) + t + Z;
        if (l.second == 3) return legend_sgr(settings_) + t + Z;
        return t;
    };
    const std::pair<std::string, int> none{std::string(), 0};
    const int right_w = inner - sep_w - col_w;
    for (size_t i = 0; i < n; ++i)   // style 4 = already painted, exactly `inner` columns wide
        lines.push_back({paint(i < left.size() ? left[i] : none, col_w) + sep +
                         paint(i < right.size() ? right[i] : none, right_w), 4});
}

// Top Tracks tab, second pane: ADD SMART HISTORY TO QUEUE, a 4 x 4 grid of
// lists (kSmartLabels) under a row of column titles, the columns split by the
// list separator in the border colour. The cursor is only drawn while this
// pane has focus (history_pane_ == 1).
std::vector<std::string> App::build_history_add_panel(int total_width, int height) const {
    const int inner = std::max(20, total_width - 4);
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    std::vector<std::string> out;
    out.push_back(box_top(std::string("ADD SMART HISTORY TO QUEUE") + (history_pane_ == 1 ? focus_indicator(settings_, border_ansi) : ""),
                          total_width, border_ansi));

    const std::string sep_txt = " " + (settings_.list_separator.empty() ? std::string("|") : settings_.list_separator) + " ";
    const int sep_w = display_width(sep_txt);
    const int col_w = std::max(4, (inner - 3 * sep_w) / 4);
    const int last_w = std::max(4, inner - 3 * sep_w - 3 * col_w);   // the last column takes the rounding rest
    const std::string sep = border_ansi + sep_txt + R;
    const bool focused = (history_pane_ == 1);
    const int played = static_cast<int>(history_top_view_.size()); // distinct titles, whatever the sort
    for (int i = 0; i < std::max(1, height); ++i) {
        std::string line;
        for (int c = 0; c < 4; ++c) {
            const int w = (c == 3) ? last_w : col_w;
            std::string text, sgr;
            if (i == 0) {
                text = kSmartColTitles[c];
                sgr = header_sgr(settings_);
            } else if (i <= 4) {
                const int row = i - 1;
                text = kSmartLabels[c][row];
                if (c == 0 && played < kHistoryAddCounts[row])
                    text += played == 0 ? " (none yet)" : " (" + std::to_string(played) + ")";
                if (focused && c == history_add_col_ && row == history_add_sel_)
                    sgr = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            }
            if (c > 0) line += sep;
            line += sgr + pad_right(truncate_str(text, w), w) + R;
        }
        out.push_back(bar + " " + line + " " + bar);
    }
    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}


// ---------------------------------------------------------------------------
// READ LYRICS (Mode::ReadLyrics, CTRL+L)
//
// A full-screen reader for the lyrics saved for a track. Two one-line panes on top: READ LYRICS (the lyrics files of
// the track) and TRANSLATION (the translations saved for the chosen file); TAB moves the keys between them. Below:
// the plain lyrics of the chosen file, timestamps removed, centred. `t` opens the translate overlay (service, language
// to translate into, language of the lyrics) and ENTER there fetches a translation, which is saved in
// <track folder>/lyrics/translations/ and shown in "( )" right below every original line. SHIFT+t only shows / hides
// the saved translations (and steps through them when there are several).
//
// A translation takes a while. It runs on a worker thread that does not depend on the overlay: it goes on when the
// translate overlay is closed, when another file is chosen and even when READ LYRICS is closed (it is saved when it is
// done). Asking for a second one meanwhile is answered with a warning.
// ---------------------------------------------------------------------------

static std::string n_files_label(size_t n) {
    return n == 0 ? std::string() : std::to_string(n) + (n == 1 ? " lyrics file" : " lyrics files");
}

void App::readlyrics_open() {
    fs::path track;
    std::string title;
    if (has_track_ && !current_path_.empty()) {
        track = current_path_;
        title = (metadata_.artist == "-" || metadata_.artist.empty()) ? metadata_.name : metadata_.artist + " - " + metadata_.name;
    } else if (list_source_ == ListSource::Local && selected_ >= 0 && selected_ < static_cast<int>(local_view_.size())) {
        const LocalTrack& t = local_view_[static_cast<size_t>(selected_)];
        track = t.path;
        title = t.folder_artist.empty() ? t.title : t.folder_artist + " - " + t.title;
    }
    if (track.empty()) { status_line_ = "no track to read lyrics of -- play or hover a track first"; return; }
    rd_track_ = track;
    rd_title_ = title.empty() ? path_utf8(track.stem()) : title;
    rd_versions_ = list_lyrics_versions(track);
    rd_sel_ = 0;
    for (size_t i = 0; i < rd_versions_.size(); ++i) if (rd_versions_[i].active) { rd_sel_ = static_cast<int>(i); break; }
    rd_first_ = 0;
    rd_pane_ = 0;
    rd_confirm_del_ = false;
    rd_opts_open_ = false;
    rd_search_open_ = false;
    rd_opts_warn_.clear();
    rd_status_.clear();
    rd_status_warn_ = false;
    if (rd_busy_) { rd_status_ = "a translation is still running (" + language_name(rd_job_to_) + " with " + translate_service_name(rd_job_svc_) + ")"; }
    readlyrics_load_version();
    mode_ = Mode::ReadLyrics;
    force_redraw_ = true;   // full-screen takeover
}

void App::readlyrics_close() {
    if (rd_opts_dirty_) { save_settings(settings_); rd_opts_dirty_ = false; }
    rd_opts_open_ = false;
    rd_search_open_ = false;
    rd_confirm_del_ = false;
    // A translation that is still running is NOT stopped: it is saved when done (readlyrics_poll() tells).
    mode_ = Mode::Browse;
    force_redraw_ = true;
}

void App::readlyrics_load_version() {
    rd_lines_.clear();
    rd_trans_.clear();
    rd_tlist_.clear();
    rd_from_.clear();
    rd_show_ = rd_same_ = false;
    rd_scroll_ = 0;
    rd_tsel_ = rd_tfirst_ = 0;
    rd_confirm_del_ = false;
    if (rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) return;
    const LyricsVersion& v = rd_versions_[static_cast<size_t>(rd_sel_)];
    rd_lines_ = lyrics_plain_lines(read_lyrics_version_text(rd_track_, v));
    readlyrics_refresh_translations(settings_.translate_to);
}

// Reads the saved translations of the chosen file again; `prefer_to` is the language to select when it is there.
void App::readlyrics_refresh_translations(const std::string& prefer_to) {
    rd_tlist_.clear();
    rd_same_ = false;
    if (rd_lines_.empty() || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) { readlyrics_select_translation(-1); return; }
    const LyricsVersion& v = rd_versions_[static_cast<size_t>(rd_sel_)];
    rd_tlist_ = list_lyrics_translations(rd_track_, v, rd_lines_);
    int pick = rd_tlist_.empty() ? -1 : 0;
    for (size_t i = 0; i < rd_tlist_.size(); ++i) if (rd_tlist_[i].to == prefer_to) { pick = static_cast<int>(i); break; }
    readlyrics_select_translation(pick);
    LyricsTranslation note;   // "these lyrics are in <language> already", saved by an earlier try
    if (load_lyrics_translation(rd_track_, v, settings_.translate_to, rd_lines_, note) && note.same) rd_same_ = true;
}

void App::readlyrics_select_translation(int index) {
    rd_trans_.clear();
    rd_from_.clear();
    rd_to_.clear();
    rd_svc_.clear();
    if (index < 0 || index >= static_cast<int>(rd_tlist_.size()) || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) {
        rd_tsel_ = 0;
        return;
    }
    rd_tsel_ = index;
    const TranslationInfo& info = rd_tlist_[static_cast<size_t>(index)];
    LyricsTranslation t;
    if (load_lyrics_translation(rd_track_, rd_versions_[static_cast<size_t>(rd_sel_)], info.to, rd_lines_, t) && !t.same) {
        rd_trans_ = std::move(t.lines);
        rd_from_ = t.from;
        rd_svc_ = t.service;
        rd_to_ = info.to;
    }
}

// SHIFT+t: hidden -> the chosen translation -> the next one ... -> hidden. Never fetches anything.
void App::readlyrics_toggle_translations() {
    rd_status_warn_ = false;
    if (rd_tlist_.empty()) {
        rd_status_ = rd_busy_ ? "no translation yet -- the running one is not finished" : "no translation saved for this file -- [t] fetches one";
        rd_status_warn_ = true;
        return;
    }
    const int n = static_cast<int>(rd_tlist_.size());
    if (!rd_show_) {
        rd_show_ = true;
    } else if (rd_tsel_ < n - 1) {
        readlyrics_select_translation(rd_tsel_ + 1);
    } else {
        rd_show_ = false;
        readlyrics_select_translation(0);
    }
    if (rd_show_) {
        const TranslationInfo& t = rd_tlist_[static_cast<size_t>(rd_tsel_)];
        rd_status_ = "showing the " + language_name(t.to) + " translation" + (n > 1 ? " (" + std::to_string(rd_tsel_ + 1) + "/" + std::to_string(n) + ", SHIFT+t for the next)" : std::string());
    } else {
        rd_status_ = "translations hidden";
    }
}

void App::readlyrics_delete_translation() {
    rd_confirm_del_ = false;
    if (rd_tsel_ < 0 || rd_tsel_ >= static_cast<int>(rd_tlist_.size()) || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) return;
    const std::string to = rd_tlist_[static_cast<size_t>(rd_tsel_)].to;
    const bool ok = delete_lyrics_translation(rd_track_, rd_versions_[static_cast<size_t>(rd_sel_)], to);
    const int keep = rd_tsel_;
    readlyrics_refresh_translations("");
    readlyrics_select_translation(rd_tlist_.empty() ? -1 : std::min(keep, static_cast<int>(rd_tlist_.size()) - 1));
    if (rd_tlist_.empty()) rd_show_ = false;
    rd_status_warn_ = !ok;
    rd_status_ = ok ? "deleted the " + language_name(to) + " translation" : "could not delete the " + language_name(to) + " translation";
}

// ENTER in the translate overlay: fetch a translation with the service and languages set there.
void App::readlyrics_translate() {
    rd_opts_warn_.clear();
    if (rd_busy_) {
        rd_opts_warn_ = "WARNING: the last translation is not finished yet -- please wait";
        return;
    }
    if (rd_lines_.empty() || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) {
        rd_opts_warn_ = "there are no lyrics to translate";
        return;
    }
    TranslateRequest req;
    req.service = settings_.translate_service;
    req.from = settings_.translate_from;
    req.to = settings_.translate_to;
    rd_opts_open_ = false;
    if (rd_opts_dirty_) { save_settings(settings_); rd_opts_dirty_ = false; }
    rd_busy_ = true;
    rd_job_to_ = req.to;
    rd_job_svc_ = req.service;
    rd_job_track_ = rd_track_;
    rd_job_version_ = rd_versions_[static_cast<size_t>(rd_sel_)];
    rd_status_warn_ = false;
    rd_status_ = "translating into " + language_name(req.to) + " with " + translate_service_name(req.service) +
                 " ... this takes a while (needs an internet connection); you can go on reading";
    const std::vector<std::string> plain = rd_lines_;
    const LyricsVersion v = rd_job_version_;
    const fs::path track = rd_track_, script = translate_script_;
    std::thread([this, plain, v, track, script, req]() { run_guarded("lyrics translation", [&] {
        LyricsTranslation t = translate_lyrics_lines(plain, req, script);
        if (t.ok) save_lyrics_translation(track, v, req.to, plain, t);
        std::lock_guard<std::mutex> lk(rd_mutex_);
        rd_result_ = std::move(t);
        rd_result_ready_ = true;
    }); }).detach();
}

void App::readlyrics_poll() {
    if (!rd_result_ready_) return;
    LyricsTranslation t;
    {
        std::lock_guard<std::mutex> lk(rd_mutex_);
        if (!rd_result_ready_) return;
        t = std::move(rd_result_);
        rd_result_ = LyricsTranslation{};
        rd_result_ready_ = false;
    }
    rd_busy_ = false;
    const std::string what = language_name(rd_job_to_);
    // Is the file the translation was made for the one on the screen?
    const bool on_screen = mode_ == Mode::ReadLyrics && rd_sel_ >= 0 && rd_sel_ < static_cast<int>(rd_versions_.size()) &&
        lyrics_translation_file(rd_track_, rd_versions_[static_cast<size_t>(rd_sel_)], "xx") == lyrics_translation_file(rd_job_track_, rd_job_version_, "xx");
    std::string msg;
    bool warn = false;
    if (!t.ok) { msg = "translation into " + what + " failed: " + t.error; warn = true; }
    else if (t.same) msg = "these lyrics are in " + what + " already -- nothing to translate ([t] changes the languages)";
    else msg = "translated from " + language_name(t.from) + " to " + what + " with " + translate_service_name(rd_job_svc_) + " -- saved in the lyrics folder (translations)";
    if (on_screen) {
        if (t.ok) {
            readlyrics_refresh_translations(rd_job_to_);
            if (!t.same && !rd_tlist_.empty()) rd_show_ = true;
            if (t.same) rd_same_ = true;
        }
        rd_status_ = msg;
        rd_status_warn_ = warn;
    } else {
        status_line_ = "lyrics: " + msg;      // READ LYRICS is closed (or shows another file): tell on the main screen
        if (mode_ == Mode::ReadLyrics) { rd_status_ = msg; rd_status_warn_ = warn; }
    }
}

// The translate overlay ([t]): UP / DOWN pick the row, LEFT / RIGHT change it (a letter jumps to the next language
// starting with it), ENTER fetches the translation, ESC closes it. A running translation is not touched by any of it.
void App::readlyrics_options_key(int key) {
    const bool arrow = last_key_was_arrow();
    auto close_panel = [&]() {
        rd_opts_open_ = false;
        rd_opts_warn_.clear();
        if (rd_opts_dirty_) { save_settings(settings_); rd_opts_dirty_ = false; }
    };
    if (key == 27 || key_is("HKeyReadLyrics", key)) { close_panel(); return; }
    if (key == '\r' || key == '\n') { readlyrics_translate(); return; }
    if (arrow && key == 'A') { rd_opts_row_ = std::max(0, rd_opts_row_ - 1); return; }
    if (arrow && key == 'B') { rd_opts_row_ = std::min(2, rd_opts_row_ + 1); return; }
    // the value lists: 0 services, 1 target languages, 2 source languages ("auto" first)
    std::vector<std::string> ids;
    std::vector<std::string> names;
    std::string* cur = nullptr;
    if (rd_opts_row_ == 0) {
        for (const auto& s : translate_services()) { ids.push_back(s.first); names.push_back(s.second); }
        cur = &settings_.translate_service;
    } else {
        if (rd_opts_row_ == 2) { ids.push_back("auto"); names.push_back("Auto-detect"); }
        for (const auto& l : translate_languages()) { ids.push_back(l.first); names.push_back(l.second); }
        cur = rd_opts_row_ == 1 ? &settings_.translate_to : &settings_.translate_from;
    }
    int idx = 0;
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == *cur) { idx = static_cast<int>(i); break; }
    const int n = static_cast<int>(ids.size());
    int next = idx;
    if (arrow && (key == 'C' || key == 'D')) next = (idx + (key == 'C' ? 1 : n - 1)) % n;
    else if (rd_opts_row_ > 0 && std::isalpha(key) && key < 128) {   // jump to the next language that starts with the letter
        const char want = static_cast<char>(std::tolower(key));
        for (int k = 1; k <= n; ++k) {
            const int i = (idx + k) % n;
            if (!names[static_cast<size_t>(i)].empty() && std::tolower(static_cast<unsigned char>(names[static_cast<size_t>(i)][0])) == want) { next = i; break; }
        }
    } else return;
    if (next == idx) return;
    *cur = ids[static_cast<size_t>(next)];
    rd_opts_dirty_ = true;
    rd_opts_warn_.clear();
    rd_same_ = false;
}

void App::handle_readlyrics_key(int key) {
    if (key == 0) return;
    if (rd_opts_open_) { readlyrics_options_key(key); return; }
    if (rd_search_open_) { readlyrics_search_key(key); return; }
    const bool arrow = last_key_was_arrow();
    if (key == kKeyCtrlS && !rd_confirm_del_) { readlyrics_search_open(); return; }
    if (rd_confirm_del_) {   // "delete this translation?" -- only y confirms
        if (!arrow && (key == 'y' || key == 'Y')) readlyrics_delete_translation();
        else { rd_confirm_del_ = false; rd_status_ = "kept"; rd_status_warn_ = false; }
        return;
    }
    if (key == 27 || key_is("HKeyReadLyrics", key) || key == 'q' || key == 'Q') { readlyrics_close(); return; }
    if (!arrow && key == '\t') { rd_pane_ = rd_pane_ == 0 ? 1 : 0; rd_status_.clear(); return; }
    if (!arrow && key == 't') { rd_opts_open_ = true; rd_opts_row_ = 0; rd_opts_warn_.clear(); return; }
    if (!arrow && key == 'T') { readlyrics_toggle_translations(); return; }
    const int n = static_cast<int>(rd_versions_.size());
    // LEFT / RIGHT choose in the pane that has the keys ('D' is Left, 'C' is Right)
    if (arrow && (key == 'D' || key == 'C')) {
        const int dir = key == 'D' ? -1 : 1;
        if (rd_pane_ == 0) {
            const int ns = std::clamp(rd_sel_ + dir, 0, std::max(0, n - 1));
            if (ns != rd_sel_) { rd_sel_ = ns; rd_status_.clear(); readlyrics_load_version(); }
        } else if (!rd_tlist_.empty()) {
            const int nt = static_cast<int>(rd_tlist_.size());
            const int ns = std::clamp(rd_tsel_ + dir, 0, nt - 1);
            if (ns != rd_tsel_ || !rd_show_) { readlyrics_select_translation(ns); rd_show_ = true; rd_status_.clear(); }
        }
        return;
    }
    if (rd_pane_ == 1 && (key == kKeyDelete || (!arrow && (key == 'd' || key == 'D')))) {
        if (rd_tlist_.empty()) { rd_status_ = "there is no translation to delete"; rd_status_warn_ = true; }
        else rd_confirm_del_ = true;
        return;
    }
    if (arrow && key == 'A') { if (rd_scroll_ > 0) --rd_scroll_; return; }
    if (arrow && key == 'B') { ++rd_scroll_; return; }          // clamped against the real content while drawing
    if (key == ' ') { rd_scroll_ += std::max(1, term_rows_ - 14); return; }
    if (key == 'b' || key == 'B') { rd_scroll_ = std::max(0, rd_scroll_ - std::max(1, term_rows_ - 14)); return; }
    if (key == kKeyHome) { rd_scroll_ = 0; return; }
    if (key == kKeyEnd) { rd_scroll_ = 1 << 20; return; }       // clamped to the last page while drawing
}

// --- CTRL+s: search the lyrics files ------------------------------------------------------------------------------
// Lyrics live in a "lyrics" folder inside the folder of each track (and of the library folders / the download cache),
// so those are the folders that are searched: one listing per distinct folder, done when the search is opened.
void App::readlyrics_search_open() {
    std::set<std::string> dirs;
    auto add_dir = [&](const fs::path& folder) { if (!folder.empty()) dirs.insert(path_utf8(folder / "lyrics")); };
    for (const auto& t : all_local_tracks_) add_dir(t.path.parent_path());
    for (const auto& p : settings_.local_music_paths) add_dir(path_from_utf8(p));
    add_dir(rd_track_.parent_path());
    rd_search_all_.clear();
    rd_search_dirs_ = 0;
    for (const auto& d : dirs) {
        std::error_code ec;
        const fs::path dir = path_from_utf8(d);
        if (!fs::is_directory(dir, ec)) continue;
        ++rd_search_dirs_;
        const std::string folder = path_utf8(dir.parent_path().filename());
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            const std::string ext = ascii_lower_str(path_utf8(it->path().extension()));
            if (ext != ".lrc" && ext != ".txt") continue;
            LyricsFileHit h;
            h.path = it->path();
            h.name = path_utf8(it->path().stem());
            h.lname = ascii_lower_str(h.name);
            h.folder = folder;
            rd_search_all_.push_back(std::move(h));
            if (rd_search_all_.size() >= 200000) break;
        }
    }
    std::sort(rd_search_all_.begin(), rd_search_all_.end(), [](const LyricsFileHit& a, const LyricsFileHit& b) {
        return a.lname != b.lname ? a.lname < b.lname : a.folder < b.folder;
    });
    rd_search_q_.clear();
    rd_search_sel_ = rd_search_first_ = 0;
    readlyrics_search_rebuild();
    rd_search_open_ = true;
    rd_status_.clear();
}

// Every word of the query has to be somewhere in the file name (any order, case-insensitive for ASCII letters).
void App::readlyrics_search_rebuild() {
    std::vector<std::string> words;
    {
        std::istringstream iss(ascii_lower_str(rd_search_q_));
        std::string w;
        while (iss >> w) words.push_back(w);
    }
    rd_search_view_.clear();
    for (size_t i = 0; i < rd_search_all_.size(); ++i) {
        bool ok = true;
        for (const auto& w : words) if (rd_search_all_[i].lname.find(w) == std::string::npos) { ok = false; break; }
        if (ok) rd_search_view_.push_back(static_cast<int>(i));
    }
    const int n = static_cast<int>(rd_search_view_.size());
    rd_search_sel_ = std::clamp(rd_search_sel_, 0, std::max(0, n - 1));
    rd_search_first_ = std::clamp(rd_search_first_, 0, std::max(0, n - 1));
}

// Makes the reader show one lyrics file. It is handed over as the only "version" of a track that sits in the folder
// above the lyrics folder: that is what makes the translations of this file land in (and come from) the folder the file
// is in, exactly as for a real track (the track file itself is never opened).
void App::readlyrics_show_file(const LyricsFileHit& hit) {
    LyricsVersion v;
    v.kind = LyricsVersion::Kind::Fetched;
    v.file = hit.path;
    v.label = path_utf8(hit.path.filename());
    v.detail = hit.folder;
    v.active = true;
    rd_track_ = hit.path.parent_path().parent_path() / path_from_utf8(hit.name + ".audio");
    rd_title_ = hit.name;
    rd_versions_.assign(1, v);
    rd_sel_ = 0;
    rd_first_ = 0;
    rd_pane_ = 0;
    readlyrics_load_version();
    rd_status_ = "showing a lyrics file from the search -- CTRL+L closes the reader, open it again for the playing track";
    rd_status_warn_ = false;
}

void App::readlyrics_search_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(rd_search_view_.size());
    if (key == 27 || key == kKeyCtrlS) { rd_search_open_ = false; return; }
    if (key == '\r' || key == '\n') {
        if (n == 0) return;
        const LyricsFileHit hit = rd_search_all_[static_cast<size_t>(rd_search_view_[static_cast<size_t>(rd_search_sel_)])];
        rd_search_open_ = false;
        readlyrics_show_file(hit);
        return;
    }
    if (arrow && key == 'A') { rd_search_sel_ = std::max(0, rd_search_sel_ - 1); return; }
    if (arrow && key == 'B') { rd_search_sel_ = std::min(std::max(0, n - 1), rd_search_sel_ + 1); return; }
    if (arrow) return;
    if (key == kKeyHome) { rd_search_sel_ = 0; return; }
    if (key == kKeyEnd) { rd_search_sel_ = std::max(0, n - 1); return; }
    if (key == 127 || key == 8) { pop_utf8_char(rd_search_q_); rd_search_sel_ = rd_search_first_ = 0; readlyrics_search_rebuild(); return; }
    if (is_text_key(key) && rd_search_q_.size() < 100) {
        rd_search_q_ += static_cast<char>(key);
        rd_search_sel_ = rd_search_first_ = 0;
        readlyrics_search_rebuild();
    }
}

std::vector<std::string> App::build_readlyrics_search_panel() const {
    const int PW = 74, inner = PW - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    std::vector<std::string> out;
    out.push_back(box_top("SEARCH LYRICS FILES", PW, border));
    // the query, with a caret
    {
        const std::string pre = "Search: ";
        const int room = std::max(1, inner - display_width(pre) - 1);
        std::string shown = rd_search_q_;   // a long query keeps its END visible: cut from the front
        while (display_width(shown) > room && !shown.empty()) {
            size_t i = 1;
            while (i < shown.size() && (static_cast<unsigned char>(shown[i]) & 0xC0) == 0x80) ++i;
            shown.erase(0, i);
        }
        out.push_back(bar + " " + header_sgr(settings_) + pre + R + shown + "\x1b[7m \x1b[0m" +
                      std::string(static_cast<size_t>(std::max(0, inner - display_width(pre) - display_width(shown) - 1)), ' ') + " " + bar);
    }
    out.push_back(bar + " " + std::string(static_cast<size_t>(inner), ' ') + " " + bar);
    const int list_rows = std::clamp(term_rows_ - 12, 3, 14);
    const int n = static_cast<int>(rd_search_view_.size());
    int first = std::clamp(rd_search_first_, 0, std::max(0, n - 1));
    const int sel = std::clamp(rd_search_sel_, 0, std::max(0, n - 1));
    if (sel < first) first = sel;
    if (sel >= first + list_rows) first = sel - list_rows + 1;
    for (int r = 0; r < list_rows; ++r) {
        const int i = first + r;
        std::string painted;
        if (i < n) {
            const LyricsFileHit& h = rd_search_all_[static_cast<size_t>(rd_search_view_[static_cast<size_t>(i)])];
            const std::string tail = h.folder.empty() ? std::string() : "  (" + h.folder + ")";
            const int tail_w = std::min(display_width(tail), inner / 3);
            std::string left = truncate_str(h.name, std::max(5, inner - tail_w));
            std::string line = pad_right(left, inner - tail_w) + pad_right(truncate_str(tail, tail_w), tail_w);
            painted = i == sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + line + R : line;
        } else if (r == 0 && n == 0) {
            painted = pad_right(rd_search_all_.empty() ? "no lyrics files found in the lyrics folders of the library" : "no lyrics file matches", inner);
        } else {
            painted = std::string(static_cast<size_t>(inner), ' ');
        }
        out.push_back(bar + " " + painted + " " + bar);
    }
    out.push_back(bar + " " + std::string(static_cast<size_t>(inner), ' ') + " " + bar);
    const std::string info = std::to_string(n) + " of " + std::to_string(rd_search_all_.size()) + " files  in " + std::to_string(rd_search_dirs_) + (rd_search_dirs_ == 1 ? " folder" : " folders");
    out.push_back(bar + " " + legend_sgr(settings_) + pad_right(truncate_str(info, inner), inner) + R + " " + bar);
    out.push_back(box_bottom(PW, "[↑↓] Pick  [ENTER] Open  [ESC] Close", border_bottom));
    return out;
}

std::vector<std::string> App::build_readlyrics_options_panel() const {
    const int PW = 66, inner = PW - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    std::vector<std::string> out;
    out.push_back(box_top("TRANSLATE", PW, border));
    auto row = [&](const std::string& label, const std::string& value, bool sel) {
        const std::string lab = pad_right(label, 15);
        const std::string val = pad_right(truncate_str("< " + value + " >", inner - 15), inner - 15);
        out.push_back(bar + " " + header_sgr(settings_) + lab + R + (sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : std::string()) +
                      val + R + " " + bar);
    };
    auto plain = [&](const std::string& s, bool legend, const char* sgr = nullptr) {
        out.push_back(bar + " " + (sgr ? std::string(sgr) : legend ? legend_sgr(settings_) : std::string()) + pad_right(truncate_str(s, inner), inner) + R + " " + bar);
    };
    std::string from_name = settings_.translate_from == "auto" ? "Auto-detect" : language_name(settings_.translate_from);
    row("SERVICE", translate_service_name(settings_.translate_service), rd_opts_row_ == 0);
    row("TRANSLATE INTO", language_name(settings_.translate_to), rd_opts_row_ == 1);
    row("LYRICS ARE IN", from_name, rd_opts_row_ == 2);
    plain("", false);
    const std::string& svc = settings_.translate_service;
    plain(svc == "google" ? "Google Translate's key-less web endpoint (unofficial)."
                          : "MyMemory: free, about 5000 characters a day per address.", true);
    plain(settings_.translate_from == "auto" ? "If detection is wrong, set LYRICS ARE IN by hand." : "The language of the lyrics is set by hand.", true);
    if (!rd_opts_warn_.empty()) plain(rd_opts_warn_, false, "\x1b[31m");
    else if (rd_busy_) plain("Running: " + language_name(rd_job_to_) + " with " + translate_service_name(rd_job_svc_) + " (goes on in the background)", true);
    else plain("", false);
    out.push_back(box_bottom(PW, "[↑↓] Row  [←→] Change  [ENTER] Translate  [ESC] Close", border_bottom));
    return out;
}

void App::build_readlyrics_screen(std::ostringstream& frame, int W, int target_height) {
    if (W < 60) W = 60;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const int inner = W - 4;
    auto boxed = [&](const std::string& painted) { return bar + " " + painted + " " + bar; };

    // One line of choices that scrolls sideways: the lyrics files / the translations. `sel` is highlighted.
    auto chooser = [&](const std::vector<std::string>& items, int& sel, int& first) {
        const int n = static_cast<int>(items.size());
        const int avail = std::max(10, inner - 4);   // the two 2-cell scroll marks
        const std::string sep_txt = " " + (settings_.list_separator.empty() ? std::string("|") : settings_.list_separator) + " ";
        const int sw = display_width(sep_txt);
        auto iw = [&](int i) { return std::min(avail, display_width(items[static_cast<size_t>(i)])); };
        sel = std::clamp(sel, 0, n - 1);
        first = std::clamp(first, 0, n - 1);
        if (sel < first) first = sel;
        auto span_to_sel = [&](int f) { int w = 0; for (int i = f; i <= sel; ++i) w += iw(i) + (i > f ? sw : 0); return w; };
        while (first < sel && span_to_sel(first) > avail) ++first;
        std::vector<int> shown;
        int used = 0;
        for (int i = first; i < n; ++i) {
            const int add = iw(i) + (i > first ? sw : 0);
            if (used + add > avail) break;
            used += add;
            shown.push_back(i);
        }
        std::string mid;
        for (size_t k = 0; k < shown.size(); ++k) {
            const int i = shown[k];
            if (k > 0) mid += border + sep_txt + R;
            const std::string txt = truncate_str(items[static_cast<size_t>(i)], avail);
            mid += (i == sel) ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + txt + R : txt;
        }
        mid += std::string(static_cast<size_t>(std::max(0, avail - used)), ' ');
        const bool more_left = first > 0;
        const bool more_right = !shown.empty() && shown.back() < n - 1;
        return border + (more_left ? "◀ " : "  ") + R + mid + border + (more_right ? " ▶" : "  ") + R;
    };

    // --- top pane 1: ONE line with the lyrics files of the track ---
    frame << box_top("READ LYRICS" + (rd_pane_ == 0 ? focus_indicator(settings_, border) : std::string()), W, border) << "\n";
    {
        std::string line;
        if (rd_versions_.empty()) {
            line = pad_right("no lyrics saved for this track yet -- they are fetched while it plays", inner);
        } else {
            std::vector<std::string> items;
            for (const auto& v : rd_versions_) {
                std::string t = (v.active ? "● " : "") + v.label;
                if (!v.detail.empty()) t += " (" + v.detail + ")";
                items.push_back(t);
            }
            line = chooser(items, rd_sel_, rd_first_);
        }
        frame << boxed(line) << "\n";
    }
    frame << box_bottom(W, n_files_label(rd_versions_.size()), border_bottom) << "\n";

    // --- top pane 2: ONE line with the translations saved for the chosen file ---
    const bool show = rd_show_ && !rd_tlist_.empty() && rd_trans_.size() == rd_lines_.size();
    frame << box_top("TRANSLATION" + (rd_pane_ == 1 ? focus_indicator(settings_, border) : std::string()), W, border) << "\n";
    {
        std::string line;
        if (rd_tlist_.empty()) {
            line = pad_right(rd_lines_.empty() ? "nothing to translate" : "no translation yet -- [t] opens the translate overlay", inner);
        } else {
            std::vector<std::string> items;
            for (size_t i = 0; i < rd_tlist_.size(); ++i) {
                const TranslationInfo& t = rd_tlist_[i];
                items.push_back(std::string(show && static_cast<int>(i) == rd_tsel_ ? "● " : "") + language_name(t.to) +
                                (t.service.empty() ? std::string() : " (" + translate_service_name(t.service) + ")"));
            }
            line = chooser(items, rd_tsel_, rd_tfirst_);
        }
        frame << boxed(line) << "\n";
    }
    {
        std::string label;
        if (rd_busy_) label = "translating into " + language_name(rd_job_to_) + " with " + translate_service_name(rd_job_svc_) + " ...";
        else if (!rd_tlist_.empty()) label = std::to_string(rd_tlist_.size()) + (rd_tlist_.size() == 1 ? " translation" : " translations") + (show ? "" : " (hidden)");
        frame << box_bottom(W, label, border_bottom) << "\n";
    }

    // --- footer first (the lyrics pane takes the rows it leaves) ---
    std::string hint;
    if (rd_confirm_del_ && rd_tsel_ >= 0 && rd_tsel_ < static_cast<int>(rd_tlist_.size()))
        hint = "Delete the " + language_name(rd_tlist_[static_cast<size_t>(rd_tsel_)].to) + " translation?  [y] Yes | [any other key] No";
    else   // the same legend in both panes (TAB only moves the focus indicator): the keys that act on the focused pane say so in one entry
        hint = "[←→] Choose file/Translation | [TAB] Pane | [↑↓] Scroll | [SPACE/b] Page | [HOME/END] Top/Bottom | [d/DEL] Delete translation | [t] Translate | [SHIFT+t] Show/Hide translations | [CTRL+s] Search | [ESC] Close";
    std::vector<std::string> hint_lines(1);
    {
        size_t pos = 0;
        for (;;) {
            size_t sep = hint.find(" | ", pos);
            std::string entry = (sep == std::string::npos) ? hint.substr(pos) : hint.substr(pos, sep - pos);
            std::string& cur = hint_lines.back();
            std::string joined = cur.empty() ? entry : cur + " | " + entry;
            if (cur.empty() || display_width(joined) <= W || hint_lines.size() >= 2) cur = joined;
            else hint_lines.push_back(entry);
            if (sep == std::string::npos) break;
            pos = sep + 3;
        }
    }
    const int legend_rows = static_cast<int>(hint_lines.size());
    const int status_rows = rd_status_.empty() ? 0 : 1;
    const int budget = std::min(target_height, term_rows_ - 1);
    const int body = std::max(3, budget - 6 /*two top panes*/ - 2 /*lyrics pane borders*/ - legend_rows - status_rows);
    // two blank rows above and below the lyrics (none on a terminal too short for them)
    const int margin = body >= 10 ? 2 : 0;
    const int text_rows = body - 2 * margin;
    const int wrap_w = std::max(10, inner - 8);   // a little air left and right: the lines are centred

    // --- lyrics pane ---
    struct Row { std::string text; int style; };   // 0 original, 1 translation, 2 blank
    std::vector<Row> rows;
    auto lower = [](std::string s) { for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return s; };
    for (size_t i = 0; i < rd_lines_.size(); ++i) {
        if (rd_lines_[i].empty()) { rows.push_back({std::string(), 2}); continue; }
        for (auto& w : wrap_lines(rd_lines_[i], wrap_w, 8)) rows.push_back({w, 0});
        if (show) {
            const std::string& t = rd_trans_[i];
            if (!t.empty() && lower(t) != lower(rd_lines_[i]))
                for (auto& w : wrap_lines("(" + t + ")", wrap_w, 8)) rows.push_back({w, 1});
        }
    }
    if (rd_lines_.empty()) {
        rows.push_back({rd_versions_.empty() ? "no lyrics saved for this track" : "this file has no readable lyrics", 2});
    }
    const int max_scroll = std::max(0, static_cast<int>(rows.size()) - text_rows);
    rd_scroll_ = std::clamp(rd_scroll_, 0, max_scroll);

    std::string info;
    if (!rows.empty() && !rd_lines_.empty())
        info = "row " + std::to_string(std::min<int>(rd_scroll_ + text_rows, static_cast<int>(rows.size()))) + "/" + std::to_string(rows.size());
    const std::string sep2 = info.empty() ? "" : "  ";
    if (show) info += sep2 + "translation: " + language_name(rd_from_) + " > " + language_name(rd_to_) +
                      (rd_svc_.empty() ? std::string() : " (" + translate_service_name(rd_svc_) + ")");
    else if (rd_same_ && rd_tlist_.empty()) info += sep2 + "already " + language_name(settings_.translate_to);
    else if (!rd_tlist_.empty()) info += sep2 + "translation hidden -- [SHIFT+t] shows it";

    frame << box_top(truncate_str(rd_title_, std::max(10, W - 8)), W, border) << "\n";
    const std::string orig_sgr = ansi_for(settings_.active_line_color, false);
    const std::string trans_sgr = ansi_for(settings_.inactive_line_color, false);
    for (int k = 0; k < body; ++k) {
        const int idx = rd_scroll_ + k - margin;
        std::string painted;
        if (k >= margin && k < body - margin && idx >= 0 && idx < static_cast<int>(rows.size())) {
            const Row& r = rows[static_cast<size_t>(idx)];
            const std::string cut = truncate_str(r.text, inner);
            const int lead = std::max(0, (inner - display_width(cut)) / 2);   // centred
            const std::string t = pad_right(std::string(static_cast<size_t>(lead), ' ') + cut, inner);
            painted = r.style == 0 ? orig_sgr + t + R : r.style == 1 ? trans_sgr + t + R : t;
        } else {
            painted = std::string(static_cast<size_t>(inner), ' ');
        }
        frame << boxed(painted) << "\n";
    }
    frame << box_bottom(W, info, border_bottom) << "\n";

    for (const auto& l : hint_lines) frame << legend_sgr(settings_) << truncate_str(l, W) << "\x1b[0m\n";
    if (status_rows) frame << (rd_status_warn_ ? "\x1b[31m" : "\x1b[32m") << truncate_str(rd_status_, W) << "\x1b[0m\n";
}

// The THEME overlays of the Settings screen (see theme_ui_ in app.h): the theme picker (also used to choose a
// template) and the full-size CREATE THEME editor. Drawn with absolute positions like the Settings screen.
void App::build_theme_screen(std::ostringstream& frame, int W, int player_h) const {
    if (W < 80) W = 80;
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const int MAX_Y = std::max(player_h - 1, 11);
    auto B = [&](int y) {
        if (!settings_.border_smooth) return ansi_for(y >= MAX_Y ? settings_.border_color_bottom : settings_.border_color, false); // rough: only the bottom line changes
        return gradient_ansi(settings_.border_color, settings_.border_color_bottom,
                             (MAX_Y > 1) ? static_cast<float>(y - 1) / (MAX_Y - 1) : 0.0f, false);
    };
    auto pos = [&](int y, int x, const std::string& s) { frame << "\x1b[" << y << ";" << x << "H" << s; };
    auto repeat = [](const std::string& s, int n) { std::string r; for (int i = 0; i < n; ++i) r += s; return r; };
    auto pad = [](const std::string& s, int width, bool left_align = true) {
        const int ulen = static_cast<int>(display_width(s));
        if (ulen >= width) return s;
        const std::string sp(static_cast<size_t>(width - ulen), ' ');
        return left_align ? s + sp : sp + s;
    };
    auto box_row = [&](int y) { pos(y, 1, B(y) + "│" + R); pos(y, W, B(y) + "│" + R); };
    auto top_border = [&](const std::string& title) {
        const std::string t = "─ " + title + " ";
        pos(1, 1, B(1) + "┌" + t + repeat("─", W - 2 - static_cast<int>(display_width(t))) + "┐" + R);
    };
    auto bottom_border = [&](int y) { pos(y, 1, B(y) + "└" + repeat("─", W - 2) + "┘" + R); };

    if (theme_ui_ == 4 || theme_ui_ == 5) {   // export / import overlays
        top_border(theme_ui_ == 4 ? "EXPORT THEME" : "IMPORT THEMES");
        const bool editing_io = (mode_ == Mode::ColorEdit);
        int cursor_y = 0, cursor_x = 0;
        auto field = [&](int y, const char* label, const std::string& value, bool sel, int width) {
            pos(y, 3, header_sgr(settings_) + pad(label, 15) + R); pos(y, 18, ":");
            std::string v;
            if (sel && editing_io) {
                const size_t c = std::min(edit_caret_, color_edit_buffer_.size()), a = std::min(edit_anchor_, color_edit_buffer_.size());
                EditPaint p = paint_edit_field(color_edit_buffer_, c, a, width, change_sgr(settings_), false);
                v = change_sgr(settings_) + p.s + std::string(static_cast<size_t>(std::max(0, width + 1 - p.cols)), ' ');
                cursor_y = y;
                cursor_x = 20 + paint_edit_field(color_edit_buffer_, c, a, width, "", false).caret;
            } else {
                v = pad(truncate_str(value, width), width + 1);
            }
            pos(y, 20, (sel && !editing_io ? HI : std::string()) + v + R);
        };
        for (int y = 2; y < MAX_Y; ++y) box_row(y);
        const int fw = std::max(20, W - 24);
        if (theme_ui_ == 4) {
            field(3, "NAME", theme_io_name_, theme_io_row_ == 0, themes::kMaxNameLen);
            field(4, "FOLDER", theme_io_dir_, theme_io_row_ == 1, fw);
            const std::string file = (theme_dir_path(theme_io_dir_) / themes::file_name_for(theme_io_name_)).string();
            pos(6, 3, legend_sgr(settings_) + truncate_str("Exports the colors now set as one theme file: " + file, W - 5) + R);
            pos(7, 3, legend_sgr(settings_) + "(player and radio colors are both kept in the file; import it with SHIFT+i)" + R);
        } else {
            field(3, "FOLDER", theme_io_dir_, theme_io_row_ == 0, fw);
            pos(5, 3, header_sgr(settings_) + "Theme files found (" + std::to_string(theme_io_files_.size()) + ")" + R);
            const int nf = static_cast<int>(theme_io_files_.size());
            const int visible = std::max(1, MAX_Y - 7);
            const int scroll = std::clamp(theme_io_sel_ - visible / 2, 0, std::max(0, nf - visible));
            for (int r = 0; r < visible && scroll + r < nf; ++r) {
                const int i = scroll + r;
                const std::string name = theme_io_files_[static_cast<size_t>(i)].filename().string();
                const size_t nthemes = themes::read_theme_file(theme_io_files_[static_cast<size_t>(i)]).size();
                const bool sel = (theme_io_row_ == 1 && i == theme_io_sel_);
                pos(6 + r, 3, (sel ? HI : std::string()) + pad(truncate_str(name, W - 20), W - 20) + R);
                pos(6 + r, W - 14, legend_sgr(settings_) + std::to_string(nthemes) + (nthemes == 1 ? " theme" : " themes") + R);
            }
            if (nf == 0) pos(6, 3, legend_sgr(settings_) + "no theme files (*.theme, *.txt) in this folder" + R);
        }
        bottom_border(MAX_Y);
        pos(MAX_Y + 1, 1, legend_sgr(settings_) + (editing_io ? "[ENTER] Apply | [ESC] Cancel | [\u2190\u2192] Cursor | [Ctrl+C/X/V] Copy/Cut/Paste"
            : theme_ui_ == 4 ? "[\u2191\u2193] Field | [ENTER] Edit | [s] Export | [ESC] Cancel"
                             : "[\u2191\u2193] Select | [ENTER] Import the file (or edit the folder) | [a] Import all | [ESC] Cancel") + R);
        if (!status_line_.empty()) pos(MAX_Y + 2, 1, "\x1b[32m" + truncate_str(status_line_, W - 2) + R);
        if (editing_io && cursor_y > 0) frame << "\x1b[" << cursor_y << ";" << cursor_x << "H\x1b[?25h";
        return;
    }

    if (theme_ui_ == 1 || theme_ui_ == 3) {
        top_border(theme_ui_ == 1 ? "CHOOSE THEME" : "CHOOSE THEME AS TEMPLATE");
        const int n = static_cast<int>(theme_list_.size());
        const int visible = std::max(1, MAX_Y - 3);
        const int scroll = std::clamp(theme_sel_ - visible / 2, 0, std::max(0, n - visible));
        // the marker: the theme in use (picker), or the one the draft matches / is based on (template picker)
        int cur = -1;
        if (theme_ui_ == 1) cur = theme_current_index();
        else {
            for (int k = 0; k < n && cur < 0; ++k) if (theme_matches(theme_draft_, theme_list_[static_cast<size_t>(k)])) cur = k;
            for (int k = 0; k < n && cur < 0; ++k) if (theme_list_[static_cast<size_t>(k)].name == theme_base_.name) cur = k;
        }
        // swatches: (key, is background)
        static const struct { const char* key; bool bg; } kSw[] = {
            {"ColorBorderTop", false}, {"ColorBorderBottom", false}, {"ColorDiskTop", false}, {"ColorDiskBottom", false},
            {"ColorMetadataKey", false}, {"ColorMetadataVal", false}, {"ColorProgressBarPlayed", false},
            {"ColorListInactiveFg", false}, {"ColorListPlayingFg", false}, {"ColorListCursorBg", true}, {"ColorHeader", false}};
        int y = 2;
        for (int r = 0; r < visible && y < MAX_Y; ++r, ++y) {
            box_row(y);
            const int i = scroll + r;
            if (i >= n) continue;
            const themes::Theme& t = theme_list_[static_cast<size_t>(i)];
            std::string name = pad(truncate_str(t.name, themes::kMaxNameLen), themes::kMaxNameLen);
            pos(y, 3, (i == cur ? header_sgr(settings_) + "●" + R : " "));
            pos(y, 5, (i == theme_sel_ ? HI : std::string()) + name + R);
            std::string sw;
            for (const auto& k : kSw) {
                const std::string* v = t.get(k.key, false);
                const std::string val = v ? *v : "";
                sw += k.bg ? bg_ansi_for(val) + "    " + R + " " : ansi_for(val, false) + "████" + R + " ";
            }
            pos(y, 33, sw);
        }
        while (y < MAX_Y) { box_row(y); ++y; }
        bottom_border(MAX_Y);
        if (leave_prompt_ == 4 && theme_sel_ >= 0 && theme_sel_ < n)
            pos(MAX_Y + 1, 1, "\x1b[1;7m" + pad(" Do you want to delete theme \"" + truncate_str(theme_list_[static_cast<size_t>(theme_sel_)].name, themes::kMaxNameLen) + "\"? [y] Yes, delete | [n / ESC] No, keep it", W - 1) + R);
        else
        pos(MAX_Y + 1, 1, legend_sgr(settings_) + (theme_ui_ == 1
            ? "[↑↓] Select | [ENTER] Apply the theme | [SHIFT+d] Delete your own theme | [ESC] Back"
            : "[↑↓] Select | [ENTER] Use as template (copies its colors into the new theme) | [ESC] Back") + R);
        if (!status_line_.empty()) pos(MAX_Y + 2, 1, "\x1b[32m" + truncate_str(status_line_, W - 2) + R);
        return;
    }

    // ---- CREATE THEME ----
    static const char* grp[21] = {"BORDER_COLOR", "DISK", "METADATA", "VIZ", "PROGRESS_BAR",
                                  "SEARCH", "", "LIST", "", "", "QUEUE", "", "", "LYRICS", "", "", "HEADER", "LEGEND", "TAB_NAMES", "CHANGE", "FOCUS"};
    static const char* l1n[21] = {"TOP", "TOP", "KEY", "LEFT", "PLAYED", "FG", "INACTIVE  FG",
                                  "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG", "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                  "INACTIVE  FG", "ACTIVE L  FG", "ACTIVE W  FG", "TEXT", "TEXT", "CURRENT", "TEXT", "TEXT"};
    static const char* l2n[21] = {"BOTTOM", "BOTTOM", "VAL", "RIGHT", "PENDING", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG",
                                  "", "", "OTHER", "BG", ""};
    top_border("CREATE THEME");

    const bool editing = (mode_ == Mode::ColorEdit);
    auto edit_paint = [&](int width) {
        const size_t c = std::min(edit_caret_, color_edit_buffer_.size());
        const size_t a = std::min(edit_anchor_, color_edit_buffer_.size());
        EditPaint p = paint_edit_field(color_edit_buffer_, c, a, width, change_sgr(settings_), false);
        p.s += std::string(static_cast<size_t>(std::max(0, width - p.cols)), ' ');
        return p;
    };
    int cursor_y = 0, cursor_x = 0;

    // top row: NAME [......]   [ CHOOSE THEME AS TEMPLATE ]   (based on ...)
    box_row(2);
    pos(2, 3, header_sgr(settings_) + pad("NAME", 15) + R); pos(2, 18, ":");
    {
        const bool sel = (theme_row_ < 0 && theme_col_ == 0);
        const bool ed = sel && editing;
        const int fw = themes::kMaxNameLen + 1;
        std::string v;
        if (ed) {
            EditPaint p = edit_paint(fw);
            v = change_sgr(settings_) + p.s;   // paint_edit_field only re-emits the colour after a selection
            cursor_y = 2;
            cursor_x = 20 + paint_edit_field(color_edit_buffer_, std::min(edit_caret_, color_edit_buffer_.size()),
                                             std::min(edit_anchor_, color_edit_buffer_.size()), fw, "", false).caret;
        } else {
            v = pad(theme_name_.empty() ? std::string("(type a name)") : theme_name_, fw);
        }
        pos(2, 20, (sel && !ed ? HI : std::string()) + (!ed && theme_name_.empty() ? legend_sgr(settings_) : std::string()) + v + R);
    }
    {
        const bool sel = (theme_row_ < 0 && theme_col_ == 1);
        pos(2, 49, (sel ? HI : std::string()) + " CHOOSE THEME AS TEMPLATE " + R);
        pos(2, 77, legend_sgr(settings_) + truncate_str("based on " + theme_base_.name, std::max(1, W - 78)) + R);
    }
    pos(3, 1, B(3) + "├" + repeat("─", W - 2) + "┤" + R);

    const int visible = std::max(1, MAX_Y - 4);
    const int scroll = (theme_row_ < 0) ? 0 : std::clamp(theme_row_ - visible / 2, 0, std::max(0, 21 - visible));
    int y = 4;
    for (int r = 0; r < visible && y < MAX_Y; ++r, ++y) {
        box_row(y);
        const int i = scroll + r;
        if (i >= 21) continue;
        pos(y, 3, header_sgr(settings_) + pad(grp[i], 15) + R); pos(y, 18, ":");
        pos(y, 20, pad(l1n[i], 14, false)); pos(y, 35, ":");
        const std::string v0 = [&] { const std::string* c = color_cell(const_cast<Settings&>(theme_draft_), i, 0); return c ? *c : std::string(); }();
        {
            const bool sel = (i == theme_row_ && theme_col_ == 0);
            const bool ed = sel && editing;
            std::string v = ed ? edit_paint(7).s : pad(v0, 7);
            if (ed) { cursor_y = y; cursor_x = 38 + paint_edit_field(color_edit_buffer_, std::min(edit_caret_, color_edit_buffer_.size()),
                                                                     std::min(edit_anchor_, color_edit_buffer_.size()), 7, "", false).caret; }
            pos(y, 38, (sel && !ed ? HI : std::string()) + (ed ? change_sgr(settings_) : std::string()) + v + R);
        }
        pos(y, 46, ansi_for(v0, false) + "██" + R);
        if (l2n[i][0]) {
            const std::string* c1 = color_cell(const_cast<Settings&>(theme_draft_), i, 1);
            const std::string v1 = c1 ? *c1 : std::string();
            pos(y, 50, pad(l2n[i], 7, false)); pos(y, 58, ":");
            const bool sel = (i == theme_row_ && theme_col_ == 1);
            const bool ed = sel && editing;
            std::string v = ed ? edit_paint(7).s : pad(v1, 7);
            if (ed) { cursor_y = y; cursor_x = 61 + paint_edit_field(color_edit_buffer_, std::min(edit_caret_, color_edit_buffer_.size()),
                                                                     std::min(edit_anchor_, color_edit_buffer_.size()), 7, "", false).caret; }
            pos(y, 61, (sel && !ed ? HI : std::string()) + (ed ? change_sgr(settings_) : std::string()) + v + R);
            const bool is_bg = std::string(l2n[i]) == "BG";
            pos(y, 69, is_bg ? bg_ansi_for(v1) + "  " + R : ansi_for(v1, false) + "██" + R);
        }
        // a live sample of the row, drawn from the draft
        const Settings& d = theme_draft_;
        std::string rt;
        const std::string a0 = v0;
        const std::string* c1p = color_cell(const_cast<Settings&>(d), i, 1);
        const std::string a1 = c1p ? *c1p : std::string();
        if (i == 0 || i == 1 || i == 3) rt = gradient_preview_bar(a0, a1, 23);
        else if (i == 2) rt = ansi_for(a0, false) + "NAME : " + R + ansi_for(a1, false) + "SONG.MP3" + R;
        else if (i == 4) rt = ansi_for(a0, false) + "[###########" + R + ansi_for(a1, false) + "----------]" + R;
        else if (i == 5 || i == 6 || (i >= 7 && i <= 15)) rt = bg_ansi_for(a1) + ansi_for(a0, false) + "THIS IS AN EXAMPLE TEXT" + R;
        else if (i == 16) { const std::string pr = sgr_params_for(a0); rt = (pr.empty() ? std::string("\x1b[1m") : "\x1b[1;" + pr + "m") + "SECTION HEADER" + R; }
        else if (i == 17) rt = ansi_for(a0, false) + "[ESC] close | [ENTER] OK" + R;
        else if (i == 18) rt = ansi_for(a0, false) + "[COLORS]" + R + "  " + ansi_for(a1, false) + "ON/OFF  ANIMA/VIZ" + R;
        else if (i == 19) rt = ansi_for(a0, false) + bg_ansi_for(a1) + "EDITING..." + R;
        else if (i == 20) { const std::string pr = sgr_params_for(a0); rt = ansi_for(settings_.list_color, false) + "LOCAL AUDIO FILES " + R + (pr.empty() ? std::string("\x1b[1m") : "\x1b[1;" + pr + "m") + "\u25c0" + R; }
        if (!rt.empty()) pos(y, 72, rt);
    }
    while (y < MAX_Y) { box_row(y); ++y; }
    bottom_border(MAX_Y);
    if (leave_prompt_ == 3)
        pos(MAX_Y + 1, 1, "\x1b[1;7m" + pad(" Do you want to overwrite theme \"" + truncate_str(themes::sanitize_name(theme_name_), themes::kMaxNameLen) + "\"? [y] Yes, overwrite | [n / ESC] No, keep editing", W - 1) + R);
    else if (leave_prompt_ == 2)
        pos(MAX_Y + 1, 1, "\x1b[1;7m" + pad(" Unsaved theme! [s] Save it | [d] Discard it | [ESC] Keep editing", W - 1) + R);
    else
    pos(MAX_Y + 1, 1, legend_sgr(settings_) + (editing
        ? "[ENTER] Apply | [ESC] Cancel | [←→] Cursor | [SHIFT+←→] Mark | [Ctrl+C/X/V] Copy/Cut/Paste"
        : "[↑↓←→] Navigate | [ENTER] Edit/Open | [s] Save theme | [ESC] Discard") + R);
    if (!status_line_.empty()) pos(MAX_Y + 2, 1, "\x1b[32m" + truncate_str(status_line_, W - 2) + R);
    if (editing && cursor_y > 0) frame << "\x1b[" << cursor_y << ";" << cursor_x << "H\x1b[?25h";
}

void App::build_settings_screen(std::ostringstream& frame, int W, int player_h) const {
    // Literal port of the reference SettingsEngine::render() -- same
    // columns, same labels, same schema text, same group-blanking, same
    // divider, same tab-wrap algorithm, same gradient border (applied to
    // this panel's own chrome too, not just the preview swatches), same
    // preview formulas, same bottom hint/status lines, same cursor
    // placement formula. Deliberately not "improved" or restructured.
    if (W < 80) W = 80;
    if (theme_ui_ != 0) { build_theme_screen(frame, W, player_h); return; }
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    // MAX_Y is the row of this panel's own bottom border; the hint line
    // and status line render below it (rows MAX_Y+1, MAX_Y+2), so the
    // total screen rows used here is MAX_Y+2 -- set to exactly match
    // player_h (total rows the Browse-mode view renders), never taller,
    // never shorter, per explicit requirement. Floored modestly so a
    // pathologically small player view still leaves room to render the
    // tab bar/hint/status at all.
    int MAX_Y = std::max(player_h - 1, 11);
    auto B = [&](int y) {
        if (!settings_.border_smooth) return ansi_for(y >= MAX_Y ? settings_.border_color_bottom : settings_.border_color, false); // rough: only the bottom line changes
        return gradient_ansi(settings_.border_color, settings_.border_color_bottom,
                             (MAX_Y > 1) ? static_cast<float>(y - 1) / (MAX_Y - 1) : 0.0f, false);
    };
    auto pos = [&](int y, int x, const std::string& s) { frame << "\x1b[" << y << ";" << x << "H" << s; };
    auto repeat = [](const std::string& s, int n) {
        std::string r; r.reserve(s.size() * static_cast<size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i) r += s;
        return r;
    };
    // Matches pad(): no truncation if s is already >= width, just like
    // the reference -- a longer-than-expected value overflows into the
    // next column rather than getting cut off. Values here are always
    // short in practice (numbers, true/false, single-char hotkeys).
    auto pad = [](const std::string& s, int width, bool left_align = true) {
        int ulen = display_width(s);
        if (ulen >= width) return s;
        std::string spaces(width - ulen, ' ');
        return left_align ? (s + spaces) : (spaces + s);
    };

    // --- in-place value editing (mode_ == Mode::ColorEdit) -----------------
    // Width of the value field on the row currently being edited. The Colors
    // and non-path rows keep their fixed 7/20 columns; a path row is 20
    // columns minimum and grows with what has actually been typed, never
    // past the panel's right edge -- it used to be painted at the full width
    // all the time, so entering one drew a red bar to the far right even
    // when it was still empty.
    auto edit_field_width = [&]() -> int {
        if (settings_tab_ == 0) return 7;
        if (settings_tab_ == 3 && path_row_is_text(settings_row_)) {
            const PathRow er = path_row(settings_row_);
            const int edge = std::max(10, W - 36);
            const int want = display_width(color_edit_buffer_) + 1;
            return std::min(edge, std::max(20, want));
        }
        return 20;
    };
    // The painted value itself: selection reversed, window scrolled so the
    // caret stays inside the field, then padded to the full field width.
    // pad() cannot do this -- it measures with display_width(), which does
    // not see through the ANSI codes paint_edit_field() emits, so the column
    // count comes back with them included.
    auto edit_paint = [&](int width) {
        size_t c = std::min(edit_caret_, color_edit_buffer_.size());
        size_t a = std::min(edit_anchor_, color_edit_buffer_.size());
        EditPaint p = paint_edit_field(color_edit_buffer_, c, a, width, change_sgr(settings_), false);
        p.s += std::string(std::max(0, width - p.cols), ' ');
        return p;
    };

    static const char* kTabNames[] = {"COLORS", "ON/OFF", "ANIMA/VIZ", "PATHS", "REFERENCE", "ABOUT APP"};

    // 1. Tab-wrap algorithm.
    std::vector<int> top_tabs, bot_tabs;
    // Uses the widths the top border line really occupies (see the drawing
    // loop below): the "SETTINGS" header is 22 columns; every tab is
    // "  " + label + "  " + "┌" = 5 + label columns, followed by a 3-column
    // separator ("──┐") -- or a single "─" if it is the last tab on the
    // line. The active tab's label also carries "[" "]" (+2). The row must
    // end at column W-1 at the latest (the final "┐" sits in column W).
    // The old estimate (name + 10 per tab, strict "<") over-counted by 2+
    // columns per tab, so with six tabs ABOUT APP was pushed onto the
    // bottom border although the top border had room.
    int w_track = 22; // columns used so far, every tab counted with its 3-col separator
    bool top_full = false;
    for (int i = 0; i < kSettingsTabCount; ++i) {
        const int lab_cols = static_cast<int>(std::string(kTabNames[i]).size())
                             + (i == settings_tab_ ? 2 : 0);
        const int tab_cols = 5 + lab_cols;
        // Assume this tab is the last on the line (trailing "─" = 1 column).
        if (!top_full && w_track + tab_cols + 1 <= W - 1) {
            top_tabs.push_back(i);
            w_track += tab_cols + 3;
        } else {
            top_full = true; // keep the tab order: once one spills, all later ones do too
            bot_tabs.push_back(i);
        }
    }
    int w = 0;
    std::string l1, l2;
    auto add = [&](const std::string& t, const std::string& b, int width) { l1 += t; l2 += b; w += width; };
    add("\u250c\u2500 SETTINGS \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2510", "\u2502                    \u2514", 22);
    for (size_t idx = 0; idx < top_tabs.size(); ++idx) {
        int i = top_tabs[idx];
        std::string lab = (i == settings_tab_) ? ("[" + std::string(kTabNames[i]) + "]") : kTabNames[i];
        int lab_len = static_cast<int>(lab.size());
        bool is_last = (idx == top_tabs.size() - 1);
        add("  " + R + ansi_for(i == settings_tab_ ? settings_.tab_current_color : settings_.tab_other_color, false) + lab + R + B(1) + "  \u250c",
            repeat("\u2500", 4 + lab_len) + "\u2518", 5 + lab_len);
        if (is_last) add("\u2500", " ", 1);
        else add("\u2500\u2500\u2510", "  \u2514", 3);
    }
    if (w < W - 1) add(repeat("\u2500", W - 1 - w), repeat(" ", W - 1 - w), W - 1 - w);
    add("\u2510", "\u2502", 1);
    pos(1, 1, B(1) + l1 + R);
    pos(2, 1, B(2) + l2 + R);

    // 2. Content.
    int y = 3;
    if (settings_tab_ == 0) {
        static const char* grp[21]  = {"BORDER_COLOR", "DISK", "METADATA", "VIZ", "PROGRESS_BAR",
                                        "SEARCH", "", "LIST", "", "", "QUEUE", "", "", "LYRICS", "", "", "HEADER", "LEGEND", "TAB_NAMES", "CHANGE", "FOCUS"};
        static const char* l1n[21]  = {"TOP", "TOP", "KEY", "LEFT", "PLAYED",
                                        "FG", "INACTIVE  FG",
                                        "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                        "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                        "INACTIVE  FG", "ACTIVE L  FG", "ACTIVE W  FG",
                                        "TEXT", "TEXT", "CURRENT", "TEXT", "TEXT"};
        static const char* l2n[21]  = {"BOTTOM", "BOTTOM", "VAL", "RIGHT", "PENDING",
                                        "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG",
                                        "", "", "OTHER", "BG", ""}; // HEADER, LEGEND and FOCUS are foreground-only fields: no background cell at all
        // THEME | CREATE THEME (row 0): pick a ready-made colour scheme, or make one of your own.
        {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 3, header_sgr(settings_) + pad("THEME", 15) + R); pos(y, 18, ":");
            const int ci = theme_current_index();
            const std::string tn = ci >= 0 ? theme_list_[static_cast<size_t>(ci)].name : std::string("(custom)");
            const bool sel0 = (settings_row_ == 0 && settings_col_ == 0);
            const bool sel1 = (settings_row_ == 0 && settings_col_ == 1);
            pos(y, 20, (sel0 ? HI : "") + pad(truncate_str(tn, themes::kMaxNameLen), themes::kMaxNameLen + 1) + R);
            pos(y, 50, (sel1 ? HI : "") + " CREATE THEME " + R);
            y++;
            pos(y, 1, B(y) + "\u251c" + repeat("\u2500", W - 2) + "\u2524" + R); y++;
        }
        for (int i = 0; i < 21; ++i) {
            if (i == 1) { // SMOOTHNESS: how the frame's colour runs from BORDER_COLOR TOP to BOTTOM
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 3, header_sgr(settings_) + pad("", 15) + R); pos(y, 18, ":");
                pos(y, 20, pad("SMOOTHNESS", 14, false)); pos(y, 35, ":");
                const bool sel = (settings_row_ == 2);
                pos(y, 38, (sel ? HI : "") + pad(settings_get_value(2, 0), 7) + R);
                if (sel) pos(y, 46, ansi_for(settings_.tab_current_color, false) + "< \u2194 >" + R);
                pos(y, 72, legend_sgr(settings_) + (settings_.border_smooth ? "frame fades from TOP to BOTTOM" : "only the bottom border line = BOTTOM") + R);
                y++;
            }
            if (i == 5) { pos(y, 1, B(y) + "\u251c" + repeat("\u2500", W - 2) + "\u2524" + R); y++; }
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);

            pos(y, 3, header_sgr(settings_) + pad(grp[i], 15) + R); pos(y, 18, ":");

            pos(y, 20, pad(l1n[i], 14, false)); pos(y, 35, ":");
            {
                bool sel = (tab_row_of_color(i) == settings_row_ && settings_col_ == 0 && mode_ != Mode::ColorEdit);
                bool ed = (tab_row_of_color(i) == settings_row_ && settings_col_ == 0 && mode_ == Mode::ColorEdit);
                std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(tab_row_of_color(i), 0), 7);
                pos(y, 38, (sel ? HI : "") + (ed ? change_sgr(settings_) : "") + v + R);
            }

            // Second (background) column: the HEADER and LEGEND rows have no background
            // cell at all, so it renders neither a label nor a ":" here --
            // without this guard there'd be a dangling colon at col 56 with
            // nothing between it and the preview.
            // Colour swatches right behind the values (the radio settings do the same):
            // foreground cells show "██" in the colour, background cells a block of it.
            pos(y, 46, ansi_for(settings_get_value(tab_row_of_color(i), 0), false) + "\u2588\u2588" + R);
            if (l2n[i][0]) {
                pos(y, 50, pad(l2n[i], 7, false)); pos(y, 58, ":");
                bool sel = (tab_row_of_color(i) == settings_row_ && settings_col_ == 1 && mode_ != Mode::ColorEdit);
                bool ed = (tab_row_of_color(i) == settings_row_ && settings_col_ == 1 && mode_ == Mode::ColorEdit);
                std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(tab_row_of_color(i), 1), 7);
                pos(y, 61, (sel ? HI : "") + (ed ? change_sgr(settings_) : "") + v + R);
                const bool is_bg = std::string(l2n[i]) == "BG";
                pos(y, 69, is_bg ? bg_ansi_for(settings_get_value(tab_row_of_color(i), 1)) + "  " + R
                                 : ansi_for(settings_get_value(tab_row_of_color(i), 1), false) + "\u2588\u2588" + R);
            }

            std::string valA = settings_get_value(tab_row_of_color(i), 0), valB = settings_get_value(tab_row_of_color(i), 1);
            std::string rt;
            if (i == 0) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 1) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 2) rt = ansi_for(valA, false) + "NAME : " + R + ansi_for(valB, false) + "SONG.MP3" + R;
            else if (i == 3) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 4) rt = ansi_for(valA, false) + "[###########" + R + ansi_for(valB, false) + "----------]" + R;
            else if (i == 5) rt = search_idle_sgr(settings_) + "/l:" + R + search_sgr(settings_) + "QUERY" + R;
            else if (i == 6) rt = bg_ansi_for(valB) + ansi_for(valA, false) + "/l:" + std::string(15, ' ') + R;
            else if (i >= 7 && i <= 14) rt = bg_ansi_for(valB) + ansi_for(valA, false) + "THIS IS AN EXAMPLE TEXT" + R;
            else if (i == 15) {
                std::string aL_F = ansi_for(settings_.active_line_color, false), aL_B = bg_ansi_for(settings_.active_line_bg_color);
                std::string aW_F = ansi_for(valA, false), aW_B = bg_ansi_for(valB);
                std::string iN_F = ansi_for(settings_.inactive_line_color, false), iN_B = bg_ansi_for(settings_.inactive_line_bg_color);
                rt = aL_B + aL_F + "THIS IS " + R + aW_B + aW_F + "AN " + R + iN_B + iN_F + "EXAMPLE TEXT" + R;
            } else if (i == 16) {
                // Same bold + color construction header_sgr() draws the real
                // section titles with, so the swatch shows the exact result.
                std::string params = sgr_params_for(valA);
                rt = (params.empty() ? std::string("\x1b[1m") : "\x1b[1;" + params + "m") + "SECTION HEADER" + R;
            } else if (i == 17) {
                // Same prefix legend_sgr() gives the real hint lines, so the
                // swatch shows the exact result (an unset color is plain text).
                rt = ansi_for(valA, false) + "[ESC] close | [ENTER] OK" + R;
            } else if (i == 18) {
                rt = ansi_for(valA, false) + "[COLORS]" + R + "  " + ansi_for(valB, false) + "ON/OFF  ANIMA/VIZ" + R;
            } else if (i == 19) {
                // Same construction change_sgr() gives the real activated
                // field (a PATHS row, a meta editor tag, ...), so the swatch
                // shows the exact result.
                rt = ansi_for(valA, false) + bg_ansi_for(valB) + "EDITING..." + R;
            } else if (i == 20) {
                // Same construction focus_sgr() gives the real indicator after a pane title.
                const std::string params = sgr_params_for(valA);
                rt = ansi_for(settings_.list_color, false) + "LOCAL AUDIO FILES " + R + (params.empty() ? std::string("\x1b[1m") : "\x1b[1;" + params + "m") + "\u25c0" + R;
            }
            if (!rt.empty()) pos(y, 72, rt);
            y++;
        }
    } else if (settings_tab_ == 1 || settings_tab_ == 2) {
        // ON/OFF and ANIMA/VIZ: category headers and their rows (settings_layout()). The list scrolls with the
        // selected row when the terminal is too short for it; the ANIMA/VIZ note stays at the bottom.
        const auto& lay = settings_layout(settings_tab_);
        const std::vector<std::string> note = settings_tab_ == 2 ? settings_anima_note(W) : std::vector<std::string>();
        const int note_rows = note.empty() ? 0 : static_cast<int>(note.size()) + 1;   // + the blank line above it
        const int visible = std::max(1, MAX_Y - 3 - note_rows);
        const int total = static_cast<int>(lay.size());
        const int first = settings_tab_first_line(W, visible);
        int row_of_line = 0;   // selectable row number of the line being drawn
        for (int i = 0; i < first; ++i) if (!lay[static_cast<size_t>(i)].header) ++row_of_line;
        for (int i = first; i < total && y < 3 + visible; ++i) {
            const SetLine& L = lay[static_cast<size_t>(i)];
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            if (L.header) {
                pos(y, 4, header_sgr(settings_) + L.text + R);
            } else {
                const int r = row_of_line++;
                const bool sel = (r == settings_row_ && mode_ != Mode::ColorEdit);
                const bool ed = (r == settings_row_ && mode_ == Mode::ColorEdit);
                pos(y, 6, pad(L.text, 25)); pos(y, 32, ":");
                std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(r, 0), 20);
                pos(y, 35, (sel ? HI : "") + (ed ? change_sgr(settings_) : "") + v + R);
                if (sel && !settings_options_for(settings_tab_, r).empty())
                    pos(y, 57, ansi_for(settings_.tab_current_color, false) + "< \u2194 >\x1b[0m");
            }
            y++;
        }
        // there is more above / below: small marks at the right edge
        if (first > 0) pos(3, W - 3, legend_sgr(settings_) + "\u25b2" + R);
        if (first + visible < total) pos(3 + visible - 1, W - 3, legend_sgr(settings_) + "\u25bc" + R);
        if (!note.empty()) {
            int ny = MAX_Y - static_cast<int>(note.size());
            for (const auto& ln : note) {
                pos(ny, 1, B(ny) + "\u2502" + R); pos(ny, W, B(ny) + "\u2502" + R);
                pos(ny, 4, legend_sgr(settings_) + ln + R);
                ++ny;
            }
        }
    } else if (settings_tab_ == 3) {
        // PATHS tab: the LOCAL PATH / DOWNLOAD PATH / PLAYLIST PATH
        // sections (build_path_rows()) -- editable path rows, each list
        // ending in a "+ new path" row except the single download folder.
        // Scrolls as one list (viewport follows settings_row_, centered)
        // so a long list never grows the panel past player_h; see
        // path_display_row() for how a selectable row maps to the line it
        // is drawn on.
        int display_count = path_display_total();
        int visible = std::max(1, MAX_Y - 3);
        int cur_display = path_display_row(settings_row_);
        int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));

        int disp = 0;
        auto in_view = [&]() { return disp >= scroll && y < MAX_Y; };
        auto draw_header = [&](const char* text, const char* note = "") {
            // empty spacer row (just the side borders)
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                y++;
            }
            disp++;

            // the header itself, in the configurable Header color (bold;
            // palette index 10 by default, exactly what it always was)
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, header_sgr(settings_) + text + R);
                if (*note) pos(y, 6 + static_cast<int>(std::strlen(text)) + 3, legend_sgr(settings_) + note + R);
                y++;
            }
            disp++;
        };
        auto draw_path_row = [&](const PathRow& r) {
            if (r.kind == PathRow::Kind::Path && (r.label && *r.label)) {   // blank spacer above DOWNLOAD / PLAYLIST / HISTORY PATH
                if (in_view()) { pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R); y++; }
                disp++;
            }
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                const bool sel = (r.sel == settings_row_ && mode_ != Mode::ColorEdit);
                const bool ed = (r.sel == settings_row_ && mode_ == Mode::ColorEdit);
                if (r.kind == PathRow::Kind::Path) {
                    // DOWNLOAD / PLAYLIST / HISTORY PATH are single paths: the section header says what they are, so the value
                    // sits right under it (no second "Download Path :" label). Local paths are numbered.
                    const bool single = (r.label && *r.label);   // DOWNLOAD / PLAYLIST / HISTORY PATH: the header name is the row label
                    const int vx = 35;
                    if (single) pos(y, 6, header_sgr(settings_) + pad(r.label, 25) + R);
                    else pos(y, 6, pad("Local Path " + std::to_string(r.path_index + 1), 25));
                    pos(y, 32, ":");
                    // The value column runs from col 35 right up to the border --
                    // a folder path is far longer than a hotkey's 20 columns, and
                    // nothing may spill over the panel edge.
                    int val_w = std::max(10, W - (vx + 1));
                    std::string v;
                    if (ed) {
                        // While editing the field is only as wide as what has
                        // been typed (20 minimum, growing with the text, never
                        // past the panel edge -- edit_field_width()), so an
                        // empty "+ new path" line no longer draws a red bar all
                        // the way to the right border. The window inside it
                        // follows the caret, not the tail, so a selection
                        // anywhere in a long path stays visible.
                        v = edit_paint(edit_field_width()).s;
                    } else {
                        // The cursor highlight covers the entry, not the whole
                        // column: a path is highlighted exactly as wide as what
                        // is typed into it, the way a hotkey field highlights
                        // its own key. An empty row -- a not-yet-typed new path
                        // -- keeps the full-width bar instead, so there is a
                        // visible place to start typing.
                        std::string raw = settings_get_value(r.sel, 0);
                        int w = raw.empty()
                                ? val_w
                                : std::clamp(static_cast<int>(display_width(truncate_str(raw, val_w))), 1, val_w);
                        v = pad(truncate_str(raw, w), w);
                    }
                    pos(y, vx, (sel ? HI : "") + (ed ? change_sgr(settings_) : "") + v + R);
                } else { // AddPath
                    pos(y, 6, pad(r.label, 25)); pos(y, 32, ":");
                    pos(y, 35, (sel ? HI : "") + std::string(20, ' ') + R);
                    if (sel) pos(y, 57, legend_sgr(settings_) + "[ENTER] add a line\x1b[0m");
                }
                y++;
            }
            disp++;
        };
        for (const PathRow& r : build_path_rows()) {
            if (y >= MAX_Y) break;
            if (r.kind == PathRow::Kind::Header) {
                const std::string lab = r.label;
                draw_header(r.label); (void)lab;
            }
            else if (r.kind == PathRow::Kind::Note) {
                if (in_view()) {
                    pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                    pos(y, 6, legend_sgr(settings_) + truncate_str(r.label, W - 8) + R);
                    y++;
                }
                disp++;
            }
            else draw_path_row(r);
        }
    } else if (settings_tab_ == 4) {
        // Reference tab: a one-line read-only note pointing at the cheat
        // sheet first, then the rebindable hotkeys grouped under
        // category headers (kRefRows), then a read-only display of the font-mapping table (section 4 of the config, "A={A,a}" style)
        // loaded from config.txt -- as "A = A, a" rows. Combined they're
        // usually taller than the player view, so this scrolls as one list
        // (viewport follows settings_row_, centered) rather than ever
        // growing the panel past player_h. See ref_display_row() for how a
        // selectable row maps to the row it's drawn on.
        std::vector<char> letters;
        for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
        int total_selectable = kRefEnd + static_cast<int>(letters.size());
        int display_count = ref_display_row(total_selectable - 1) + 1;
        int visible = std::max(1, MAX_Y - 3);
        int cur_display = ref_display_row(settings_row_);
        int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));

        // Walk every display line (headers + rows) in order, only
        // actually drawing (and advancing y) once we're inside the
        // visible scroll window -- same "disp/scroll/visible" shape the
        // rest of this file's scrolling panels use.
        int disp = 0;
        auto in_view = [&]() { return disp >= scroll && y < MAX_Y; };
        auto draw_header = [&](const char* text) {
            // empty spacer row (just the side borders)
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                y++;
            }
            disp++;

            // the header itself, in the configurable Header color (bold;
            // palette index 10 by default, exactly what it always was)
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, header_sgr(settings_) + text + R);
                y++;
            }
            disp++;
        };
        auto draw_hotkey_row = [&](int selectable_row) {
            const RefHotkeyRow& row = kRefRows[selectable_row - kRefStart];
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, pad(row.label, 25)); pos(y, 32, ":");
                bool sel = (selectable_row == settings_row_ && mode_ != Mode::ColorEdit);
                bool ed = (selectable_row == settings_row_ && mode_ == Mode::ColorEdit);
                std::string v = ed ? edit_paint(edit_field_width()).s : pad(pretty_key(settings_get_value(selectable_row, 0)), 20);
                pos(y, 35, (sel ? HI : "") + (ed ? change_sgr(settings_) : "") + v + R);
                y++;
            }
            disp++;
        };
        auto draw_note = [&](const char* text) {
            // blank spacer line, then the read-only note in the legend color.
            // Two display lines, matching the `headers += 2` that
            // ref_display_row() adds just before the hotkeys.
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                y++;
            }
            disp++;
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, legend_sgr(settings_) + text + R);
                y++;
            }
            disp++;
        };
        auto draw_reset_row = [&]() {
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, pad("Reset All Keys To Default", 25)); pos(y, 32, ":");
                const bool sel = settings_row_ == 0 && mode_ != Mode::ColorEdit;
                pos(y, 35, (sel ? HI : "") + pad("[ENTER] reset", 20) + R);
                y++;
            }
            disp++;
        };
        auto draw_font_row = [&](char c, int selectable_row) {
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                const auto& pair = settings_.font_map.at(c);
                bool sel = (selectable_row == settings_row_);
                std::string line = std::string(1, c) + " = " + pair.first + ", " + pair.second;
                pos(y, 6, (sel ? HI : "") + line + R);
                y++;
            }
            disp++;
        };

        if (y < MAX_Y) draw_note(kRefNote);
        if (y < MAX_Y) draw_reset_row();
        for (int i = 0; i < kRefRowCount && y < MAX_Y; ++i) {
            if (kRefRows[i].header) draw_header(kRefRows[i].header);
            if (y < MAX_Y) draw_hotkey_row(kRefStart + i);
        }
        if (y < MAX_Y) draw_header("FONT / CHARACTER MAP");
        for (size_t li = 0; li < letters.size() && y < MAX_Y; ++li)
            draw_font_row(letters[li], kRefEnd + static_cast<int>(li));
    } else if (settings_tab_ == 5) {
        // About App: shows the hard coded about_app_text() (about_text.h),
        // scrolled so it never exceeds player_h.
        int visible = std::max(1, MAX_Y - 3);
        int total = static_cast<int>(about_app_text().size());
        int scroll = std::clamp(settings_row_, 0, std::max(0, total - visible));
        for (int r = 0; r < visible; ++r) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            int idx = scroll + r;
            if (idx < total) pos(y, 6, about_app_text()[idx]);
            y++;
        }
    }

    while (y < MAX_Y) { pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R); y++; }

    // 3. Bottom frame & overflow tabs.
    if (bot_tabs.empty()) {
        pos(y, 1, B(y) + "\u2514" + repeat("\u2500", W - 2) + "\u2518" + R);
    } else {
        // Measured in display COLUMNS, not bytes: "\u2514" and "\u2500" are 3 bytes
        // but one column each, so bot.size() over-counted by 2 per box-drawing
        // character and the row came out ~4 columns short of the right border.
        // Every tab segment is name + 5 columns: " [" NAME "] " or "  " NAME "  ",
        // plus the trailing "\u2500". A tab that would not fit before the closing
        // "\u2518" is dropped rather than letting the row wrap.
        std::string bot = "\u2514\u2500";
        int bot_cols = 2;
        for (int i : bot_tabs) {
            const int seg_cols = static_cast<int>(std::string(kTabNames[i]).size()) + 5;
            if (bot_cols + seg_cols > W - 1) break;
            bot += (i == settings_tab_ ? " [" + std::string(kTabNames[i]) + "] \u2500" : "  " + std::string(kTabNames[i]) + "  \u2500");
            bot_cols += seg_cols;
        }
        const int rem_bot = std::max(0, W - 1 - bot_cols); // columns of "\u2500" before the corner
        pos(y, 1, B(y) + bot + repeat("\u2500", rem_bot) + "\u2518" + R);
    }
    y++;

    // While a field is being edited (ENTER), TAB and the arrows stay in it:
    // LEFT/RIGHT move the caret, and only ENTER (apply) or ESC (cancel) leave.
    if (leave_prompt_ == 1) pos(y, 1, "\x1b[1;7m" + pad(" Unsaved changes! [s] Save and leave | [d] Discard and leave | [ESC] Keep editing", W - 1) + "\x1b[0m");
    else
    pos(y, 1, legend_sgr(settings_) + (mode_ == Mode::ColorEdit
        ? "[ENTER] Apply | [ESC] Cancel | [\u2190\u2192] Cursor | [SHIFT+\u2190\u2192] Mark | [HOME/END] Jump | [Ctrl+C/X/V] Copy/Cut/Paste"
        : settings_tab_ == 4
        ? "[TAB] Switch | [\u2191\u2193] Navigate | [ENTER] Change | [DEL] Default | [Ctrl+Shift+U] Undo | [s] Save | [ESC/q] Discard"
        : settings_tab_ == 0 ? "[TAB] Switch | [\u2191\u2193\u2190\u2192] Navigate | [ENTER] Edit | [e] Edit theme | [SHIFT+e/i] Export/Import | [s] Save | [ESC/q] Discard"
        : "[TAB] Switch | [\u2191\u2193\u2190\u2192] Navigate/Cycle | [ENTER] Edit | [s] Save | [ESC/q] Discard") + "\x1b[0m");
    y++;
    // The log/status line lives here now -- render_frame() deliberately no
    // longer prints it under the Browse list (an untruncated message there
    // could exceed the terminal width, wrap, and push a full-height frame
    // into a scroll, which is what made the terminal's own scrollbar appear
    // and shift the whole UI left by a column). This row sits on the panel's
    // own last screen line, so it is truncated to the panel width: a long
    // path or error message just ends early instead of wrapping.
    if (settings_dirty_) pos(y, 1, "\x1b[32mUnsaved changes! Save with `s` or discard with `ESC`.\x1b[0m");
    else if (!status_line_.empty()) pos(y, 1, "\x1b[32m" + truncate_str(status_line_, W - 2) + "\x1b[0m");

    // 4. In-place text editing cursor placement.
    if (mode_ == Mode::ColorEdit) {
        // The caret column comes from the same window the renderer painted
        // (edit_field_width() + paint_edit_field()), so it cannot disagree
        // with what is on screen: byte length could -- a multi-byte
        // character is one column but many bytes, and a path longer than the
        // field is scrolled to the caret rather than showing its tail.
        const size_t ec = std::min(edit_caret_, color_edit_buffer_.size());
        const size_t ea = std::min(edit_anchor_, color_edit_buffer_.size());
        const int caret_col = paint_edit_field(color_edit_buffer_, ec, ea, edit_field_width(), "", false).caret;
        if (settings_tab_ == 0) {
            const int cc = std::max(0, color_row_of_tab(settings_row_));
            int cy = 5 + cc + (cc >= 1 ? 1 : 0) + (cc >= 5 ? 1 : 0);
            int cx = ((settings_col_ == 0) ? 38 : 61) + caret_col;
            frame << "\x1b[" << cy << ";" << cx << "H\x1b[?25h";
        } else {
            int cy = 3 + settings_row_;
            int cx = 35 + caret_col;
            if (settings_tab_ == 3) { const PathRow cr = path_row(settings_row_); (void)cr; }
            if (settings_tab_ == 1 || settings_tab_ == 2) {
                // ON/OFF and ANIMA/VIZ scroll when the terminal is short: recompute the exact scroll the renderer used.
                const int note_rows = settings_tab_ == 2 ? static_cast<int>(settings_anima_note(W).size()) + 1 : 0;
                const int visible = std::max(1, MAX_Y - 3 - note_rows);
                cy = 3 + (settings_item_line(settings_tab_, settings_row_) - settings_tab_first_line(W, visible));
            } else if (settings_tab_ == 3) {
                // PATHS tab scrolls once its row list exceeds the visible
                // window. Recompute the same scroll offset used when
                // rendering (see the settings_tab_==3 branch above, and
                // path_display_row()) so the text cursor lands on the row
                // actually drawn there. Reached only for path rows -- an
                // AddPath row opens a new empty path row before editing.
                int visible = std::max(1, MAX_Y - 3);
                int cur_display = path_display_row(settings_row_);
                int display_count = path_display_total();
                int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));
                cy = 3 + (cur_display - scroll);
            } else if (settings_tab_ == 4) {
                // Reference tab scrolls once its row list exceeds the
                // visible window -- the common case, since it holds every
                // rebindable hotkey plus any font-map rows. Recompute the
                // same scroll offset used when rendering (see the
                // settings_tab_==4 branch above, and ref_display_row()) so
                // the text cursor lands on the row actually drawn there
                // instead of one that's already scrolled off-screen. Reached
                // only for the rebindable hotkeys -- the Enter handler
                // blocks ColorEdit for the read-only font-map rows.
                int visible = std::max(1, MAX_Y - 3);
                int cur_display = ref_display_row(settings_row_);
                std::vector<char> letters;
                for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
                int total_selectable = kRefEnd + static_cast<int>(letters.size());
                int display_count = ref_display_row(total_selectable - 1) + 1;
                int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));
                cy = 3 + (cur_display - scroll);
            }
            frame << "\x1b[" << cy << ";" << cx << "H\x1b[?25h";
        }
    }
}
// ---------------------------------------------------------------------
// Console / log overlay (HKeyConsole)
// ---------------------------------------------------------------------

void App::build_console_screen(std::ostringstream& frame, int W, int target_height) const {
    std::string border = ansi_for(settings_.border_color, false);
    frame << box_top("CONSOLE / LOGS", W, border) << "\n";

    std::vector<std::string> log_lines = ConsoleLog::instance().lines();
    // Must always equal the player view's own height (target_height,
    // computed by player_view_height() -- see its comment in app.h),
    // never just "whatever the terminal happens to fit". A terminal much
    // taller than the actual Browse-mode view would otherwise leave this
    // overlay awkwardly mismatched from the view it's standing in for.
    int visible = std::max(1, target_height - 2); // minus this overlay's own top/bottom border rows
    int total = static_cast<int>(log_lines.size());
    int start = std::max(0, total - visible); // always shows the tail, newest at the bottom

    for (int r = 0; r < visible; ++r) {
        int idx = start + r;
        std::string line = (idx < total) ? log_lines[idx] : "";
        frame << box_line(line, W, border) << "\n";
    }
    frame << box_bottom(W, "[t / ESC] close", border) << "\n";
}

// ---------------------------------------------------------------------
// Cheatsheet overlay (HKeyCheatsheet)
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Karaoke overlay (Mode::Karaoke, HKeyKaraoke = "k")
// ---------------------------------------------------------------------

// The karaoke art on the left (braille, 30 rows x 28 cells). Blank U+2800
// cells stay uncoloured; everything else wears the disk colours with a
// moving colour wave, like the radio's ON AIR sign.
static constexpr int kKaraokeArtW = 28;
static constexpr int kKaraokeArtRows = 30;
static const char* const kKaraokeArt[kKaraokeArtRows] = {
    "⠀⠀⠀⢠⡞⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⢦⠀⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⢀⡠⠤⡆⠀⠀⠀⠀⠀⠀⠀⢀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⢸⠀⢀⡇⠀⠀⠀⠀⠀⠀⢤⠜⠣⡤⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⢀⡤⣼⠀⠻⠃⠀⠀⠀⠀⠀⠀⠠⠕⠺⠄⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠈⠉⠁⠀⠀⠀⠀⣿⣿⡆⠀⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⢰⣿⣿⣿⣿⣿⣿⣿⣿⣷⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠉⠉⣿⣿⡿⠉⢹⣿⣿⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⢀⣼⣿⡟⠁⠀⢸⣿⡿⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠸⣿⡿⠋⠀⢾⣿⣿⣿⠃⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠉⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⢰⣿⣿⣿⣿⣿⣿⡷⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⢀⣤⣤⣤⣤⣤⣤⣤⣤⡄⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠈⠛⠛⠛⠛⠛⢻⣿⣿⠇⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⣀⣴⣾⣿⠏⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠰⣿⣿⠿⠋⠁⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠀⢠⣤⣄⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⢀⣤⣤⣤⣤⣼⣿⣿⣤⣤⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠈⠛⠛⢻⣿⣿⣼⣿⠛⠛⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⢀⣴⣿⣿⢿⣿⣿⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠸⣿⡿⠛⣡⣼⣿⡟⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠛⠛⠛⠁⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⣰⣿⣷⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⣰⣿⣿⣿⣿⣿⣿⣿⣿⠆⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⢾⣿⡟⠁⠀⣹⣿⡏⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠉⠀⠀⣠⣿⣿⠁⠀⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⣤⣾⣿⡿⠁⠀⠀⢰⠲⣄⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠈⠛⠁⠀⠀⠀⠀⠸⠀⡸⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣰⠒⡇⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠈⠉⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀",
    "⠀⠀⠀⠈⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠛⠋⠀⠀⠀⠀⠀",
};

// Decodes UTF-8 into code points (invalid bytes become U+FFFD, which is not in the font).
static std::vector<char32_t> karaoke_codepoints(const std::string& s) {
    std::vector<char32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + static_cast<size_t>(len) > s.size()) { out.push_back(0xFFFD); ++i; continue; }
        char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
        out.push_back(cp);
        i += static_cast<size_t>(len);
    }
    return out;
}

// One lyric line in BIG letters (karaoke lyrics size 2..5), drawn in braille.
// Latin text uses the built-in 5x7 pixel font (karaoke_font.h) scaled by
// `scale` = size - 1, so a letter is 3*scale columns wide and 2*scale rows
// tall. A line with characters that font lacks (Japanese, Chinese, Korean,
// Cyrillic, Greek ...) takes its glyphs from an installed font instead
// (glyph_raster.h); those are bigger, because a kanji needs about 16 dots to
// stay readable: the cell is 8*scale + 8 dots high, i.e. 4 / 6 / 8 / 10 rows
// for the sizes 2 / 3 / 4 / 5, and a full-width character is as many dots wide.
// Words wrap like the normal lyrics (a word with no spaces, like a Japanese
// line, breaks wherever the row is full), each row is centred in `width`
// columns, and the colours follow the same rules as render_lyric_line_wrapped()
// (sung words in the active-word colour, the word being sung bold, the rest of
// the active line in the active-line colour, other lines in the inactive colour).
// Returns false -- and leaves `out` alone -- when the line holds a character no
// font has (emoji, scripts that need shaping, or no CJK font installed); the
// caller then shows that line as normal text instead.
static bool render_big_lyric_line(const LyricLine& line, double elapsed, int width, bool is_active, int scale,
                                  const Settings& st, std::vector<std::string>& out) {
    struct Word { std::vector<char32_t> cps; double t; bool has_ts; };
    std::vector<Word> words;
    if (!line.words.empty()) {
        for (const auto& wt : line.words) {
            std::istringstream iss(wt.second);
            std::string w;
            while (iss >> w) words.push_back({karaoke_codepoints(w), wt.first, true});
        }
    } else {
        std::istringstream iss(line.full_text);
        std::string w;
        while (iss >> w) words.push_back({karaoke_codepoints(w), line.start_time, false});
    }
    bool pixel_font = true;   // every character is in the built-in font
    for (const auto& w : words) for (char32_t cp : w.cps) if (!karaoke_glyph(cp)) pixel_font = false;
    if (words.empty()) { out.push_back(std::string(static_cast<size_t>(std::max(0, width)), ' ')); return true; }

    // Cell height in dots, and the glyph (width + dots) of every character of the line.
    const int cell_h = pixel_font ? 8 * scale : 8 * scale + 8;
    std::unordered_map<char32_t, RasterGlyph> glyphs;
    if (!pixel_font) {
        if (!glyph_font_available()) return false;
        for (const auto& w : words) for (char32_t cp : w.cps) {
            if (!glyphs.count(cp) && !glyph_rasterize(cp, cell_h, glyphs[cp])) return false;
        }
        glyphs.erase(U' ');
    }
    // The separator between two words: a pixel-font space, or a quarter of the cell height.
    const int space_w = pixel_font ? 6 * scale : std::max(3, cell_h / 4);
    auto char_w = [&](char32_t cp) { return pixel_font ? 6 * scale : glyphs.at(cp).w; };
    auto text_w = [&](const std::vector<char32_t>& v, size_t n) {
        int w = 0;
        for (size_t i = 0; i < n; ++i) w += char_w(v[i]);
        return w;
    };

    const int cap = std::max(2, width * 2);                // dots per output row (2 dots per braille column)
    // State per word: 0 inactive line, 1 active line (not sung yet), 2 sung, 3 being sung.
    int singing = -1;
    if (is_active) for (size_t i = 0; i < words.size(); ++i) if (words[i].has_ts && words[i].t <= elapsed) singing = static_cast<int>(i);
    auto state_of = [&](size_t i) -> int {
        if (!is_active) return 0;
        if (!words[i].has_ts) return 1;
        if (static_cast<int>(i) == singing) return 3;
        return words[i].t <= elapsed ? 2 : 1;
    };
    const std::string sgr[4] = {
        ansi_for(st.inactive_line_color) + bg_ansi_for(st.inactive_line_bg_color),
        ansi_for(st.active_line_color) + bg_ansi_for(st.active_line_bg_color),
        ansi_for(st.active_word_color) + bg_ansi_for(st.active_word_bg_color),
        "\x1b[1m" + ansi_for(st.active_word_color) + bg_ansi_for(st.active_word_bg_color),
    };

    // Word wrap into rows of at most `cap` dots (a word longer than a row is split where the row is full).
    struct Piece { std::vector<char32_t> cps; int state; bool space; };
    std::vector<std::vector<Piece>> rows(1);
    int used = 0;
    for (size_t i = 0; i < words.size(); ++i) {
        std::vector<char32_t> rest = words[i].cps;
        while (!rest.empty()) {
            const int need = used + (used > 0 ? space_w : 0) + text_w(rest, rest.size());   // row length with this word
            if (need <= cap) {
                if (used > 0) { rows.back().push_back({{}, 0, true}); used += space_w; }
                rows.back().push_back({rest, state_of(i), false});
                used += text_w(rest, rest.size());
                rest.clear();
            } else if (used > 0) {
                rows.emplace_back(); used = 0;
            } else {
                size_t n = 0;
                int w = 0;
                while (n < rest.size() && w + char_w(rest[n]) <= cap) { w += char_w(rest[n]); ++n; }
                n = std::max<size_t>(n, 1);
                rows.back().push_back({std::vector<char32_t>(rest.begin(), rest.begin() + static_cast<long>(n)), state_of(i), false});
                rest.erase(rest.begin(), rest.begin() + static_cast<long>(n));
                rows.emplace_back(); used = 0;
            }
        }
    }
    if (rows.back().empty() && rows.size() > 1) rows.pop_back();

    for (const auto& row : rows) {
        // Pixel grid of the row: cell_h dots tall; each dot keeps its word's state (+1, 0 = off).
        int pw = 0;
        for (const auto& p : row) pw += p.space ? space_w : text_w(p.cps, p.cps.size());
        const int ph = cell_h;
        std::vector<int8_t> px(static_cast<size_t>(std::max(1, pw) * ph), 0);
        int x0 = 0;
        for (const auto& p : row) {
            if (p.space) { x0 += space_w; continue; }
            for (char32_t cp : p.cps) {
                if (pixel_font) {
                    const uint8_t* g = karaoke_glyph(cp);
                    for (int gy = 0; g && gy < 8; ++gy)
                        for (int gx = 0; gx < 5; ++gx)
                            if (g[gy] & (1 << (4 - gx)))
                                for (int sy = 0; sy < scale; ++sy)
                                    for (int sx = 0; sx < scale; ++sx)
                                        px[static_cast<size_t>((gy * scale + sy) * pw + x0 + gx * scale + sx)] = static_cast<int8_t>(p.state + 1);
                } else {
                    const RasterGlyph& g = glyphs.at(cp);
                    for (int gy = 0; gy < g.h && gy < ph; ++gy)
                        for (int gx = 0; gx < g.w; ++gx)
                            if (g.dots[static_cast<size_t>(gy * g.w + gx)])
                                px[static_cast<size_t>(gy * pw + x0 + gx)] = static_cast<int8_t>(p.state + 1);
                }
                x0 += char_w(cp);
            }
        }
        const int cols = (pw + 1) / 2;
        const int left = std::max(0, (width - cols) / 2);
        static const int kBit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
        for (int cy = 0; cy < (ph + 3) / 4; ++cy) {
            std::string line(static_cast<size_t>(left), ' ');
            std::string last;
            for (int cx = 0; cx < cols; ++cx) {
                int bits = 0, state = 0;
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int x = cx * 2 + dx, y = cy * 4 + dy;
                        if (x >= pw || y >= ph) continue;
                        const int v = px[static_cast<size_t>(y * pw + x)];
                        if (v) { bits |= kBit[dy][dx]; state = std::max(state, v); }
                    }
                if (!bits) {
                    if (!last.empty()) { line += "\x1b[0m"; last.clear(); }
                    line += ' ';
                    continue;
                }
                const std::string& a = sgr[state - 1];
                if (a != last) { line += "\x1b[0m" + a; last = a; }
                const int cp = 0x2800 + bits;
                line += static_cast<char>(0xE0 | (cp >> 12));
                line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                line += static_cast<char>(0x80 | (cp & 0x3F));
            }
            if (!last.empty()) line += "\x1b[0m";
            line += std::string(static_cast<size_t>(std::max(0, width - left - cols)), ' ');
            out.push_back(line);
        }
    }
    return true;
}

void App::build_karaoke_screen(std::ostringstream& frame, int W, int H) {
    const std::string R = "\x1b[0m";
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border + settings_.box_vertical + R;
    H = std::max(8, H);
    W = std::max(20, W);
    const int body_h = H - 3;                       // top border, bottom border, legend
    const int inner = W - 4;

    // --- title: what is playing ---
    std::string title = "KARAOKE";
    if (has_track_) {
        std::string name = metadata_.name.empty() || metadata_.name == "-" ? path_utf8(current_path_.stem()) : metadata_.name;
        title += " · " + name;
        if (!metadata_.artist.empty() && metadata_.artist != "-") title += " – " + metadata_.artist;
    }
    frame << box_top(truncate_str(title, std::max(10, W - 8)), W, border) << "\n";

    // --- layout: the art on BOTH sides, 4 columns between art and lyrics, the lyrics centred in between. Recomputed
    // every frame from the terminal size, so a resize simply re-flows everything. When the lyrics would get narrower
    // than 30 columns the two pictures are left out and the lyrics take the whole width.
    constexpr int kGap = 4;
    // Big Japanese / Chinese / Korean letters (sizes 4 and 5) need the room: with the pictures on both sides only
    // about 50 columns are left, which holds two or three letters per row, so the lyrics take the whole width then.
    bool wide_script = false;
    if (settings_.karaoke_lyrics_size >= 4) {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (lyrics_ready_ && lyrics_path_ == current_path_) {
            for (const auto& l : lyrics_result_.lines) {
                for (char32_t cp : karaoke_codepoints(l.full_text)) {
                    if (cp >= 0x1100 && !karaoke_glyph(cp) && (cp <= 0x11FF || (cp >= 0x2E80 && cp <= 0xD7FF) || (cp >= 0xF900 && cp <= 0xFFEF))) { wide_script = true; break; }
                }
                if (wide_script) break;
            }
        }
    }
    const bool show_art = !wide_script && inner - 2 * (kKaraokeArtW + kGap) >= 30;
    const int side_w = show_art ? kKaraokeArtW + kGap : 0;     // art + gap, on each side
    const int lyr_w = inner - 2 * side_w;

    std::vector<std::string> art_rows(static_cast<size_t>(body_h), std::string(static_cast<size_t>(kKaraokeArtW), ' '));
    if (show_art) {
        // Wave from the left to the right edge: the gradient runs disk TOP colour -> BOTTOM colour across the art,
        // a ripple rides on it and moves to the right (0.25 cycles per second, the ON AIR sign's default).
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const float phase = static_cast<float>(std::fmod(now * 0.25, 1000.0));
        const std::string c_end = settings_.disk_color_gradient && !settings_.disk_color_end.empty()
                                  ? settings_.disk_color_end : settings_.disk_color;
        const int first = std::max(0, (kKaraokeArtRows - body_h) / 2);           // crop evenly on a short terminal
        const int top = std::max(0, (body_h - kKaraokeArtRows) / 2);             // centre on a tall one
        for (int y = first; y < kKaraokeArtRows && top + (y - first) < body_h; ++y) {
            std::string line, last;
            int x = 0;
            const char* p = kKaraokeArt[y];
            while (*p && x < kKaraokeArtW) {
                const unsigned char c0 = static_cast<unsigned char>(*p);
                const int len = c0 < 0x80 ? 1 : (c0 >> 5) == 0x6 ? 2 : (c0 >> 4) == 0xE ? 3 : 4;
                const std::string g(p, static_cast<size_t>(len));
                p += len;
                if (g == "⠀" || g == " ") { line += ' '; ++x; continue; }
                const float d = static_cast<float>(x) / static_cast<float>(kKaraokeArtW - 1);
                const float ripple = 0.5f + 0.5f * std::sin(6.2831853f * (d * 1.5f - phase));
                float t = 0.65f * d + 0.35f * ripple;
                t = std::round(std::clamp(t, 0.0f, 1.0f) * 32.0f) / 32.0f;
                const std::string a = gradient_ansi(settings_.disk_color, c_end, t);
                if (a != last) { line += a; last = a; }
                line += g;
                ++x;
            }
            if (!last.empty()) line += R;
            line += std::string(static_cast<size_t>(std::max(0, kKaraokeArtW - x)), ' ');
            art_rows[static_cast<size_t>(top + (y - first))] = line;
        }
    }

    // --- centre: the lyrics, karaoke style ---
    std::vector<std::string> lyr_rows(static_cast<size_t>(body_h), std::string(static_cast<size_t>(lyr_w), ' '));
    auto centred = [&](const std::string& text, const std::string& sgr) {
        const std::string t = truncate_str(text, lyr_w);
        const int pad = std::max(0, (lyr_w - display_width(t)) / 2);
        return sgr + std::string(static_cast<size_t>(pad), ' ') + t + R + std::string(static_cast<size_t>(std::max(0, lyr_w - pad - display_width(t))), ' ');
    };
    std::vector<LyricLine> lines;
    double delay = 0.0;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (lyrics_ready_ && lyrics_path_ == current_path_) {   // the lyrics engine runs for the overlay in any case
            lines = lyrics_result_.lines;
            delay = lyrics_result_.delay;
            message = lyrics_result_.message;
        }
    }
    const double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    if (!has_track_) {
        message = "Nothing is playing -- start a track and the lyrics appear here.";
    } else if (!lyrics_ready_ || lyrics_path_ != current_path_) {
        message = "fetching lyrics ...";
    } else if (lines.empty() && message.empty()) {
        message = "No lyrics found for this track.";
    }
    const int size = std::clamp(settings_.karaoke_lyrics_size, 1, 5);
    if (lines.empty()) {
        lyr_rows[static_cast<size_t>(body_h / 2)] = centred(message, legend_sgr(settings_));
    } else {
        Settings ks = settings_;          // karaoke look: centred, whole lines, the sung words highlighted
        ks.lyrics_alignment = 0;
        ks.lyrics_animation = 0;
        const double lt = elapsed - delay;
        bool synced = false;
        for (const auto& l : lines) if (l.start_time > 0.0) { synced = true; break; }
        int active = 0;
        if (synced) {
            for (size_t i = 0; i < lines.size(); ++i) { if (lines[i].start_time <= lt) active = static_cast<int>(i); else break; }
        } else if (total_sec_ > 0) {   // plain lyrics: scroll along with the song
            active = std::clamp(static_cast<int>(elapsed / static_cast<double>(total_sec_) * lines.size()), 0, static_cast<int>(lines.size()) - 1);
        }
        // One lyric line -> its rows: big letters from size 2 on (normal text for a line the font cannot draw).
        auto render_line = [&](int li) {
            const bool is_active = synced && li == active;
            const double t = (!synced && li == active) ? 1e12 : lt;   // no timing: the current line in the active colour
            const bool hl = is_active || (!synced && li == active);
            std::vector<std::string> rows;
            // A line that would be taller than the screen (a long Japanese line at size 5) steps down a size at a
            // time, and ends up as normal text when even the smallest big letters are too tall for it.
            const int max_rows = std::max(6, body_h);
            for (int sc = size - 1; size >= 2 && sc >= 1; --sc) {
                rows.clear();
                if (!render_big_lyric_line(lines[static_cast<size_t>(li)], t, lyr_w, hl, sc, ks, rows)) break;
                if (static_cast<int>(rows.size()) <= max_rows) return rows;
            }
            return render_lyric_line_wrapped(lines[static_cast<size_t>(li)], t, lyr_w, hl, ks);
        };
        // The lines around the active one, a blank row between two lines; the active one sits in the middle.
        std::vector<std::string> flat;
        int active_row = 0, active_count = 1;
        const int line_rows = size >= 2 ? 2 * (size - 1) + 1 : 2;   // rough height of one line incl. the gap
        const int reach = body_h / line_rows + 1;
        const int lo = std::max(0, active - reach), hi = std::min(static_cast<int>(lines.size()) - 1, active + reach);
        for (int li = lo; li <= hi; ++li) {
            auto rows = render_line(li);
            if (li == active) { active_row = static_cast<int>(flat.size()); active_count = static_cast<int>(rows.size()); }
            for (auto& r : rows) flat.push_back(std::move(r));
            flat.push_back(std::string(static_cast<size_t>(lyr_w), ' '));
        }
        const int start = active_row + active_count / 2 - body_h / 2;
        for (int r = 0; r < body_h; ++r) {
            const int idx = start + r;
            if (idx >= 0 && idx < static_cast<int>(flat.size())) lyr_rows[static_cast<size_t>(r)] = flat[static_cast<size_t>(idx)];
        }
    }

    const std::string gap(static_cast<size_t>(kGap), ' ');
    for (int r = 0; r < body_h; ++r) {
        frame << bar << " ";
        if (show_art) frame << art_rows[static_cast<size_t>(r)] << gap;
        frame << lyr_rows[static_cast<size_t>(r)];
        if (show_art) frame << gap << art_rows[static_cast<size_t>(r)];
        frame << " " << bar << "\n";
    }

    std::string footer;
    if (has_track_) footer = "[ " + format_mmss(elapsed) + " / " + format_mmss(static_cast<double>(total_sec_)) + " ]";
    if (std::chrono::steady_clock::now() < karaoke_flash_until_)
        footer += std::string(footer.empty() ? "" : "  ") + "[ lyrics size " + std::to_string(size) + " / 5 ]";
    frame << box_bottom(W, footer, border_bottom) << "\n";
    const std::string up = mode_switch_key_label();
    const std::string down = up == "*" ? "_" : up == "SHIFT and +" ? "SHIFT and -" : "SHIFT+-";
    const std::string size_keys = up == "SHIFT and +" ? "SHIFT and +/-" : up + " / " + down;   // "[SHIFT and +/-]" rather than "[SHIFT and + / SHIFT and -]"
    const std::string legend = "[ESC/" + hotkey_text("HKeyKaraoke", "k") + "] Close | [" + size_keys + "] Lyrics size | ["
        + hotkey_text("HKeyTogglePlayPause", "p") + "] Play/Pause | [" + hotkey_text("HKeyPlayNextSong", "n") + "/"
        + hotkey_text("HKeyPlayPreviousSong", "b") + "] Next/Prev | [←→] Seek | [" + hotkey_text("HKeyIncreaseVolume", "+") + "/"
        + hotkey_text("HKeyDecreaseVolume", "-") + "] Volume | [" + hotkey_text("HKeyToggleMute", "x") + "] Mute | [" + hotkey_text("HKeyLyricsTiming", "ALT+L") + "] Timing";
    frame << legend_sgr(settings_) << truncate_str(legend, W) << R;
}

// ---------------------------------------------------------------------
// Help wizard (Mode::Wizard, HKeyWizard = SHIFT+w) -- see app.h for the
// state and src/wizard.h for the text, its markup and the key handling.
// This only draws what wizard::render() hands over.
// ---------------------------------------------------------------------

bool App::wizard_allowed_over_mode() const {
    switch (mode_) {
        case Mode::Settings:    return theme_ui_ == 0 && leave_prompt_ == 0;   // not in the theme editor or a question
        case Mode::Console: case Mode::Cheatsheet: case Mode::History: case Mode::ReadLyrics:
        case Mode::OsciMenu: case Mode::NormMenu: case Mode::SpectroMenu: case Mode::SleepTimer:
        case Mode::LyricsEdit: case Mode::Karaoke:
            return true;
        case Mode::Equalizer:   return !eq_naming_;                            // the "Save as:" prompt takes typed text
        case Mode::Playlist: case Mode::MetaEdit: return !text_entry_active(); // a text field has the keyboard
        default:                return false;                                  // text fields (search, playlist and meta editors ...), prompts
    }
}

void App::wizard_open() {
    if (!wizard::kEnabled) return;   // switched off (wizard.h)
    if (mode_ != Mode::Wizard) wizard_return_mode_ = mode_;
    wizard::open(wizard_st_, !settings_.wizard_seen);
    mode_ = Mode::Wizard;
}

void App::wizard_close() {
    mode_ = wizard_return_mode_;
    wizard_return_mode_ = Mode::Browse;
    force_redraw_ = true;
    // The first close is what turns the auto-open off for good. Written
    // straight away rather than at exit, so a crash cannot bring the
    // welcome back on the next start.
    if (!settings_.wizard_seen) {
        settings_.wizard_seen = true;
        save_settings(settings_);
    }
}

void App::build_wizard_screen(std::ostringstream& frame, int W) {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string base = ansi_for(settings_.list_color, false);
    // Same height as every other full-screen view (see build_cheatsheet_screen).
    const int body_rows = std::max(1, term_rows_ - 3);

    // The disk of the first page turns at a fixed 0.10 here, whatever Settings -> ANIMA/VIZ -> Disk Speed says and
    // whether or not anything is playing, so the greeting always has some life.
    std::vector<std::string> disk;
    if (wizard::shows_disk(wizard_st_.page)) disk = disk_.frame(wizard_angle_);
    const wizard::View v = wizard::render(wizard_st_, W, body_rows, disk);

    const std::string lead(static_cast<size_t>(v.left), ' ');
    const std::string trail(static_cast<size_t>(std::max(0, W - v.left - v.box_w)), ' ');
    // The title is drawn in the header colour; the border colour comes back right in front of the dashes.
    frame << lead << box_top(header_sgr(settings_) + v.title + R + "\x01", v.box_w, border) << trail << "\n";

    for (const wizard::Row& row : v.rows) {
        frame << lead << bar;
        for (const wizard::Span& s : row) {
            switch (s.role) {
                case wizard::Role::Header: frame << header_sgr(settings_) << s.text << R; break;
                case wizard::Role::Legend: frame << legend_sgr(settings_) << s.text << R; break;
                case wizard::Role::Cursor:
                    frame << ansi_for(settings_.list_cursor_color, false) << bg_ansi_for(settings_.list_cursor_bg_color) << s.text << R;
                    break;
                case wizard::Role::Disk:
                case wizard::Role::Scroll:
                    frame << gradient_ansi(settings_.disk_color, settings_.disk_color_end, s.t) << s.text << R;
                    break;
                default: frame << base << s.text << R; break;
            }
        }
        frame << bar << trail << "\n";
    }

    std::string bottom = "[" + hotkey_text("HKeyWizard", "SHIFT+w") + " / ESC] close";
    bottom += "  [t] TOC";
    if (v.can_back) bottom += "  [<-] back";
    if (v.can_next) bottom += "  [->] next";
    if (v.toc_page) bottom += "  [\xE2\x86\x91\xE2\x86\x93] select  [SHIFT+\xE2\x86\x91\xE2\x86\x93] jump  [ENTER] open";
    else if (v.scrollable) bottom += "  [\xE2\x86\x91\xE2\x86\x93] scroll  [SHIFT+\xE2\x86\x91\xE2\x86\x93] page";
    frame << lead << box_bottom(v.box_w, bottom, border_bottom) << trail << "\n";
}

void App::build_cheatsheet_screen(std::ostringstream& frame, int W) const {
    std::string border = ansi_for(settings_.border_color, false);
    frame << box_top("CHEATSHEET", W, border) << "\n";

    // Similar shape to the Reference tab of Settings (kRefRows) for the
    // rebindable hotkeys, but this screen is the ONE place that also lists
    // every literal, non-rebindable key command -- the Reference tab no
    // longer carries its own copy of those (see its own comment for why),
    // so this table's rows are this app's entire command list, full stop.
    // `header` is set only on a category's first row and is drawn as a
    // section title in the Header colour above a blank spacer row. `action`
    // is either
    //   * a plain action name, looked up in settings_.hotkeys, so the key
    //     shown is whatever the user actually has bound (config.txt /
    //     rebound in Settings), never a hardcoded assumption; or
    //   * '#'-prefixed: a literal key label. SHIFT+b, Ctrl+Shift+S/X, ESC,
    //     the playlist/meta editors' own fixed navigation and text-editing
    //     keys, and friends are checked as raw key codes in handle_*_key()
    //     rather than looked up in settings_.hotkeys, so there is no hotkey
    //     entry for them to reference.
    struct CheatRow { const char* header; const char* action; const char* desc; };
    static const CheatRow rows[] = {
        // --- System -- listed first. Besides the rebindable system hotkeys
        // this also carries the literal keys used across multiple overlays
        // (not tied to one editor's own legend below). ---
        {"SYSTEM (MAIN UI)", "#@SWITCHKEY", "Switch to the RADIO mode (types the * character; playback is paused and the player waits in the background)"},
        {nullptr, "HKeySetting", "Open Settings panel, hit again to save and quit"},
        {nullptr, "HKeyConsole", "Console / logs"},
        {nullptr, "HKeyWizard", "Help wizard: the user guide as tutorial pages (arrow keys turn them)"},
        {nullptr, "HKeyCheatsheet", "This cheatsheet"},
        {nullptr, "HKeyQuit", "Quit"},
        {nullptr, "#ESC", "Close setting / overlay / menu"},
        {nullptr, "#ENTER", "Confirm / select"},
        {nullptr, "#ARROW KEYS", "Navigate (context-dependent)"},
        {nullptr, "#Y / N", "Confirm or cancel a prompt"},
        // --- Playback ---
        {"PLAYBACK (MAIN UI)", "HKeyPlay", "Play the selected track"},
        {nullptr, "HKeyTogglePlayPause", "Play / pause"},
        {nullptr, "HKeyPlayNextSong", "Play next in list/queue"},
        {nullptr, "HKeyPlayPreviousSong", "Play previous in list"},
        {nullptr, "HKeyShuffleNext", "Shuffle to a random next track"},
        {nullptr, "HKeyCyclePlayMode", "Cycle play mode (list/repeat/shuffle/stop/queue then stop)"},
        {nullptr, "HKeySeekForward", "Seek forward 5s"},
        {nullptr, "HKeySeekBackward", "Seek backward 5s"},
        {nullptr, "HKeyIncreaseVolume", "Volume up"},
        {nullptr, "HKeyDecreaseVolume", "Volume down"},
        {nullptr, "HKeyToggleMute", "Mute (without pausing)"},
        {nullptr, "HKeyToggleNormalize", "Toggle loudness normalization"},
        {nullptr, "HKeyNormMenu", "Normalization overlay: on/off, target level, max boost live (toggle)"},
        {nullptr, "#UP/DOWN  LEFT/RIGHT", "Normalization overlay: pick a value / change it   [SPACE] on/off   [R] reset   [ESC] close"},
        {nullptr, "HKeySleepTimer", "Sleep timer overlay: pause after 15/30/60/90/120 min (optional fade-out over the last 10 %) or stop after this song (toggle)"},
        {nullptr, "#UP/DOWN  ENTER", "Sleep timer overlay: pick an entry / set it ('Fade out' toggles the fade)   [ESC] close (the Stop play mode is left alone)"},
        // --- Navigation & View: moving around the lists and what they show ---
        {"NAVIGATION & VIEW (MAIN UI)", "HKeyNavigateUp", "Explore list (up)"},
        {nullptr, "HKeyNavigateDown", "Explore list (down)"},
        {nullptr, "HKeySwitchBetweenCards", "Switch between panels (list / queue)"},
        {nullptr, "HKeyFilterForFolder", "Filter by folder"},
        {nullptr, "HKeyClearFilter", "Clear filter"},
        {nullptr, "HKeyCycleSortMode", "Cycle local list sort mode (folder order / title A-Z / artist A-Z)"},
        {nullptr, "HKeyToggleMetaOnly", "Toggle metadata-only track list (no filename)"},
        {nullptr, "HKeyToggleWaveform", "Toggle waveform style (raw/smooth)"},
        {nullptr, "HKeyFolderOverlay", "Big folder overlay: tree of the library's folders (ENTER opens, f shows the folder in the list, / searches)"},
        {nullptr, "HKeyListOverlay", "Big list overlay: larger LOCAL AUDIO FILES pane (toggle)"},
        {nullptr, "HKeyQueueOverlay", "Big queue overlay: larger QUEUE pane (toggle; replaces the list overlay)"},
        {nullptr, "#SHIFT+UP/DOWN", "Big list / queue / folder / conversion overlay: previous / next page (faster scrolling)"},
        {nullptr, "#ESC", "Big list / queue / folder / conversion overlay: close (playback, queue and list keys keep working)"},
        {nullptr, "#ENTER, LEFT/RIGHT", "Big folder overlay: open / close the hovered folder (LEFT on a closed one jumps to its parent)"},
        {nullptr, "#CTRL+p", "Big folder / conversion overlay: play the hovered file (the overlay stays open)"},
        {nullptr, "#/", "Big folder overlay: search the folder tree (ENTER or ESC leaves the search field)"},
        {nullptr, "HKeyRefreshUi", "Refresh UI (redraw)"},
        // --- File conversion: its own category ---
        {"FILE CONVERSION (MAIN UI)", "HKeyConvertOverlay", "File conversion overlay: turn high resolution files into MP3 / AAC / Opus (marked files go to the CONVERSION EXPORT PATH, the originals stay untouched)"},
        {nullptr, "#SPACE, a", "Conversion overlay: mark / unmark the hovered file / mark or unmark everything shown"},
        {nullptr, "#ENTER, LEFT/RIGHT", "Conversion overlay: ENTER converts the marked files (or the hovered one); LEFT/RIGHT choose the target (MP3 320 / V0 / 192, AAC 256, Opus 128)"},
        {nullptr, "#t, r, h", "Conversion overlay pre-filter: file type (SHIFT+t backwards) / minimum bitrate (SHIFT+r backwards) / hi-res only (above 16 bit or 48 kHz)"},
        {nullptr, "#/, f:, p:", "Conversion overlay search bar: / full file search; type f: (folders) or p: (saved playlists) and ENTER on one to show only its files; typing / again returns to the full search, ESC steps back"},
        {nullptr, "#c", "Conversion overlay: cancel the running conversion (it keeps running when the overlay is closed)"},
        // --- Visualizations and lyrics ---
        {"VISUALIZATIONS AND LYRICS (MAIN UI)", "HKeyToggleLyrics", "Cycle the lyrics area forward: sphere / oscilloscope / spectrogram / lyrics (what is off on Settings > ON/OFF is skipped)"},
        {nullptr, "HKeyToggleLyricsBack", "Cycle the lyrics area backward (same order, reversed)"},
        // --- Visualizations: oscilloscope and spectrogram (in the lyrics area, full screen, in their own windows) ---
        {nullptr, "HKeyOscMenu", "Oscilloscope overlay: style, frame rate, music mode, decay, dot threshold, tail, glow, Z axis ... live -- braille and image keep their own values (toggle)"},
        {nullptr, "#UP/DOWN  LEFT/RIGHT", "Oscilloscope overlay: pick a value / change it   [R] reset the style in use   [ESC] close"},
        {nullptr, "#OSCI MUSIC MODE", "Oscilloscope overlay row: unprocessed signal, fixed scale, no phase portrait -- for oscilloscope music (lossless files)"},
        {nullptr, "HKeySpectroMenu", "Spectrogram options overlay: style, motion, scale, frequencies, gain, range, window, colors, time span ... (toggle)"},
        {nullptr, "#UP/DOWN  LEFT/RIGHT", "Spectrogram overlay: pick a value / change it   [R] Audacity defaults   [ESC] close"},
        {nullptr, "HKeySpectroFull", "Spectrogram full screen with frequency labels and time ruler (toggle; ESC closes)"},
        {nullptr, "HKeyScopeBig", "Oscilloscope full screen: YT mode left, XY mode right, braille or image style (toggle; ESC closes; the options overlay opens on top of it)"},
        {nullptr, "#SHIFT+a  SHIFT+v  SHIFT+c  SHIFT+p", "Oscilloscope full screen: axes / grid on or off   axis values on or off   grid colour (small window: type 0-255, empty = colour of inactive list elements)   grid metrics (small window: time unit, grid divisions, steady readouts in the picture names: RMS level, peak hold, L/R correlation, L/R balance)"},
        {nullptr, "HKeyScopeWindow", "Scope window: the oscilloscope in its own window, drawn by the graphics card, braille or image style as set in the SHIFT+o overlay (toggle; needs SDL2)"},
        {nullptr, "HKeySpectroWindow", "Spectrogram window: the spectrogram in its own window, scrolling smoothly at the monitor's refresh rate (toggle; needs SDL2)"},
        {nullptr, "#F / F11 / T / ESC", "Scope / spectrogram window (keys inside it): fullscreen (also double-click) / always on top / close"},
        // --- Lyrics & karaoke ---
        {"LYRICS & KARAOKE (MAIN UI)", "HKeyRetryLyrics", "LYRICS REFETCH AND VERSION CONTROL: pick the lyrics in use (embedded / fetched / older) with SPACE, delete a saved copy with d (asks first), or ENTER to refetch them (with your own title / artist if you like); the old version is kept"},
        {nullptr, "HKeyLyricsTiming", "Lyrics timing overlay: shift the lyrics earlier / later (toggle; only while synced lyrics are loaded)"},
        {nullptr, "#LEFT/RIGHT  UP/DOWN", "Lyrics timing overlay: -/+ 0.1 s / -/+ 0.5 s   [R] reset   [ENTER] save to the .lrc   [ESC] cancel"},
        {nullptr, "HKeyReadLyrics", "READ LYRICS: the saved lyrics of the playing (else hovered) track as plain text; two top lines list the lyrics files (embedded / fetched / older) and the saved translations, which are shown in ( ) below each line"},
        {nullptr, "#CTRL+s", "READ LYRICS: search EVERY lyrics file by name (all lyrics folders of the library) instead of the playing track's lyrics: type to filter, UP/DOWN pick, ENTER shows the file, ESC closes the search (CTRL+L opens the playing track's lyrics again)"},
        {nullptr, "#LEFT/RIGHT  TAB  UP/DOWN  t  SHIFT+t  d", "READ LYRICS: choose the file / translation, TAB switches the pane, scroll (SPACE, b, HOME, END too)   [t] translate overlay (ENTER fetches)   [SHIFT+t] show / next / hide translations   [d] delete a translation   [ESC] close"},
        {nullptr, "HKeyKaraoke", "Karaoke overlay: the lyrics over the whole screen, sung words highlighted (toggle; fetches the lyrics even with the lyrics engine off; playback keys keep working)"},
        {nullptr, "#@KARAOKESIZE", "Karaoke overlay: lyrics bigger / smaller, sizes 1-5 (the characters * and _; saved when the overlay closes)"},
        // --- Search ---
        {"SEARCH LOCAL (MAIN UI)", "HKeySearch", "Search the local library (title, artist and album; type to filter live, ENTER keeps the result)"},
        {nullptr, "#/r:", "Search ARTISTS only: type r: first (e.g. r:beatles) -- only the artist tag / folder name is matched"},
        {nullptr, "#/a:", "Search ALBUMS only: type a: first (e.g. a:abbey road) -- only the album tag is matched (tags are read in the background, so it gets more complete after a moment)"},
        {nullptr, "#/p:", "Search saved playlists (type /p:query)"},
        {nullptr, "#/f:", "Search folders (type /f:query), ENTER lists all files of that folder"},
        {"SEARCH ONLINE (MAIN UI)", "#/s:", "Search online (YouTube; type s:query)"},
        {nullptr, "#/sc:", "Search SoundCloud (type it in the search box; ENTER plays, the download key saves it)"},
        {nullptr, "#/b:", "Search Bandcamp (tracks; ENTER plays the free stream, the download key saves it)"},
        // --- Queue ---
        {"QUEUE (MAIN UI)", "HKeyAddHoveringSongToQueue", "Add hovering track as NEXT (repeated presses keep their order)"},
        {nullptr, "HKeyQueueAddEnd", "Add hovering track to the END of the queue"},
        {nullptr, "HKeyRemoveHoveringSongFromQueue", "Remove hovering track from queue"},
        {nullptr, "HKeyQueueMoveUp", "Move hovering queue item up"},
        {nullptr, "HKeyQueueMoveDown", "Move hovering queue item down"},
        {nullptr, "HKeyQueueMoveTop", "Move hovering queue item to the top"},
        {nullptr, "HKeyQueueMoveBottom", "Move hovering queue item to the bottom"},
        {nullptr, "HKeyQueueLock", "Lock / unlock the queue (locked = default: played track goes to the end; unlocked: it leaves)"},
        {nullptr, "HKeyClearQueue", "Clear the whole queue (asks Yes / No first)"},
        {nullptr, "HKeyQueueUndoClear", "Undo the last queue clear"},
        {nullptr, "HKeyQueueToPlaylist", "Save the queue as a playlist (opens the playlist editor)"},
        // --- Playlists -- HKeyPlaylist opens the overlay; every other row
        // is the playlist editor's own fixed legend (build_playlist_screen()'s
        // footer), none of which is a rebindable hotkey.
        {"PLAYLISTS", "HKeyPlaylist", "Open Playlists (create / manage)"},
        {nullptr, "#LEFT/RIGHT", "Switch tab (Create/Edit vs Saved Playlists); inside a name / search field after typing: move the caret"},
        {nullptr, "#ESC", "Leave the field you typed in (LEFT/RIGHT switch tabs again); outside a field: close (asks to save unsaved changes)"},
        {nullptr, "#" MUISC_ALT_NAME_UC "+LEFT/RIGHT", "Switch tab from anywhere, also while typing"},
        {nullptr, "#TAB", "Cycle focus (name field / library picker / track list)"},
        {nullptr, "#UP/DOWN", "Navigate the focused list/picker (fixed arrow keys)"},
        {nullptr, "#ENTER", "Add hovering track to playlist / load selected playlist"},
        {nullptr, "#e", "Saved Playlists tab: export the hovered playlist as M3U8 (default) or M3U -- overlay with folder and format"},
        {nullptr, "#i", "Saved Playlists tab: import .m3u / .m3u8 files from a folder (default: export/playlist folder) -- SPACE marks, a marks all"},
        {nullptr, "#4 / 5", "Move the hovering track up/down"},
        {nullptr, "#D / DEL / BACKSPACE", "Remove hovering track / delete selected playlist"},
        {nullptr, "#CTRL+s", "Save the playlist"},
        {nullptr, "#SHIFT+LEFT/RIGHT", "Mark text (name / search fields)"},
        {nullptr, "#CTRL+C/X/V", "Copy / cut / paste text"},
        // --- Meta editor -- same deal: HKeyMetaEditor opens it, everything
        // else is build_meta_screen()'s own fixed legend (both tabs).
        {"META EDITOR", "HKeyMetaEditor", "Meta editor: edit file name / artist / title / album / year"},
        {nullptr, "#LEFT/RIGHT", "Switch tab (Edit vs Fetch List); in the search field after typing and in the field editor: move the caret"},
        {nullptr, "#ESC", "Leave the search field / field editor; outside a field: close (pending edits are kept)"},
        {nullptr, "#" MUISC_ALT_NAME_UC "+LEFT/RIGHT", "Switch tab from anywhere, also while typing"},
        {nullptr, "#TAB", "Meta editor: cycle panels (search / library / fields)"},
        {nullptr, "#UP/DOWN", "Navigate the focused list/picker (fixed arrow keys)"},
        {nullptr, "#ENTER", "Edit the hovering field (Fetch List tab: fetch the full list; fetched field with several options: choose)"},
        {nullptr, "#SHIFT+LEFT/RIGHT", "Mark text (search field / field editor)"},
        {nullptr, "#CTRL+C/X/V", "Copy / cut / paste text (search field / field editor)"},
        {nullptr, "#a", "Meta editor: add the hovering file to the fetch list"},
        {nullptr, "#SHIFT+f", "Meta editor (library pane): add the full list -- every title the pane shows now (search and missing-tag filter applied) -- to the fetch list"},
                {nullptr, "#TAB  LEFT/RIGHT  SPACE (Fetch List)", "Meta editor, Fetch List tab: TAB moves between the list and the fetch option line; LEFT/RIGHT move over the boxes, SPACE ticks one (missing only / all exclude the rest; the tag boxes combine); ENTER fetches the full list"},
        {nullptr, "#UP/DOWN  ENTER  DEL/d  ESC", "Meta editor choice overlay: select / take it / discard the fetched value and all options / close"},
        {nullptr, "#r", "Meta editor: toggle edited files on top of the library pane"},
        {nullptr, "#x, SHIFT+/t/a/l/y", "Filter + sort the library pane by missing metadata: x = missing any, SHIFT+t = title, SHIFT+a = artist, SHIFT+l = album, SHIFT+y = year (same order as the fields)"},
        {nullptr, "#SHIFT+p", "Meta editor (library pane / fetch list): start the hovering track"},
        {nullptr, "HKeyFetchTags", "Meta editor / main list: fetch the missing tags (empty ones only) of the hovered title (AcoustID)"},
        {nullptr, "#SHIFT+n", "Meta editor (library pane / fetch list): fetch ALL tags of the hovered title, overwriting existing ones"},
        {nullptr, "#SHIFT+c", "Meta editor, Fetch List tab: cancel the running AcoustID fetch (what had arrived stays as pending edits, the rest stays on the list). Leaving the editor does not stop a fetch; quitting does"},
        {nullptr, "HKeyRescan", "Rescan the library (new files); also in the meta data editor (library pane / fetch list focused)"},
        {nullptr, "#DEL / d", "Remove hovering track from the fetch list"},
        {nullptr, "#CTRL+s", "Apply the meta editor's pending edits to the files (asks first)"},
        {nullptr, "#CTRL+SHIFT+x", "Discard the meta editor's pending edits"},
        // --- Settings ---
        {"SETTINGS", "#TAB", "Next tab (not while a field is being edited)"},
        {nullptr, "#UP/DOWN  LEFT/RIGHT", "Previous / next row  |  cycle a switch or choice / pick the first or second colour cell"},
        {nullptr, "#ENTER", "Edit the selected colour, path, value or key in place"},
        {nullptr, "#LEFT/RIGHT (editing)", "Move the caret; SHIFT+LEFT/RIGHT marks, HOME/END jump, CTRL+C/X/V copy / cut / paste"},
        {nullptr, "#ENTER / ESC (editing)", "Apply the edit / cancel it -- only these two leave the field"},
        {nullptr, "#s / ESC / q", "Save to config.txt and close / close without saving (with unsaved changes it asks first: [s] save, [d] discard, ESC keeps editing)"},
        {nullptr, "#ENTER (THEME row)", "Colors tab: ENTER opens CHOOSE THEME (scrollable list, ENTER applies, SHIFT+d deletes a theme of your own); the CREATE THEME button builds your own theme (s saves it)"},
        {nullptr, "#e", "Colors tab: edit the theme in use in the theme creator (a theme of your own can be overwritten, a built-in one needs a new name)"},
        {nullptr, "#SHIFT+e / SHIFT+i", "Colors tab: export the current colors as a theme file / import theme files from a folder"},
        // --- Settings (REFERENCE tab) ---
        {"SETTINGS (REFERENCE TAB)", "#ENTER", "Change the key of the selected command (type the new key, ENTER applies)"},
        {nullptr, "#DEL", "Restore the default key of the selected command"},
        {nullptr, "#ENTER on the first line", "Reset all keys to their defaults"},
        {nullptr, "#ALT+L, CTRL+L ...", "While a key is edited: a combination that cannot be typed (ALT+L, CTRL+L, CTRL+SHIFT+U / Z) is entered by pressing it; ENTER with an empty field unbinds the command"},
        {nullptr, "#CTRL+SHIFT+u", "Undo the last key change or reset (up to 5, newest first)"},
        // --- Listening history ---
        {"HISTORY", "HKeyHistory", "Listening history: last plays, top tracks, top playlists, habits"},
        {nullptr, "#LEFT/RIGHT  1 / 2 / 3 / 4", "History overlay: switch tab (TAB never switches tabs)"},
        {nullptr, "#UP/DOWN  ENTER", "History overlay: move the cursor / scroll Top Playlists and Habits; ENTER plays the hovered track"},
        {nullptr, "#r", "History overlay: most-played first <-> least-played first (Top Tracks)"},
        {nullptr, "#TAB", "Top Tracks tab: track list <-> ADD SMART HISTORY TO QUEUE pane"},
        {nullptr, "#ARROWS  ENTER", "Smart history pane: pick a list (Top 10-100, top of the week / month / quarter / year, morning / day / evening / night, newly added, least played) / add it to the queue"},
        {nullptr, "#ESC", "Smart history pane: back to the track list (ESC again closes the history)"},
        // --- Equalizer ---
        {"EQUALIZER", "HKeyEqualizer", "Equalizer: 10 bands, presets, custom presets, on/off"},
        {nullptr, "#LEFT / RIGHT", "Equalizer: select band"},
        {nullptr, "#UP / DOWN", "Equalizer: band gain +1 / -1 dB"},
        {nullptr, "#, / . / TAB", "Equalizer: previous / next preset (built-in, then custom)"},
        {nullptr, "#SPACE / 0 / r", "Equalizer: on-off / zero the band / reset to Flat"},
        {nullptr, "#s", "Equalizer: save the current curve as a custom preset (ENTER saves, ESC cancels)"},
        {nullptr, "#DEL / x", "Equalizer: delete the selected custom preset (press twice)"},
        // --- Downloads ---
        {"DOWNLOADS", "HKeyDownloadStream", "Save stream to the download folder (Settings > Download Path, else .cache/mousiki)"},
    };

    // Same height as every other full-screen view: term_rows_ - 1 lines (the
    // terminal's last row stays untouched so a trailing newline can never
    // scroll), of which 2 are this overlay's own top/bottom border rows. It
    // used to stop 3 lines short of that, leaving blank rows below the box
    // on every terminal size (120x30 included) -- there is no status/prompt
    // row here that would need them.
    int visible = std::max(1, term_rows_ - 3);

    // Every entry becomes display lines first (a category costs a blank spacer + its title, a long description wraps),
    // so the scroll range counts what is really drawn. The description column starts after the widest key + 2 spaces.
    const int inner = std::max(0, W - 4);
    auto key_of = [&](const CheatRow& r) {
        if (r.action[0] == '#') return pretty_key(std::string(r.action + 1));
        auto it = settings_.hotkeys.find(r.action);
        return pretty_key((it != settings_.hotkeys.end() && !it->second.empty()) ? it->second : std::string("-"));
    };
    int key_w = 0;
    for (const auto& r : rows) key_w = std::max(key_w, display_width(key_of(r)));
    key_w = std::min(key_w, std::max(10, inner / 2));
    const int desc_col = key_w + 2;
    const int desc_w = std::max(10, inner - desc_col);
    struct DLine { std::string plain, sgr; };
    std::vector<DLine> dlines;
    for (const auto& r : rows) {
        if (r.header) {
            dlines.push_back({std::string(), ""});                       // blank spacer row...
            dlines.push_back({r.header, header_sgr(settings_)});         // ...then the title, Header colour + bold
        }
        const std::string key = key_of(r);
        const auto parts = wrap_words(r.desc, desc_w);
        for (size_t i = 0; i < parts.size(); ++i)
            dlines.push_back({(i == 0 ? pad_right(truncate_str(key, key_w), desc_col) : std::string(static_cast<size_t>(desc_col), ' ')) + parts[i], ""});
    }
    const int total = static_cast<int>(dlines.size());
    const int max_scroll = std::max(0, total - visible);
    cheatsheet_scroll_ = std::clamp(cheatsheet_scroll_, 0, max_scroll);

    // A coloured line can't go through box_line(): that pads by counting bytes, so it would treat the escape codes
    // as columns. Pad the plain text first, then wrap the padded result.
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    int shown = 0;
    for (int i = cheatsheet_scroll_; i < total && shown < visible; ++i, ++shown) {
        const DLine& d = dlines[static_cast<size_t>(i)];
        frame << bar << " ";
        if (!d.sgr.empty()) frame << d.sgr;
        frame << pad_right(d.plain, inner);
        if (!d.sgr.empty()) frame << R;
        frame << " " << bar << "\n";
    }
    while (shown < visible) { frame << box_line("", W, border) << "\n"; ++shown; }

    std::string bottom = "[" + hotkey_text("HKeyCheatsheet", "?") + " / ESC] close";
    if (max_scroll > 0) bottom += "  [\u2191\u2193] scroll " + std::to_string(cheatsheet_scroll_ + 1) + "/"
                                + std::to_string(total);
    frame << box_bottom(W, bottom, border) << "\n";
}

// ---------------------------------------------------------------------
// Bulk add overlay (paste-a-playlist-link panel, "a" while Queue focused)
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Bulk add overlay (paste-a-playlist-link panel, "a" while Queue focused)
// ---------------------------------------------------------------------
// Deliberately NOT sized like Console/Settings/Cheatsheet -- those match
// the player view or the terminal on purpose (real overlays meant to
// take over the screen). This one is a small floating panel that only
// takes as many rows as it actually has content for: an input row while
// typing, then a short starred checklist once results come in. It never
// grows to fill the terminal.

// ---------------------------------------------------------------------
// Big list overlay (HKeyListOverlay, SHIFT+l) -- see app.h for the design.
// ---------------------------------------------------------------------

namespace {
inline int list_total_for(ListSource src, size_t local, size_t online, size_t playlists, size_t folders) {
    return static_cast<int>(src == ListSource::Local ? local : src == ListSource::Online ? online
                          : src == ListSource::Folder ? folders : playlists);
}
} // namespace

// The overlay is only shown (and only steers scrolling) in the modes that
// have the main UI underneath it. In the full-screen takeovers (Settings,
// Playlist, ...) the flag simply stays set and the overlay is back when
// they close.
bool App::list_overlay_active() const {
    if (!list_overlay_open_) return false;
    switch (mode_) {
        case Mode::Browse: case Mode::Search: case Mode::BulkAdd:
        case Mode::RetryLyrics: case Mode::ClearQueue: case Mode::OsciMenu: case Mode::SpectroMenu: case Mode::NormMenu: case Mode::Equalizer: case Mode::SleepTimer: case Mode::LyricsEdit: return true;
        default: return false;
    }
}

// The key as it should read in a legend: what is actually bound (so a
// rebinding shows up), with the SHIFT+ spelling used in the README for an
// uppercase letter and for "$" / "%" (Shift+4 / Shift+5 on US and German
// layouts alike).
std::string App::hotkey_text(const char* action, const char* fallback) const {
    auto it = settings_.hotkeys.find(action);
    std::string k = (it != settings_.hotkeys.end()) ? it->second : std::string(fallback);
    if (k.empty()) return "-"; // unbound
    if (k == "$") return "SHIFT+4";
    if (k == "%") return "SHIFT+5";
    if (k == "(" || k == ")" || k == "=") return symbol_key_label(k);   // the window keys and the scope overlay key, named for this keyboard
    if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z') return std::string("SHIFT+") + static_cast<char>(k[0] + 32);   // SHIFT+x, like the literal legends
    if (k.find('+') != std::string::npos && k.size() > 1) return pretty_key(k);   // ALT+L, CTRL+L, CTRL+SHIFT+U ...
    return k;
}

namespace {
// "[a] first  [b] second ..." packed greedily into lines of at most `inner_w`
// columns, at most `max_lines` of them (items that no longer fit are dropped
// from the end, which is why the most important ones come first).
std::vector<std::string> pack_legend(const std::vector<std::string>& items, int inner_w, int max_lines) {
    std::vector<std::string> lines;
    if (max_lines <= 0) return lines;
    std::string cur;
    for (const auto& it : items) {
        const std::string candidate = cur.empty() ? it : cur + "   " + it;
        if (cur.empty() || display_width(candidate) <= inner_w) { cur = candidate; continue; }
        lines.push_back(cur);
        if (static_cast<int>(lines.size()) >= max_lines) return lines;
        cur = it;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}
// "SHIFT+4" + "SHIFT+5" -> "SHIFT+4/5", anything else -> "a/b".
std::string join_keys(const std::string& a, const std::string& b) {
    const std::string pre = "SHIFT+";
    if (a.rfind(pre, 0) == 0 && b.rfind(pre, 0) == 0) return a + "/" + b.substr(pre.size());
    return a + "/" + b;
}
} // namespace

// Key legend of the big list overlay: ONE OR MORE plain lines BELOW the frame
// (like the playlist / meta editor footers), in their gray. Every entry is
// kept whole -- an entry that no longer fits the panel width moves to the next
// line instead of being cut in two -- and the lines are padded to the panel
// width so they also wipe the background underneath. Never more lines than
// leave >= 3 list rows. Geometry (which shortens the frame by these lines)
// and painter both ask this.
std::vector<std::string> App::list_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kListOverlayChromeRows - 5, 1, 3);
    std::vector<std::string> items = {
        "[" + hotkey_text("HKeyAddHoveringSongToQueue", "a") + "] add to queue (next)",
        "[" + hotkey_text("HKeyQueueAddEnd", "e") + "] add to end of queue",
        "[SHIFT+UP/DOWN] page",
        "[ESC] close",
    };
    return pack_legend(items, std::max(0, panel_w), max_lines);
}

// Key legend of the big queue overlay; same scheme as list_overlay_legend().
std::vector<std::string> App::queue_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kQueueOverlayChromeRows - 5, 1, 3);
    std::vector<std::string> items = {
        "[" + hotkey_text("HKeyRemoveHoveringSongFromQueue", "d") + "] delete",
        "[" + hotkey_text("HKeyClearQueue", "X") + "] clear all",
        "[" + join_keys(hotkey_text("HKeyQueueMoveUp", "4"), hotkey_text("HKeyQueueMoveDown", "5")) + "] move up/down",
        "[" + join_keys(hotkey_text("HKeyQueueMoveTop", "$"), hotkey_text("HKeyQueueMoveBottom", "%")) + "] move to top/bottom",
        "[" + hotkey_text("HKeyQueueLock", "!") + "] " + (queue_locked_ ? "unlock" : "lock"),
        "[" + hotkey_text("HKeyQueueToPlaylist", "CTRL+SHIFT+U") + "] queue to playlist",
        "[" + hotkey_text("HKeyQueueUndoClear", "CTRL+SHIFT+Z") + "] undo clear",
        "[SHIFT+UP/DOWN] page",
        "[ESC] close",
    };
    return pack_legend(items, std::max(0, panel_w), max_lines);
}

namespace {
// One legend line as drawn below an overlay frame: in the legend color (the
// Colors tab's LEGEND row, `sgr` = legend_sgr()), padded to the panel width
// (so it overwrites whatever the background has under it).
std::string legend_row(const std::string& text, int panel_w, const std::string& sgr) {
    const int pad = std::max(0, panel_w - display_width(text));
    return sgr + text + std::string(pad, ' ') + "\x1b[0m";
}
} // namespace

void App::list_overlay_geometry(int W, int& panel_w, int& list_rows) const {
    panel_w = std::min(W, std::max(40, std::min(W - 6, 160)));
    // Rows 2 .. term_rows_-1 belong to the panel: search bar (3) + list box
    // border (2) + the list rows themselves.
    // ...minus the key legend lines below the frame, so the whole overlay
    // still ends on row term_rows_ - 1 and never clips (30-row terminals
    // included).
    const int legend = static_cast<int>(list_overlay_legend(panel_w).size());
    list_rows = std::max(1, term_rows_ - 2 - kListOverlayChromeRows - legend);
}

// Keeps selected_ inside a window of `rows` rows and the window inside the
// list (no empty rows after the last entry when the list is long enough).
void App::list_overlay_fit_scroll(int rows) {
    rows = std::max(1, rows);
    const int total = list_total_for(list_source_, local_view_.size(), online_view_.size(), playlist_view_.size(), folder_view_.size());
    if (total <= 0) { selected_ = 0; scroll_ = 0; return; }
    selected_ = std::clamp(selected_, 0, total - 1);
    scroll_ = std::clamp(scroll_, 0, std::max(0, total - rows));
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + rows) scroll_ = selected_ - rows + 1;
}

void App::list_overlay_open() {
    if (convert_overlay_open_) convert_overlay_close();
    folder_overlay_open_ = false; // the overlays are mutually exclusive
    queue_overlay_open_ = false; // the two overlays are mutually exclusive (focus is reset just below)
    list_overlay_open_ = true;
    queue_focus_ = false; // the queue pane is hidden while the overlay is up
    list_overlay_fit_scroll(overlay_list_rows_);
}

void App::list_overlay_close() {
    list_overlay_open_ = false;
    // Both panes share selected_/scroll_: pull the window back so the cursor
    // is inside the small pane's (much shorter) view again.
    list_overlay_fit_scroll(list_visible_rows_);
}

// One page up/down. The cursor keeps its row within the window, so the eye
// stays in the same place while the list flips underneath it. At either end
// (window can't move any further) the cursor jumps to the first/last entry.
void App::list_overlay_page(int dir) {
    const int total = list_total_for(list_source_, local_view_.size(), online_view_.size(), playlist_view_.size(), folder_view_.size());
    if (total <= 0) return;
    const int rows = std::max(1, overlay_list_rows_);
    const int max_scroll = std::max(0, total - rows);
    const int rel = std::clamp(selected_ - scroll_, 0, rows - 1);
    const int new_scroll = std::clamp(scroll_ + dir * rows, 0, max_scroll);
    if (new_scroll == scroll_) {
        selected_ = (dir > 0) ? total - 1 : 0;
    } else {
        scroll_ = new_scroll;
        selected_ = std::clamp(new_scroll + rel, 0, total - 1);
    }
    list_overlay_fit_scroll(rows);
}

// The search bar and the list pane at overlay size. The list itself is the
// normal build_list_panel() (same rows, colours, sort/folder title), just
// given the wider/taller box -- only its bottom border is replaced, to carry
// the page counter and the queue size (the queue pane is hidden, so this is
// the only feedback for "add to queue"); the key legend follows as separate
// gray lines BELOW the frame.
std::vector<std::string> App::build_list_overlay_panel(int panel_w, int list_rows) const {
    std::vector<std::string> out = build_search_bar(panel_w);
    std::vector<std::string> list = build_list_panel(panel_w, list_rows);
    if (!list.empty()) {
        const int total = list_total_for(list_source_, local_view_.size(), online_view_.size(), playlist_view_.size(), folder_view_.size());
        const int rows = std::max(1, list_rows);
        const int pages = std::max(1, (total + rows - 1) / rows);
        const int page = std::clamp((scroll_ + rows / 2) / rows + 1, 1, pages);
        const int remaining = total - (scroll_ + rows);

        std::vector<std::string> parts; // dropped from the back until it fits
        if (remaining > 0) parts.push_back("( " + std::to_string(remaining) + " more )");
        parts.push_back("page " + std::to_string(page) + "/" + std::to_string(pages));
        parts.push_back("queue: " + std::to_string(queue_.size()));
        std::string footer;
        for (;;) {
            footer.clear();
            for (size_t i = 0; i < parts.size(); ++i) footer += (i ? "  " : "") + parts[i];
            if (parts.empty() || display_width(footer) + 5 <= panel_w) break;
            parts.pop_back();
        }
        if (parts.empty()) footer.clear();
        list.back() = box_bottom(panel_w, footer, ansi_for(settings_.border_color_bottom, false));
        // Key legend: below the frame, outside it, in the legend color.
        for (const auto& l : list_overlay_legend(panel_w)) list.push_back(legend_row(l, panel_w, legend_sgr(settings_)));
    }
    out.insert(out.end(), list.begin(), list.end());
    return out;
}

// ---------------------------------------------------------------------
// Big folder overlay (HKeyFolderOverlay, SHIFT+f) -- see app.h for the design.
// ---------------------------------------------------------------------

namespace {
// true when `dir` is `root` or lies below it (component-wise, so "/a/bc" is not under "/a/b").
bool fo_is_under(const fs::path& dir, const fs::path& root) {
    auto d = dir.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++d) {
        if (d == dir.end() || *d != *r) return false;
    }
    return true;
}
fs::path fo_normal_root(const std::string& s) {
    fs::path r = path_from_utf8(s).lexically_normal();
    if (!r.has_filename() && r.has_parent_path()) r = r.parent_path();   // "/music/" -> "/music"
    return r;
}
std::string fo_name_of(const std::string& p) {
    const std::string n = path_utf8(path_from_utf8(p).filename());
    return n.empty() ? p : n;
}
} // namespace

bool App::folder_overlay_active() const {
    return folder_overlay_open_ && mode_ == Mode::Browse;
}

// Builds the folder tree from the scanned library: the roots are the configured local music folders (a folder holding
// tracks outside every root becomes a root of its own); only folders with tracks at or below them are listed.
void App::folder_overlay_build_tree() {
    fo_subdirs_.clear(); fo_files_.clear(); fo_count_.clear(); fo_roots_.clear();
    std::vector<fs::path> roots;
    for (const auto& r : settings_.local_music_paths) if (!r.empty()) roots.push_back(fo_normal_root(r));

    std::map<std::string, std::set<std::string>> sub;
    std::set<std::string> root_keys, seen;
    for (const auto& t : all_local_tracks_) {
        const fs::path dir = t.path.parent_path();
        const std::string key = path_utf8(dir);
        fo_files_[key].push_back(path_utf8(t.path));
        if (!seen.insert(key).second) continue;
        const fs::path* best = nullptr;
        size_t best_len = 0;
        for (const auto& r : roots) {
            if (!fo_is_under(dir, r)) continue;
            size_t n = 0;
            for (auto it = r.begin(); it != r.end(); ++it) ++n;
            if (!best || n > best_len) { best = &r; best_len = n; }
        }
        const fs::path root = best ? *best : dir;
        root_keys.insert(path_utf8(root));
        fs::path cur = dir;
        while (cur != root) {
            const fs::path par = cur.parent_path();
            if (par.empty() || par == cur) break;
            if (!sub[path_utf8(par)].insert(path_utf8(cur)).second) break;   // the rest of the chain exists already
            cur = par;
        }
    }
    auto by_name = [](const std::string& a, const std::string& b) {
        const std::string x = lower(fo_name_of(a)), y = lower(fo_name_of(b));
        return x != y ? x < y : a < b;
    };
    for (auto& kv : sub) {
        std::vector<std::string> v(kv.second.begin(), kv.second.end());
        std::sort(v.begin(), v.end(), by_name);
        fo_subdirs_[kv.first] = std::move(v);
    }
    for (auto& kv : fo_files_) std::sort(kv.second.begin(), kv.second.end(), by_name);
    fo_roots_.assign(root_keys.begin(), root_keys.end());
    std::sort(fo_roots_.begin(), fo_roots_.end(), by_name);

    std::function<int(const std::string&)> count = [&](const std::string& d) -> int {
        auto c = fo_count_.find(d);
        if (c != fo_count_.end()) return c->second;
        int n = 0;
        auto f = fo_files_.find(d);
        if (f != fo_files_.end()) n += static_cast<int>(f->second.size());
        auto s = fo_subdirs_.find(d);
        if (s != fo_subdirs_.end()) for (const auto& sd : s->second) n += count(sd);
        fo_count_[d] = n;
        return n;
    };
    for (const auto& r : fo_roots_) count(r);
}

// The visible rows: the tree walk (a folder's children only when it is open), narrowed by the search words. With a
// query a folder is listed when its name or anything below it matches; the matches below it are shown, and a folder
// that matches by itself shows all of its entries once it is opened.
void App::folder_overlay_rebuild_view() {
    std::string keep;
    if (fo_sel_ >= 0 && fo_sel_ < static_cast<int>(fo_view_.size())) keep = fo_view_[static_cast<size_t>(fo_sel_)].path;
    fo_view_.clear();

    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : fo_query_) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }
    const bool filtering = !words.empty();
    auto name_match = [&](const std::string& p) {
        const std::string n = fo_name_of(p);
        for (const auto& w : words) if (!contains_ci(n, w)) return false;
        return true;
    };
    std::map<std::string, bool> below;
    std::function<bool(const std::string&)> has_below = [&](const std::string& d) -> bool {
        auto c = below.find(d);
        if (c != below.end()) return c->second;
        bool hit = false;
        auto f = fo_files_.find(d);
        if (f != fo_files_.end()) for (const auto& file : f->second) if (name_match(file)) { hit = true; break; }
        auto s = fo_subdirs_.find(d);
        if (!hit && s != fo_subdirs_.end())
            for (const auto& sd : s->second) if (name_match(sd) || has_below(sd)) { hit = true; break; }
        return below[d] = hit;
    };

    std::function<void(const std::string&, int)> emit = [&](const std::string& d, int depth) {
        const bool open = fo_expanded_.count(d) > 0;
        const bool forced = filtering && has_below(d);
        FoRow r;
        r.path = d;
        r.label = depth == 0 ? d : fo_name_of(d);
        r.is_dir = true;
        r.depth = depth;
        r.expanded = open || forced;
        auto cnt = fo_count_.find(d);
        r.count = cnt == fo_count_.end() ? 0 : cnt->second;
        fo_view_.push_back(r);
        if (!open && !forced) return;
        const bool everything = !filtering || (open && name_match(d));
        auto s = fo_subdirs_.find(d);
        if (s != fo_subdirs_.end())
            for (const auto& sd : s->second)
                if (everything || name_match(sd) || has_below(sd)) emit(sd, depth + 1);
        auto f = fo_files_.find(d);
        if (f != fo_files_.end())
            for (const auto& file : f->second) {
                if (!everything && !name_match(file)) continue;
                FoRow fr;
                fr.path = file;
                fr.label = fo_name_of(file);
                fr.depth = depth + 1;
                fo_view_.push_back(fr);
            }
    };
    for (const auto& r : fo_roots_)
        if (!filtering || name_match(r) || has_below(r)) emit(r, 0);

    fo_sel_ = 0;
    if (!keep.empty())
        for (size_t i = 0; i < fo_view_.size(); ++i) if (fo_view_[i].path == keep) { fo_sel_ = static_cast<int>(i); break; }
}

void App::folder_overlay_open() {
    if (convert_overlay_open_) convert_overlay_close();
    if (list_overlay_open_) list_overlay_close();   // the overlays are mutually exclusive
    if (queue_overlay_open_) queue_overlay_close();
    folder_overlay_open_ = true;
    fo_search_ = false;
    fo_query_.clear();
    folder_overlay_build_tree();
    // Start on the folder of the hovered track, with the way down to it opened.
    std::string target;
    if (list_source_ == ListSource::Local && selected_ >= 0 && selected_ < static_cast<int>(local_view_.size()))
        target = path_utf8(local_view_[static_cast<size_t>(selected_)].path.parent_path());
    else if (!folder_filter_.empty()) target = folder_filter_;
    if (!target.empty() && fo_files_.count(target)) {
        fs::path cur = path_from_utf8(target).parent_path();
        std::set<std::string> roots(fo_roots_.begin(), fo_roots_.end());
        std::string k = path_utf8(cur);
        while (!k.empty()) {
            if (fo_subdirs_.count(k)) fo_expanded_.insert(k);
            if (roots.count(k)) break;
            const fs::path par = path_from_utf8(k).parent_path();
            if (par.empty() || path_utf8(par) == k) break;
            k = path_utf8(par);
        }
    }
    fo_sel_ = 0; fo_scroll_ = 0;
    folder_overlay_rebuild_view();
    if (!target.empty())
        for (size_t i = 0; i < fo_view_.size(); ++i)
            if (fo_view_[i].is_dir && fo_view_[i].path == target) { fo_sel_ = static_cast<int>(i); break; }
}

void App::folder_overlay_close() {
    folder_overlay_open_ = false;
    fo_search_ = false;
}

// `f` in the overlay: the hovered folder (or the folder of the hovered file) goes into the LOCAL AUDIO FILES pane,
// exactly as `f` on a file does there; the file itself ends up hovered. Closes the overlay.
void App::folder_overlay_filter() {
    if (fo_sel_ < 0 || fo_sel_ >= static_cast<int>(fo_view_.size())) return;
    const FoRow row = fo_view_[static_cast<size_t>(fo_sel_)];
    const std::string dir = row.is_dir ? row.path : path_utf8(path_from_utf8(row.path).parent_path());
    folder_filter_ = dir;
    last_local_query_.clear();   // otherwise an old query would hide files of the folder
    local_sort_mode_ = 0;        // folder order, like `f`
    list_source_ = ListSource::Local;
    refresh_local_view();
    if (!row.is_dir) {
        for (size_t i = 0; i < local_view_.size(); ++i)
            if (path_utf8(local_view_[i].path) == row.path) { selected_ = static_cast<int>(i); break; }
        scroll_ = std::max(0, selected_ - list_visible_rows_ / 2);
    }
    folder_overlay_close();
    queue_focus_ = false;
    if (local_view_.empty()) status_line_ = "no files directly in this folder -- open one of its subfolders";
    log_event("filtered: " + fo_name_of(dir));
}

bool App::folder_overlay_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int total = static_cast<int>(fo_view_.size());
    const int rows = std::max(1, fo_rows_);
    auto move_to = [&](int idx) { fo_sel_ = total > 0 ? std::clamp(idx, 0, total - 1) : 0; };
    auto page = [&](int dir) {
        if (total <= 0) return;
        const int max_scroll = std::max(0, total - rows);
        const int rel = std::clamp(fo_sel_ - fo_scroll_, 0, rows - 1);
        const int ns = std::clamp(fo_scroll_ + dir * rows, 0, max_scroll);
        if (ns == fo_scroll_) fo_sel_ = dir > 0 ? total - 1 : 0;
        else { fo_scroll_ = ns; fo_sel_ = std::clamp(ns + rel, 0, total - 1); }
    };

    // Up / Down work in the list and in the search field alike.
    if (arrow && (key == 'A' || key == 'B')) {
        if (last_key_was_shifted()) page(key == 'A' ? -1 : 1);
        else move_to(fo_sel_ + (key == 'A' ? -1 : 1));
        return true;
    }

    const FoRow* cur = (fo_sel_ >= 0 && fo_sel_ < total) ? &fo_view_[static_cast<size_t>(fo_sel_)] : nullptr;
    if (key == kKeyCtrlP) {      // CTRL+p: play the hovered file; the overlay stays open
        if (!cur) return true;
        if (cur->is_dir) { status_line_ = "CTRL+p plays a file -- open the folder (ENTER) and pick one"; return true; }
        for (const auto& t : all_local_tracks_)
            if (path_utf8(t.path) == cur->path) {
                queue_focus_ = false;
                start_local_track(t);
                return true;
            }
        status_line_ = "file is not in the library";
        return true;
    }
    if (fo_search_) {   // typing the query
        if (key == 27 || key == '\r' || key == '\n' || key == 9) { fo_search_ = false; return true; }
        if (key == 127 || key == 8) { pop_utf8_char(fo_query_); folder_overlay_rebuild_view(); return true; }
        if (arrow) return true;
        if (is_text_key(key) && fo_query_.size() < 100) { fo_query_ += static_cast<char>(key); folder_overlay_rebuild_view(); }
        return true;
    }

    if (key == 27) { folder_overlay_close(); return true; }
    if (key == '/' || key == 9) { fo_search_ = true; return true; }

    if (key == '\r' || key == '\n') {
        if (!cur) return true;
        if (cur->is_dir) {
            if (fo_expanded_.count(cur->path)) fo_expanded_.erase(cur->path); else fo_expanded_.insert(cur->path);
            folder_overlay_rebuild_view();
        } else {
            status_line_ = "[" + hotkey_text("HKeyFilterForFolder", "f") + "] shows this file's folder in the list";
        }
        return true;
    }
    if (arrow && key == 'C') {   // Right: open
        if (cur && cur->is_dir && !cur->expanded) { fo_expanded_.insert(cur->path); folder_overlay_rebuild_view(); }
        return true;
    }
    if (arrow && key == 'D') {   // Left: close, or go up to the parent folder
        if (!cur) return true;
        if (cur->is_dir && cur->expanded && fo_expanded_.count(cur->path)) {
            fo_expanded_.erase(cur->path);
            folder_overlay_rebuild_view();
        } else {
            for (int i = fo_sel_ - 1; i >= 0; --i)
                if (fo_view_[static_cast<size_t>(i)].is_dir && fo_view_[static_cast<size_t>(i)].depth < cur->depth) { fo_sel_ = i; break; }
        }
        return true;
    }

    // Everything else goes through the hotkeys: `f` (and the overlay key itself) are ours, the playback keys fall
    // through to the normal dispatch, anything that would act on the hidden list is swallowed.
    int lookup = (key == '\r') ? '\n' : key;
    std::string action = resolve_hotkey_action(lookup);
    if (action.empty() && lookup >= 'a' && lookup <= 'z') action = resolve_hotkey_action(lookup - 32);
    if (action.empty() && lookup >= 'A' && lookup <= 'Z') action = resolve_hotkey_action(lookup + 32);
    if (action == "HKeyFolderOverlay") { folder_overlay_close(); return true; }
    if (action == "HKeyFilterForFolder") { folder_overlay_filter(); return true; }
    static const char* const kPass[] = {
        "HKeyTogglePlayPause", "HKeyPlayNextSong", "HKeyPlayPreviousSong", "HKeyShuffleNext", "HKeyCyclePlayMode",
        "HKeySeekForward", "HKeySeekBackward", "HKeyIncreaseVolume", "HKeyDecreaseVolume", "HKeyToggleMute",
        "HKeyToggleNormalize", "HKeyQuit"};
    for (const char* a : kPass) if (action == a) return false;
    return true;
}

void App::folder_overlay_geometry(int W, int& panel_w, int& rows) const {
    panel_w = std::min(W, std::max(40, std::min(W - 6, 160)));
    const int legend = static_cast<int>(folder_overlay_legend(panel_w).size());
    rows = std::max(1, term_rows_ - 2 - kListOverlayChromeRows - legend);
}

std::vector<std::string> App::folder_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kListOverlayChromeRows - 5, 1, 3);
    std::vector<std::string> items = {
        "[ENTER/←→] open/close folder",
        "[" + hotkey_text("HKeyFilterForFolder", "f") + "] folder to list",
        "[CTRL+p] play",
        "[/] search",
        "[SHIFT+↑↓] page",
        "[ESC] close",
    };
    return pack_legend(items, std::max(0, panel_w), max_lines);
}

std::vector<std::string> App::build_folder_overlay_panel(int panel_w, int list_rows) const {
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // --- search bar (same look as the main one) ---
    {
        const int search_w = panel_w - 5;
        out.push_back(box_top("SEARCH LOCAL FOLDERS", search_w, border_ansi) + border_ansi + "╭───╮\x1b[0m");
        const int inner = std::max(0, search_w - 4);
        const std::string pre = "/f:";
        std::string q = fo_query_;
        const int room = std::max(0, inner - display_width(pre) - (fo_search_ ? 1 : 0));
        if (display_width(q) > room) q = truncate_str(q, room);
        const int fill = std::max(0, inner - display_width(pre) - display_width(q) - (fo_search_ ? 1 : 0));
        std::string line = bar + " " + search_idle_sgr(settings_) + pre + search_run(settings_, q, !q.empty())
                         + (fo_search_ ? search_idle_sgr(settings_) + "█" : std::string())
                         + std::string(static_cast<size_t>(fill), ' ') + "\x1b[0m " + bar;
        out.push_back(line + border_ansi + settings_.box_vertical + " " + std::string(1, play_mode_letter()) + " " + settings_.box_vertical + "\x1b[0m");
        out.push_back(box_bottom(search_w, "", border_ansi_bottom) + border_ansi_bottom + "╰───╯\x1b[0m");
    }

    // --- the tree ---
    const int inner = panel_w - 4;
    const int total = static_cast<int>(fo_view_.size());
    out.push_back(box_top("LOCAL AUDIO FILES FOLDER", panel_w, border_ansi));
    for (int row = 0; row < list_rows; ++row) {
        const int idx = fo_scroll_ + row;
        std::string content;
        bool playing = false;
        if (idx < total) {
            const FoRow& r = fo_view_[static_cast<size_t>(idx)];
            const std::string indent(static_cast<size_t>(r.depth) * 2, ' ');
            const std::string mark = r.is_dir ? (r.expanded ? "▾ " : "▸ ") : "  ";
            const std::string head = indent + mark;
            if (r.is_dir) {
                const std::string cnt = std::to_string(r.count) + (r.count == 1 ? " track" : " tracks");
                const int name_w = std::max(5, inner - display_width(head) - 2 - static_cast<int>(cnt.size()));
                content = head + pad_right(truncate_str(apply_font_map(r.label, settings_.font_map), name_w), name_w) + "  " + cnt;
            } else {
                content = head + apply_font_map(r.label, settings_.font_map);
                playing = has_track_ && path_utf8(current_path_) == r.path;
            }
        }
        const std::string padded = pad_right(truncate_str(content, inner), inner);
        if (idx == fo_sel_ && idx < total)
            out.push_back(bar + " " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + padded + "\x1b[0m " + bar);
        else if (playing)
            out.push_back(bar + " " + ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color) + padded + "\x1b[0m " + bar);
        else
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color) + padded + "\x1b[0m " + bar);
    }
    {
        const int rows = std::max(1, list_rows);
        const int pages = std::max(1, (total + rows - 1) / rows);
        const int page = std::clamp((fo_scroll_ + rows / 2) / rows + 1, 1, pages);
        const int remaining = total - (fo_scroll_ + rows);
        std::vector<std::string> parts;
        if (remaining > 0) parts.push_back("( " + std::to_string(remaining) + " more )");
        parts.push_back("page " + std::to_string(page) + "/" + std::to_string(pages));
        std::string footer;
        for (;;) {
            footer.clear();
            for (size_t i = 0; i < parts.size(); ++i) footer += (i ? "  " : "") + parts[i];
            if (parts.empty() || display_width(footer) + 5 <= panel_w) break;
            parts.pop_back();
        }
        if (parts.empty()) footer.clear();
        out.push_back(box_bottom(panel_w, footer, border_ansi_bottom));
    }
    for (const auto& l : folder_overlay_legend(panel_w)) out.push_back(legend_row(l, panel_w, legend_sgr(settings_)));
    return out;
}

// ---------------------------------------------------------------------
// File conversion overlay (HKeyConvertOverlay, SHIFT+c) -- see app.h for the design.
// ---------------------------------------------------------------------

namespace {
const char* const kCvTypeNames[] = {"ALL FILES", "LOSSLESS", "FLAC", "WAV", "M4A", "OGG", "OPUS", "AAC", "MP3"};
constexpr int kCvTypeCount = 9;
const int kCvMinKbps[] = {0, 320, 500, 1000};
constexpr int kCvMinCount = 4;

std::string cv_size_str(long long b) {
    char buf[32];
    if (b >= 1000000000LL) std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(b) / 1e9);
    else if (b >= 1000000LL) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / 1e6);
    else std::snprintf(buf, sizeof buf, "%lld KB", b / 1000);
    return buf;
}

// The extension alone decides the types; M4A (and so LOSSLESS) needs the probe, because an .m4a is lossless only as ALAC.
bool cv_type_ok(int type, const std::string& ext, const AudioInfo* inf) {
    switch (type) {
        case 0: return true;
        case 1: return ext == ".flac" || ext == ".wav" || (inf && inf->probed && inf->lossless);
        case 2: return ext == ".flac";
        case 3: return ext == ".wav";
        case 4: return ext == ".m4a";
        case 5: return ext == ".ogg";
        case 6: return ext == ".opus";
        case 7: return ext == ".aac";
        case 8: return ext == ".mp3";
        default: return true;
    }
}
// Files the probe thread has to look at for a type filter (the ones the type could still let through).
bool cv_probe_candidate(int type, const std::string& ext) {
    return cv_type_ok(type, ext, nullptr) || (type == 1 && ext == ".m4a");
}

std::string cv_khz(int sr) {
    char buf[24];
    if (sr % 1000 == 0) std::snprintf(buf, sizeof buf, "%dk", sr / 1000);
    else std::snprintf(buf, sizeof buf, "%.1fk", sr / 1000.0);
    return buf;
}
} // namespace

bool App::convert_overlay_active() const {
    return convert_overlay_open_ && mode_ == Mode::Browse;
}

std::string App::conversion_export_dir() const {
    if (!settings_.conversion_export_path.empty()) return settings_.conversion_export_path;
    return path_utf8(cache_.download_dir() / "conversion-export");
}

AudioInfo App::cv_info_of(const std::string& path) const {
    std::lock_guard<std::mutex> lk(cv_mu_);
    auto it = cv_info_.find(path);
    return it == cv_info_.end() ? AudioInfo{} : it->second;
}

// Every library file except the ones already in the export folder (converting a conversion makes no sense).
void App::convert_overlay_load_files() {
    cv_files_.clear();
    const fs::path out = path_from_utf8(conversion_export_dir()).lexically_normal();
    for (const auto& t : all_local_tracks_) {
        if (fo_is_under(t.path.parent_path().lexically_normal(), out)) continue;
        CvFile f;
        f.path = path_utf8(t.path);
        f.name = truncate_str(path_utf8(t.path.parent_path().filename()), 24) + "/" + path_utf8(t.path.filename());   // "Album/song.flac"
        f.ext = ascii_lower_str(path_utf8(t.path.extension()));
        std::error_code ec;
        const auto sz = fs::file_size(t.path, ec);
        f.size = ec ? 0 : static_cast<long long>(sz);
        cv_files_.push_back(std::move(f));
    }
    std::sort(cv_files_.begin(), cv_files_.end(), [](const CvFile& a, const CvFile& b) {
        return ascii_lower_str(a.path) < ascii_lower_str(b.path);
    });
    // marks of files that are gone
    std::set<std::string> have;
    for (const auto& f : cv_files_) have.insert(f.path);
    for (auto it = cv_marked_.begin(); it != cv_marked_.end();) it = have.count(*it) ? std::next(it) : cv_marked_.erase(it);
}

// (Re)starts the probe thread for the files the current type filter can still let through and that have no result yet.
void App::convert_overlay_probe_restart() {
    cv_probe_stop_ = true;
    if (cv_probe_th_.joinable()) cv_probe_th_.join();
    cv_probe_stop_ = false;
    std::vector<std::string> todo;
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        for (const auto& f : cv_files_)
            if (cv_probe_candidate(cv_type_, f.ext) && !cv_info_.count(f.path)) todo.push_back(f.path);
    }
    cv_probe_total_ = static_cast<int>(todo.size());
    cv_probe_done_ = 0;
    if (todo.empty()) return;
    cv_probe_th_ = std::thread([this, todo = std::move(todo)]() {
        for (const auto& p : todo) {
            if (cv_probe_stop_.load()) break;
            AudioInfo a = probe_audio_info(path_from_utf8(p));
            {
                std::lock_guard<std::mutex> lk(cv_mu_);
                cv_info_[p] = a;   // a failed probe is stored too (probed=false), so it is not asked again
            }
            ++cv_probe_done_;
        }
    });
}

void App::convert_overlay_rebuild_view() {
    std::string keep;
    if (cv_sel_ >= 0 && cv_sel_ < static_cast<int>(cv_view_.size())) keep = cv_files_[static_cast<size_t>(cv_view_[static_cast<size_t>(cv_sel_)])].path;
    cv_view_.clear();

    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : ascii_lower_str(cv_query_)) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }
    const bool need_probe = cv_hires_ || cv_minkbps_ > 0;
    std::map<std::string, AudioInfo> info;
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        info = cv_info_;
    }
    for (size_t i = 0; i < cv_files_.size(); ++i) {
        const CvFile& f = cv_files_[i];
        auto it = info.find(f.path);
        const AudioInfo* inf = it == info.end() ? nullptr : &it->second;
        if (cv_scope_ && !cv_scope_paths_.count(f.path)) continue;
        if (!cv_type_ok(cv_type_, f.ext, inf)) continue;
        if (need_probe) {
            if (!inf || !inf->probed) continue;
            if (cv_hires_ && !inf->hires) continue;
            if (inf->bitrate_kbps < kCvMinKbps[cv_minkbps_]) continue;
        }
        if (!words.empty()) {
            const std::string hay = ascii_lower_str(f.path);
            bool all = true;
            for (const auto& w : words) if (hay.find(w) == std::string::npos) { all = false; break; }
            if (!all) continue;
        }
        cv_view_.push_back(static_cast<int>(i));
    }
    cv_sel_ = 0;
    if (!keep.empty())
        for (size_t i = 0; i < cv_view_.size(); ++i)
            if (cv_files_[static_cast<size_t>(cv_view_[i])].path == keep) { cv_sel_ = static_cast<int>(i); break; }
    const int n = convert_overlay_total();
    cv_scroll_ = std::clamp(cv_scroll_, 0, std::max(0, n - std::max(1, cv_rows_)));
}

// The folder / playlist list of the "/f:" / "/p:" modes; rebuilt when the query or the mode changes.
void App::convert_overlay_rebuild_picks() {
    if (cv_pick_ == 1) {          // "/f:" -- folders that hold files
        cv_folders_.clear();
        const fs::path out = path_from_utf8(conversion_export_dir()).lexically_normal();
        for (auto& f : filter_folders(cv_query_))
            if (!fo_is_under(path_from_utf8(f.path).lexically_normal(), out)) cv_folders_.push_back(std::move(f));
        cv_sel_ = 0;
    } else if (cv_pick_ == 2) {   // "/p:" -- saved playlists
        cv_playlists_ = filter_playlists(cv_query_);
        cv_sel_ = 0;
    }
    cv_sel_ = 0;
    cv_scroll_ = 0;
}

int App::convert_overlay_total() const {
    return cv_pick_ == 1 ? static_cast<int>(cv_folders_.size())
         : cv_pick_ == 2 ? static_cast<int>(cv_playlists_.size())
                         : static_cast<int>(cv_view_.size());
}

// ENTER on a folder / playlist row of the "/f:" / "/p:" list: from now on the file list only holds the files of that
// folder (the files directly in it, like `f` in the main list) or playlist. ESC shows everything again.
void App::convert_overlay_pick() {
    std::set<std::string> paths;
    std::string label;
    if (cv_pick_ == 1) {
        if (cv_sel_ < 0 || cv_sel_ >= static_cast<int>(cv_folders_.size())) return;
        const FolderSummary& f = cv_folders_[static_cast<size_t>(cv_sel_)];
        for (const auto& file : cv_files_)
            if (path_utf8(path_from_utf8(file.path).parent_path()) == f.path) paths.insert(file.path);
        label = "folder " + f.name;
    } else if (cv_pick_ == 2) {
        if (cv_sel_ < 0 || cv_sel_ >= static_cast<int>(cv_playlists_.size())) return;
        const std::string name = cv_playlists_[static_cast<size_t>(cv_sel_)].name;
        if (auto pl = load_playlist(name))
            for (const auto& t : pl->tracks) if (!t.missing) paths.insert(path_utf8(t.path));
        label = "playlist " + name;
    } else {
        return;
    }
    cv_scope_ = true;
    cv_scope_paths_ = std::move(paths);
    cv_scope_label_ = label;
    cv_pick_ = 0;
    cv_query_.clear();
    cv_search_ = false;
    cv_sel_ = cv_scroll_ = 0;
    convert_overlay_rebuild_view();
}

void App::convert_overlay_open() {
    if (folder_overlay_open_) folder_overlay_close();
    if (list_overlay_open_) list_overlay_close();   // the overlays are mutually exclusive
    if (queue_overlay_open_) queue_overlay_close();
    convert_overlay_open_ = true;
    cv_search_ = false;
    cv_query_.clear();
    cv_pick_ = 0;
    cv_scope_ = false;
    cv_scope_paths_.clear();
    cv_sel_ = cv_scroll_ = 0;
    convert_overlay_load_files();
    convert_overlay_probe_restart();
    cv_probe_seen_ = -1;
    convert_overlay_rebuild_view();
}

void App::convert_overlay_close() {
    convert_overlay_open_ = false;
    cv_search_ = false;
    cv_probe_stop_ = true;
    if (cv_probe_th_.joinable()) cv_probe_th_.join();
    cv_probe_stop_ = false;
}

// Once per frame: probe results re-filter the list; a running conversion reports on the status line (only when the
// numbers change, so other messages are not drowned).
void App::convert_overlay_poll() {
    if (convert_overlay_open_) {
        const int d = cv_probe_done_.load();
        if (d != cv_probe_seen_) {
            cv_probe_seen_ = d;
            convert_overlay_rebuild_view();
        }
    }
    const ConvertProgress p = converter_.snapshot();
    if (p.running) {
        const int n = p.done + p.skipped + p.failed;
        if (n != cv_last_done_) {
            cv_last_done_ = n;
            status_line_ = "converting " + std::to_string(std::min(n + 1, p.total)) + "/" + std::to_string(p.total) +
                           ": " + p.current + "  (" + hotkey_text("HKeyConvertOverlay", "C") + ": details)";
        }
    } else if (p.finished && cv_last_done_ != -2) {
        cv_last_done_ = -2;
        status_line_ = std::string(p.cancelled ? "conversion cancelled: " : "conversion done: ") + std::to_string(p.done) + " converted, " +
                       std::to_string(p.skipped) + " skipped, " + std::to_string(p.failed) + " failed -> " + p.out_dir;
        ConsoleLog::instance().log_basic(status_line_);
        for (const auto& e : p.errors) ConsoleLog::instance().log_basic("conversion: " + e);
    }
}

// ENTER: converts the marked files, or the hovered one when nothing is marked.
void App::convert_overlay_start() {
    if (converter_.running()) { status_line_ = "a conversion is running -- [c] cancels it"; return; }
    std::vector<const CvFile*> picked;
    if (!cv_marked_.empty()) {
        for (const auto& f : cv_files_) if (cv_marked_.count(f.path)) picked.push_back(&f);
    } else if (cv_sel_ >= 0 && cv_sel_ < static_cast<int>(cv_view_.size())) {
        picked.push_back(&cv_files_[static_cast<size_t>(cv_view_[static_cast<size_t>(cv_sel_)])]);
    }
    if (picked.empty()) { status_line_ = "nothing to convert -- mark files with SPACE"; return; }

    const fs::path out = path_from_utf8(conversion_export_dir());
    std::vector<fs::path> roots;
    for (const auto& r : settings_.local_music_paths) if (!r.empty()) roots.push_back(fo_normal_root(r));
    const std::string ext = convert_preset(cv_preset_).ext;

    std::vector<ConvertItem> items;
    for (const CvFile* f : picked) {
        ConvertItem it;
        it.src = path_from_utf8(f->path);
        // keep the folder structure below the library folder the file lives in
        const fs::path dir = it.src.parent_path().lexically_normal();
        const fs::path* best = nullptr;
        size_t best_len = 0;
        for (const auto& r : roots) {
            if (!fo_is_under(dir, r)) continue;
            size_t n = 0;
            for (auto i = r.begin(); i != r.end(); ++i) ++n;
            if (!best || n > best_len) { best = &r; best_len = n; }
        }
        fs::path rel = best ? dir.lexically_relative(*best) : fs::path(dir.filename());
        if (rel == ".") rel.clear();
        it.dst = out / rel / path_from_utf8(path_utf8(it.src.stem()) + ext);
        const AudioInfo a = cv_info_of(f->path);
        if (a.probed) { it.duration = a.duration; it.sample_rate = a.sample_rate; it.channels = a.channels; }
        items.push_back(std::move(it));
    }
    const int n = static_cast<int>(items.size());
    cv_last_done_ = -1;
    if (converter_.start(std::move(items), cv_preset_, path_utf8(out))) {
        cv_marked_.clear();
        status_line_ = "converting " + std::to_string(n) + (n == 1 ? " file" : " files") + " to " + convert_preset(cv_preset_).label;
        log_event(status_line_ + " -> " + path_utf8(out));
    }
}

bool App::convert_overlay_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int total = convert_overlay_total();
    const int rows = std::max(1, cv_rows_);
    auto move_to = [&](int idx) { cv_sel_ = total > 0 ? std::clamp(idx, 0, total - 1) : 0; };
    auto page = [&](int dir) {
        if (total <= 0) return;
        const int max_scroll = std::max(0, total - rows);
        const int rel = std::clamp(cv_sel_ - cv_scroll_, 0, rows - 1);
        const int ns = std::clamp(cv_scroll_ + dir * rows, 0, max_scroll);
        if (ns == cv_scroll_) cv_sel_ = dir > 0 ? total - 1 : 0;
        else { cv_scroll_ = ns; cv_sel_ = std::clamp(ns + rel, 0, total - 1); }
    };
    const bool files = cv_pick_ == 0;
    const CvFile* cur = (files && cv_sel_ >= 0 && cv_sel_ < total) ? &cv_files_[static_cast<size_t>(cv_view_[static_cast<size_t>(cv_sel_)])] : nullptr;
    // Back to the plain file search: no folder / playlist list, no query, no scope.
    auto full_search = [&]() {
        cv_pick_ = 0;
        cv_scope_ = false;
        cv_scope_paths_.clear();
        cv_query_.clear();
        cv_sel_ = cv_scroll_ = 0;
        convert_overlay_rebuild_view();
    };

    if (arrow && (key == 'A' || key == 'B')) {
        if (last_key_was_shifted()) page(key == 'A' ? -1 : 1);
        else move_to(cv_sel_ + (key == 'A' ? -1 : 1));
        return true;
    }
    if (key == kKeyCtrlP) {      // CTRL+p: play the hovered file; the overlay stays open
        if (!cur) return true;
        for (const auto& t : all_local_tracks_)
            if (path_utf8(t.path) == cur->path) { queue_focus_ = false; start_local_track(t); return true; }
        return true;
    }
    if (cv_search_) {   // typing the query
        if (key == 27 || key == 9) { cv_search_ = false; return true; }
        if (key == '\r' || key == '\n') {
            if (cv_pick_ != 0) convert_overlay_pick(); else cv_search_ = false;
            return true;
        }
        if (arrow) return true;
        if (key == '/') { full_search(); return true; }   // "/" again: back to the full file search
        if (key == 127 || key == 8) {
            if (cv_query_.empty() && cv_pick_ != 0) { cv_pick_ = 0; convert_overlay_rebuild_view(); }   // erases the "f:" / "p:"
            else {
                pop_utf8_char(cv_query_);
                if (cv_pick_ == 0) convert_overlay_rebuild_view(); else convert_overlay_rebuild_picks();
            }
            return true;
        }
        if (is_text_key(key) && cv_query_.size() < 100) {
            cv_query_ += static_cast<char>(key);
            if (cv_pick_ == 0 && cv_query_.size() == 2 && cv_query_[1] == ':' && (cv_query_[0] == 'f' || cv_query_[0] == 'F' || cv_query_[0] == 'p' || cv_query_[0] == 'P')) {
                cv_pick_ = (cv_query_[0] == 'f' || cv_query_[0] == 'F') ? 1 : 2;   // the bar now reads /f: or /p:
                cv_query_.clear();
                convert_overlay_rebuild_picks();
            } else if (cv_pick_ == 0) convert_overlay_rebuild_view();
            else convert_overlay_rebuild_picks();
        }
        return true;
    }
    if (key == 27) {   // ESC: leave the folder / playlist list, then the scope, then the overlay
        if (cv_pick_ != 0) { cv_pick_ = 0; cv_query_.clear(); cv_sel_ = cv_scroll_ = 0; convert_overlay_rebuild_view(); }
        else if (cv_scope_) { cv_scope_ = false; cv_scope_paths_.clear(); cv_sel_ = cv_scroll_ = 0; convert_overlay_rebuild_view(); }
        else convert_overlay_close();
        return true;
    }
    if (!arrow && (key == '/' || key == 9)) { cv_search_ = true; return true; }

    if (arrow && (key == 'C' || key == 'D')) {   // LEFT / RIGHT: the target
        const int n = convert_preset_count();
        cv_preset_ = (cv_preset_ + (key == 'C' ? 1 : n - 1)) % n;
        return true;
    }
    if (!arrow) {
        if (key == ' ' && files) {   // mark / unmark, then on to the next row
            if (cur) {
                if (!cv_marked_.erase(cur->path)) cv_marked_.insert(cur->path);
                move_to(cv_sel_ + 1);
            }
            return true;
        }
        if (key == 'a' && files) {   // everything shown: mark all, or unmark all when all are marked already
            bool all = total > 0;
            for (int idx : cv_view_) if (!cv_marked_.count(cv_files_[static_cast<size_t>(idx)].path)) { all = false; break; }
            for (int idx : cv_view_) {
                const std::string& p = cv_files_[static_cast<size_t>(idx)].path;
                if (all) cv_marked_.erase(p); else cv_marked_.insert(p);
            }
            return true;
        }
        if (key == 't' || key == 'T') {
            cv_type_ = (cv_type_ + (key == 't' ? 1 : kCvTypeCount - 1)) % kCvTypeCount;
            convert_overlay_probe_restart();
            cv_probe_seen_ = -1;
            convert_overlay_rebuild_view();
            return true;
        }
        if (key == 'r' || key == 'R') {
            cv_minkbps_ = (cv_minkbps_ + (key == 'r' ? 1 : kCvMinCount - 1)) % kCvMinCount;
            convert_overlay_rebuild_view();
            return true;
        }
        if (key == 'h' || key == 'H') { cv_hires_ = !cv_hires_; convert_overlay_rebuild_view(); return true; }
        if (key == 'c') {
            if (converter_.running()) { converter_.cancel(); status_line_ = "cancelling the conversion ..."; }
            return true;
        }
    }
    if (key == '\r' || key == '\n') {
        if (cv_pick_ != 0) convert_overlay_pick(); else convert_overlay_start();
        return true;
    }

    // The rest goes through the hotkeys: the overlay key itself closes, the playback keys fall through to the normal
    // dispatch, anything that would act on the hidden list is swallowed.
    int lookup = (key == '\r') ? '\n' : key;
    std::string action = resolve_hotkey_action(lookup);
    if (action.empty() && lookup >= 'a' && lookup <= 'z') action = resolve_hotkey_action(lookup - 32);
    if (action.empty() && lookup >= 'A' && lookup <= 'Z') action = resolve_hotkey_action(lookup + 32);
    if (action == "HKeyConvertOverlay") { convert_overlay_close(); return true; }
    static const char* const kPass[] = {
        "HKeyTogglePlayPause", "HKeyPlayNextSong", "HKeyPlayPreviousSong", "HKeyShuffleNext", "HKeyCyclePlayMode",
        "HKeySeekForward", "HKeySeekBackward", "HKeyIncreaseVolume", "HKeyDecreaseVolume", "HKeyToggleMute",
        "HKeyToggleNormalize", "HKeyQuit"};
    for (const char* a : kPass) if (action == a) return false;
    return true;
}

void App::convert_overlay_geometry(int W, int& panel_w, int& rows) const {
    panel_w = std::min(W, std::max(40, std::min(W - 6, 160)));
    const int legend = static_cast<int>(convert_overlay_legend(panel_w).size());
    rows = std::max(1, term_rows_ - 2 - kListOverlayChromeRows - legend - 2);   // 2 rows of the box hold the filter and status lines
}

std::vector<std::string> App::convert_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kListOverlayChromeRows - 5, 1, 3);
    std::vector<std::string> items = {
        "[SPACE] mark",
        "[a] mark all",
        "[ENTER] convert",
        "[←→] target",
        "[t] type",
        "[r] min kbps",
        "[h] hi-res",
        "[/ , /p: , /f:] full or filtered path search",
        "[CTRL+p] play",
        "[c] cancel",
        "[SHIFT+↑↓] page",
        "[ESC] close",
    };
    return pack_legend(items, std::max(0, panel_w), max_lines);
}

std::vector<std::string> App::build_convert_overlay_panel(int panel_w, int list_rows) const {
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    std::vector<std::string> out;

    // --- search bar (same look as the main one) ---
    {
        const int search_w = panel_w - 5;
        out.push_back(box_top("SEARCH FILES TO CONVERT", search_w, border_ansi) + border_ansi + "╭───╮\x1b[0m");
        const int inner = std::max(0, search_w - 4);
        const std::string pre = cv_pick_ == 1 ? "/f:" : cv_pick_ == 2 ? "/p:" : "/";
        std::string q = cv_query_;
        const int room = std::max(0, inner - display_width(pre) - (cv_search_ ? 1 : 0));
        if (display_width(q) > room) q = truncate_str(q, room);
        const int fill = std::max(0, inner - display_width(pre) - display_width(q) - (cv_search_ ? 1 : 0));
        std::string line = bar + " " + search_idle_sgr(settings_) + pre + search_run(settings_, q, !q.empty())
                         + (cv_search_ ? search_idle_sgr(settings_) + "█" : std::string())
                         + std::string(static_cast<size_t>(fill), ' ') + R + " " + bar;
        out.push_back(line + border_ansi + settings_.box_vertical + " " + std::string(1, play_mode_letter()) + " " + settings_.box_vertical + R);
        out.push_back(box_bottom(search_w, "", border_ansi_bottom) + border_ansi_bottom + "╰───╯\x1b[0m");
    }

    const int inner = panel_w - 4;
    const int total = convert_overlay_total();
    const ConvertProgress prog = converter_.snapshot();
    {
        std::string title = cv_pick_ == 1 ? "FOLDERS (ENTER: show its files)" : cv_pick_ == 2 ? "PLAYLISTS (ENTER: show its files)" : "FILES TO CONVERT";
        if (cv_pick_ == 0 && cv_scope_) title += ": " + truncate_str(cv_scope_label_, 40);
        out.push_back(box_top(title, panel_w, border_ansi));
    }
    auto boxed = [&](const std::string& text, const std::string& sgr) {
        out.push_back(bar + " " + sgr + pad_right(truncate_str(text, inner), inner) + R + " " + bar);
    };

    // row 0: the pre-filter and the target
    boxed("TYPE [t] " + std::string(kCvTypeNames[cv_type_]) + "   MIN KBPS [r] " +
              (kCvMinKbps[cv_minkbps_] == 0 ? std::string("any") : std::to_string(kCvMinKbps[cv_minkbps_])) +
              "   HI-RES [h] " + (cv_hires_ ? "on" : "off") + "   TARGET [←→] " + convert_preset(cv_preset_).label,
          header_sgr(settings_));
    // row 1: progress / result
    {
        std::string text;
        if (prog.running) {
            const int n = prog.done + prog.skipped + prog.failed;
            const int w = 20;
            const int fill = static_cast<int>(prog.fraction * w + 0.5);
            text = "CONVERTING " + std::to_string(std::min(n + 1, prog.total)) + "/" + std::to_string(prog.total) + "  [" +
                   std::string(static_cast<size_t>(fill), '#') + std::string(static_cast<size_t>(w - fill), '-') + "] " +
                   std::to_string(static_cast<int>(prog.fraction * 100)) + "%  " + prog.current + "   [c] cancel";
        } else if (prog.finished) {
            text = std::string(prog.cancelled ? "CANCELLED: " : "DONE: ") + std::to_string(prog.done) + " converted, " +
                   std::to_string(prog.skipped) + " skipped (already there), " + std::to_string(prog.failed) + " failed  ->  " + prog.out_dir;
            if (!prog.errors.empty()) text += "   first error: " + prog.errors.front();
        } else if (cv_pick_ != 0) {
            text = std::string("Type to filter the ") + (cv_pick_ == 1 ? "folders" : "playlists") + ", ENTER shows the files of the one under the cursor, / returns to the full search";
        } else if (cv_scope_) {
            text = "Only the files of " + cv_scope_label_ + ": " + std::to_string(cv_scope_paths_.size()) + " in the library, " +
                   std::to_string(cv_view_.size()) + " match the pre-filter   [ESC] shows everything again";
        } else {
            text = "Export folder: " + conversion_export_dir() + "   (Settings -> PATHS -> CONVERSION EXPORT PATH)";
        }
        boxed(text, legend_sgr(settings_));
    }

    // the files
    for (int row = 0; row < list_rows; ++row) {
        const int idx = cv_scroll_ + row;
        std::string content;
        if (idx < total && cv_pick_ != 0) {
            std::string label, count;
            if (cv_pick_ == 1) {
                const FolderSummary& f = cv_folders_[static_cast<size_t>(idx)];
                label = (f.parent.empty() ? std::string() : f.parent + "/") + f.name;
                count = std::to_string(f.track_count) + (f.track_count == 1 ? " file" : " files");
            } else {
                const PlaylistSummary& pl = cv_playlists_[static_cast<size_t>(idx)];
                label = pl.name;
                count = std::to_string(pl.track_count) + (pl.track_count == 1 ? " track" : " tracks");
            }
            const int name_w = std::max(5, inner - 2 - static_cast<int>(count.size()));
            content = pad_right(truncate_str(apply_font_map(label, settings_.font_map), name_w), name_w) + "  " + count;
        } else if (idx < total) {
            const CvFile& f = cv_files_[static_cast<size_t>(cv_view_[static_cast<size_t>(idx)])];
            const AudioInfo a = cv_info_of(f.path);
            std::string fmt = f.ext.size() > 1 ? f.ext.substr(1) : f.ext;
            for (auto& c : fmt) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            std::string spec = "…", kbps = "…", est;
            if (a.probed) {
                spec = (a.bits > 0 ? std::to_string(a.bits) + "/" : std::string()) + cv_khz(a.sample_rate);
                kbps = std::to_string(a.bitrate_kbps) + " kbps";
                if (a.duration > 0) est = "-> ~" + cv_size_str(convert_estimate_bytes(cv_preset_, a.duration));
            }
            const std::string right = pad_right(fmt, 5) + " " + pad_right(spec, 9) + " " + pad_left(kbps, 9) + " " +
                                      pad_left(cv_size_str(f.size), 9) + "  " + pad_right(est, 12);
            const std::string head = std::string(cv_marked_.count(f.path) ? "[x] " : "[ ] ");
            const int name_w = std::max(5, inner - display_width(head) - display_width(right) - 1);
            content = head + pad_right(truncate_str(apply_font_map(f.name, settings_.font_map), name_w), name_w) + " " + right;
        }
        const std::string padded = pad_right(truncate_str(content, inner), inner);
        const bool playing = idx < total && cv_pick_ == 0 && has_track_ && path_utf8(current_path_) == cv_files_[static_cast<size_t>(cv_view_[static_cast<size_t>(idx)])].path;
        if (idx == cv_sel_ && idx < total)
            out.push_back(bar + " " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + padded + R + " " + bar);
        else if (playing)
            out.push_back(bar + " " + ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color) + padded + R + " " + bar);
        else
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color) + padded + R + " " + bar);
    }
    {
        const int rows = std::max(1, list_rows);
        const int pages = std::max(1, (total + rows - 1) / rows);
        const int page = std::clamp((cv_scroll_ + rows / 2) / rows + 1, 1, pages);
        const int remaining = total - (cv_scroll_ + rows);
        std::vector<std::string> parts;
        if (!cv_marked_.empty()) parts.push_back(std::to_string(cv_marked_.size()) + " marked");
        if (cv_probe_total_ > 0 && cv_probe_done_.load() < cv_probe_total_)
            parts.push_back("reading " + std::to_string(cv_probe_done_.load()) + "/" + std::to_string(cv_probe_total_));
        if (remaining > 0) parts.push_back("( " + std::to_string(remaining) + " more )");
        parts.push_back("page " + std::to_string(page) + "/" + std::to_string(pages));
        std::string footer;
        for (;;) {
            footer.clear();
            for (size_t i = 0; i < parts.size(); ++i) footer += (i ? "  " : "") + parts[i];
            if (parts.empty() || display_width(footer) + 5 <= panel_w) break;
            parts.pop_back();
        }
        if (parts.empty()) footer.clear();
        out.push_back(box_bottom(panel_w, footer, border_ansi_bottom));
    }
    for (const auto& l : convert_overlay_legend(panel_w)) out.push_back(legend_row(l, panel_w, legend_sgr(settings_)));
    return out;
}

// ---------------------------------------------------------------------
// Big queue overlay (HKeyQueueOverlay, SHIFT+k) -- see app.h for the design.
// ---------------------------------------------------------------------

// Shown in the same modes as the list overlay (the ones with the main UI
// underneath); in the full-screen takeovers the flag just stays set.
bool App::queue_overlay_active() const {
    if (!queue_overlay_open_) return false;
    switch (mode_) {
        case Mode::Browse: case Mode::Search: case Mode::BulkAdd:
        case Mode::RetryLyrics: case Mode::ClearQueue: case Mode::OsciMenu: case Mode::SpectroMenu: case Mode::NormMenu: case Mode::Equalizer: case Mode::SleepTimer: case Mode::LyricsEdit: return true;
        default: return false;
    }
}

void App::queue_overlay_geometry(int W, int& panel_w, int& queue_rows) const {
    panel_w = std::min(W, std::max(40, std::min(W - 6, 160)));
    // Rows 2 .. term_rows_-1 belong to the panel: queue box border (2) + the queue rows.
    // ...minus the key legend lines below the frame (see list_overlay_geometry()).
    const int legend = static_cast<int>(queue_overlay_legend(panel_w).size());
    queue_rows = std::max(1, term_rows_ - 2 - kQueueOverlayChromeRows - legend);
}

void App::queue_overlay_open() {
    if (convert_overlay_open_) convert_overlay_close();
    folder_overlay_open_ = false; // the overlays are mutually exclusive
    if (list_overlay_open_) list_overlay_close(); // mutually exclusive with the big list overlay
    // Remember the focus to give it back on close. Coming from the list
    // overlay the queue was unfocused (list_overlay_open() forces that), so
    // closing the queue overlay then lands on the list again.
    queue_overlay_prev_focus_ = queue_focus_;
    queue_overlay_open_ = true;
    queue_focus_ = true; // arrow keys, d, 4/5 and a all act on the queue while it is open
    clamp_queue_selected();
}

void App::queue_overlay_close() {
    queue_overlay_open_ = false;
    queue_focus_ = queue_overlay_prev_focus_;
    // Both panes share queue_selected_/queue_scroll_: pull the window back so
    // the cursor is inside the small pane's (much shorter) view again.
    clamp_queue_selected();
}

// One page up/down; same behaviour as list_overlay_page(): the cursor keeps
// its row within the window, and at either end it jumps to the last/first entry.
void App::queue_overlay_page(int dir) {
    const int total = static_cast<int>(queue_.size());
    if (total <= 0) return;
    const int rows = std::max(1, overlay_queue_rows_);
    const int max_scroll = std::max(0, total - rows);
    const int rel = std::clamp(queue_selected_ - queue_scroll_, 0, rows - 1);
    const int new_scroll = std::clamp(queue_scroll_ + dir * rows, 0, max_scroll);
    if (new_scroll == queue_scroll_) {
        queue_selected_ = (dir > 0) ? total - 1 : 0;
    } else {
        queue_scroll_ = new_scroll;
        queue_selected_ = std::clamp(new_scroll + rel, 0, total - 1);
    }
    clamp_queue_selected();
}

// The normal build_queue_panel() (same rows and colours) in the wider/taller
// box; only its bottom border is replaced, to carry the page counter, the
// number of tracks; the key legend follows below the frame (see build_list_overlay_panel()).
std::vector<std::string> App::build_queue_overlay_panel(int panel_w, int queue_rows) const {
    std::vector<std::string> out = build_queue_panel(panel_w, queue_rows);
    if (!out.empty()) {
        const int total = static_cast<int>(queue_.size());
        const int rows = std::max(1, queue_rows);
        const int pages = std::max(1, (total + rows - 1) / rows);
        const int page = std::clamp((queue_scroll_ + rows / 2) / rows + 1, 1, pages);
        const int remaining = total - (queue_scroll_ + rows);

        std::vector<std::string> parts; // dropped from the back until it fits
        if (remaining > 0) parts.push_back("( " + std::to_string(remaining) + " more )");
        parts.push_back("page " + std::to_string(page) + "/" + std::to_string(pages));
        parts.push_back(std::to_string(total) + (total == 1 ? " track" : " tracks"));
        std::string footer;
        for (;;) {
            footer.clear();
            for (size_t i = 0; i < parts.size(); ++i) footer += (i ? "  " : "") + parts[i];
            if (parts.empty() || display_width(footer) + 5 <= panel_w) break;
            parts.pop_back();
        }
        if (parts.empty()) footer.clear();
        out.back() = box_bottom(panel_w, footer, ansi_for(settings_.border_color_bottom, false));
        // Key legend: below the frame, outside it, in the legend color.
        for (const auto& l : queue_overlay_legend(panel_w)) out.push_back(legend_row(l, panel_w, legend_sgr(settings_)));
    }
    return out;
}

// ---------------------------------------------------------------------
// Floating panels (Bulk Add, Retry Lyrics) -- see app.h's comment on
// draw_floating_panel() for why these don't clear the screen.
// ---------------------------------------------------------------------

// BorderGradient=smooth for a panel that is stamped onto the screen (absolute cursor moves, so it never passes through
// clamp_output_rows()): its own border rows fade from the top to the bottom border colour, top row to bottom row.
std::vector<std::string> App::panel_gradient(const std::vector<std::string>& lines) const {
    if (!settings_.border_smooth || lines.size() < 2) return lines;
    static const std::vector<std::string> kGlyphs = {"\u2502", "\u2500", "\u256d", "\u256e", "\u2570", "\u256f", "\u251c", "\u2524", "\u252c", "\u2534", "\u253c"};
    std::vector<std::string> glyphs = kGlyphs;
    for (const std::string* g : {&settings_.box_upper_left, &settings_.box_upper_right, &settings_.box_lower_left,
                                 &settings_.box_lower_right, &settings_.box_vertical, &settings_.box_horizontal})
        if (!g->empty()) glyphs.push_back(*g);
    const std::string top = ansi_for(settings_.border_color, false), bottom = ansi_for(settings_.border_color_bottom, false);
    return border_gradient::apply(lines, top, bottom, glyphs, [&](float t) {
        return gradient_ansi(settings_.border_color, settings_.border_color_bottom, t, false);
    });
}

void App::draw_floating_panel(std::ostringstream& frame, const std::vector<std::string>& lines_in, int panel_w, int W, int fixed_col, bool exact_center) const {
    const std::vector<std::string> lines = panel_gradient(lines_in);
    int panel_h = static_cast<int>(lines.size());
    int start_col = fixed_col > 0 ? fixed_col : 1 + std::max(0, (W - panel_w) / 2);
    int start_row = 1 + std::max(0, (term_rows_ - panel_h) / 2 - (exact_center ? 0 : kFloatingPanelUpShift));
    start_row = std::clamp(start_row, 1, std::max(1, term_rows_ - panel_h));
    float_rect_[0] = start_col - 1; float_rect_[1] = start_row - 1; float_rect_[2] = panel_w; float_rect_[3] = panel_h;   // 0-based, for the scope image
    for (int i = 0; i < panel_h; ++i) {
        frame << "\x1b[" << (start_row + i) << ";" << start_col << "H" << lines[i];
    }
}

std::vector<std::string> App::build_bulk_add_panel() const {
    const int W = kBulkAddPanelWidth;   // 62, matches the reference design
    const int inner_w = W - 2;          // 60 -- nested box width, flush against the outer border (no gap)
    std::string border = ansi_for(settings_.border_color, false);
    std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    auto wrap = [&](const std::string& inner_line) { return obar + inner_line + obar; };

    std::vector<std::string> lines;
    lines.push_back(box_top("BULK ADD", W, border));

    // --- input box (nested) ---
    lines.push_back(wrap(box_top("", inner_w, border)));
    std::string cursor_line = bulk_add_buffer_.empty()
        ? "//:paste yt playlist link here"
        : bulk_add_buffer_ + "\u2588"; // block cursor once typing starts, matches the Search bar's style
    lines.push_back(wrap(box_line(cursor_line, inner_w, border)));
    lines.push_back(wrap(box_bottom(inner_w, "", border)));

    // --- results box (nested) -- always present at a fixed row count so
    // the panel's total footprint never changes between phase 1 (typing)
    // and phase 2 (results in) -- unused rows are just blank, not
    // omitted, which is what keeps draw_floating_panel()'s fixed-
    // rectangle overwrite artifact-free without ever needing a clear. ---
    lines.push_back(wrap(box_top("", inner_w, border)));

    const auto& items = pending_bulk_add_.items;
    int total = static_cast<int>(items.size());
    int show = bulk_add_results_ready_ ? std::min(total, kBulkAddVisibleRows) : 0;
    int scroll = bulk_add_results_ready_ ? std::clamp(bulk_add_scroll_, 0, std::max(0, total - show)) : 0;

    // Column widths measured directly from the reference design at its
    // 56-column content width (inner_w - 4): title=27, channel=15, the
    // rest is mark + " | " separators + the unpadded time text.
    const int title_w = 27, chan_w = 15;

    for (int r = 0; r < kBulkAddVisibleRows; ++r) {
        if (r >= show) { lines.push_back(wrap(box_line("", inner_w, border))); continue; }
        int idx = scroll + r;
        const auto& it = items[idx];
        bool starred = idx < static_cast<int>(bulk_add_selected_.size()) && bulk_add_selected_[idx];
        bool hovering = idx == bulk_add_cursor_;

        std::string mark = starred ? "*" : " ";
        std::string title = truncate_str(it.title, title_w);
        std::string chan = truncate_str(it.uploader.empty() ? "-" : it.uploader, chan_w);
        std::string time_str = "-";
        if (it.duration_sec >= 0) {
            int secs = static_cast<int>(it.duration_sec);
            time_str = std::to_string(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + std::to_string(secs % 60);
        }

        std::string line = mark + " | " + pad_right(title, title_w) + " | " + pad_right(chan, chan_w) + " | " + time_str;
        // Built plain first, then box_line() pads/truncates it (its
        // truncate_str/pad_right count raw bytes, not display columns --
        // they don't skip ANSI escapes), and only *after* that do we
        // splice the reverse-video hover highlight into the already-
        // finished bar+content+bar string. Wrapping `line` in "\x1b[7m"
        // before handing it to box_line() would get its own length
        // miscounted against the ANSI bytes and risk truncate_str
        // slicing straight through the trailing "\x1b[0m" reset,
        // leaking reverse-video onto every line after it -- exactly the
        // bug build_list_panel avoids by coloring after padding, not
        // before (see its own hover/cursor rendering).
        std::string boxed = box_line(line, inner_w, border);
        if (hovering) {
            std::string vbar_len = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
            size_t start = vbar_len.size() + 1; // past "bar + ' '"
            size_t end = boxed.size() - vbar_len.size() - 1; // before "' ' + bar"
            boxed = boxed.substr(0, start) + "\x1b[7m" + boxed.substr(start, end - start) + "\x1b[0m" + boxed.substr(end);
        }
        lines.push_back(wrap(boxed));
    }

    // Bottom border of the results box carries two plain-text labels --
    // "[ N more ]" on the left (only once there's more than fits), "ALL"
    // and "SELECT" on the right as the two commit actions. Just text, no
    // button/tab border art.
    int more = bulk_add_results_ready_ ? (total - show) : 0;
    std::string left_label = more > 0 ? ("[ " + std::to_string(more) + " more ]") : "";
    std::string right_label = bulk_add_results_ready_ ? "ALL   SELECT" : "";
    std::string prefix = settings_.box_lower_left + settings_.box_horizontal;
    if (!left_label.empty()) prefix += " " + left_label + " ";
    std::string suffix = right_label.empty() ? "" : (" " + right_label + " ");
    suffix += settings_.box_lower_right;
    int used = display_width(prefix) + display_width(suffix);
    int dashes = std::max(0, inner_w - used);
    std::string bottom = prefix;
    for (int i = 0; i < dashes; ++i) bottom += settings_.box_horizontal;
    bottom += suffix;
    bottom = pad_right(bottom, inner_w);
    lines.push_back(wrap(border.empty() ? bottom : (border + bottom + "\x1b[0m")));

    // Outer box's own bottom border carries "[ESC] cancel" directly.
    lines.push_back(box_bottom(W, "[ESC] cancel", border));
    return lines;
}

// ---------------------------------------------------------------------
// Retry Lyrics (HKeyRetryLyrics, 'l') -- manual title/artist override
// ---------------------------------------------------------------------

std::vector<App::RLField> App::rl_visible_fields() const {
    std::vector<RLField> f;
    f.insert(f.end(), {
        RLField::Title, RLField::Artist, RLField::Ft,
        RLField::TypeReverb, RLField::TypeSlowed, RLField::TypeUltraSlowed,
        RLField::TypeSpedup, RLField::TypeRemix, RLField::TypeOther,
    });
    if (rl_remix_) f.push_back(RLField::RemixText);
    if (rl_other_) f.push_back(RLField::OtherText);
    if (!rl_versions_.empty()) f.push_back(RLField::Source);   // the list sits at the bottom of the overlay
    return f;
}

bool* App::rl_bool_ptr(RLField f) {
    switch (f) {
        case RLField::TypeReverb: return &rl_reverb_;
        case RLField::TypeSlowed: return &rl_slowed_;
        case RLField::TypeUltraSlowed: return &rl_ultra_slowed_;
        case RLField::TypeSpedup: return &rl_spedup_;
        case RLField::TypeRemix: return &rl_remix_;
        case RLField::TypeOther: return &rl_other_;
        default: return nullptr;
    }
}

std::string* App::rl_text_ptr(RLField f) {
    switch (f) {
        case RLField::Title: return &rl_title_;
        case RLField::Artist: return &rl_artist_;
        case RLField::Ft: return &rl_ft_;
        case RLField::RemixText: return &rl_remix_text_;
        case RLField::OtherText: return &rl_other_text_;
        default: return nullptr;
    }
}

// Pre-fills from the current track's metadata_ rather than opening
// blank, and resets every other bit of state -- so reopening after a
// previous override (or a previous cancel) never leaks stale values
// from last time.
void App::rl_open_from_current_track() {
    rl_title_ = metadata_.name;
    rl_artist_ = (metadata_.artist == "-") ? "" : metadata_.artist;
    rl_ft_.clear();

    // Best-effort ft./feat. split: if the title itself carries a
    // "feat."/"ft." tag, pull it out into its own field rather than
    // leaving it embedded (so it doesn't get double-appended once the
    // TYPE tags get tacked on after the title at submit time).
    static const std::vector<std::string> markers = {" feat. ", " feat ", " ft. ", " ft "};
    std::string lower_title = ascii_lower_str(metadata_.name);
    for (const auto& marker : markers) {
        size_t pos = lower_title.find(marker);
        if (pos != std::string::npos) {
            rl_title_ = metadata_.name.substr(0, pos);
            rl_ft_ = metadata_.name.substr(pos + marker.size());
            // trim a trailing ')' if the split landed inside "(feat. X)"
            if (!rl_ft_.empty() && rl_ft_.back() == ')') rl_ft_.pop_back();
            while (!rl_title_.empty() && (rl_title_.back() == ' ' || rl_title_.back() == '(')) rl_title_.pop_back();
            break;
        }
    }

    rl_remix_text_.clear();
    rl_other_text_.clear();
    rl_reverb_ = rl_slowed_ = rl_ultra_slowed_ = rl_spedup_ = rl_remix_ = rl_other_ = false;
    rl_focus_ = RLField::Title;
    rl_versions_ = list_lyrics_versions(current_path_);
    rl_src_sel_ = 0;
    for (size_t i = 0; i < rl_versions_.size(); ++i) if (rl_versions_[i].active) { rl_src_sel_ = static_cast<int>(i); break; }
    rl_confirm_delete_ = false;
    rl_src_scroll_ = 0;
    rl_fix_scroll();
}

void App::rl_fix_scroll() {
    const int n = static_cast<int>(rl_versions_.size());
    rl_src_sel_ = std::clamp(rl_src_sel_, 0, std::max(0, n - 1));
    rl_src_scroll_ = std::clamp(rl_src_scroll_, 0, std::max(0, n - 3));
    if (rl_src_sel_ < rl_src_scroll_) rl_src_scroll_ = rl_src_sel_;
    if (rl_src_sel_ > rl_src_scroll_ + 2) rl_src_scroll_ = rl_src_sel_ - 2;
}

// [d] + y on the list: the highlighted saved copy is deleted. When it was the copy in use, the lyrics are reloaded
// without the network (the lyrics embedded in the file if there are any, otherwise none) -- a deliberate delete must
// not bring the same lyrics straight back.
void App::rl_delete_version() {
    if (rl_src_sel_ < 0 || rl_src_sel_ >= static_cast<int>(rl_versions_.size())) return;
    const LyricsVersion v = rl_versions_[static_cast<size_t>(rl_src_sel_)];
    if (!delete_lyrics_version(v)) { status_line_ = "could not delete those lyrics"; return; }
    rl_versions_ = list_lyrics_versions(current_path_);
    rl_fix_scroll();
    if (v.active) {
        bool embedded = false;
        for (const auto& x : rl_versions_) if (x.kind == LyricsVersion::Kind::Embedded) embedded = true;
        if (embedded) {
            std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
            launch_lyrics_fetch(metadata_.name, artist, current_path_);
        } else {
            ++lyrics_epoch_;
            std::lock_guard<std::mutex> lock(lyrics_mutex_);
            lyrics_result_ = LyricsResult();
            lyrics_result_.status = LyricsStatus::NotFound;
            lyrics_result_.message = "no lyrics";
            lyrics_path_ = current_path_;
            lyrics_ready_ = true;
        }
    }
    status_line_ = "lyrics deleted";
    log_event("lyrics: deleted " + v.label);
}

// SPACE on the version list: the highlighted version becomes the one that loads for this track. The copy that was in use
// is put aside as an older version, so every switch can be undone.
void App::rl_choose_version() {
    if (rl_src_sel_ < 0 || rl_src_sel_ >= static_cast<int>(rl_versions_.size())) return;
    const LyricsVersion v = rl_versions_[static_cast<size_t>(rl_src_sel_)];
    if (v.active) { status_line_ = "those lyrics are in use already"; return; }
    if (!activate_lyrics_version(current_path_, v)) { status_line_ = "could not switch the lyrics (is the lyrics folder writable?)"; return; }
    rl_versions_ = list_lyrics_versions(current_path_);
    rl_src_sel_ = 0;
    for (size_t i = 0; i < rl_versions_.size(); ++i) if (rl_versions_[i].active) { rl_src_sel_ = static_cast<int>(i); break; }
    rl_fix_scroll();
    std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
    launch_lyrics_fetch(metadata_.name, artist, current_path_);   // loads whatever is in use now (no network)
    log_event("lyrics: using " + v.label);
}

// Builds "title [ft. X] [tags...]" and launches the override fetch --
// this is the actual point of the whole form: feeding a corrected
// query into fetch_synced_lyrics() instead of the track's real
// metadata, for tracks whose auto-fetched lyrics are wrong/missing.
void App::rl_submit() {
    std::string query = rl_title_;
    if (!rl_ft_.empty()) query += " ft. " + rl_ft_;

    // Tags go after the title (per your note: "you usually need to place
    // them after title in url"). Slowed/Ultra Slowed/Spedup are mutually
    // exclusive so at most one of these three contributes; Reverb/Remix/
    // Other are independent and can stack with it and each other.
    if (rl_slowed_) query += " slowed";
    else if (rl_ultra_slowed_) query += " ultra slowed";
    else if (rl_spedup_) query += " sped up";
    if (rl_reverb_) query += " reverb";
    if (rl_remix_) query += rl_remix_text_.empty() ? " remix" : (" " + rl_remix_text_ + " remix");
    if (rl_other_ && !rl_other_text_.empty()) query += " " + rl_other_text_;

    launch_lyrics_fetch(query, rl_artist_, current_path_, /*force_network=*/true);
    log_event("retrying lyrics: \"" + query + "\"");

    mode_ = Mode::Browse;
}

// Equaliser overlay (SHIFT+e).
//
// Presets are addressed by one "unified" index: 0 .. kEqPresets.size()-1 are
// the built-in presets, everything after that is settings_.eq_custom_presets
// in the order saved.
int App::eq_preset_count() const {
    return static_cast<int>(kEqPresets.size() + settings_.eq_custom_presets.size());
}

const EqGains& App::eq_preset_gains(int index) const {
    const int builtin = static_cast<int>(kEqPresets.size());
    if (index < builtin) return kEqPresets[std::clamp(index, 0, builtin - 1)].gains;
    const int c = std::clamp(index - builtin, 0, static_cast<int>(settings_.eq_custom_presets.size()) - 1);
    return settings_.eq_custom_presets[c].gains;
}

std::string App::eq_preset_name(int index) const {
    const int builtin = static_cast<int>(kEqPresets.size());
    if (index < builtin) return kEqPresets[std::clamp(index, 0, builtin - 1)].name;
    const int c = index - builtin;
    if (c < static_cast<int>(settings_.eq_custom_presets.size())) return settings_.eq_custom_presets[c].name;
    return "Custom";
}

// The preset the sliders currently equal, or -1. The preset the cycle last
// stopped on wins a tie, so a custom preset saved with the same curve as a
// built-in one keeps showing under its own name.
int App::eq_current_preset() const {
    const int n = eq_preset_count();
    const EqGains& g = settings_.eq_gains;
    if (eq_last_preset_ >= 0 && eq_last_preset_ < n && eq_gains_equal(eq_preset_gains(eq_last_preset_), g))
        return eq_last_preset_;
    for (int i = 0; i < n; ++i) {
        if (eq_gains_equal(eq_preset_gains(i), g)) return i;
    }
    return -1;
}

void App::eq_open() {
    const int m = eq_current_preset();
    if (m >= 0) eq_last_preset_ = m;
    eq_naming_ = false;
    eq_name_buf_.clear();
    eq_status_.clear();
    eq_delete_armed_ = false;
    mode_ = Mode::Equalizer;
}

void App::eq_apply() {
    player_.set_equalizer(settings_.eq_enabled, settings_.eq_gains);
}

// Changing a band while the EQ is off switches it on: what you adjust is what
// you hear. Gains snap to whole dB (the grid the sliders are drawn on).
void App::eq_set_gain(int band, float db) {
    band = std::clamp(band, 0, kEqBands - 1);
    settings_.eq_gains[band] = std::clamp(std::round(db), kEqMinDb, kEqMaxDb);
    settings_.eq_enabled = true;
    eq_apply();
}

void App::eq_select_preset(int dir) {
    const int n = eq_preset_count();
    const int cur = eq_current_preset();
    const int base = cur >= 0 ? cur : std::clamp(eq_last_preset_, 0, n - 1);
    const int next = ((base + dir) % n + n) % n;
    settings_.eq_gains = eq_preset_gains(next);
    settings_.eq_enabled = true;
    eq_last_preset_ = next;
    eq_apply();
}

// S: opens the "Save as:" prompt. When the sliders were moved away from a
// custom preset the prompt starts with that preset's name, so ENTER alone
// updates it; otherwise it starts empty.
void App::eq_begin_naming() {
    eq_name_buf_.clear();
    const int builtin = static_cast<int>(kEqPresets.size());
    if (eq_current_preset() < 0 && eq_last_preset_ >= builtin && eq_last_preset_ < eq_preset_count())
        eq_name_buf_ = eq_preset_name(eq_last_preset_);
    eq_naming_ = true;
    edit_owner_ = "eq-name"; // claim the shared caret -- see edit_focus()
    edit_caret_ = edit_anchor_ = eq_name_buf_.size();
}

// ENTER in the prompt: stores the current curve under the typed name. A name
// that a custom preset already has (any capitalisation) replaces that preset;
// built-in names and "Custom" are refused. The list is written to config.txt
// at once, so a preset survives even if the program is closed abruptly.
void App::eq_commit_name() {
    std::string name;
    for (char c : eq_name_buf_) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || u == 127) continue;                   // control characters
        name.push_back((c == '=' || c == '{' || c == '}') ? '-' : c); // would confuse the config parser
    }
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();

    if (name.empty()) {
        eq_status_ = "Nothing saved: the name is empty";
        eq_naming_ = false;
        eq_name_buf_.clear();
        return;
    }
    if (eq_name_reserved(name)) {
        eq_status_ = "\"" + name + "\" is a built-in name, pick another";
        eq_naming_ = false;
        eq_name_buf_.clear();
        return;
    }

    const int builtin = static_cast<int>(kEqPresets.size());
    int existing = -1;
    for (size_t i = 0; i < settings_.eq_custom_presets.size(); ++i) {
        if (eq_name_equal(settings_.eq_custom_presets[i].name, name)) { existing = static_cast<int>(i); break; }
    }
    if (existing >= 0) {
        settings_.eq_custom_presets[existing].name = name;
        settings_.eq_custom_presets[existing].gains = settings_.eq_gains;
        eq_last_preset_ = builtin + existing;
        eq_status_ = "Updated \"" + name + "\"";
    } else if (settings_.eq_custom_presets.size() >= kEqMaxCustomPresets) {
        eq_status_ = "Limit of " + std::to_string(kEqMaxCustomPresets) + " custom presets reached";
        eq_naming_ = false;
        eq_name_buf_.clear();
        return;
    } else {
        EqCustomPreset cp;
        cp.name = name;
        cp.gains = settings_.eq_gains;
        settings_.eq_custom_presets.push_back(std::move(cp));
        eq_last_preset_ = builtin + static_cast<int>(settings_.eq_custom_presets.size()) - 1;
        eq_status_ = "Saved \"" + name + "\"";
    }
    eq_naming_ = false;
    eq_name_buf_.clear();
    save_settings(settings_);
}

// DEL / X: removes the custom preset the sliders currently equal. Built-in
// presets cannot be deleted. The first press only asks; the second one, with
// no other key in between, deletes. The sliders keep their gains (they simply
// read "Custom" afterwards) and the cycle continues from the neighbour.
void App::eq_delete_custom(bool confirmed) {
    const int builtin = static_cast<int>(kEqPresets.size());
    const int cur = eq_current_preset();
    if (cur < builtin) {
        eq_status_ = cur < 0 ? "Select a custom preset to delete it" : "Built-in presets cannot be deleted";
        return;
    }
    const std::string name = eq_preset_name(cur);
    if (!confirmed) {
        eq_delete_armed_ = true;
        eq_status_ = "Press DEL / X again to delete \"" + name + "\"";
        return;
    }
    settings_.eq_custom_presets.erase(settings_.eq_custom_presets.begin() + (cur - builtin));
    eq_last_preset_ = cur - 1;
    eq_status_ = "Deleted \"" + name + "\"";
    save_settings(settings_);
}

std::vector<std::string> App::build_eq_panel() const {
    const int W = kEqPanelWidth;
    const int inner = W - 4;               // 54: 4 axis columns + 10 bands x 5 columns
    constexpr int kCell = 5;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string head = ansi_for(settings_.header_color, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto framed = [&](const std::string& content) { return bar + " " + content + " " + bar; };
    auto plain_row = [&](const std::string& plain) {
        return framed(pad_right(truncate_str(plain, inner), inner));
    };
    auto centre = [&](const std::string& t) { // exactly kCell columns
        const int w = display_width(t);
        const int left = std::max(0, (kCell - w) / 2);
        return pad_right(std::string(left, ' ') + t, kCell);
    };

    // Every legend line of this overlay -- the two rows under the sliders and
    // the footer in the bottom border -- is drawn in the border colour, so the
    // whole key legend reads as one block. (Other panels colour their legends
    // differently; here this was chosen on purpose.)
    const std::string legend = border_bottom;
    // The legend rows are centred in the panel, and so is the footer in the
    // bottom border (the dashes are split evenly on both sides of it).
    auto legend_row = [&](const std::string& plain) {
        const std::string t = truncate_str(plain, inner);
        const int tw = display_width(t);
        const int left = std::max(0, (inner - tw) / 2);
        const std::string body = std::string(left, ' ') + t + std::string(std::max(0, inner - tw - left), ' ');
        return bar + " " + (legend.empty() ? body : legend + body + R) + " " + bar;
    };
    auto legend_bottom = [&](const std::string& footer) {
        const std::string t = " " + truncate_str(footer, W - 6) + " ";
        const int dashes = std::max(0, W - 2 - display_width(t));
        const int left = dashes / 2, right = dashes - left;
        std::string out = settings_.box_lower_left;
        for (int i = 0; i < left; ++i) out += settings_.box_horizontal;
        out += t;
        for (int i = 0; i < right; ++i) out += settings_.box_horizontal;
        out += settings_.box_lower_right;
        return legend.empty() ? out : legend + out + R;
    };

    const EqGains& g = settings_.eq_gains;
    const bool on = settings_.eq_enabled;
    const int preset = eq_current_preset();
    char pre[24];
    { const float pa = eq_auto_preamp_db(g); std::snprintf(pre, sizeof pre, pa > -0.05f ? "0.0 dB" : "%+.1f dB", pa); }

    std::vector<std::string> lines;
    lines.push_back(box_top("Equalizer", W, border));
    lines.push_back(plain_row(std::string("Preset: ") + (preset >= 0 ? eq_preset_name(preset) : std::string("Custom")) +
                              "   EQ: " + (on ? "ON" : "OFF") + "   Preamp: " + pre));

    // Plot: 13 rows of 2 dB each, or 7 rows of 4 dB when the terminal is short.
    const int step = term_rows_ >= 24 ? 2 : 4;
    const int half_rows = 12 / step;
    const std::string dim = border;                                   // EQ off: bars in the border grey
    const std::string full = "\u2588\u2588\u2588", low = "\u2584\u2584\u2584", up = "\u2580\u2580\u2580";
    for (int i = -half_rows; i <= half_rows; ++i) {
        const int L = -i * step; // +12 at the top, -12 at the bottom
        std::string axis;
        if (L == 0 || L == 12 || L == -12 || L == 6 || L == -6) {
            char a[8];
            std::snprintf(a, sizeof a, "%+d", L);
            axis = pad_left(a, 3) + " ";
        } else {
            axis = "    ";
        }
        std::string content = axis;
        for (int b = 0; b < kEqBands; ++b) {
            const float v = g[b];
            const bool sel = b == eq_band_;
            const std::string colour = sel ? head : (on ? std::string() : dim);
            std::string glyph;
            if (L == 0) {
                content += (sel ? head : border) + "\u2500\u2500\u2500\u2500\u2500" + R;
                continue;
            } else if (L > 0) {
                if (v >= L) glyph = full;
                else if (v > L - step) glyph = low;
            } else {
                if (v <= L) glyph = full;
                else if (v < L + step && v < 0) glyph = up;
            }
            if (glyph.empty()) content += "     ";
            else content += " " + colour + glyph + (colour.empty() ? "" : R) + " ";
        }
        lines.push_back(framed(content));
    }

    // Values and band names underneath; the selected band is reversed.
    std::string vals = "    ", names = "    ";
    for (int b = 0; b < kEqBands; ++b) {
        char num[8];
        std::snprintf(num, sizeof num, "%+d", static_cast<int>(std::lround(g[b])));
        const std::string v = centre(g[b] == 0.0f ? "0" : num);
        const std::string n = centre(kEqLabels[b]);
        vals += b == eq_band_ ? HI + v + R : v;
        names += b == eq_band_ ? HI + n + R : n;
    }
    lines.push_back(framed(vals));
    lines.push_back(framed(names));

    // One row for the "Save as:" prompt or the last action's feedback. It is
    // always present (blank when idle) so the panel keeps the same height.
    if (eq_naming_) {
        const std::string prefix = "Save as: ";
        const EditPaint p = paint_edit_field(eq_name_buf_, edit_caret_, edit_anchor_,
                                             std::max(1, inner - display_width(prefix) - 1), "", true);
        const int fill = std::max(0, inner - display_width(prefix) - p.cols);
        lines.push_back(framed(prefix + p.s + std::string(fill, ' ')));
    } else {
        lines.push_back(plain_row(eq_status_));
    }

    if (eq_naming_) {
        lines.push_back(legend_row("Type a name (max " + std::to_string(kEqNameMaxBytes) + " characters)"));
        lines.push_back(legend_row("An existing custom name is replaced"));
        lines.push_back(legend_bottom("[ENTER] save  [ESC] cancel"));
    } else {
        lines.push_back(legend_row("[</>] band  [UP/DOWN] gain  [,/.] preset  [0] zero"));
        lines.push_back(legend_row("[S] save preset  [DEL/X] delete preset"));
        lines.push_back(legend_bottom("[SPACE] on/off  [R] flat  [" + hotkey_text("HKeyEqualizer", "E") + " / ESC] close"));
    }
    return lines;
}

// Oscilloscope tuning overlay (SHIFT+o). The rows come from osci_visible_rows() (osci_settings.cpp): which ones are
// shown depends on the style (braille / image). The ranges match what settings.cpp clamps on load.
void App::osci_menu_adjust(int dir) {
    const std::vector<int> rows = osci_visible_rows(settings_);
    if (rows.empty()) return;
    osci_menu_row_ = std::clamp(osci_menu_row_, 0, static_cast<int>(rows.size()) - 1);
    const int id = rows[osci_menu_row_];
    osci_adjust(settings_, id, dir);
    // the style row changes which rows exist: keep the cursor on the same one
    const std::vector<int> now = osci_visible_rows(settings_);
    for (size_t i = 0; i < now.size(); ++i) if (now[i] == id) { osci_menu_row_ = static_cast<int>(i); return; }
    osci_menu_row_ = std::clamp(osci_menu_row_, 0, static_cast<int>(now.size()) - 1);
}

std::vector<std::string> App::build_osci_menu_panel() const {
    const int W = kOsciMenuPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    const std::vector<int> rows = osci_visible_rows(settings_);
    std::vector<std::string> lines;
    lines.push_back(box_top("Oscilloscope", W, border));
    for (size_t i = 0; i < rows.size(); ++i) {
        const bool sel = static_cast<int>(i) == osci_menu_row_;
        const std::string plain = std::string(sel ? "> " : "  ") + pad_right(osci_row_label(rows[i]), 22) +
                                  pad_left(osci_row_value(settings_, rows[i]), inner - 24);
        lines.push_back(row(plain, sel));
    }
    lines.push_back(row("", false));
    lines.push_back(row("[UP/DOWN] select  [LEFT/RIGHT] change", false));
    lines.push_back(box_bottom(W, "[R] reset  [" + hotkey_text("HKeyOscMenu", "O") + " / ESC] close", border_bottom));
    return lines;
}

// Loudness normalisation overlay (SHIFT+v). Rows: 0 on/off, 1 target level
// (LUFS), 2 max boost (dB). The ranges match what settings.cpp clamps on
// load and what Player::set_normalization() accepts.
namespace {
struct NormKnob { const char* label; double lo, hi, step; };
constexpr NormKnob kNormKnobs[3] = {
    {"Normalize",     0.0,   1.0,  1.0},   // on/off, handled separately
    {"Target level",  -40.0, 0.0,  1.0},   // LUFS
    {"Max boost",     0.0,   24.0, 1.0},   // dB
};

// "-16" instead of "-16.0": whole numbers drop the decimal, a value that was
// typed into config.txt with a fraction ("-14.5") keeps it.
std::string fmt_loudness(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    std::string s(buf);
    if (s.size() > 2 && s.back() == '0' && s[s.size() - 2] == '.') {
        s.pop_back(); // the zero
        s.pop_back(); // ...and the point that only existed to show it
    }
    return s;
}
} // namespace

// Pushes the normalisation settings into the audio side. The player glides to
// the new gain within about a second, so there is no click.
void App::norm_apply() {
    player_.set_normalization(settings_.normalize,
                              static_cast<float>(settings_.normalize_target_lufs),
                              static_cast<float>(settings_.normalize_max_boost_db));
}

void App::norm_menu_adjust(int dir) {
    if (norm_menu_row_ == 0) {          // Left = off, Right = on
        settings_.normalize = dir > 0;
        norm_apply();
        return;
    }
    double* v = norm_menu_row_ == 1 ? &settings_.normalize_target_lufs
                                    : &settings_.normalize_max_boost_db;
    const NormKnob& k = kNormKnobs[std::clamp(norm_menu_row_, 0, 2)];
    // Round to the step grid so repeated presses never accumulate drift (and a
    // fractional value from config.txt snaps onto whole numbers).
    const double next = std::round((*v + dir * k.step) / k.step) * k.step;
    *v = std::clamp(next, k.lo, k.hi);
    norm_apply();
}

std::vector<std::string> App::build_norm_menu_panel() const {
    const int W = kNormMenuPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m", DIM = "\x1b[90m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    auto dim_row = [&](const std::string& plain) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + DIM + body + R + " " + bar;
    };

    const std::string vals[3] = {
        settings_.normalize ? "on" : "off",
        fmt_loudness(settings_.normalize_target_lufs) + " LUFS",
        fmt_loudness(settings_.normalize_max_boost_db) + " dB",
    };

    // What the player is doing right now with the track that is playing: its
    // measured loudness and the gain that results (capped by Max boost).
    std::string live;
    const float lufs = player_.track_lufs();
    if (!has_track_) {
        live = "No track playing";
    } else if (std::isnan(lufs)) {
        live = "Track: measuring loudness...";
    } else if (!settings_.normalize) {
        char num[48];
        std::snprintf(num, sizeof num, "Track %.1f LUFS   normalization is off", static_cast<double>(lufs));
        live = num;
    } else {
        char num[48];
        std::snprintf(num, sizeof num, "Track %.1f LUFS   applied gain %+.1f dB",
                      static_cast<double>(lufs), static_cast<double>(player_.normalization_gain_db()));
        live = num;
    }

    std::vector<std::string> lines;
    lines.push_back(box_top("Loudness normalization", W, border));
    for (int i = 0; i < 3; ++i) {
        const std::string plain = std::string(i == norm_menu_row_ ? "> " : "  ") +
                                  pad_right(kNormKnobs[i].label, 18) + pad_left(vals[i], 12);
        lines.push_back(row(plain, i == norm_menu_row_));
    }
    lines.push_back(dim_row(live));
    lines.push_back(row("[UP/DOWN] select   [LEFT/RIGHT] change", false));
    lines.push_back(box_bottom(W, "[SPACE] on/off  [R] reset  [" + hotkey_text("HKeyNormMenu", "V") + " / ESC] close", border_bottom));
    return lines;
}

// ---------------------------------------------------------------------
// Lyrics timing overlay (Alt+L) -- see app.h for the design.
// ---------------------------------------------------------------------
void App::lyrics_edit_open() {
    std::lock_guard<std::mutex> lk(lyrics_mutex_);
    lyrics_edit_from_karaoke_ = false;   // the karaoke path sets it again right after this call
    if (!settings_.lyrics_on() || !has_track_ || !lyrics_ready_ || lyrics_result_.lines.empty()) {
        status_line_ = "lyrics timing: no synced lyrics are loaded for this track";
        return;
    }
    lyrics_edit_orig_delay_ = lyrics_result_.delay;
    mode_ = Mode::LyricsEdit;
}

// Back to wherever the overlay was opened from: the karaoke overlay if it came
// from there (Alt+L works inside it), otherwise the main screen.
void App::lyrics_edit_close() {
    if (lyrics_edit_from_karaoke_) {
        lyrics_edit_from_karaoke_ = false;
        mode_ = Mode::Karaoke;
    } else {
        mode_ = Mode::Browse;
    }
}

void App::lyrics_edit_adjust(double step) {
    std::lock_guard<std::mutex> lk(lyrics_mutex_);
    // Round to hundredths so repeated presses never accumulate float drift.
    const double next = std::round((lyrics_result_.delay + step) * 100.0) / 100.0;
    lyrics_result_.delay = std::clamp(next, -120.0, 120.0);
}

std::vector<std::string> App::build_lyrics_edit_panel() const {
    const int W = kLyricsEditPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m", DIM = "\x1b[90m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, const std::string& sgr) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (sgr.empty() ? body : sgr + body + R) + " " + bar;
    };

    std::vector<LyricLine> lines;
    double delay = 0.0;
    {
        std::lock_guard<std::mutex> lk(lyrics_mutex_);
        lines = lyrics_result_.lines;
        delay = lyrics_result_.delay;
    }
    const double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    const double lyric_t = elapsed - delay;
    int active = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].start_time <= lyric_t) active = static_cast<int>(i); else break;
    }
    auto text_at = [&](int i) { return (i >= 0 && i < static_cast<int>(lines.size())) ? lines[i].full_text : std::string(); };

    char num[32];
    std::snprintf(num, sizeof num, "%+.1f s", delay);
    const std::string state = delay > 0.0049 ? "lyrics appear later" : delay < -0.0049 ? "lyrics appear earlier" : "as in the lyrics file";

    std::vector<std::string> out;
    out.push_back(box_top("Lyrics Timing", W, border));
    out.push_back(row(std::string("Offset  ") + num + "   (" + state + ")", ""));
    out.push_back(row("", ""));
    out.push_back(row("  " + text_at(active - 1), DIM));
    out.push_back(row("> " + text_at(active), HI));
    out.push_back(row("  " + text_at(active + 1), DIM));
    out.push_back(row("[LEFT/RIGHT] -/+ 0.1 s   [DOWN/UP] -/+ 0.5 s", ""));
    out.push_back(row("[SHIFT+LEFT/RIGHT] seek the track -/+ 5 s", ""));
    out.push_back(box_bottom(W, "[R] reset  [ENTER] save  [ESC] cancel", border_bottom));
    return out;
}

// ---------------------------------------------------------------------
// Sleep timer (HKeySleepTimer = Shift+Z) -- see app.h for the design.
// ---------------------------------------------------------------------
namespace {
constexpr int kSleepMinutes[5] = {15, 30, 60, 90, 120};
std::string fmt_countdown(long long secs) {
    if (secs < 0) secs = 0;
    char buf[24];
    if (secs >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", secs / 3600, (secs / 60) % 60, secs % 60);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld", secs / 60, secs % 60);
    return buf;
}
} // namespace

void App::sleep_timer_cancel() {
    player_.set_fade(1.0f);
    sleep_timer_active_ = false;
    sleep_timer_minutes_ = 0;
    sleep_stop_after_track_ = false;
}

void App::sleep_timer_apply(int row) {
    // The two kinds of timer are mutually exclusive: whatever was armed is
    // replaced (picking the running minute entry again simply restarts it).
    sleep_timer_cancel();
    if (row >= 0 && row < 5) {
        sleep_timer_minutes_ = kSleepMinutes[row];
        sleep_timer_deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(sleep_timer_minutes_);
        sleep_timer_active_ = true;
        status_line_ = "sleep timer: playback pauses in " + std::to_string(sleep_timer_minutes_) + " min";
    } else if (row == 5) {
        if (!has_track_) {
            status_line_ = "sleep timer: nothing is playing, so there is no current song to stop after";
            return;
        }
        sleep_stop_after_track_ = true;
        status_line_ = settings_.play_mode == 3
            ? "sleep timer: stops after this song (Stop mode is on as well)"
            : "sleep timer: stops after this song";
    } else {
        status_line_ = "sleep timer: off";
    }
}

// Fade length of a minute timer: 10 % of the time, 30 s .. 10 min (the radio's rule).
static double sleep_fade_seconds(int minutes) { return std::clamp(minutes * 6.0, 30.0, 600.0); }

void App::sleep_timer_tick() {
    // A pending "stop after song" with nothing playing (and nothing about to
    // start) has no song left to wait for -- drop it rather than letting it
    // hit whatever track is started next.
    if (sleep_stop_after_track_ && !has_track_ && !advancing_ && !load_in_progress_.load())
        sleep_stop_after_track_ = false;
    if (!sleep_timer_active_) return;
    if (std::chrono::steady_clock::now() < sleep_timer_deadline_) {
        const double left = std::chrono::duration<double>(sleep_timer_deadline_ - std::chrono::steady_clock::now()).count();
        const double len = sleep_fade_seconds(sleep_timer_minutes_);
        if (settings_.sleep_fade && left < len) { const double t = left / len; player_.set_fade(static_cast<float>(t * t)); }
        else player_.set_fade(1.0f);
        return;
    }
    if (has_track_ && !advancing_ && device_play_pending_gen_.load() == 0) {
        // Pause, not stop: the position survives and one press of play resumes.
        if (!player_.is_paused()) player_.pause();
        player_.set_fade(1.0f);   // the next press of play comes back at the normal volume
        sleep_timer_active_ = false;
        sleep_timer_minutes_ = 0;
        status_line_ = "sleep timer: playback paused";
    } else if (!has_track_ && !advancing_ && !load_in_progress_.load()) {
        // Nothing is playing any more (Stop mode, end of the list, ...).
        sleep_timer_active_ = false;
        sleep_timer_minutes_ = 0;
        status_line_ = "sleep timer: finished";
    }
    // else: a track is loading / being handed over -- try again next frame, so
    // the pause lands on the new track instead of racing its start.
}

std::string App::sleep_timer_label() const {
    if (sleep_timer_active_) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(
            sleep_timer_deadline_ - std::chrono::steady_clock::now()).count();
        return "SLEEP " + fmt_countdown(left);
    }
    if (sleep_stop_after_track_) return "SLEEP after song";
    return "";
}

std::vector<std::string> App::build_sleep_timer_panel() const {
    const int W = kSleepTimerPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    // "label ........ right" within `inner` columns.
    auto two = [&](bool cursor, const std::string& left, const std::string& right) {
        const std::string l = std::string(cursor ? "> " : "  ") + left;
        const int gap = std::max(1, inner - display_width(l) - display_width(right));
        return l + std::string(static_cast<size_t>(gap), ' ') + right;
    };

    long long left_secs = 0;
    if (sleep_timer_active_)
        left_secs = std::chrono::duration_cast<std::chrono::seconds>(sleep_timer_deadline_ - std::chrono::steady_clock::now()).count();

    std::vector<std::string> lines;
    lines.push_back(box_top("Sleep Timer", W, border));
    for (int i = 0; i < 5; ++i) {
        const bool running = sleep_timer_active_ && sleep_timer_minutes_ == kSleepMinutes[i];
        lines.push_back(row(two(i == sleep_menu_row_, std::to_string(kSleepMinutes[i]) + " minutes",
                                running ? fmt_countdown(left_secs) + " left" : ""), i == sleep_menu_row_));
    }
    lines.push_back(row(two(sleep_menu_row_ == 5, "Stop after current song", sleep_stop_after_track_ ? "on" : ""), sleep_menu_row_ == 5));
    lines.push_back(row(two(sleep_menu_row_ == 6, "Fade out", settings_.sleep_fade ? "on" : "off"), sleep_menu_row_ == 6));
    lines.push_back(row(two(sleep_menu_row_ == 7, "Off", (!sleep_timer_active_ && !sleep_stop_after_track_) ? "(no timer)" : ""), sleep_menu_row_ == 7));
    lines.push_back(row(settings_.sleep_fade ? "Volume glides down over the last 10 % of" : "Playback is paused when the time is up.", false));
    lines.push_back(row(settings_.sleep_fade ? "the time (30 s - 10 min), then it pauses." : "Minute timers pause, they do not stop.", false));
    lines.push_back(box_bottom(W, "[UP/DOWN] [ENTER] set  [ESC] close", border_bottom));
    return lines;
}

// "Want to clear queue?" -- small Yes/No confirmation, stamped over the live
// Browse view by draw_floating_panel() like Bulk Add / Retry Lyrics. Fixed
// size (8 rows x kClearQueuePanelWidth) so it overwrites cleanly without a clear.
std::vector<std::string> App::build_clear_queue_panel() const {
    const int W = kClearQueuePanelWidth;
    const int inner = W - 4;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;

    // Centred plain-text row. Only ever fed plain text (display_width() counts
    // escape bytes as columns), so the highlighted buttons are built below.
    auto centred = [&](const std::string& plain) {
        const int fill = std::max(0, inner - display_width(plain));
        const int left = fill / 2;
        return bar + " " + std::string(static_cast<size_t>(left), ' ') + plain +
               std::string(static_cast<size_t>(fill - left), ' ') + " " + bar;
    };

    const size_t n = queue_.size();
    const std::string yes = "  Yes  ", no = "  No  ", gap = "    ";
    const int btn_w = display_width(yes) + display_width(gap) + display_width(no);
    const int fill = std::max(0, inner - btn_w);
    const int left = fill / 2;
    const std::string buttons =
        bar + " " + std::string(static_cast<size_t>(left), ' ') +
        (clear_queue_choice_ == 0 ? HI + yes + R : yes) + gap +
        (clear_queue_choice_ == 1 ? HI + no + R : no) +
        std::string(static_cast<size_t>(fill - left), ' ') + " " + bar;

    std::vector<std::string> lines;
    lines.push_back(box_top("Clear Queue", W, border));
    lines.push_back(centred(""));
    lines.push_back(centred("Want to clear queue?"));
    lines.push_back(centred(std::to_string(n) + " track" + (n == 1 ? "" : "s") + " will be removed"));
    lines.push_back(centred(""));
    lines.push_back(buttons);
    lines.push_back(centred(""));
    lines.push_back(box_bottom(W, "[y / n]  [ENTER] confirm", border_bottom));
    return lines;
}

std::vector<std::string> App::build_retry_lyrics_panel() const {
    const int W = kRetryLyricsPanelWidth; // 62, matches the reference design
    const int label_w = 14;               // left label column, blank on box top/bottom rows
    const int box_w = W - 2 - label_w;    // 46 -- nested input box width
    std::string border = ansi_for(settings_.border_color, false);
    std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    auto wrap = [&](const std::string& left, const std::string& right) { return obar + left + right + obar; };
    auto label = [&](const std::string& text) { return pad_left(text, label_w - 2) + " :"; };
    auto blank_label = [&] { return std::string(label_w, ' '); };

    // A text field: label appears only on the middle (content) row, the
    // nested box's own top/bottom rows get a blank label column.
    // `placeholder` shows only when the field is both empty AND not
    // currently focused -- e.g. REMIX/OTHER's "NOT SELECTED". While
    // actively editing an empty field, show just the cursor rather than
    // the placeholder text glued to it (which would otherwise look like
    // "NOT SELECTED" was real, already-typed content).
    auto text_field = [&](RLField f, const std::string& lbl, const std::string& value,
                           const std::string& placeholder, std::vector<std::string>& out) {
        out.push_back(wrap(blank_label(), box_top("", box_w, border)));
        bool focused = (rl_focus_ == f);
        std::string shown = (value.empty() && !focused) ? placeholder : value;
        if (focused) shown += "\u2588"; // block cursor, same convention as Search/Bulk Add
        out.push_back(wrap(label(lbl), box_line(shown, box_w, border)));
        out.push_back(wrap(blank_label(), box_bottom(box_w, "", border)));
    };

    std::vector<std::string> lines;
    lines.push_back(box_top("LYRICS REFETCH AND VERSION CONTROL", W, border));

    {
        const int inner = W - 2;
        lines.push_back(obar + border + pad_right(" RETRY WITH ANOTHER TITLE / ARTIST", inner) + "\x1b[0m" + obar);
    }

    text_field(RLField::Title, "SONG TITLE", rl_title_, "", lines);
    text_field(RLField::Artist, "ARTIST NAME", rl_artist_, "", lines);
    text_field(RLField::Ft, "FT  ( opt )", rl_ft_, "", lines);

    // TYPE: two rows of plain (non-boxed) checkbox-style toggles.
    // Slowed/Ultra Slowed/Spedup are a radio group (only one can show
    // "[o]" at a time); Reverb/Remix/Other are independent.
    //
    // Built and measured as plain ASCII first, then the focus highlight
    // is spliced in by byte offset *after* pad_right/truncate_str has
    // already run -- same reasoning as the bulk-add hover fix above:
    // those two don't skip ANSI escapes when counting width, so
    // wrapping an option in "\x1b[7m" before truncating/padding the row
    // risks slicing through the reset code and leaking reverse-video
    // onto the rest of the panel. Every word here is plain ASCII, so a
    // byte offset is also a column offset -- no UTF-8 width subtleties
    // to worry about.
    auto opt_plain = [&](bool checked, const std::string& word) { return (checked ? "[o] " : " o  ") + word; };
    auto type_row = [&](std::initializer_list<std::tuple<RLField, bool, std::string>> opts) {
        std::string plain = " ";
        int focus_start = -1, focus_len = 0;
        for (const auto& [f, checked, word] : opts) {
            std::string s = opt_plain(checked, word);
            if (rl_focus_ == f) { focus_start = static_cast<int>(plain.size()); focus_len = static_cast<int>(s.size()); }
            plain += s;
            plain += " ";
        }
        std::string padded = pad_right(truncate_str(plain, box_w), box_w);
        if (focus_start >= 0 && focus_start + focus_len <= static_cast<int>(padded.size())) {
            padded = padded.substr(0, focus_start) + "\x1b[7m" + padded.substr(focus_start, focus_len) +
                     "\x1b[0m" + padded.substr(focus_start + focus_len);
        }
        return padded;
    };
    lines.push_back(wrap(label("TYPE"),
        type_row({{RLField::TypeReverb, rl_reverb_, "reverb"},
                  {RLField::TypeSlowed, rl_slowed_, "slowed"},
                  {RLField::TypeUltraSlowed, rl_ultra_slowed_, "ultra slowed"}})));
    lines.push_back(wrap(blank_label(),
        type_row({{RLField::TypeSpedup, rl_spedup_, "spedup"},
                  {RLField::TypeRemix, rl_remix_, "remix"},
                  {RLField::TypeOther, rl_other_, "other /pls specify"}})));

    // REMIX / OTHER: only present while their TYPE toggle is on (the panel grows and shrinks with them).
    if (rl_remix_) text_field(RLField::RemixText, "REMIX", rl_remix_text_, "NOT SELECTED", lines);
    if (rl_other_) text_field(RLField::OtherText, "OTHER", rl_other_text_, "NOT SELECTED", lines);

    // One empty line, then the note (two columns of air on each side). While [d] waits for its y / n the same two rows
    // carry the warning instead.
    {
        const int inner = W - 2;
        auto note_row = [&](const std::string& sgr, const std::string& text) {
            return obar + std::string(2, ' ') + sgr + pad_right(truncate_str(text, inner - 4), inner - 4) + "\x1b[0m" + std::string(2, ' ') + obar;
        };
        lines.push_back(obar + std::string(inner, ' ') + obar);
        if (rl_confirm_delete_ && rl_src_sel_ >= 0 && rl_src_sel_ < static_cast<int>(rl_versions_.size())) {
            const LyricsVersion& v = rl_versions_[static_cast<size_t>(rl_src_sel_)];
            lines.push_back(note_row("\x1b[1;31m", "Delete \"" + v.label + "\" for good? This cannot be undone."));
            lines.push_back(note_row("\x1b[1;31m", "[y] yes, delete it    any other key: keep it"));
        } else {
            const std::string leg = legend_sgr(settings_);
            lines.push_back(note_row(leg, "[ENTER] fetches again; the lyrics in use are kept as an"));
            lines.push_back(note_row(leg, "older version, so saved timing is never overwritten."));
        }
    }

    lines.push_back(obar + std::string(W - 2, ' ') + obar);   // the one empty line between the note and the list
    // --- the versions (below the note): lyrics written into the file, the fetched copy in use and the older ones a refetch put aside.
    // Three rows are shown; with more versions the list scrolls (arrows at the right edge say which way). ---
    {
        const int inner = W - 2;
        auto heading = [&](const std::string& text) {
            return obar + border + pad_right(" " + text, inner) + "\x1b[0m" + obar;
        };
        lines.push_back(heading("LYRICS IN USE"));
        constexpr int kShown = 3;
        const int n = static_cast<int>(rl_versions_.size());
        const int first = std::clamp(rl_src_scroll_, 0, std::max(0, n - kShown));
        for (int r = 0; r < kShown; ++r) {
            const int i = first + r;
            std::string plain;
            if (n == 0 && r == 0) plain = "   none yet -- [ENTER] fetches them";
            else if (i < n) {
                const LyricsVersion& v = rl_versions_[static_cast<size_t>(i)];
                plain = std::string(v.active ? " (o) " : " ( ) ") + v.label + (v.detail.empty() ? "" : "  " + v.detail);
            }
            std::string row = pad_right(truncate_str(plain, inner - 2), inner - 2);
            if (rl_focus_ == RLField::Source && i == rl_src_sel_ && i < n) row = "\x1b[7m" + row + "\x1b[0m";
            const bool more_up = (r == 0 && first > 0), more_down = (r == kShown - 1 && first + kShown < n);
            const std::string mark = more_up ? "\u25b2" : more_down ? "\u25bc" : " ";
            lines.push_back(obar + row + " " + (more_up || more_down ? border + mark + "\x1b[0m" : mark) + obar);
        }
    }

    lines.push_back(box_bottom(W, rl_confirm_delete_ ? "[y] delete  [n] keep" : "[SPACE] choose version  [d] delete  [ENTER] Refetch  [ESC] cancel", border));
    return lines;
}

// The Console and Settings overlays must always be exactly as tall as
// the Browse-mode player view -- see the comment on this declaration in
// app.h. This recomputes the same panel line counts render_frame()'s
// Browse-mode branch does, plus list_visible_rows_ (already kept
// up to date each frame -- see render_frame()) for the list/queue
// panel's share.
int App::player_view_height(int w) const {
    int h = static_cast<int>(build_metadata_panel(w).size());
    h += static_cast<int>(build_progress_panel(w).size());
    h += static_cast<int>(build_search_bar(w).size());
    // The list rows the Browse view would get on THIS terminal, computed from
    // term_rows_ right here instead of read from list_visible_rows_: that
    // member is only refreshed by Browse frames, so after a resize (going
    // full screen while a menu is open) it was one resize behind and the
    // overlays kept the old, shorter height. Same formula as render_frame():
    // the rows left under the chrome minus the box's 2 border rows and the
    // untouched last terminal row.
    h += std::max(0, term_rows_ - h - 1 - 2);
    h += 2; // the list/queue box's own top+bottom border rows (build_list_panel()/
            // build_queue_panel() add them ON TOP of the content rows they're given)
    h += 1; // the row Browse draws the AcoustID prompt / loading indicator in
            // (idle Browse no longer prints anything there -- the log moved to
            // Settings -- but overlays still size to this taller frame: it is
            // the worst case, and overlay_budget() caps the result at
            // term_rows_ - 1 anyway)
    return h;
}

// ---------------------------------------------------------------------
// Frame assembly
// ---------------------------------------------------------------------

// Full-screen modes (Settings, Console, Cheatsheet, Playlist, Meta editor,
// History) build their frame with a leading "ESC[2J" -- erase the whole screen
// -- and used to send it on EVERY frame, 25 times a second, even though they
// are mostly static. Erasing and then repainting the screen 25x/s is exactly
// what a terminal shows as flicker whenever it paints between the erase and
// the repaint (WSL/ConPTY does this readily).
//
// Once the mode is already on screen (see hard_clear in render_frame()) the
// erase is not needed: this drops it and instead repaints in place, clearing
// only what is actually stale -- the rest of every line that is shorter than
// the terminal ("ESC[K"), and everything below the last line ("ESC[0J").
// Lines that already span the full width get no "ESC[K": with the cursor
// parked in the last column (pending wrap) it would erase that last cell.
static std::string soften_fullscreen_frame(const std::string& frame, int cols) {
    static const std::string kClear = "\x1b[2J";
    // Only valid for frames that are painted top-to-bottom with plain newlines.
    // A frame that places its content with absolute cursor moves ("ESC[row;colH",
    // as the Settings screen does for every cell) neither overwrites the cells it
    // no longer uses -- after a tab switch the old tab would show through -- nor
    // ends with the cursor at the bottom, so the trailing "ESC[0J" would wipe
    // part of the screen (a caret parked mid-screen while editing a value took
    // everything below it along). Those keep their full clear.
    {
        size_t i = (frame.compare(0, kClear.size(), kClear) == 0) ? kClear.size() : 0;
        if (frame.compare(i, 3, "\x1b[H") == 0) i += 3; // the plain cursor-home that follows the clear
        for (; i < frame.size(); ++i) {
            if (frame[i] != '\x1b' || i + 1 >= frame.size() || frame[i + 1] != '[') continue;
            size_t j = i + 2;
            while (j < frame.size() && !(static_cast<unsigned char>(frame[j]) >= 0x40 && static_cast<unsigned char>(frame[j]) <= 0x7E)) ++j;
            if (j < frame.size() && std::string("HfdGABCDEFsu").find(frame[j]) != std::string::npos) return frame;
            i = j;
        }
    }
    std::string in = frame;
    if (in.compare(0, kClear.size(), kClear) == 0) in.erase(0, kClear.size());

    // Only valid for frames painted top to bottom with plain newlines. A frame
    // that places text with absolute cursor moves ("ESC[row;colH" -- the
    // Settings screen does this for every cell) only overwrites the cells it
    // writes; whatever the previous tab/screen drew elsewhere would stay on
    // screen and the two would be mushed together. Those frames keep the full
    // erase (the identical-frame skip and the synchronized write still stop it
    // from flickering).
    for (size_t i = 0; i + 1 < in.size(); ++i) {
        if (in[i] != '\x1b' || in[i + 1] != '[') continue;
        size_t j = i + 2;
        bool has_digit = false;
        while (j < in.size() && ((in[j] >= '0' && in[j] <= '9') || in[j] == ';' || in[j] == '?')) {
            if (in[j] >= '0' && in[j] <= '9') has_digit = true;
            ++j;
        }
        if (j < in.size() && in[j] == 'H' && has_digit) return frame;
        i = j;
    }

    std::string out;
    out.reserve(in.size() + 256);
    size_t pos = 0;
    while (pos < in.size()) {
        size_t nl = in.find('\n', pos);
        if (nl == std::string::npos) {           // trailing partial line: "ESC[0J" below covers it
            out.append(in, pos, std::string::npos);
            break;
        }
        // Visible text of this line = everything except CSI escape sequences.
        std::string visible;
        visible.reserve(nl - pos);
        for (size_t i = pos; i < nl; ) {
            if (in[i] == '\x1b' && i + 1 < nl && in[i + 1] == '[') {
                i += 2;
                while (i < nl && !(static_cast<unsigned char>(in[i]) >= 0x40 && static_cast<unsigned char>(in[i]) <= 0x7E)) ++i;
                if (i < nl) ++i;                  // the final byte
            } else {
                visible += in[i++];
            }
        }
        out.append(in, pos, nl - pos);
        if (display_width(visible) < cols) out += "\x1b[K";
        out += '\n';
        pos = nl + 1;
    }
    out += "\x1b[0J";
    return out;
}

// `line` (text with colour codes, `line_w` columns) with `text` (`text_w` columns, its own colours) laid over it from
// column x: what is under the text is left out, the colours in effect after it carry on.
std::string App::overlay_cells(const std::string& line, int x, const std::string& text, int text_w, int line_w) {
    std::string out, sgr;   // sgr: the last colour code seen (re-applied after the text)
    int col = 0;
    size_t i = 0;
    auto next_char = [&](size_t k) {   // one UTF-8 character
        const unsigned char c = static_cast<unsigned char>(line[k]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        return std::min(line.size(), k + n);
    };
    auto copy_escape = [&](bool keep) {   // line[i] == ESC: one CSI sequence
        size_t j = i + 1;
        if (j < line.size() && line[j] == '[') { ++j; while (j < line.size() && !(line[j] >= '@' && line[j] <= '~')) ++j; }
        if (j < line.size()) ++j;
        const std::string seq = line.substr(i, j - i);
        if (!seq.empty() && seq.back() == 'm') sgr = seq == "\x1b[0m" ? std::string() : sgr + seq;
        if (keep) out += seq;
        i = j;
    };
    while (i < line.size() && col < x) {
        if (line[i] == '\x1b') { copy_escape(true); continue; }
        const size_t j = next_char(i);
        const int w = display_width(line.substr(i, j - i));
        if (col + w > x) break;
        out += line.substr(i, j - i);
        col += w;
        i = j;
    }
    out += std::string(static_cast<size_t>(std::max(0, x - col)), ' ');
    out += "\x1b[0m" + text;
    col = std::max(col, x);
    int skip = text_w;   // the columns under the text
    while (i < line.size() && skip > 0) {
        if (line[i] == '\x1b') { copy_escape(false); continue; }
        const size_t j = next_char(i);
        skip -= display_width(line.substr(i, j - i));
        i = j;
    }
    out += "\x1b[0m" + sgr + std::string(static_cast<size_t>(std::max(0, -skip)), ' ');
    out += line.substr(i);
    (void)line_w;
    return out;
}

// The karaoke overlay uses the lyrics engine whatever the main screen does: with the engine off there (".", Settings ->
// ON/OFF, Use Lyrics) nothing was fetched for the playing track, so the overlay starts the fetch itself -- once per
// track, also after a track change while it is open. The main screen's setting is left as it is.
void App::karaoke_ensure_lyrics() {
    if (!has_track_ || current_path_.empty()) return;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (lyrics_path_ == current_path_) return;   // fetched (or being fetched) for this track
    }
    std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
    launch_lyrics_fetch(metadata_.name, artist, current_path_);
}

std::string App::render_frame(TerminalIO& term) {
    frame_main_len_ = std::string::npos;   // set by the main screen: where its line part ends (the overlays follow)
    spectro_used_ = false;
    spectro_area_[0] = -1;
    caption_rect_[2] = 0;
    caption_text_.clear();
    gfx_ok_ = false;          // set again at the very end of a Browse frame (the Settings / Console / ... screens return early and show no picture)
    gfx2_.active = false;
    gfx_.active = false;      // build_metadata_panel() switches it on again when the image style is drawn this frame
    float_rect_[2] = float_rect_[3] = 0;
    int term_cols = term.cols();
    // Was clamped to a minimum of 80 regardless of the real terminal
    // width -- on a narrower phone terminal (the screenshots suggest
    // something closer to 40-46 visible columns), every line rendered
    // here would already be wider than the physical screen and get
    // wrapped by the terminal itself before the next frame's cursor-home
    // redraw overwrites it. That reads exactly like "truncated mid-word"
    // or "garbled" text even though nothing in this file actually cut it
    // off -- the terminal did, one line later than expected. Lowering the
    // floor so the app actually renders to the real width instead of
    // always assuming at least 80 columns are available.
    int W = std::clamp(term_cols, 40, 200);

    // Real terminal row count -- see term_rows_'s comment in app.h for
    // the full story on why this now actually gets consulted. rows()
    // itself already falls back to a sane default (40) if the ioctl
    // fails, so no extra guarding needed here beyond a floor against
    // truly pathological values feeding into subtraction below.
    term_rows_ = std::max(term.rows(), 4);

    // Big list overlay (SHIFT+l): size it against the real terminal every
    // frame -- before anything below scrolls/paints the list -- so
    // list_nav_rows() (which every scroll-follows-the-cursor calculation
    // asks) already reflects the overlay's row count, and re-anchor the
    // window so a resize can never leave the cursor off-screen.
    int overlay_panel_w = 0;
    if (list_overlay_active()) {
        list_overlay_geometry(W, overlay_panel_w, overlay_list_rows_);
        list_overlay_fit_scroll(overlay_list_rows_);
    }
    if (convert_overlay_active()) {
        convert_overlay_poll();
        convert_overlay_geometry(W, overlay_panel_w, cv_rows_);
        const int n = convert_overlay_total();
        cv_sel_ = n > 0 ? std::clamp(cv_sel_, 0, n - 1) : 0;
        cv_scroll_ = std::clamp(cv_scroll_, 0, std::max(0, n - cv_rows_));
        if (cv_sel_ < cv_scroll_) cv_scroll_ = cv_sel_;
        if (cv_sel_ >= cv_scroll_ + cv_rows_) cv_scroll_ = cv_sel_ - cv_rows_ + 1;
    } else {
        convert_overlay_poll();   // a running conversion keeps reporting on the status line with the overlay closed
    }
    if (folder_overlay_active()) {
        folder_overlay_geometry(W, overlay_panel_w, fo_rows_);
        const int n = static_cast<int>(fo_view_.size());
        fo_sel_ = n > 0 ? std::clamp(fo_sel_, 0, n - 1) : 0;
        fo_scroll_ = std::clamp(fo_scroll_, 0, std::max(0, n - fo_rows_));
        if (fo_sel_ < fo_scroll_) fo_scroll_ = fo_sel_;
        if (fo_sel_ >= fo_scroll_ + fo_rows_) fo_scroll_ = fo_sel_ - fo_rows_ + 1;
    }
    // Big queue overlay (SHIFT+k): same per-frame sizing for the queue window.
    // (Never open together with the list overlay, so they can share overlay_panel_w.)
    if (queue_overlay_active()) {
        queue_overlay_geometry(W, overlay_panel_w, overlay_queue_rows_);
        clamp_queue_selected();
    }

    // Browse/BulkAdd/RetryLyrics all share the same live background (the
    // latter two float a small panel on top of it -- see
    // draw_floating_panel()'s comment in app.h), so switching between
    // them never needs a full clear, only a redraw. Settings/Console/
    // Cheatsheet are still genuine full-screen takeovers, so entering or
    // leaving any of *those* still forces one, same as a real mode
    // change always has.
    auto mode_family = [this](Mode m) {
        // The timing overlay opened from karaoke floats over the karaoke screen,
        // not the main one, so it belongs to karaoke's family -- otherwise
        // opening and closing it would force a full clear and flicker.
        if (m == Mode::LyricsEdit && lyrics_edit_from_karaoke_) return 7;
        switch (m) {
            case Mode::Browse: case Mode::Search: case Mode::BulkAdd: case Mode::RetryLyrics: case Mode::ClearQueue: case Mode::OsciMenu: case Mode::SpectroMenu: case Mode::NormMenu: case Mode::Equalizer: case Mode::SleepTimer: case Mode::LyricsEdit: return 0;
            case Mode::Settings: case Mode::ColorEdit: return 1;
            case Mode::Console: return 2;
            case Mode::Cheatsheet: return 3;
            case Mode::Playlist: return 4;
            case Mode::MetaEdit: return 5;
            case Mode::History: return 6;
            case Mode::Karaoke: return 7;
            case Mode::Wizard: return 8;
            case Mode::ReadLyrics: return 9;
        }
        return 0;
    };
    bool hard_clear = (W != last_render_w_) || (term_rows_ != last_render_rows_) || (mode_family(mode_) != mode_family(last_render_mode_)) || force_redraw_;
    if (last_render_w_ != -1 && W != last_render_w_) {
        // Verbose-only: raw ioctl terminal size alongside the clamped
        // app-usable width, i.e. "what the OS actually told us" versus
        // what we did with it.
        ConsoleLog::instance().log_verbose(
            "terminal resized: " + std::to_string(last_render_w_) + " -> " + std::to_string(W) +
            " cols (raw ioctl cols=" + std::to_string(term_cols) + ", rows=" + std::to_string(term_rows_) + ")");
    }
    force_redraw_ = false; // one-shot -- consumed by this frame
    last_render_w_ = W;
    last_render_rows_ = term_rows_;
    last_render_mode_ = mode_;
    const char* clear_prefix = hard_clear ? "\x1b[2J\x1b[H" : "\x1b[H";

    // SHIFT+0: the oscilloscope over the whole screen, YT left and XY right (the options overlay SHIFT+o can sit on top of it)
    if (scope_big_ && (mode_ == Mode::Browse || mode_ == Mode::OsciMenu)) {
        std::string out = build_scope_big(W, clear_prefix);
        frame_main_len_ = out.size();
        if (mode_ == Mode::OsciMenu) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_osci_menu_panel(), kOsciMenuPanelWidth, W);
            out += floating.str();
        } else if (scope_big_color_edit_) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_scope_color_panel(), kScopeColorPanelWidth, W);
            out += floating.str();
        } else if (scope_metrics_open_) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_scope_metrics_panel(), kScopeMetricsPanelWidth, W);
            out += floating.str();
        }
        // image style: a floating panel cuts a hole into the pictures it covers (their pixels there are cleared; Sixel also
        // keeps those cells), and a changed hole means new pictures
        gfx_ok_ = gfx_.active;
        for (int k = 0; k < 2 && gfx_.active; ++k) {
            GfxFrame& F = k == 0 ? gfx_ : gfx2_;
            int cut[4] = {-1, -1, -1, -1};
            const int fx = float_rect_[0], fy = float_rect_[1], fw = float_rect_[2], fh = float_rect_[3];
            if (fw > 0 && fh > 0) {
                const int c0 = std::max(fx, F.col), c1 = std::min(fx + fw, F.col + F.cols);
                const int r0 = std::max(fy, F.row), r1 = std::min(fy + fh, F.row + F.rows);
                if (c0 < c1 && r0 < r1) { cut[0] = c0 - F.col; cut[1] = r0 - F.row; cut[2] = c1 - c0; cut[3] = r1 - r0; }
            }
            if (cut[2] > 0) {
                F.cut_c = cut[0]; F.cut_r = cut[1]; F.cut_w = cut[2]; F.cut_h = cut[3];
                const int xa = F.cut_c * F.w / F.cols, xb = (F.cut_c + F.cut_w) * F.w / F.cols;
                const int ya = F.cut_r * F.h / F.rows, yb = (F.cut_r + F.cut_h) * F.h / F.rows;
                for (int y = ya; y < yb && y < F.h; ++y)
                    for (int x = xa; x < xb && x < F.w; ++x) {
                        F.level[static_cast<size_t>(y) * F.w + x] = 0;
                        if (!F.grid.empty()) F.grid[static_cast<size_t>(y) * F.w + x] = 0;
                    }
            } else F.cut_w = 0;
            if (!std::equal(cut, cut + 4, scope_cut_last_ + k * 4)) F.fresh = true;
            std::copy(cut, cut + 4, scope_cut_last_ + k * 4);
        }
        return out;
    }

    // SHIFT+u: the spectrogram over the whole screen (the options overlay SHIFT+i can sit on top of it)
    if (spectro_full_ && (mode_ == Mode::Browse || mode_ == Mode::SpectroMenu)) {
        std::string out = build_spectro_full(W, clear_prefix);
        frame_main_len_ = out.size();
        if (mode_ == Mode::SpectroMenu) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_spectro_menu_panel(), kSpectroMenuPanelWidth, W);
            out += floating.str();
        }
        return out;
    }

    // Every overlay below lays itself out against player_view_height() --
    // the height of the Browse view it stands in for -- but that value can
    // exceed what actually fits: list_visible_rows_ is only recomputed
    // while Browse itself is being rendered, so anything that grows the
    // metadata panel while another mode is up (playback starting, for
    // instance) leaves it stale and pvH points past the bottom of the
    // screen. clamp_output_rows() keeps term_rows_ - 1 lines, so the
    // budget asked for here is min(pvH, term_rows_ - 1): an overlay laid
    // out taller than the screen would have its own footer -- hint line
    // plus status line -- chopped off on every single frame, which is the
    // same bug that used to hide Browse's status line entirely.
    auto overlay_budget = [&](int w) {
        return std::min(player_view_height(w), std::max(1, term_rows_ - 1));
    };

    // Finishes a full-screen frame: clamp to the terminal height, and -- unless
    // this is the frame that switches the mode / resizes (hard_clear) -- repaint
    // in place instead of erasing the whole screen again (see
    // soften_fullscreen_frame()).
    auto fullscreen_out = [&](std::ostringstream& frame) {
        std::string clamped = clamp_output_rows(frame.str(), term_rows_);
        return hard_clear ? clamped : soften_fullscreen_frame(clamped, term_cols);
    };

    if (mode_ == Mode::Settings || mode_ == Mode::ColorEdit) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_settings_screen(frame, W, overlay_budget(W));
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::Console) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_console_screen(frame, W, overlay_budget(W));
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::Cheatsheet) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_cheatsheet_screen(frame, W);
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::Wizard) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_wizard_screen(frame, W);
        return fullscreen_out(frame);
    }

    // The karaoke screen, and the lyrics timing overlay when it was opened from
    // it (Alt+L): the panel floats over the big lyrics, which is the whole point
    // of adjusting the offset from here.
    if (mode_ == Mode::Karaoke || (mode_ == Mode::LyricsEdit && lyrics_edit_from_karaoke_)) {
        karaoke_ensure_lyrics();   // the karaoke overlay always has the lyrics engine, whatever the main screen uses
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_karaoke_screen(frame, W, term_rows_ - 1);
        std::string out = fullscreen_out(frame);
        if (mode_ == Mode::LyricsEdit) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_lyrics_edit_panel(), kLyricsEditPanelWidth, W);
            out += floating.str();
        }
        return out;
    }

    if (mode_ == Mode::Playlist) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_playlist_screen(frame, W, overlay_budget(W));
        std::string out = fullscreen_out(frame);
        if (playlist_export_open_) {
            // The export overlay floats on top (absolute cursor positions, no newlines) -- appended AFTER the row clamp,
            // which would otherwise cut it off with everything past the last screen row.
            std::ostringstream panel;
            const int pw = std::clamp(W - 10, 50, 100);
            draw_floating_panel(panel, build_playlist_export_panel(pw), pw, W);
            out += panel.str();
        }
        if (playlist_import_open_) { // same floating treatment as the export overlay
            std::ostringstream panel;
            const int pw = std::clamp(W - 10, 50, 100);
            draw_floating_panel(panel, build_playlist_import_panel(pw), pw, W);
            out += panel.str();
        }
        return out;
    }

    if (mode_ == Mode::MetaEdit) {
        // The five field values are resolved right before drawing them:
        // reading a tag can promote into the shared cache, and the panel
        // builder itself is const (see meta_refresh_hover_values()).
        meta_refresh_hover_values();
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_meta_screen(frame, W, overlay_budget(W));
        std::string shown = fullscreen_out(frame);
        if (meta_choice_open_) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_meta_choice_panel(), kMetaChoicePanelWidth, W);
            shown += panel.str();
        }
        // Saving takes a moment for a few files and a while for hundreds: the panel only appears when it lasts longer than ~1 s.
        if (meta_save_.active && std::chrono::steady_clock::now() - meta_save_.started > std::chrono::milliseconds(1000)) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_meta_save_panel(), kMetaSavePanelWidth, W);
            shown += panel.str();
        }
        return shown;
    }

    if (mode_ == Mode::History) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_history_screen(frame, W, overlay_budget(W));
        return fullscreen_out(frame);
    }

    if (mode_ == Mode::ReadLyrics) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_readlyrics_screen(frame, W, overlay_budget(W));
        std::string shown = fullscreen_out(frame);
        if (rd_opts_open_) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_readlyrics_options_panel(), 66, W);
            shown += panel.str();
        }
        if (rd_search_open_) {
            std::ostringstream panel;
            draw_floating_panel(panel, build_readlyrics_search_panel(), 74, W);
            shown += panel.str();
        }
        return shown;
    }

    // --- Browse mode (and the background behind BulkAdd/RetryLyrics):
    // figure out how many list/queue rows actually fit before building
    // anything, so the panel is sized right the first time instead of
    // being built tall and then chopped.
    auto metadata_lines = build_metadata_panel(W);
    auto progress_lines = build_progress_panel(W);
    auto search_lines = build_search_bar(W);
    // The AcoustID confirmation raised by SHIFT+b lives in Browse's
    // status area too (there is no footer here), but the disclaimer alone is
    // 110 characters -- wider than many terminals -- so instead of trusting
    // one physical line to hold it (which would wrap in the terminal and
    // push the whole frame into a scroll), it is wrapped here and as many
    // rows as it actually needs are reserved for it, exactly like the single
    // loading indicator row -- and, since neither of them is there most of
    // the time, no row at all is reserved when both are absent (see below).
    std::vector<std::string> prompt_lines;
    if (meta_prompt_ != MetaPrompt::None && mode_ == Mode::Browse) {
        prompt_lines = wrap_lines(meta_fetch_disclaimer() + "   [Y]es   [N]o   [ESC] cancel",
                                  std::max(10, W - 2), 2);
    }
    // The ONLY things still drawn below the list box are the AcoustID
    // prompt (wrapped, so as many rows as it actually needs) or -- mutually
    // exclusive with it -- the live "resolving/downloading..." indicator.
    // The log/status line that used to own this slot moved to the Settings
    // screen, so when neither is present nothing is printed down there and
    // therefore no row is reserved for it either: a row reserved only to
    // sit blank adds an extra empty line under a frame that already ends at
    // term_rows_ - 1, and it withholds a row the list/queue panes could
    // show a track in. Since status_rows always equals the number of rows
    // that will really be printed below the box (0, or the prompt's wrapped
    // line count, or 1), the frame comes out term_rows_ - 1 tall in every
    // case -- with or without a prompt/loading row, no jitter, no scroll.
    // The resolving/downloading note lives in the search box's top border (build_search_bar), not down here.
    int status_rows = static_cast<int>(prompt_lines.size());
    int fixed_h = static_cast<int>(metadata_lines.size() + progress_lines.size() + search_lines.size())
                + status_rows; // rows really drawn under the list box (0 when none are)
    // Two things used to be missing from this budget, and together they made
    // the frame exactly 2 lines too tall on EVERY terminal:
    //   * build_list_panel()/build_queue_panel() draw a top and a bottom
    //     border row around the `list_h` content rows they're handed, and
    //   * there used to be a blank separator line before the status line.
    // clamp_output_rows() keeps term_rows_ - 1 lines, so those last 2 lines
    // -- blank + status/loading -- were chopped off every single frame: no
    // Browse message ever reached the screen at all ("added to queue",
    // "queued 3 tracks", the SHIFT+b AcoustID question, ...). The blank
    // line is gone (the last row sits directly under the list box now),
    // and the box's 2 border rows are subtracted from the room the list may
    // use, so the frame ends up exactly term_rows_ - 1 lines tall with the
    // prompt/loading row as its last one -- or, when there is no prompt and
    // nothing is loading, with the list box's bottom border as its last one
    // and the terminal's final row left blank. Ordinary status messages are
    // no longer printed here at all -- see where that row is built below.
    // -1 extra margin: leave the terminal's very last row untouched so a
    // trailing '\n' after the final printed line can never itself force
    // a scroll (see clamp_output_rows()'s comment for the same reasoning
    // applied as a hard backstop).
    int available_for_list = term_rows_ - fixed_h - 1 - 2;
    // No upper cap: the list/queue panes take every row the terminal leaves,
    // so a maximised window is filled to its last row. (It used to be capped
    // at kListVisibleRows = 8, which is exactly what a 30-row terminal has
    // room for -- on a taller one the extra rows stayed blank.)
    list_visible_rows_ = std::max(0, available_for_list);

    ensure_visible_row_meta();

    std::ostringstream frame;
    frame << clear_prefix;

    for (auto& l : metadata_lines) frame << l << "\n";
    for (auto& l : progress_lines) frame << l << "\n";
    for (auto& l : search_lines) frame << l << "\n";

    int list_h = list_visible_rows_;
    if (settings_.element_queue) {
        int list_w = W / 2;
        int queue_w = W - list_w; // exact 50/50, remainder (odd W) goes to queue
        auto list_lines = build_list_panel(list_w, list_h);
        auto queue_lines = build_queue_panel(queue_w, list_h);
        size_t rows = std::max(list_lines.size(), queue_lines.size());
        for (size_t i = 0; i < rows; ++i) {
            std::string l = (i < list_lines.size()) ? list_lines[i] : std::string(list_w, ' ');
            std::string r = (i < queue_lines.size()) ? queue_lines[i] : std::string(queue_w, ' ');
            frame << l << r << "\n";
        }
    } else {
        for (auto& l : build_list_panel(W, list_h)) frame << l << "\n";
    }

    // No blank separator above this last row any more: with the list box's
    // border rows now counted in available_for_list, dropping that row is
    // what actually leaves room for it (see the comment there -- it used to
    // be cut off by clamp_output_rows() every frame).
    //
    // The row is NOT the log/status line any more. status_line_ (written by
    // log_event() and by every action's own message -- "added to queue",
    // "SAVED", "searching online ...") was the one line in this frame that
    // was printed raw: never width-limited, so a message longer than W
    // wrapped in the terminal and added a physical row to an already
    // full-height frame. One row too many is all it takes for the frame to
    // scroll: the terminal's own scrollbar appears, that scrollbar costs
    // the last text column, and then *every* full-width box row of the next
    // frame wraps too -- the UI visibly shifts left and keeps scrolling.
    // The log now lives on the Settings screen instead (see the status line
    // at the bottom of build_settings_screen()), where a stray wrap is
    // cosmetic and never touches this frame. So nothing is written into this
    // slot when no prompt is up and nothing is loading -- and status_rows
    // (above) reserves nothing in that case either, which is what keeps the
    // frame at exactly term_rows_ - 1 lines instead of leaving one blank
    // line short of the bottom while the panes above lose a row to it. When
    // something IS drawn here, its rows are reserved up front, so the prompt
    // and the loading indicator keep a fixed position and the layout never
    // jitters.
    if (!prompt_lines.empty()) {
        // Same yellow-on-black as the meta menu's own confirmation footer.
        // wrap_lines() already cut these to W - 2, and the " "+...+" "
        // padding adds exactly the 2 columns that takes back, so the result
        // is exactly W wide -- this line can never wrap either.
        for (const auto& l : prompt_lines) frame << "\x1b[43;30m " << l << " \x1b[0m\n";
    }

    frame << "\x1b[0J";

    // Hard safety net on top of the list_visible_rows_ sizing above: even
    // if the fixed chrome alone (metadata+progress+search bar) is taller
    // than the terminal -- a case list_visible_rows_ can't do anything
    // about, since it only controls the list panel -- this guarantees
    // the actual byte stream handed to the terminal never contains more
    // rows than the terminal has, so it structurally cannot scroll no
    // matter what future panels/config combinations produce.
    std::string out = clamp_output_rows(frame.str(), term_rows_);
    frame_main_len_ = out.size();

    // Bulk Add / Retry Lyrics / Clear Queue: stamp their floating panel on top
    // of the still-live background just built above, rather than replacing
    // it. Uses absolute positioning (draw_floating_panel()), so it is simply
    // appended after the background's own sequential top-to-bottom writes --
    // whichever content lands on a given screen cell last in the stream
    // wins, and the panel is emitted after, so it draws over the background
    // wherever they overlap without needing a clear.
    //
    // It MUST be appended after the clamp above, not before: on a terminal
    // where the background already fills term_rows_ - 1 lines (any terminal
    // too short for the full-height list, e.g. 30 rows), clamp_output_rows()
    // cuts everything after the last kept newline -- which used to include
    // the panel, so it never appeared while the mode still swallowed keys.
    // The panel adds no '\n' (absolute moves only) and draw_floating_panel()
    // already keeps it inside the screen, so it cannot cause a scroll.
    std::ostringstream floating;
    if (list_overlay_active()) {
        // Stamped first so the small forms (Retry Lyrics, Bulk Add, Clear
        // Queue) still float on top of it when they are opened from inside
        // the overlay. Absolute moves only -- no '\n', so it cannot scroll --
        // and rows past the last usable terminal row are dropped.
        const auto lines = panel_gradient(build_list_overlay_panel(overlay_panel_w, overlay_list_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2; // one background row stays visible above and below
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_ - 1) break;
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    } else if (convert_overlay_active()) {
        const auto lines = panel_gradient(build_convert_overlay_panel(overlay_panel_w, cv_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_ - 1) break;
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    } else if (folder_overlay_active()) {
        const auto lines = panel_gradient(build_folder_overlay_panel(overlay_panel_w, fo_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_ - 1) break;
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    } else if (queue_overlay_active()) {
        // Same placement as the list overlay; the queue pane's own rows live in it.
        const auto lines = panel_gradient(build_queue_overlay_panel(overlay_panel_w, overlay_queue_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_ - 1) break;
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    }
    if (mode_ == Mode::BulkAdd) {
        draw_floating_panel(floating, build_bulk_add_panel(), kBulkAddPanelWidth, W);
    } else if (mode_ == Mode::RetryLyrics) {
        draw_floating_panel(floating, build_retry_lyrics_panel(), kRetryLyricsPanelWidth, W, 0, true);
    } else if (mode_ == Mode::ClearQueue) {
        draw_floating_panel(floating, build_clear_queue_panel(), kClearQueuePanelWidth, W);
    } else if (mode_ == Mode::OsciMenu) {
        // Centred like the other overlays; a scope image under it is cropped to the columns that stay free.
        draw_floating_panel(floating, build_osci_menu_panel(), kOsciMenuPanelWidth, W);
    } else if (mode_ == Mode::SpectroMenu) {
        draw_floating_panel(floating, build_spectro_menu_panel(), kSpectroMenuPanelWidth, W);
    } else if (mode_ == Mode::NormMenu) {
        draw_floating_panel(floating, build_norm_menu_panel(), kNormMenuPanelWidth, W);
    } else if (mode_ == Mode::LyricsEdit) {
        draw_floating_panel(floating, build_lyrics_edit_panel(), kLyricsEditPanelWidth, W);
    } else if (mode_ == Mode::SleepTimer) {
        draw_floating_panel(floating, build_sleep_timer_panel(), kSleepTimerPanelWidth, W);
    } else if (mode_ == Mode::Equalizer) {
        draw_floating_panel(floating, build_eq_panel(), kEqPanelWidth, W);
    }
    out += floating.str();
    if (gfx_.active) {
        const bool floating_mode = mode_ != Mode::Browse && mode_ != Mode::Search;
        const bool covered_all = list_overlay_active() || queue_overlay_active() || folder_overlay_active() || convert_overlay_active();
        if (covered_all || (mode_family(mode_) != 0)) gfx_.active = false;
        else {
            gfx_.crop = 0;
            const int fx = float_rect_[0], fy = float_rect_[1], fw = float_rect_[2], fh = float_rect_[3];
            if (floating_mode && fw > 0 && fy < gfx_.row + gfx_.rows && fy + fh > gfx_.row && fx < gfx_.col + gfx_.cols && fx + fw > gfx_.col) {
                // an overlay covers part of the picture: free the columns up to its right edge (all of it when it sits further right)
                gfx_.crop = fx <= gfx_.col ? std::clamp(fx + fw - gfx_.col, 0, gfx_.cols) : gfx_.cols;
            }
        }
        if (gfx_.active && gfx_.crop != gfx_last_crop_) gfx_.fresh = true;
        gfx_last_crop_ = gfx_.active ? gfx_.crop : -1;
    } else gfx_last_crop_ = -1;
    gfx_ok_ = gfx_.active;
    return out;
}

// Keeps at most (term_rows - 1) lines of `frame` (the -1 leaves the
// terminal's last row untouched, so the final line's trailing '\n' can
// never itself trigger a scroll) and drops everything after that,
// escape-code prefixes and all -- this is the hard backstop described
// in term_rows_'s comment in app.h: whatever the panel-sizing logic
// above computed, the actual printed output can never exceed what the
// real terminal can show without scrolling. Content past the cutoff is
// simply not drawn this frame rather than causing any corruption.
std::string App::clamp_output_rows(const std::string& frame, int term_rows) const {
    int max_lines = std::max(1, term_rows - 1);
    int newlines_seen = 0;
    size_t keep = frame.size();
    for (size_t i = 0; i < frame.size(); ++i) {
        if (frame[i] == '\n') {
            ++newlines_seen;
            if (newlines_seen >= max_lines) { keep = i + 1; break; }
        }
    }
    // BorderGradient=smooth: every frame leaves through here, so this is where the border lines get their row's colour
    // (the cursor-positioned Settings screens do their own gradient and have no line breaks).
    if (settings_.border_smooth && newlines_seen >= 2) {
        static const std::vector<std::string> kGlyphs = {"\u2502", "\u2500", "\u256d", "\u256e", "\u2570", "\u256f", "\u251c", "\u2524", "\u252c", "\u2534", "\u253c"};
        std::vector<std::string> glyphs = kGlyphs;
        for (const std::string* g : {&settings_.box_upper_left, &settings_.box_upper_right, &settings_.box_lower_left,
                                     &settings_.box_lower_right, &settings_.box_vertical, &settings_.box_horizontal})
            if (!g->empty()) glyphs.push_back(*g);
        const std::string top = ansi_for(settings_.border_color, false), bottom = ansi_for(settings_.border_color_bottom, false);
        return border_gradient::apply(frame.substr(0, keep), top, bottom, glyphs, [&](float t) {
            return gradient_ansi(settings_.border_color, settings_.border_color_bottom, t, false);
        });
    }
    return keep == frame.size() ? frame : frame.substr(0, keep); // already within budget, or cut to it
}

// ---------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------

int App::run() {
    // Coming back from the radio the App is still alive (it was only suspended): nothing is initialised or restored again.
    const bool resuming = suspended_;
    suspended_ = false;
    if (resuming) settings_.start_mode = read_start_mode();   // the radio's settings may have changed it meanwhile
    bool restored = false;
    if (!resuming) {
    ConsoleLog::instance().init(settings_.console_verbosity == 1 ? LogVerbosity::Verbose : LogVerbosity::Basic);
    // Flush what the constructor's local-library scan found before logging
    // was ready to record it -- see local_scan_diagnostics_'s declaration
    // for why this can't just be logged from inside scan() directly. Basic
    // level, not Verbose: "why does my library look wrong" is exactly the
    // kind of thing someone shouldn't need to raise console_verbosity to see.
    for (const auto& line : local_scan_diagnostics_) {
        ConsoleLog::instance().log_basic(line);
    }
    {
        // Verbose-only startup facts -- "what the OS provided" at the
        // very start of the session, before anything else has run.
#if defined(_WIN32)
        ConsoleLog::instance().log_verbose("os: Windows");
#else
        struct utsname uts{};
        if (uname(&uts) == 0) {
            ConsoleLog::instance().log_verbose(std::string("os: ") + uts.sysname + " " + uts.release + " " + uts.machine);
        }
#endif
        ConsoleLog::instance().log_verbose("home: " + std::string(std::getenv("HOME") ? std::getenv("HOME") : "(unset)"));
    }

    // Session snapshot restore -- only when the feature's on. A missing
    // or corrupt snapshot.json is treated identically to "no snapshot at
    // all" (load_snapshot() already guards that), so this always falls
    // back to the normal cold-start behavior on any failure.
    if (settings_.autosave_enabled) {
        SnapshotData snap;
        if (load_snapshot(snap)) {
            restore_snapshot(snap);
            restored = true;
            // Consumed exactly once -- see snapshot.h's delete_snapshot()
            // comment for why this happens right after a successful
            // restore rather than only at the next autosave/exit.
            delete_snapshot();
            log_event("restored previous session");
        }
    }
    if (!restored && !local_view_.empty()) {
        selected_ = 0;
        start_local_track(local_view_[0]);
    }
    // First start after an install: greet with the help wizard. Closing it
    // writes WizardSeen=true, so this happens exactly once -- and only on a
    // cold start, never when coming back from the radio (which keeps the
    // player suspended, not restarted).
    if (wizard::kEnabled && !settings_.wizard_seen) wizard_open();
    }   // !resuming

    TerminalIO term;
    if (resuming) { quit_ = false; switch_mode_ = false; force_redraw_ = true; }
    last_frame_time_ = std::chrono::steady_clock::now();
    last_autosave_at_ = std::chrono::steady_clock::now();
    ConsoleLog::instance().log_verbose("terminal: " + std::to_string(term.rows()) + "x" + std::to_string(term.cols()) + " (rows x cols, raw ioctl)");

    std::string last_frame_str;
    std::vector<std::string> prev_lines;   // the main screen's lines as last sent (diff_frame_lines)
    std::string last_floating;             // the overlay part of the last frame
    auto last_frame_written_at = std::chrono::steady_clock::now();
    bool gfx_shown = false;
    unsigned long gfx_tick = 0;


    while (!quit_) {
        const auto frame_start = std::chrono::steady_clock::now();
        // the image styles (oscilloscope or spectrogram) need to know what the terminal speaks: ask once, and again when
        // the protocol setting changes
        if ((settings_.osci_style == 1 || settings_.spectro.style == 1) && gfx_probed_pref_ != settings_.gfx_protocol) {
            if (gfx_shown) { write_frame(gfx_clear(gfx_proto_)); gfx_shown = false; }
            gfx_proto_ = gfx_probe(settings_.gfx_protocol);
            gfx_probed_pref_ = settings_.gfx_protocol;
        }
        // Drain every key already queued before rendering, rather than
        // one per frame. A single keystroke can arrive as more than one
        // poll_key() call's worth of data -- any non-ASCII character (a
        // German umlaut, most concretely) is a multi-byte UTF-8 sequence
        // dispensed one byte per call, on both platforms (see
        // TerminalIO::poll_key() / win_poll_key()). Rendering between
        // those calls meant the frame in between showed a buffer ending
        // in a lone, incomplete lead byte -- which decodes as a
        // replacement glyph -- for one frame, before the next poll
        // completed the sequence and it snapped to the real character.
        // Draining first means the frame that actually renders always
        // has a complete, valid buffer. This never blocks waiting for
        // more input: poll_key() is non-blocking and returns 0 the
        // moment nothing already-received is left to hand back.
        // A single-line text field is being edited right now: hand Ctrl+C /
        // Ctrl+X / Ctrl+V to the app as keystrokes instead of letting the
        // console treat Ctrl+C as "kill this process" (Windows, where the
        // ENABLE_PROCESSED_INPUT bit is what does that) or swallow it via
        // ISIG (POSIX). Re-derived every frame rather than toggled on mode
        // entry/exit: mode_ can change in ways that skip a hand-off (ESC out
        // of an edit, a prompt), and leaving the flag stuck would mean
        // Ctrl+C no longer quits the app. Same key set both platforms, see
        // win_poll_key()/poll_key() for how it arrives as kKeyCtrlC/X/V.
        const bool playlist_text_field =
            mode_ == Mode::Playlist &&
            ((playlist_export_open_ && playlist_export_field_ == 0) ||
             (playlist_import_open_ && playlist_import_field_ == 0) ||
             (!playlist_export_open_ && !playlist_import_open_ &&
              ((playlist_tab_ == 0 && playlist_edit_focus_ <= 1) ||
               (playlist_tab_ == 1 && playlist_manage_focus_ == 0))));
        set_text_entry(mode_ == Mode::Search ||
                       mode_ == Mode::ColorEdit ||
                       (mode_ == Mode::MetaEdit && meta_tab_ == 0 &&
                        (meta_focus_ == 0 || meta_focus_ == 2)) ||
                       playlist_text_field);

        for (int key = term.poll_key(); key != 0; key = term.poll_key()) {
            const bool in_settings = mode_ == Mode::Settings || mode_ == Mode::ColorEdit;
            if (in_settings) status_line_.clear();   // a message stays until the next key; then the "unsaved changes" note shows again
            handle_key(key);
            if (mode_ == Mode::Settings || mode_ == Mode::ColorEdit) settings_update_dirty();
        }

        poll_pending_search();
        poll_pending_load();
        poll_pending_waveform();
        poll_pending_bulk_add();
        poll_pending_row_meta_tags();
        poll_pending_meta_fetch(); // AcoustID results -> pending edits
        poll_meta_save();          // the next slice of a running save
        readlyrics_poll();         // a finished lyrics translation
        maybe_autosave();

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time_).count();
        last_frame_time_ = now;
        viz_dt_ = dt;

        if (has_track_) {
            player_.poll_elapsed();
            // No auto-advance while a device handoff is still in flight:
            // finished_ belongs to the previous track until play() swaps the
            // new one in (see device_play_pending_gen_).
            if (!advancing_ && device_play_pending_gen_.load() == 0 && player_.finished()) advance_track();
        }
        sleep_timer_tick();
        player_.set_scope_raw(settings_.osci().music);   // oscilloscope music mode: the scopes get the unprocessed signal
        scope_window_sync();
        spectro_window_sync();
        enforce_visual_switches();
        // Disk only spins while something is actually playing — frozen
        // when idle or paused, per instruction.
        if (has_track_ && !player_.is_paused()) {
            angle_ = std::fmod(angle_ + kAngularVelocity *settings_.disk_rotation_speed * dt,
                               2.0 * 3.14159265358979323846);
        }
        // The wizard's welcome disk has an angle of its own: a fixed 0.10,
        // turning whether or not anything is playing, so the first screen a
        // new user sees is never a frozen picture.
        if (mode_ == Mode::Wizard) {
            wizard_angle_ = std::fmod(wizard_angle_ + kAngularVelocity * 0.10 * dt,
                                      2.0 * 3.14159265358979323846);
        }
        // Listening history: accrue what was actually heard this frame.
        // Paused time never counts (that's the is_paused() check), and neither
        // does the handover gap between two tracks -- there advancing_ is set
        // or a device handoff is in flight while nothing is really sounding.
        // No-op when no play is live, so it can be called unconditionally.
        if (has_track_ && !player_.is_paused() && !advancing_ &&
            device_play_pending_gen_.load() == 0) {
            history_.add_listened(dt);
        }

        // A layout bug on some odd terminal size (a negative width reaching a
        // std::string, a vector sized from a negative row count, ...) used to
        // throw out of render_frame(), through run(), and end the program --
        // which is exactly what a window being dragged smaller looks like from
        // the outside. Skip that one frame instead (the next resize event or
        // frame will usually lay out fine), log it, and force a full repaint
        // once rendering works again.
        std::string frame_str;
        try {
            frame_str = render_frame(term);
        } catch (const std::exception& e) {
            ConsoleLog::instance().log_basic(std::string("render skipped (") + std::to_string(term.cols()) + "x" + std::to_string(term.rows()) + "): " + e.what());
            force_redraw_ = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        // Static screens (menus, settings, a paused player) produce the very
        // same bytes frame after frame; sending them again only gives the
        // terminal something to repaint. Skip identical frames, but still
        // resend one every couple of seconds so the screen heals itself if
        // anything outside the app (a terminal resize/restore) scribbled on it.
        // Sixel: the text skips the picture's cells (they stay as they are on screen), so the picture is only sent when
        // there is a new one, or when it is not on screen yet / the screen was cleared (then the current one at once).
        // spectrogram picture (tiles): where the frame put it, unless a big overlay covers the whole area
        const int sp_proto = gfx_proto_ == GfxProto::Kitty ? 1 : gfx_proto_ == GfxProto::Sixel ? 2 : 0;
        // Only a main-screen (or spectrogram full-screen) frame shows it: the full-screen takeovers (settings, cheat
        // sheet, playlists ...) also lay out the top panel for their sizing, which must not count as "drawn".
        const bool main_frame = frame_main_len_ != std::string::npos;
        const bool sp_img = main_frame && spectro_area_[0] >= 0 && sp_proto != 0 && !list_overlay_active() && !queue_overlay_active();
        if (!spectro_used_ || !main_frame) spectro().set_visible(false);
        if (frame_str.find("\x1b[2J") != std::string::npos) spectro_gfx_.invalidate();
        const bool sixel_on = (gfx_ok_ && gfx_proto_ == GfxProto::Sixel && gfx_.active && gfx_.crop < gfx_.cols) || (sp_img && sp_proto == 2);
        if (sixel_on && (!gfx_shown || frame_str.find("\x1b[2J") != std::string::npos)) { gfx_.fresh = true; gfx2_.fresh = true; }
        const auto frame_now = std::chrono::steady_clock::now();
        std::string wire;   // ONE write per frame (text + picture): two writes let the terminal paint in between = flicker
        const bool heal = frame_now - last_frame_written_at > std::chrono::seconds(2);
        if (frame_str != last_frame_str || heal) {
            // The main screen: only the lines that changed (at 120 / 165 fps the whole screen every frame would be
            // megabytes per second for the terminal). Full frames: every 2 s, after a clear, or when an overlay changed.
            const size_t main_len = frame_main_len_ <= frame_str.size() ? frame_main_len_ : std::string::npos;
            const std::string floating = main_len == std::string::npos ? std::string() : frame_str.substr(main_len);
            bool diffed = false;
            if (main_len != std::string::npos && frame_str.compare(0, 3, "\x1b[H") == 0) {
                std::string d;
                const std::string body = frame_str.substr(3, main_len - 3);
                if (diff_frame_lines(body, prev_lines, d)) {
                    if (!heal && floating == last_floating) {
                        // what follows the lines continues where the whole frame would have left the cursor
                        const long nl = static_cast<long>(std::count(body.begin(), body.end(), '\n'));
                        wire = d + "\x1b[" + std::to_string(nl + 1) + ";1H\x1b[0m" + floating;
                        diffed = true;
                    }
                }
            } else prev_lines.clear();
            if (!diffed) {
                if (main_len != std::string::npos && frame_str.compare(0, 3, "\x1b[H") != 0) {
                    const size_t h = frame_str.find("\x1b[H");   // "\x1b[2J\x1b[H": remember the lines for the next diff
                    std::string ignore;
                    if (h != std::string::npos && h + 3 <= main_len) diff_frame_lines(frame_str.substr(h + 3, main_len - h - 3), prev_lines, ignore);
                }
                wire = frame_str;
            }
            last_floating = floating;
            last_frame_str = std::move(frame_str);
            last_frame_written_at = frame_now;
            gfx_fill_holes(wire, sixel_on);
        }
        // The scope image goes after the text (Sixel replaces the cells; a Kitty image sits under the text and is
        // replaced in place). The text frame is often unchanged and then not sent at all, the picture still is.
        {
            ++gfx_tick;
            const int cap = gfx_picture_cap(gfx_proto_, settings_.image_scale);   // Kitty 60, Sixel 30 (full) / 60 (half, third)
            const unsigned long every = static_cast<unsigned long>(std::max(1, (settings_.osci().frame_rate + cap / 2) / cap));
            gfx_due_ = (gfx_tick % every) == 0;
            if (gfx_ok_ && gfx_.active && gfx_proto_ != GfxProto::None) {
                const bool visible = gfx_.crop < gfx_.cols;
                if (visible && (gfx_.fresh || !gfx_shown)) {
                    wire += gfx_emit(gfx_proto_, gfx_);
                    gfx_shown = true;
                    // the oscilloscope picture covers its cells: the caption box goes on top of it again
                    if (caption_rect_[2] > 0 && !caption_text_.empty())
                        wire += "\x1b[" + std::to_string(caption_rect_[0] + 1) + ";" + std::to_string(caption_rect_[1] + 1) + "H" + caption_text_ + "\x1b[0m";
                }
                else if (!visible && gfx_shown) { wire += gfx_clear(gfx_proto_); gfx_shown = false; }
            } else if (gfx_shown) {
                wire += gfx_clear(gfx_proto_);
                gfx_shown = false;
            }
            if (gfx_proto_ == GfxProto::None) gfx_shown = false;
            // the overlay's second picture (the XY box) goes with the first one
            if (gfx_ok_ && gfx_.active && gfx2_.active && gfx_proto_ != GfxProto::None) {
                if (gfx2_.fresh || !gfx2_shown_) { wire += gfx_emit(gfx_proto_, gfx2_); gfx2_shown_ = true; }
            } else if (gfx2_shown_) {
                wire += gfx_clear(gfx_proto_, 1);
                gfx2_shown_ = false;
                // Sixel has no way to delete a picture: the overlay's two pictures stay on screen until their cells are written
                // over. The main screen's line diff and the picture holes would leave them, so the next frame is a full one
                // and the main picture is sent again (it erases its own cells first).
                if (gfx_proto_ == GfxProto::Sixel) {
                    last_frame_str.clear();
                    prev_lines.clear();
                    gfx_shown = false;
                }
            }
        }
        if (sp_img) {
            int cw = 10, chh = 20;
            gfx_cell_pixels(settings_.cell_pixels, cw, chh);
            const bool floating = float_rect_[2] > 0 && float_rect_[3] > 0;
            const bool caption = !floating && caption_rect_[2] > 0;   // the "fetching lyrics" box: a hole in the picture
            wire += spectro_gfx_.emit(sp_proto, settings_.spectro, spectro(), spectro_area_[0], spectro_area_[1], spectro_area_[2], spectro_area_[3],
                                      cw, chh, true, floating ? float_rect_[1] : caption ? caption_rect_[0] : 0,
                                      floating ? float_rect_[0] : caption ? caption_rect_[1] : 0,
                                      floating ? float_rect_[3] : caption ? 1 : 0, floating ? float_rect_[2] : caption ? caption_rect_[2] : 0);
        } else if (spectro_gfx_.shown()) wire = spectro_gfx_.clear(sp_proto) + wire;   // before the new screen's text
        if (!wire.empty()) write_frame(wire);
        // 25fps (was 12.5fps) — the 700ms waveform reveal animation only
        // got ~9 frames to work with at the old 80ms cadence, which
        // showed as a handful of visible ~11% jumps rather than a smooth
        // continuous expansion. Also smooths disk rotation and the
        // visualizer's motion generally. Text-frame rendering is cheap
        // enough that doubling the rate here is not a meaningful CPU/
        // battery concern.
        std::this_thread::sleep_until(frame_start + std::chrono::microseconds(1000000 / std::max(1, settings_.osci().frame_rate)));
    }

    if (gfx_shown) { write_frame(gfx_clear(gfx_proto_)); gfx_shown = false; }   // the picture must not outlive the mode
    if (gfx2_shown_) { write_frame(gfx_clear(gfx_proto_, 1)); gfx2_shown_ = false; }
    if (spectro_gfx_.shown()) write_frame(spectro_gfx_.clear(gfx_proto_ == GfxProto::Kitty ? 1 : 2));
    if (switch_mode_) {
        // Switching to the radio: the player is only SUSPENDED (kept in memory, so coming back is instant and shows
        // the very same screen). Playback is paused, the terminal is handed over without a flash of the shell.
        if (has_track_ && !player_.is_paused()) player_.pause();
        terminal_hold_alt_screen();
        term.restore();
        save_settings(settings_);
        if (settings_.autosave_enabled) save_snapshot(build_snapshot());
        suspended_ = true;
        return kExitSwitchMode;
    }
    term.restore();
    shutdown();
    std::cout << "\nbye.\n";
    return 0;
}

// Everything that has to happen when the player really ends (quit, or quit from the radio while the player was
// suspended): close the play in the history, stop the audio, save, join the threads.
void App::shutdown() {
    if (suspended_) settings_.start_mode = read_start_mode();   // quit from the radio: its Start Mode row may have changed it
    suspended_ = false;
    history_end_current_play();
    history_.save();
    stop_device_worker();
    {
        std::lock_guard<std::mutex> lk(player_mutex_);
        player_.stop();
    }
    save_settings(settings_);
    if (settings_.autosave_enabled) {
        save_snapshot(build_snapshot());
        ConsoleLog::instance().log_basic("saved session snapshot on exit");
    }
    if (convert_overlay_open_) convert_overlay_close();
    converter_.cancel();
    if (load_thread_.joinable()) load_thread_.join();
    if (search_thread_.joinable()) search_thread_.join();
    if (device_worker_thread_.joinable()) device_worker_thread_.join();
    if (bulk_add_thread_.joinable()) bulk_add_thread_.join();
    meta_fetch_cancel_.request();   // quitting must not wait for a long batch; what had arrived is kept below
    if (meta_fetch_thread_.joinable()) meta_fetch_thread_.join();
    poll_pending_meta_fetch();
    meta_persist();
}

// ---- Scope window (SHIFT+9) -------------------------------------------------------------------------------------
void App::scope_window_toggle() {
    if (scope_window_running()) {
        scope_window_close();
        scope_win_on_ = false;
        status_line_ = "scope window closed";
        return;
    }
    std::string err;
    scope_win_on_ = true;
    scope_window_sync(false);   // the settings are there before the first sample
    if (!scope_window_open(&err)) {
        scope_win_on_ = false;
        status_line_ = err;
        return;
    }
    status_line_ = "scope window opened: F / double-click fullscreen, T always on top, ESC or " + hotkey_text("HKeyScopeWindow", ")") + " closes";
}

void App::scope_window_sync(bool check_alive) {
    if (!scope_win_on_ && check_alive && scope_window_running()) scope_win_on_ = true;   // opened in the radio
    if (!scope_win_on_) return;
    if (check_alive && !scope_window_running()) {   // closed from its side (its X, ESC, q) or failed
        scope_win_on_ = false;
        const std::string e = scope_window_take_error();
        status_line_ = e.empty() ? "scope window closed" : e;
        return;
    }
    // The style set in the oscilloscope overlay (SHIFT+o) decides what the window shows: the image style is a smooth pixel
    // picture (Decay, Glow, Z axis, rotation, colours ...), the braille style is the terminal's dot grid scaled up.
    const bool braille = settings_.osci_style == 0;
    const OsciSet& o = settings_.osci_set[braille ? 0 : 1];
    ScopeWinConfig c;
    c.braille = braille ? 1 : 0;
    c.mode = static_cast<uint8_t>(std::clamp(settings_.osci_mode, 0, 4));
    c.dot_threshold = o.dot_threshold;
    c.tail = o.tail;
    c.trace = o.trace;
    c.decay = o.decay;
    c.glow = o.glow;
    c.z_depth = o.z_depth;
    c.z = o.z ? 1 : 0;
    c.z_source = static_cast<uint8_t>(o.z_source == 1 ? 1 : 0);
    c.rotate = o.rotate ? 1 : 0;
    c.mono_phase = o.mono_phase ? 1 : 0;
    c.interp = o.interp ? 1 : 0;
    c.color_by_x = o.palette == 0 ? 1 : 0;
    c.music = o.music ? 1 : 0;
    for (int h = 0; h < 256; ++h) {
        int r = 255, g = 255, b = 255;
        osci_palette_rgb(settings_, o.palette, static_cast<float>(h) / 255.0f, r, g, b);
        c.pal[h][0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
        c.pal[h][1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
        c.pal[h][2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
    }
    std::string title;
    if (has_track_) { const TrackMetadata dm = display_metadata(); title = dm.artist.empty() ? dm.name : dm.artist + " - " + dm.name; }
    std::snprintf(c.title, sizeof c.title, "%s", title.c_str());
    scope_window_set_config(c);
}

// ---- Spectrogram window (SHIFT+8) --------------------------------------------------------------------------------
void App::spectro_window_toggle() {
    if (spectro_window_running()) {
        spectro_window_close();
        spectro_win_on_ = false;
        status_line_ = "spectrogram window closed";
        return;
    }
    std::string err;
    spectro_win_on_ = true;
    spectro_window_sync(false);   // the settings are there before the first sample
    if (!spectro_window_open(&err)) {
        spectro_win_on_ = false;
        status_line_ = err;
        return;
    }
    status_line_ = "spectrogram window opened: F / double-click fullscreen, T always on top, ESC or " + hotkey_text("HKeySpectroWindow", "(") + " closes";
}

void App::spectro_window_sync(bool check_alive) {
    if (!spectro_win_on_ && check_alive && spectro_window_running()) spectro_win_on_ = true;   // opened in the radio
    if (!spectro_win_on_) return;
    if (check_alive && !spectro_window_running()) {   // closed from its side (its X, ESC, q) or failed
        spectro_win_on_ = false;
        const std::string e = spectro_window_take_error();
        status_line_ = e.empty() ? "spectrogram window closed" : e;
        return;
    }
    // the SHIFT+i options and the colours of the scheme in use (the gradient one from the VIZ / OSCI colours)
    SpectroWinConfig c;
    spectro_win_pack(settings_.spectro, c);
    spectro_gradient_from(settings_);
    const auto& pal = spectro_palette(settings_.spectro.scheme);
    for (int h = 0; h < 256; ++h) for (int k = 0; k < 3; ++k) c.pal[h][k] = pal[static_cast<size_t>(h)][static_cast<size_t>(k)];
    std::string title;
    if (has_track_) { const TrackMetadata dm = display_metadata(); title = dm.artist.empty() ? dm.name : dm.artist + " - " + dm.name; }
    std::snprintf(c.title, sizeof c.title, "%s", title.c_str());
    spectro_window_set_config(c);
}

// The switches of the Viz/Lyrics field depend on each other. This brings them in line, so no combination is left
// contradicting itself (called after every edit of such a row with its id, and every frame without one):
//   * an explicit pick wins: choosing a view (Lyrics/Viz, No-Lyrics Viz, Lyrics Engine) that is switched off turns its
//     "Use ..." switch on;
//   * a switch that is off takes its dependents with it, instead of being turned on again: "Use Lyrics" off turns the Lyrics
//     Engine off; "Use Oscilloscope / Spectrogram / Sphere" off moves Lyrics/Viz and No-Lyrics Viz to the next visual that
//     is on (No-Lyrics Viz to "none" when there is none; Lyrics/Viz to the lyrics when they are all that is left).
// Returns a sentence about what was changed ("" = nothing), for the status line.
std::string App::enforce_visual_switches(int changed) {
    static const char* const names[4] = {"sphere", "oscilloscope", "spectrogram", "lyrics"};
    static const char* const use_rows[4] = {"Use Sphere", "Use Oscilloscope", "Use Spectrogram", "Use Lyrics"};
    Settings& s = settings_;
    std::string note;
    auto add = [&](const std::string& t) { note += (note.empty() ? "" : "; ") + t; };
    auto set_use = [&](int v, bool on) { (v == 0 ? s.use_sphere : v == 1 ? s.use_osci : v == 2 ? s.use_spectro : s.use_lyrics) = on; };
    // 1. explicit picks
    if (changed == kSidLyricViz && s.viz_default <= 3 && !s.viz_usable(s.viz_default)) { set_use(s.viz_default, true); add(std::string(use_rows[s.viz_default]) + " turned on"); }
    if (changed == kSidNoLyricsViz && s.nolyrics_viz <= 2 && !s.viz_usable(s.nolyrics_viz)) { set_use(s.nolyrics_viz, true); add(std::string(use_rows[s.nolyrics_viz]) + " turned on"); }
    // 2. what a switch that is off takes with it
    if (!s.use_lyrics && s.element_lyrics) { s.element_lyrics = false; add("the field no longer shows the lyrics"); }
    if (s.viz_default <= 3 && !s.viz_usable(s.viz_default)) {
        const int n = s.viz_step(s.viz_default, 1, false);
        if (n != s.viz_default) { s.viz_default = n; add(std::string("Lyrics/Viz Default -> ") + kVizNames[n]); }
    }
    if (s.nolyrics_viz <= 2 && !s.viz_usable(s.nolyrics_viz)) {
        const int n = s.viz_step(s.nolyrics_viz, 1, true);
        s.nolyrics_viz = n != s.nolyrics_viz ? n : 3;
        add(std::string("No-Lyrics Viz -> ") + (s.nolyrics_viz == 3 ? "none" : names[s.nolyrics_viz]));
    }
    if (s.lyric_viz <= 2 && !s.viz_usable(s.lyric_viz)) {
        const int n = s.viz_step(s.lyric_viz, 1, true);
        if (n != s.lyric_viz) { s.lyric_viz = n; add(std::string("Lyrics/Viz -> ") + names[n]); }
        else if (s.use_lyrics && !s.element_lyrics) { s.element_lyrics = true; add("Lyrics Engine turned on (no visual is left)"); }
    }
    return note;
}

void App::lyrics_view_nudge() {
    if (!has_track_ || !settings_.lyrics_on()) return;
    const std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
    last_lyrics_status_.clear();   // a fresh caption window, not a leftover from before the lyrics were off
    launch_lyrics_fetch(metadata_.name, artist, current_path_);
}

// ---- Spectrogram (SHIFT+i options, SHIFT+u full screen) ----------------------------------------------------------
void App::spectro_menu_adjust(int dir) {
    const int row = std::clamp(spectro_menu_row_, static_cast<int>(kSrStyle), kSpectroRowCount - 1);
    if (row == kSrDisplay) {   // what the lyrics area shows: sphere -> osci -> spectro
        settings_.lyric_viz = settings_.viz_step(settings_.lyric_viz, dir > 0 ? 1 : -1, true);
        if (settings_.element_lyrics && settings_.lyric_viz == 2) status_line_ = "the spectrogram shows when there are no lyrics (. switches)";
        return;
    }
    spectro_adjust(settings_.spectro, row, dir);
}

std::vector<std::string> App::build_spectro_menu_panel() const {
    const int W = kSpectroMenuPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    std::vector<std::string> lines;
    lines.push_back(box_top("Spectrogram", W, border));
    for (int r = kSrStyle; r < kSpectroRowCount; ++r) {
        const bool sel = r == spectro_menu_row_;
        std::string val = spectro_row_value(settings_.spectro, r);
        if (r == kSrStyle && settings_.spectro.style == 1 && gfx_proto_ == GfxProto::None) val = "image (no Sixel/Kitty here)";
        lines.push_back(row(std::string(sel ? "> " : "  ") + pad_right(spectro_row_label(r), 22) + pad_left(val, inner - 24), sel));
    }
    lines.push_back(row("", false));
    lines.push_back(row("[UP/DOWN] select  [LEFT/RIGHT] change", false));
    lines.push_back(row("[R] Audacity defaults  [" + hotkey_text("HKeySpectroFull", "U") + "] full screen", false));
    lines.push_back(box_bottom(W, "[" + hotkey_text("HKeySpectroMenu", "I") + " / ESC] close", border_bottom));
    return lines;
}

// The full-screen spectrogram: a frame with the track as its title, the frequency labels on the left (like
// Audacity's ruler), a time ruler on top and the picture (or braille) filling the rest.
std::string App::build_spectro_full(int W, const char* clear_prefix) const {
    spectro_used_ = true;
    spectro_gradient_from(settings_);
    SpectroAnalyzer& a = spectro();
    a.update(settings_.spectro);
    const SpectroSettings& sp = settings_.spectro;
    const int H = std::max(6, term_rows_ - 1);
    const int label_w = 6;
    const int cols = std::max(4, W - 2 - label_w - 1);
    const int rows = std::max(2, H - 3);
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string legend = ansi_for(settings_.legend_color, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const bool image = sp.style == 1 && gfx_proto_ != GfxProto::None;
    int cw = 10, chh = 20;
    if (image) gfx_cell_pixels(settings_.cell_pixels, cw, chh);
    const int unit = image ? std::max(1, chh) : 4;
    const auto labels = spectro_axis(sp, a.rate(), rows * unit, unit);
    std::vector<std::string> lab(static_cast<size_t>(rows));
    for (const auto& l : labels) if (l.first >= 0 && l.first < rows) lab[static_cast<size_t>(l.first)] = l.second;
    std::string title = "SPECTROGRAM";
    if (has_track_) { const TrackMetadata dm = display_metadata(); title += " - " + (dm.artist.empty() || dm.artist == "-" ? dm.name : dm.artist + " - " + dm.name); }
    std::ostringstream f;
    f << clear_prefix << "\x1b[?25l";
    f << box_top(truncate_str(title, W - 6), W, border) << "\n";
    {   // time ruler: seconds of the page (sweep) or seconds before now (scroll)
        std::string ruler(static_cast<size_t>(cols), ' ');
        const int step = sp.span <= 5 ? 1 : sp.span <= 10 ? 2 : sp.span <= 30 ? 5 : 10;
        for (int t = 0; t <= sp.span; t += step) {
            const int x = static_cast<int>(static_cast<long long>(t) * (cols - 1) / std::max(1, sp.span));
            const std::string txt = sp.motion == 1 ? (t == sp.span ? "now" : "-" + std::to_string(sp.span - t) + "s") : std::to_string(t) + "s";
            const int x0 = std::clamp(x - (t == 0 ? 0 : t == sp.span ? static_cast<int>(txt.size()) - 1 : static_cast<int>(txt.size()) / 2), 0, std::max(0, cols - static_cast<int>(txt.size())));
            for (size_t k = 0; k < txt.size() && x0 + static_cast<int>(k) < cols; ++k) ruler[static_cast<size_t>(x0) + k] = txt[k];
        }
        f << bar << legend << std::string(static_cast<size_t>(label_w + 1), ' ') << ruler << R << bar << "\n";
    }
    std::vector<std::string> body;
    if (image) {
        spectro_area_[0] = 2;
        spectro_area_[1] = 1 + label_w + 1;
        spectro_area_[2] = cols;
        spectro_area_[3] = rows;
    } else body = a.braille(sp, cols, rows);
    for (int r = 0; r < rows; ++r) {
        const std::string l = sp.labels ? lab[static_cast<size_t>(r)] : std::string();
        f << bar << legend << pad_left(l, label_w) << R << " ";
        if (image) f << gfx_hole(cols);
        else f << (r < static_cast<int>(body.size()) ? body[static_cast<size_t>(r)] : std::string(static_cast<size_t>(cols), ' '));
        f << bar << "\n";
    }
    f << box_bottom(W, "[" + hotkey_text("HKeySpectroMenu", "I") + "] options  [" + hotkey_text("HKeySpectroFull", "U") + " / ESC] close  " + spectro_row_value(sp, kSrScale) + " " +
                       std::to_string(sp.min_freq) + "-" + std::to_string(std::min(sp.max_freq, a.rate() / 2)) + " Hz", border_bottom) << "\n";
    return clamp_output_rows(f.str(), term_rows_);
}

// ---- Oscilloscope overlay (SHIFT+0) --------------------------------------------------------------------------------
// Keys of the overlay itself. While the grid colour is being typed every key belongs to the prompt.
bool App::scope_big_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (scope_big_color_edit_) {
        if (arrow) return true;
        if (key == 27) { scope_big_color_edit_ = false; status_line_ = ""; }
        else if (key == '\r' || key == '\n') {
            scope_big_color_edit_ = false;
            if (scope_big_color_buf_.empty()) settings_.scope_grid_color.clear();   // empty = the colour of inactive list elements
            else settings_.scope_grid_color = std::to_string(std::clamp(std::atoi(scope_big_color_buf_.c_str()), 0, 255));
            save_settings(settings_);
        } else if ((key == 127 || key == 8) && !scope_big_color_buf_.empty()) scope_big_color_buf_.pop_back();
        else if (key >= '0' && key <= '9' && scope_big_color_buf_.size() < 3) scope_big_color_buf_.push_back(static_cast<char>(key));
        return true;
    }
    if (scope_metrics_open_) {   // the metrics window: UP / DOWN pick a row, LEFT / RIGHT change it, R resets, ESC / SHIFT+p close
        if (arrow) {
            if (key == 'A') scope_metrics_row_ = (scope_metrics_row_ + kScopeMetricRows - 1) % kScopeMetricRows;
            else if (key == 'B') scope_metrics_row_ = (scope_metrics_row_ + 1) % kScopeMetricRows;
            else if (key == 'C' || key == 'D') { scope_metric_step(settings_.scope_metrics, scope_metrics_row_, key == 'C' ? 1 : -1); save_settings(settings_); }
        } else if (key == 27 || key == 'P') scope_metrics_open_ = false;
        else if (key == 'r' || key == 'R') { settings_.scope_metrics = ScopeMetrics(); save_settings(settings_); }
        return true;
    }
    if (arrow) return false;
    if (key == 27) { scope_big_ = false; force_redraw_ = true; return true; }
    if (key == 'P') { scope_metrics_open_ = true; return true; }                                               // SHIFT+p: grid metrics
    if (key == 'A') { settings_.scope_grid = !settings_.scope_grid; save_settings(settings_); return true; }   // SHIFT+a: axes / grid
    if (key == 'V') { settings_.scope_values = !settings_.scope_values; save_settings(settings_); return true; }   // SHIFT+v: axis values
    if (key == 'C') {                                                                                          // SHIFT+c: grid colour
        scope_big_color_edit_ = true;
        scope_big_color_buf_.clear();
        return true;
    }
    return false;
}

std::string App::build_scope_big(int W, const char* clear_prefix) const {
    const int H = std::max(6, term_rows_ - 1);
    const int inner = W - 2;
    const int avail = std::max(3, H - 2);   // between the top and the bottom border
    const bool grid = settings_.scope_grid, values = settings_.scope_values;
    const ScopeBigGeom g = scope_big_geometry(inner, avail, values);
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string legend = ansi_for(settings_.legend_color, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    // both pictures are drawn from the same audio, each in its own phosphor buffer, with the values of the style in use
    // (braille characters, or -- Osci style "image" on a terminal with Kitty graphics / Sixel -- real pixel pictures)
    scope_big_xy_.copy_ring_from(scope_);
    scope_big_yt_.copy_ring_from(scope_);
    const bool image = settings_.osci_style == 1 && gfx_proto_ != GfxProto::None && g.cols > 0 && g.rows > 0;
    OscilloscopeVisualizer::Params px = osci_scope_params(settings_, viz_dt_, image ? 1 : 0);
    px.mode = 0;
    OscilloscopeVisualizer::Params py = px;
    py.mode = 1;
    const OsciSet& os = settings_.osci_set[image ? 1 : 0];
    const int pal = os.palette;
    auto color_of = [&](int tx, int cols, uint8_t hue, float k) {
        if (pal == 0) {
            const float t = cols > 1 ? static_cast<float>(tx) / static_cast<float>(cols - 1) : 0.0f;
            return osci_dim_ansi(gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t), k);
        }
        int r = 255, gg = 255, b = 255;
        osci_palette_rgb(settings_, pal, hue / 255.0f, r, gg, b);
        return osci_rgb_seq(r, gg, b, k);
    };
    // the grid and the axis values take the colour of inactive list elements unless one was typed in
    const std::string grid_ansi = ansi_for(settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color, false);
    const ScopeMetrics& sm = settings_.scope_metrics;
    std::vector<std::string> left, right;
    if (image) {
        // The terminal draws the two pictures itself: the boxes are placeholder cells here (render_frame() hands the pictures to the
        // main loop through gfx_ / gfx2_), the grid is part of each picture.
        gfx_cell_pixels(settings_.cell_pixels, cell_w_, cell_h_);
        const int cw0 = std::max(4, cell_w_), ch0 = std::max(8, cell_h_);
        const int cwq = std::clamp(640 / g.cols, 4, cw0);
        const int scale = std::clamp(settings_.image_scale, 1, 3);   // Osci image resolution: 1/scale of the screen resolution
        const int cws = std::max(2, std::min(cwq, cw0 / scale));
        const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
        int gr = 128, gg2 = 128, gb = 128;
        try_parse_rgb(settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color, gr, gg2, gb);
        for (int k = 0; k < 2; ++k) {
            GfxFrame& F = k == 0 ? gfx_ : gfx2_;
            OscilloscopeVisualizer& sc = k == 0 ? scope_big_yt_ : scope_big_xy_;
            const OscilloscopeVisualizer::Params& pp = k == 0 ? py : px;
            F.id = k; F.scale = scale; F.out_cw = cw0; F.out_ch = ch0; F.crop = 0; F.cut_w = 0;
            F.cols = g.cols; F.rows = g.rows; F.w = g.cols * cws; F.h = g.rows * chs;
            F.row = 1 + g.top + 1;   // below the top border and the picture names
            F.col = 1 + g.margin + g.lab + (k == 0 ? 0 : g.cols + g.gap + g.lab);
            sc.render_image(F.w, F.h, pp, F.level, F.hue);
            if (pal == 0) {
                for (int y = 0; y < F.h; ++y)
                    for (int x = 0; x < F.w; ++x) F.hue[static_cast<size_t>(y) * F.w + x] = static_cast<uint8_t>(x * 255 / std::max(1, F.w - 1));
            }
            for (int h2 = 0; h2 < 256; ++h2) {
                int r = 255, g3 = 255, b = 255;
                osci_palette_rgb(settings_, pal, static_cast<float>(h2) / 255.0f, r, g3, b);
                F.pal[static_cast<size_t>(h2)] = {static_cast<uint8_t>(r), static_cast<uint8_t>(g3), static_cast<uint8_t>(b)};
            }
            if (grid) F.grid = k == 0 ? scope_grid_pixels(F.w, F.h, g.cols, g.rows, 1, sm.yt_time_div, sm.yt_value_div)
                                      : scope_grid_pixels(F.w, F.h, g.cols, g.rows, 0, sm.xy_div, sm.xy_div);
            else F.grid.clear();
            F.grid_rgb = {static_cast<uint8_t>(gr), static_cast<uint8_t>(gg2), static_cast<uint8_t>(gb)};
            F.active = true;
            F.fresh = gfx_due_;
        }
        left.assign(static_cast<size_t>(g.rows), gfx_hole(g.cols));
        right = left;
    } else {
        left = scope_big_pane(scope_big_yt_, py, g.cols, g.rows, grid, 1, grid_ansi, color_of, sm.yt_time_div, sm.yt_value_div);
        right = scope_big_pane(scope_big_xy_, px, g.cols, g.rows, grid, 0, grid_ansi, color_of, sm.xy_div, sm.xy_div);
    }

    // what the axis values and the picture names show: the time span, the scales that 100 % stands for, the measurements
    ScopeBigInfo info;
    info.m = sm;
    info.rotate = os.rotate;
    info.samples = std::clamp(os.trace, 128, 1024);
    info.ms = info.samples * 1000.0 / std::max(8000, player_.sample_rate());
    info.music = px.fixed_gain;
    {
        const OscilloscopeVisualizer::Measure me = scope_big_yt_.measure(px);
        info.rms_db = me.rms_db;
        info.peak_db = me.peak_db;
        info.corr = me.corr;
        info.bal_db = me.bal_db;
    }
    const std::vector<std::string> body = scope_big_compose(g, inner, avail, left, right, legend, grid_ansi, info);
    std::string title = "OSCILLOSCOPE";
    if (has_track_) { const TrackMetadata dm = display_metadata(); title += " - " + (dm.artist.empty() || dm.artist == "-" ? dm.name : dm.artist + " - " + dm.name); }
    std::ostringstream f;
    f << clear_prefix << "\x1b[?25l";
    f << box_top(truncate_str(title, W - 6), W, border) << "\n";
    for (const std::string& line : body) f << bar << line << bar << "\n";
    const std::string foot_head = "[" + hotkey_text("HKeyOscMenu", "O") + "] options  [" + hotkey_text("HKeyScopeBig", "=") + "/ESC] close  ";
    const std::string foot = scope_pick_head({foot_head + "[SHIFT+a] grid  [SHIFT+v] values  [SHIFT+c] color  [SHIFT+p] metrics",
                                              foot_head + "[SHIFT+a/v/c/p] grid / values / color / metrics"}, W - 6);
    f << box_bottom(W, truncate_str(foot, W - 6), border_bottom) << "\n";
    return clamp_output_rows(f.str(), term_rows_);
}

// The window of the grid metrics (SHIFT+p in the oscilloscope overlay): what the axis values are measured in, the divisions of the
// grid and the measurement each picture's name shows -- separately for the YT and the XY picture.
std::vector<std::string> App::build_scope_metrics_panel() const {
    const int W = kScopeMetricsPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string legend = ansi_for(settings_.legend_color, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi, bool head) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : head ? legend + body + R : body) + " " + bar;
    };
    std::vector<std::string> lines;
    lines.push_back(box_top("Grid metrics", W, border));
    for (const auto& r : scope_metric_panel_rows(settings_.scope_metrics, scope_metrics_row_, inner)) {
        const bool head = !r.second && r.first.size() > 1 && r.first[0] != ' ' && r.first[0] != '>';
        lines.push_back(row(r.first, r.second, head));
    }
    lines.push_back(row("Readouts are averaged and update twice a second.", false, false));
    lines.push_back(row("[UP/DOWN] select  [LEFT/RIGHT] change", false, false));
    lines.push_back(box_bottom(W, "[R] reset  [SHIFT+p / ESC] close", border_bottom));
    return lines;
}

// The small window for typing the grid colour (SHIFT+c in the oscilloscope overlay): the digits typed so far, a swatch of the colour.
std::vector<std::string> App::build_scope_color_panel() const {
    const int W = kScopeColorPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain) { return bar + " " + pad_right(truncate_str(plain, inner), inner) + " " + bar; };
    const std::string shown = scope_big_color_buf_.empty() ? (settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color)
                                                           : std::to_string(std::clamp(std::atoi(scope_big_color_buf_.c_str()), 0, 255));
    std::vector<std::string> lines;
    lines.push_back(box_top("Grid color", W, border));
    lines.push_back(row("Color (0-255): " + scope_big_color_buf_ + "█"));
    const std::string label = "Preview: ";
    const int blocks = 8;
    lines.push_back(bar + " " + label + ansi_for(shown, false) + [&] { std::string b; for (int i = 0; i < blocks; ++i) b += "█"; return b; }() + R +
                    std::string(static_cast<size_t>(std::max(0, inner - static_cast<int>(label.size()) - blocks)), ' ') + " " + bar);
    lines.push_back(row("Empty = color of inactive list elements"));
    lines.push_back(box_bottom(W, "[ENTER] ok  [ESC] cancel", border_bottom));
    return lines;
}

} // namespace muisc
