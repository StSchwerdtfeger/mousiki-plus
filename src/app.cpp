#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "book_art.h"
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
#include "creator.h"
#include "mic_capture.h"
#include "process_util.h"
#include "app_internal.h"
#include "update_check.h"
#include "open_url.h"
#include "app_settings_tables.h"
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
#include <unordered_map>
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
// ASCII-only, deliberately: this folds track titles and artist names, which
// are UTF-8. std::tolower over raw bytes mangles multi-byte sequences under
// any single-byte locale -- see ascii_lower() in utf8_util.h.

// Removes exactly one full UTF-8 codepoint from the end of a text-entry
// buffer, not just its last byte. A plain pop_back() left a dangling lead
// byte behind for any accented/non-ASCII character (a German umlaut is
// two bytes; CJK is three) -- that lone byte decodes as a replacement
// glyph, and needed a second, confusing Backspace press to actually
// finish clearing what looked like one character.


bool contains_ci(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
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

} // namespace

// SGR for search bar input (the "/" box, the meta editor's and playlist
// manager's "Search:" rows): the Colors tab's SEARCH row, where an empty
// cell follows LIST INACTIVE FG/BG -- the colours these bars always had.
std::string search_sgr(const Settings& s) { // the typed query (behind its text only)
    const std::string& fg = s.search_text_color.empty() ? s.list_color : s.search_text_color;
    const std::string& bg = s.search_bg_color.empty() ? s.list_inactive_bg_color : s.search_bg_color;
    return ansi_for(fg, false) + bg_ansi_for(bg);
}
std::string search_idle_sgr(const Settings& s) { // everything else in the bar
    // Text follows LIST INACTIVE FG, but the background is NOT inherited: an
    // unset INACTIVE BG leaves the bar plain, so only the query is highlighted.
    const std::string& fg = s.search_inactive_text_color.empty() ? s.list_color : s.search_inactive_text_color;
    return ansi_for(fg, false) + bg_ansi_for(s.search_inactive_bg_color);
}
// `text` in the query colours, then back to the idle ones; nothing when empty
// (an input that was not performed is not highlighted).
std::string search_run(const Settings& s, const std::string& text, bool nonempty) {
    // The reset matters: an idle colour without a BG would otherwise leave the query's BG running.
    return nonempty ? search_sgr(s) + text + "\x1b[0m" + search_idle_sgr(s) : text;
}

// SGR prefix of the pane focus indicator "◀" (Colors tab FOCUS row, ColorFocus=): bold plus focus_color; an unset
// color degrades to plain bold like the header does.
std::string focus_sgr(const Settings& s) {
    const std::string params = sgr_params_for(s.focus_color);
    return params.empty() ? "\x1b[1m" : "\x1b[1;" + params + "m";
}

// The small "◀" that marks which pane/field within a panel has the keyboard focus. box_top() wraps the whole title
// string in `border_ansi` (one colour for the whole row), so to make just the triangle read in the focus colour, the
// title text itself switches colour for it and then back to `border_ansi` -- the same colour box_top() already has --
// so the dashes that follow the title stay in the border colour.
// The way back to the border colour is a marker (\001) that box_top() resolves: it puts the border colour straight in front of
// the first dash after the title, where the smooth border gradient (clamp_output_rows) finds it and gives it the colour of that
// ROW -- a plain colour escape followed by the blank before the dashes would stay the top colour.
std::string focus_indicator(const Settings& s, const std::string&) {
    return " " + focus_sgr(s) + "◀" + "\x1b[0m" + "\001";
}

// The text of `s` without its CSI colour sequences (what is really drawn).
std::string strip_csi(const std::string& s) {
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
    for (size_t at = label.find('\001'); at != std::string::npos; at = label.find('\001')) { label.erase(at, 1); back_to_border = true; }
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


} // namespace (render_lyric_line_wrapped is shared with app_console_karaoke.cpp: declared in app_internal.h)

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

namespace {

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

std::filesystem::path locate_script(const std::string& filename) { return find_scripts_file(filename); }

App::App(Settings* shared, bool audiobook) : audiobook_(audiobook), settings_(shared ? *shared : own_settings_) {
    if (!shared) settings_ = load_settings();
    // Still the colours of a built-in theme as an earlier version had them (with terminal-dependent colours 1-15 and empty
    // text colours)? Then the theme's fixed colours, which look the same in every terminal (see themes::renewed_builtins()).
    for (const auto& [was, now] : themes::renewed_builtins()) {
        if (!themes::file_holds(config_path(), was, false)) continue;
        theme_apply(settings_, now);
        save_settings(settings_);
        break;
    }
    // Point yt-dlp at the configured folder (if any) before anything can
    // ask where a download would go. Empty config = default cache folder,
    // which is what set_download_dir() restores on its own.
    cache_.set_download_dir(path_from_utf8(settings_.download_folder));
    if (audiobook_) history_.set_file_name("reading_history.json");   // the reading history is a file of its own
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

    if (!audiobook_) {   // (the audio book player only looks at the AUDIO BOOK PATH: library_roots())
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

    }
    all_local_tracks_ = local_source_.scan(library_roots(), &local_scan_diagnostics_);
    exclude_book_files(all_local_tracks_);   // the audio books are not part of the music library
    scanned_book_path_ = settings_.audiobook_path;
    local_view_ = all_local_tracks_;
    if (audiobook_) book_refresh_view();   // the left pane lists the books
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
std::string combo_key_name(int key) {
    for (const auto& c : kComboKeys) if (c.code == key) return c.name;
    return "";
}

// What the user typed into a key field, cleaned up: ALT/OPTION spelled either way, any case for the names ("enter",
// "ctrl+l", "up"). Single characters are left exactly as typed (a capital letter means SHIFT+letter).
std::string normalize_hotkey_string(std::string s) {
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
        case 3: return "recently added";
        default: return "folder order";
    }
}

long long App::added_time_of(const fs::path& p) const {
    const std::string key = path_utf8(p);
    std::lock_guard<std::mutex> lk(added_time_mutex_);
    auto it = added_time_cache_.find(key);
    if (it == added_time_cache_.end()) it = added_time_cache_.emplace(key, file_added_time(p)).first;
    return it->second;
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
    else if (local_sort_mode_ == 3) {
        // Newest first: the time a file arrived (file_added_time(): later of modification and creation / change time, so
        // a file that was copied or downloaded into the library counts from then). Looked up once per file and kept
        // until the next rescan, because this runs again every time background tags re-filter the list.
        std::vector<std::pair<long long, size_t>> keyed;
        keyed.reserve(tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i) keyed.emplace_back(added_time_of(tracks[i].path), i);
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<LocalTrack> sorted;
        sorted.reserve(tracks.size());
        for (const auto& k : keyed) sorted.push_back(std::move(tracks[k.second]));
        tracks = std::move(sorted);
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
    if (audiobook_) { book_refresh_view(); return; }   // (the audio book player has no track list, only the books)
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


// Re-runs the local library scan over whatever settings_.local_music_paths
// holds right now, then rebuilds the view on top of the result. This is
// what makes a path edited in the PATHS tab take effect immediately --
// config.txt used to promise "the library is scanned once at startup,
// there's no live rescan", and editing a local path from Settings now IS
// a live rescan (it still needs [S]/quit for the change to be written to
// config.txt, of course).
// After a rescan the saved playlists are brought up to date as well, not only
// when they are saved again: tracks whose file moved are pointed at the new
// path (and the playlist file is rewritten), and the cached tags of every
// playlist track are dropped so they are read from the files again.
void App::refresh_playlists_after_rescan() {
    int rewritten = 0;
    std::vector<std::string> keys;
    for (const fs::path& dir : playlist_dirs()) {
        for (const auto& summary : PlaylistManager::list(dir)) {
            auto pl = PlaylistManager::load(dir, summary.name);
            if (!pl) continue;
            Playlist moved = *pl;
            relocate_missing_tracks(moved);
            bool changed = false;
            for (size_t i = 0; i < moved.tracks.size(); ++i) {
                if (moved.tracks[i].path != pl->tracks[i].path) changed = true;
                keys.push_back(path_utf8(moved.tracks[i].path));
            }
            std::string err;
            if (changed && PlaylistManager::save(dir, moved, &err)) ++rewritten;
        }
    }
    {
        // Titles already in the queue: files moved since they were queued are found again,
        // and their tags are read anew (the queue shows the artist from the tags).
        Playlist q;
        std::vector<size_t> at;
        for (size_t i = 0; i < queue_.size(); ++i) {
            if (!queue_[i].is_local) continue;
            PlaylistTrack t;
            t.path = queue_[i].local_path;
            std::error_code ec;
            t.missing = !fs::exists(t.path, ec);
            q.tracks.push_back(std::move(t));
            at.push_back(i);
        }
        relocate_missing_tracks(q);
        for (size_t k = 0; k < at.size(); ++k) {
            QueueItem& it = queue_[at[k]];
            if (q.tracks[k].path != it.local_path) {
                if (has_track_ && current_path_ == it.local_path) current_path_ = q.tracks[k].path;
                it.local_path = q.tracks[k].path;
                it.title = path_utf8(it.local_path.stem());
                it.artist = path_utf8(it.local_path.parent_path().filename());
            }
            keys.push_back(path_utf8(it.local_path));
        }
    }
    if (!playlist_edit_tracks_.empty()) {
        Playlist open;
        open.tracks = playlist_edit_tracks_;
        relocate_missing_tracks(open);
        playlist_edit_tracks_ = open.tracks;   // not marked as changed: the saved file was already updated above
        for (const auto& t : playlist_edit_tracks_) keys.push_back(path_utf8(t.path));
    }
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        for (const auto& k : keys) row_meta_cache_.erase(k);
    }
    search_idx_.clear();
    resolve_playlist_tracks_async();
    if (rewritten > 0) log_event("playlists updated after the rescan: " + std::to_string(rewritten));
}

// The folders the library scan walks: the local music paths (with the download / cache folders), or in the audio book
// player only the AUDIO BOOK PATH.
std::vector<std::string> App::library_roots() const {
    if (audiobook_) return settings_.audiobook_path.empty() ? std::vector<std::string>{} : std::vector<std::string>{settings_.audiobook_path};
    return settings_.local_music_paths;
}

void App::rescan_library() {
    { std::lock_guard<std::mutex> lk(added_time_mutex_); added_time_cache_.clear(); }   // "recently added" sort: files may have arrived
    // Keep the streamed-track cache folder in the list even if the edit
    // happened to drop it, for the same reason the constructor injects it
    // at all: downloaded songs belong in the local view.
    std::string cache_dir_str = path_utf8(cache_.cache_dir());
    if (!audiobook_ && std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), cache_dir_str)
        == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(cache_dir_str);
    }
    // And the configured download folder, for the same reason: whatever
    // yt-dlp writes has to be scannable without a second LocalMusicPath
    // line, which is what the DOWNLOAD PATH setting promises. Skipped
    // while it is still the default -- that IS the cache dir just above.
    std::string download_dir_str = path_utf8(cache_.download_dir());
    if (!audiobook_ && download_dir_str != cache_dir_str
        && std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), download_dir_str)
               == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(download_dir_str);
    }

    // A local diagnostics vector, not local_scan_diagnostics_ -- that one
    // is read back once at startup by run() (before any rescan can happen)
    // and never again, so per-scan results go straight to the log instead.
    std::vector<std::string> diagnostics;
    all_local_tracks_ = local_source_.scan(library_roots(), &diagnostics);
    exclude_book_files(all_local_tracks_);
    scanned_book_path_ = settings_.audiobook_path;
    search_idx_.clear();
    refresh_local_view(); // applies the active query/sort/filter to the new set
    for (const auto& line : diagnostics) ConsoleLog::instance().log_basic(line);

    if (!audiobook_) refresh_playlists_after_rescan();

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

    if (audiobook_) {   // the audio book player only searches its books (the folders): plain words, an "f:" in front is allowed
        std::string q = buf;
        if (q.size() >= 2 && lower(q.substr(0, 2)) == "f:") q = q.substr(2);
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        list_source_ = ListSource::Folder;
        folder_view_ = filter_folders(q);
        selected_ = 0;
        scroll_ = 0;
        return;
    }
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

    if (audiobook_) {   // search the books (see update_live_search_preview())
        std::string q = buf;
        if (q.size() >= 2 && lower(q.substr(0, 2)) == "f:") q = q.substr(2);
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        last_folder_query_ = q;
        list_source_ = ListSource::Folder;
        folder_view_ = filter_folders(q);
        selected_ = 0;
        scroll_ = 0;
        return;
    }
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
        queue_focus_ = false;   // the saved playlists pane takes the focus, also when the queue had it before
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
    if (audiobook_) { book_open(selected_); return; }
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
        if (auto pl = PlaylistManager::load(dir, name)) {
            relocate_missing_tracks(*pl);
            return pl;
        }
    }
    return std::nullopt;
}

// A playlist stores paths. When a file is gone from its stored path (moved to
// another folder, or a folder renamed), look for the same file name in the
// library (every folder added in the settings) and use that path. One match
// is taken; with several, the one whose parent folder name matches wins, else
// the first. The playlist file itself is rewritten only when it is saved.
void App::relocate_missing_tracks(Playlist& pl) const {
    bool any = false;
    for (const auto& t : pl.tracks) if (t.missing) { any = true; break; }
    if (!any) return;
    std::unordered_multimap<std::string, const LocalTrack*> by_name;
    for (const auto& lt : all_local_tracks_) by_name.emplace(ascii_lower_str(path_utf8(lt.path.filename())), &lt);
    for (auto& t : pl.tracks) {
        if (!t.missing) continue;
        auto range = by_name.equal_range(ascii_lower_str(path_utf8(t.path.filename())));
        const LocalTrack* pick = nullptr;
        const std::string old_dir = ascii_lower_str(path_utf8(t.path.parent_path().filename()));
        for (auto it = range.first; it != range.second; ++it) {
            if (!pick) pick = it->second;
            if (ascii_lower_str(path_utf8(it->second->path.parent_path().filename())) == old_dir) { pick = it->second; break; }
        }
        if (!pick) continue;
        t.path = pick->path;
        t.title = path_utf8(t.path.stem());
        t.artist = path_utf8(t.path.parent_path().filename());
        t.missing = false;
    }
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
                                    native_sample_rate(decode_from, pl.metadata.sampling, duration), want_stereo ? 2 : 1,
                                    static_cast<double>(settings_.track_buffer_mb) * 1024.0 * 1024.0);
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
                auto abandoned = [watch, raw]() { return raw->cancelled.load() || watch.use_count() <= raw->internal_refs.load(); };
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
    const double track_secs = static_cast<double>(total_sec_);
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
    if (settings_.lyrics_on() && !audiobook_) {   // (no lyrics for audio books)
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

void App::play_next_from_queue(bool random) {
    // The queue plays in its order, whatever the play mode: next (n) and the end of a track take its first item. Only the
    // shuffle key (#, `random`) picks a random item of it.
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
        if (random && queue_.size() > 1) {
            static std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, static_cast<int>(queue_.size()) - 1);
            idx = dist(rng);
        }
    }
    play_queue_item(idx);
}

// Starts the queue item at `idx` (the head for the next key, a random one for shuffle, the highlighted one for ENTER in the queue pane;
// with `move_along` (SHIFT+ENTER) the items in front of it move to the end of the queue first).
void App::play_queue_item(int idx, bool move_along) {
    if (idx < 0 || idx >= static_cast<int>(queue_.size())) return;
    const bool queue_once = settings_.play_mode == 4 && queue_locked_;
    if (move_along && idx > 0) {   // SHIFT+ENTER: the items in front of it are passed over -- they go to the end in their order (as if they had been played)
        std::rotate(queue_.begin(), queue_.begin() + idx, queue_.end());
        for (size_t i = queue_.size() - static_cast<size_t>(idx); i < queue_.size(); ++i) queue_[i].played = queue_once;
        idx = 0;
        queue_selected_ = 0;
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

    if (audiobook_) { book_advance(); return; }   // the next chapter (or the end of the book)

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
    status_line_ = queue_playlist(playlist_view_[selected_].name);
}

std::string App::queue_playlist(const std::string& name) {
    auto pl = load_playlist(name);
    if (!pl) return "could not load \"" + name + "\"";
    int added = 0, skipped = 0;
    for (auto& t : pl->tracks) {
        if (t.missing) { ++skipped; continue; }
        queue_.push_back({true, t.title, t.artist, t.path, "", false, name});
        ++added;
    }
    clamp_queue_selected();
    queue_reveal(static_cast<int>(queue_.size()) - 1);
    return "queued " + std::to_string(added) + " track" + (added == 1 ? "" : "s") + " from \"" + name + "\""
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
std::vector<PlaylistTrack> App::queue_local_tracks(int& skipped) const {
    std::vector<PlaylistTrack> tracks;
    skipped = 0;
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
    return tracks;
}

// The small overlay first (Mode::QueueToPlaylist); the work is queue_to_playlist_apply().
void App::queue_to_playlist() {
    if (queue_.empty()) { status_line_ = "the queue is empty -- nothing to save as a playlist"; return; }
    int skipped = 0;
    if (queue_local_tracks(skipped).empty()) { status_line_ = "no local tracks in the queue (playlists hold local files only)"; return; }
    qtp_track_ = false;
    qtp_stage_ = 0;
    qtp_choice_ = 0;
    qtp_query_.clear();
    qtp_selected_ = 0;
    mode_ = Mode::QueueToPlaylist;
    force_redraw_ = true;
}

// ---------------------------------------------------------------------
// SHIFT+a: the highlighted library track -> a playlist (saved at once)
// ---------------------------------------------------------------------

void App::track_to_playlist() {
    if (queue_focus_) { status_line_ = "focus the library pane first (TAB), then press this key on a track"; return; }
    if (list_source_ == ListSource::Playlist) { status_line_ = "a playlist can't be added to a playlist -- pick a track"; return; }
    if (list_source_ == ListSource::Folder) { status_line_ = "a folder can't be added to a playlist -- open it and pick a track"; return; }
    const size_t len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (len == 0 || selected_ < 0 || selected_ >= static_cast<int>(len)) { status_line_ = "no track highlighted"; return; }
    if (atp_busy_.load()) { status_line_ = "still downloading the last track for a playlist -- wait for it"; return; }
    AtpTrack t;
    if (list_source_ == ListSource::Local) {
        const auto& r = local_view_[static_cast<size_t>(selected_)];
        t.title = r.title; t.artist = r.folder_artist; t.path = r.path; t.is_local = true;
    } else {
        const auto& r = online_view_[static_cast<size_t>(selected_)];
        t.title = r.title; t.artist = r.uploader; t.video_id = r.video_id; t.is_local = false;
    }
    atp_track_ = std::move(t);
    qtp_track_ = true;
    qtp_stage_ = 0;
    qtp_choice_ = 0;
    qtp_query_.clear();
    qtp_new_name_.clear();
    qtp_selected_ = 0;
    mode_ = Mode::QueueToPlaylist;
    force_redraw_ = true;
}

// Adds one local file to a playlist (a new one when it does not exist yet) and saves it; no editor.
void App::atp_add_file(const std::string& playlist, const std::string& title, const std::string& artist, const fs::path& file) {
    Playlist pl;
    bool existed = false;
    if (auto loaded = PlaylistManager::load(playlists_dir(), playlist)) { pl = std::move(*loaded); existed = true; }
    else pl.name = playlist;
    const std::string p = path_utf8(file);
    for (const auto& t : pl.tracks) {
        if (t.path == p) { status_line_ = "\"" + title + "\" is already in \"" + playlist + "\""; return; }
    }
    PlaylistTrack pt;
    pt.title = title; pt.artist = artist; pt.path = p; pt.missing = false;
    pl.tracks.push_back(std::move(pt));
    std::string err;
    if (!PlaylistManager::save(playlists_dir(), pl, &err)) { status_line_ = "could not save \"" + playlist + "\": " + err; return; }
    status_line_ = "added \"" + title + "\" to " + std::string(existed ? "" : "the new playlist ") + "\"" + playlist + "\" (" + std::to_string(pl.tracks.size()) + " tracks, saved)";
    if (list_source_ == ListSource::Playlist) playlist_view_ = filter_playlists(last_playlist_query_);   // a shown /p: list gets the new count
}

void App::track_to_playlist_apply(const std::string& playlist) {
    mode_ = Mode::Browse;
    qtp_track_ = false;
    if (atp_track_.is_local) { atp_add_file(playlist, atp_track_.title, atp_track_.artist, atp_track_.path); return; }
    // An online track: download it into the download folder in the background, then add the file it became.
    status_line_ = "downloading \"" + atp_track_.title + "\" for \"" + playlist + "\" ...";
    atp_busy_ = true;
    AtpTrack t = atp_track_;
    std::thread([this, t, playlist]() { run_guarded("playlist download", [&] {
        AtpJob job;
        job.playlist = playlist; job.title = t.title; job.artist = t.artist;
        std::string err;
        auto got = youtube_.resolve_by_id(t.video_id, t.title, t.artist, &err);
        if (!got) {
            job.error = "download failed: " + err;
        } else {
            std::error_code ec;
            const fs::path dest_dir = cache_.download_dir();
            fs::create_directories(dest_dir, ec);
            std::string safe_name = got->title.empty() ? t.title : got->title;
            std::string safe_artist = got->artist.empty() ? t.artist : got->artist;
            for (char& c : safe_name) if (c == '/' || c == '\\') c = '_';
            for (char& c : safe_artist) if (c == '/' || c == '\\') c = '_';
            const std::string stem = safe_artist.empty() ? safe_name : safe_name + " - " + safe_artist;
            const fs::path dest = dest_dir / path_from_utf8(stem + path_utf8(got->cached_path.extension()));
            if (fs::exists(dest, ec)) {
                job.ok = true; job.file = dest;           // downloaded before: use that file
            } else {
                fs::copy_file(got->cached_path, dest, fs::copy_options::overwrite_existing, ec);
                if (ec) job.error = "could not save the download: " + ec.message();
                else { job.ok = true; job.file = dest; }
            }
            // the cache copy goes (the file lives in the download folder now) unless it is the track being played
            if (job.ok) {
                std::error_code ec2;
                if (!(has_track_ && current_path_ == got->cached_path)) fs::remove(got->cached_path, ec2);
            }
        }
        std::lock_guard<std::mutex> lk(atp_mutex_);
        atp_done_ = std::move(job);
        atp_ready_ = true;
    }); }).detach();
}

void App::poll_pending_atp() {
    if (!atp_ready_.load()) return;
    AtpJob job;
    {
        std::lock_guard<std::mutex> lk(atp_mutex_);
        if (!atp_ready_.load()) return;
        job = std::move(atp_done_);
        atp_done_ = AtpJob{};
        atp_ready_ = false;
    }
    atp_busy_ = false;
    if (!job.ok) { status_line_ = job.error.empty() ? "download failed" : job.error; return; }
    refresh_local_view();   // the new file is part of the library now
    atp_add_file(job.playlist, job.title, job.artist, job.file);
}

void App::qtp_refresh() {
    qtp_view_ = filter_playlists(qtp_query_);   // empty query = all of them
    qtp_selected_ = std::clamp(qtp_selected_, 0, std::max(0, static_cast<int>(qtp_view_.size()) - 1));
}

void App::queue_to_playlist_apply(const std::string& existing) {
    int skipped = 0;
    std::vector<PlaylistTrack> tracks = queue_local_tracks(skipped);
    if (tracks.empty()) { status_line_ = "no local tracks in the queue (playlists hold local files only)"; return; }
    const int n = static_cast<int>(tracks.size());
    const std::string skipped_note = skipped > 0 ? " (" + std::to_string(skipped) + " online skipped)" : std::string();
    playlist_open_editor();           // fresh editor, tab 0
    if (existing.empty()) {
        playlist_edit_tracks_ = std::move(tracks);
        resolve_playlist_tracks_async();
        playlist_edit_track_selected_ = 0;
        playlist_edit_focus_ = 0;         // the name field
        playlist_edit_dirty_ = true;
        playlist_status_ = "queue -> playlist: " + std::to_string(n) + " track" + (n == 1 ? "" : "s") + skipped_note
                         + " -- type a name, Ctrl+S saves";
        return;
    }
    playlist_load_into_editor(existing);
    if (playlist_edit_name_ != existing) return;   // could not be loaded: the status line says so
    const int first_new = static_cast<int>(playlist_edit_tracks_.size());
    int added = 0;
    for (auto& t : tracks) {
        bool dup = false;
        for (const auto& e : playlist_edit_tracks_) if (e.path == t.path) { dup = true; break; }
        if (dup) continue;
        playlist_edit_tracks_.push_back(std::move(t));
        ++added;
    }
    playlist_edit_track_selected_ = std::min(first_new, std::max(0, static_cast<int>(playlist_edit_tracks_.size()) - 1));
    playlist_edit_focus_ = 2;          // the track list, on the first added track
    playlist_edit_dirty_ = added > 0;
    playlist_status_ = "queue -> \"" + existing + "\": " + std::to_string(added) + " track" + (added == 1 ? "" : "s") + " added"
                     + (n - added > 0 ? ", " + std::to_string(n - added) + " already in it" : std::string()) + skipped_note
                     + (added > 0 ? " -- Ctrl+S saves" : "");
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
    // (a snapshot written by an older version may hold audio book chapters: the audio book player has its own place, in the .cue files)
    auto is_book_file = [&](const std::string& path) {
        std::vector<LocalTrack> one{LocalTrack{path_utf8(path_from_utf8(path).stem()), path_from_utf8(path), std::string()}};
        exclude_book_files(one);
        return one.empty();
    };
    for (const auto& t : snap.queue) {
        if (t.is_local && is_book_file(t.path)) continue;
        queue_.push_back({t.is_local, t.title, t.artist, t.is_local ? path_from_utf8(t.path) : fs::path(), t.video_id, false, t.playlist});
    }
    clamp_queue_selected();

    if (snap.has_now_playing) {
        resume_start_sec_ = std::max(0.0, snap.position_sec);
        if (snap.now_playing.is_local) {
            fs::path p = path_from_utf8(snap.now_playing.path);
            std::error_code ec;
            if (is_book_file(snap.now_playing.path)) {
                resume_start_sec_ = 0.0;   // an audio book chapter does not belong to the music player
            } else if (fs::exists(p, ec)) {
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
    if (!settings_.autosave_enabled || audiobook_) return;   // (the audio book player must not overwrite the music player's session: it keeps its place in the .cue files)
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
void App::bulk_add_open() {
    mode_ = Mode::BulkAdd;
    bulk_add_buffer_.clear();
    bulk_add_results_ready_ = false;
    pending_bulk_add_ = BulkAddResult{};
    bulk_add_selected_.clear();
    bulk_add_cursor_ = 0;
    bulk_add_scroll_ = 0;
    edit_owner_.clear();   // a fresh field: the caret starts at its end
    status_line_.clear();
}

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
std::string pretty_key(const std::string& k) {
    if (k == "@SWITCHKEY") return mode_switch_key_label();   // the mode-switch key, named for this keyboard (keyboard_layout.h)
    if (k == "@SWITCHBACK") return mode_switch_back_label();   // ... and the one that goes back to the previous mode
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
std::vector<std::string> wrap_words(const std::string& text, int width) {
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

// What the radio may have changed in config.txt while the player was suspended: Start Mode, Black Background, Share Profiles and,
// with Share Profiles on, the oscilloscope / spectrogram settings (the radio writes its edits of them there).
// SHIFT and + (dir = 1) / SHIFT and - (dir = -1): leave for the next / previous mode that is switched on.
bool App::request_mode_switch(int dir) {
    const int me = audiobook_ ? 2 : 0;
    if (settings_.next_mode(me, dir) == me) {
        status_line_ = "this is the only mode switched on (Settings -> ON/OFF -> MODES)";
        return false;
    }
    g_mode_switch_dir = dir;
    if (audiobook_ && book_ask_due()) {   // leaving the audio book: the question about a bookmark first
        book_ask_then(dir > 0 ? "Switching to the next mode" : "Switching to the previous mode", [this]() { switch_mode_ = true; quit_ = true; });
        return true;
    }
    switch_mode_ = true;
    quit_ = true;
    return true;
}

void App::sync_after_radio() {
    settings_.start_mode = read_start_mode();
    {   // the mode switches and their order (the radio's settings change the same lines)
        const ModeConfig mc = read_mode_config();
        for (int i = 0; i < 3; ++i) { settings_.use_mode[i] = mc.use[i]; settings_.mode_order[i] = mc.order[i]; }
    }
    {   // the AUDIO BOOK PATH (the radio's PATHS tab can change it): a new folder changes what the library lists
        settings_.audiobook_path = read_audiobook_path();
        if (settings_.audiobook_path != scanned_book_path_) rescan_library();   // also: the other mode changed it while this one waited
    }
    settings_.black_background = read_black_background();
    settings_.share_profiles = read_share_profiles();
    if (settings_.share_profiles && !audiobook_) { VizPrefs v; if (read_player_viz(v)) settings_.take_shared_from(v); }   // the audio book mode keeps its own
    const RemoteAppConfig rc = read_remote_app();
    settings_.remote_app = rc.enabled;
    settings_.remote_port = rc.port;
    settings_.remote_pin = rc.pin;
}

void App::remote_app_apply() {
    remote::net::configure({settings_.remote_app, settings_.remote_port, settings_.remote_pin});
}

// The REMOTE APP overlay, centred on the Settings screen (absolute positions, like the screen itself).
void App::remote_overlay_draw(std::ostringstream& frame, int W) const {
    remote_ui::Style st;
    st.border = ansi_for(settings_.border_color, false);
    st.border_bottom = ansi_for(settings_.border_color_bottom, false);
    st.header = ansi_for(settings_.header_color, true);
    st.legend = ansi_for(settings_.legend_color, false);
    st.text = ansi_for(settings_.list_color, false);
    st.value = st.text;
    st.change = ansi_for(settings_.change_text_color, false) + bg_ansi_for(settings_.change_bg_color);
    st.cursor = "\x1b[7m";
    st.ul = settings_.box_upper_left; st.ur = settings_.box_upper_right;
    st.ll = settings_.box_lower_left; st.lr = settings_.box_lower_right;
    st.h = settings_.box_horizontal;  st.v = settings_.box_vertical;
    if (settings_.border_smooth)
        st.border_at = [this](float t) { return gradient_ansi(settings_.border_color, settings_.border_color_bottom, t, false); };
    const remote_ui::Values v{settings_.remote_app, settings_.remote_port, settings_.remote_pin};
    const int rows = std::max(10, term_rows_ - 1);
    int w = 0;
    const std::vector<std::string> lines = remote_ui::build(remote_ov_, v, st, W - 2, rows, w);
    const int x = std::max(1, (W - w) / 2 + 1), y = std::max(1, (rows - static_cast<int>(lines.size())) / 2 + 1);
    for (size_t i = 0; i < lines.size(); ++i) frame << "\x1b[" << y + static_cast<int>(i) << ";" << x << "H" << lines[i];
    frame << "\x1b[0m";
}

void App::act_cycle_play_mode() {
    // list -> repeat -> shuffle -> stop -> queue then stop -> list
    settings_.play_mode = (settings_.play_mode + 1) % 5;
    queue_reset_lap();
    static const char* mode_names[] = {"list", "repeat", "shuffle", "stop", "queue then stop"};
    log_event(std::string("play mode: ") + mode_names[settings_.play_mode]);
}

void App::act_wizard_toggle() {
    if (mode_ == Mode::Wizard) wizard_close();
    else wizard_open();
}

// ---- actions shared by the hotkeys and the remote control ------------------------------------------------------------------
void App::act_play_pause() {
    if (has_track_) { if (player_.is_paused()) player_.resume(); else player_.pause(); }
}

void App::act_volume_change(int delta) {
    if (has_track_) player_.set_volume(std::clamp(player_.volume() + delta, 0, 100));
}

void App::act_mute(int how) {   // force volume to 0 without touching the pause state
    const bool want = how < 0 ? !muted_ : how > 0;
    if (want == muted_) return;
    if (want) {
        pre_mute_volume_ = player_.volume();
        player_.set_volume(0);
        muted_ = true;
        log_event("muted");
    } else {
        player_.set_volume(pre_mute_volume_);
        muted_ = false;
        log_event("unmuted");
    }
}

void App::act_next() {
    if (audiobook_) { book_step(1); return; }
    // The queue (if any) takes priority, same as auto-advance-on-finish does: its next title in order (only # picks a random
    // one), Queue-then-stop respected via play_next_from_queue() -- a manual skip still always actually skips, though: Repeat/Stop/Queue-then-stop only govern
    // *automatic* advance, not an explicit "next" press.
    if (!queue_.empty()) play_next_from_queue();
    else play_relative(1);
}

void App::act_prev() {
    if (audiobook_) { book_step(-1); return; }
    // Relative to what's actually playing (see current_track_list_index()), not the hover cursor. No queue equivalent: a FIFO
    // queue has no well-defined "previous" once an item's been consumed.
    play_relative(-1);
}

// Commands from `mousiki --cmd ...` and the status file for `mousiki --status`. Called every frame.
void App::remote_poll() {
    const char* const mode_name = audiobook_ ? "audiobook" : "player";   // (the audio book is its own mode for the remote app)
    remote::tick(mode_name);
    {   // "a newer version is out": once; the popup waits until the main screen is showing
        update_check::Notice n;
        if (!update_pending_ && update_check::take(n)) {
            update_latest_ = n.latest; update_current_ = n.current; update_url_ = update_check::release_url(n);
            update_pending_ = true;
        }
        if (update_pending_ && mode_ == Mode::Browse && !meta_save_.active) {
            update_pending_ = false;
            mode_ = Mode::UpdateNotice;
            force_redraw_ = true;
        }
    }
    for (const remote::Command& c : remote::poll()) {
        const std::string& n = c.name;
        if (n == "play-pause") act_play_pause();
        else if (n == "play") { if (has_track_ && player_.is_paused()) player_.resume(); }
        else if (n == "pause" || n == "stop") { if (has_track_ && !player_.is_paused()) player_.pause(); }
        else if (n == "next") { if (audiobook_) book_step(1, true); else act_next(); }
        else if (n == "prev") { if (audiobook_) book_step(-1, true); else act_prev(); }
        else if (n == "shuffle") { if (audiobook_) status_line_ = "no shuffle in the audio book mode"; else if (!queue_.empty()) play_next_from_queue(true); else play_relative_random(); }
        else if (n == "volume-up") act_volume_change(static_cast<int>(remote::number(c.arg, 5)));
        else if (n == "volume-down") act_volume_change(-static_cast<int>(remote::number(c.arg, 5)));
        else if (n == "volume") {
            if (has_track_) {
                const int v = static_cast<int>(remote::number(c.arg, player_.volume()));
                player_.set_volume(std::clamp((!c.arg.empty() && (c.arg[0] == '+' || c.arg[0] == '-')) ? player_.volume() + v : v, 0, 100));
            }
        }
        else if (n == "mute") act_mute(1);
        else if (n == "unmute") act_mute(0);
        else if (n == "toggle-mute") act_mute(-1);
        else if (n == "seek") { if (has_track_) player_.seek_relative(remote::number(c.arg, 0.0)); }
        else if (n == "playback-mode") act_cycle_play_mode();
        else if (n == "wizard") act_wizard_toggle();
        else if (n == "remote-view") {
            phone_view_ = remote::number(c.arg, 0) > 0.5 ? 1 : 0;
            phone_note_.clear();
            phone_clear_armed_ = {};
            if (phone_view_ == 1 && audiobook_) phone_book_reveal();   // the library tree opens at the book that is open
            else if (phone_view_ == 1) {   // the local list opens on the page of the playing title
                int cols = 44, rows = 44;
                remote::net::screen_wanted(cols, rows);
                int per = 0, per2 = 0;
                remote::net::phone_lists_rows(rows, per, per2);
                int at = -1;
                if (has_track_ && current_is_local_)
                    for (size_t i = 0; i < local_view_.size(); ++i) if (local_view_[i].path == current_path_) { at = static_cast<int>(i); break; }
                phone_page_ = at >= 0 ? at / per : 0;
                phone_page2_ = 0;
                phone_focus_ = 0;
            }
        }
        else if (n == "remote-focus") { phone_focus_ = remote::number(c.arg, 0) > 0.5 ? 1 : 0; }
        else if (n == "remote-scroll") {   // a whole page of the pane in focus
            int cols = 44, rows = 44;
            remote::net::screen_wanted(cols, rows);
            int per = 0, per2 = 0;
            remote::net::phone_lists_rows(rows, per, per2);
            const int step = static_cast<int>(remote::number(c.arg, 0));
            if (audiobook_) {   // one pane: the whole library tree
                const int pages = std::max(1, (static_cast<int>(phone_book_rows().size()) + per + per2 - 2) / std::max(1, per + per2 - 1));
                phone_page_ = std::clamp(phone_page_ + step, 0, pages - 1);
            } else if (phone_focus_ == 0) {
                const int pages = std::max(1, (static_cast<int>(local_view_.size()) + per - 1) / per);
                phone_page_ = std::clamp(phone_page_ + step, 0, pages - 1);
            } else {
                const int pages = std::max(1, (static_cast<int>(playlist_summaries().size()) + per2 - 1) / per2);
                phone_page2_ = std::clamp(phone_page2_ + step, 0, pages - 1);
            }
        }
        else if (n == "book-open" || (n == "track" && audiobook_)) {   // the phone's library tree (1 = its first row)
            const std::vector<FoRow> rows = phone_book_rows();
            const int k = static_cast<int>(remote::number(c.arg, 0)) - 1;
            if (k >= 0 && k < static_cast<int>(rows.size())) {
                const FoRow& r = rows[static_cast<size_t>(k)];
                if (r.is_dir) {   // a folder opens / closes
                    if (!phone_book_open_.erase(r.path)) phone_book_open_.insert(r.path);
                } else if (book_open_dir(path_utf8(path_from_utf8(r.path).parent_path()))) {   // a chapter plays from its start (no questions)
                    for (size_t i = 0; i < chapters_.size(); ++i)
                        if (path_utf8(chapters_[i].path) == r.path) {
                            book_automark_tick(true);
                            book_prompt_paused_ = false;
                            book_play_chapter(static_cast<int>(i));
                            phone_note_ = "playing " + path_utf8(path_from_utf8(r.path).stem());
                            break;
                        }
                } else phone_note_ = "that book is no longer in the library";
            }
        }
        else if (n == "track") {   // the phone's local list: play that title (1 = the first title of the list)
            const int k = static_cast<int>(remote::number(c.arg, 0)) - 1;
            if (k >= 0 && k < static_cast<int>(local_view_.size())) {
                const LocalTrack t = local_view_[static_cast<size_t>(k)];
                if (list_source_ == ListSource::Local) {   // the main list follows
                    selected_ = k;
                    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
                    if (selected_ < scroll_) scroll_ = selected_;
                }
                start_local_track(t);
                phone_note_ = "playing " + t.title;
            }
        }
        else if (n == "clear-queue") {   // the phone taps twice (arg 0 arms, 1 clears); the command line clears at once
            if (queue_.empty()) phone_note_ = "the queue is empty";
            else if (c.arg == "0") {
                phone_clear_armed_ = std::chrono::steady_clock::now();
                phone_note_ = "tap again to clear " + std::to_string(queue_.size()) + (queue_.size() == 1 ? " title" : " titles");
            } else {
                const size_t was = queue_.size();
                queue_clear();
                phone_clear_armed_ = {};
                phone_note_ = "queue cleared (" + std::to_string(was) + (was == 1 ? " title)" : " titles)");
                status_line_ = phone_note_;
            }
        }
        else if (n == "playlist") {   // the phone's PLAYLISTS list (1 = the first playlist in A-Z order)
            const std::vector<PlaylistSummary> pls = playlist_summaries();
            const int k = static_cast<int>(remote::number(c.arg, 0)) - 1;
            if (k >= 0 && k < static_cast<int>(pls.size())) {
                const bool idle = !has_track_;
                phone_note_ = queue_playlist(pls[static_cast<size_t>(k)].name);
                status_line_ = phone_note_;
                if (idle && !queue_.empty()) { play_next_from_queue(); phone_note_ += " -- playing"; }
            }
        }
        else if (n == "switch-mode") request_mode_switch(remote::number(c.arg, 1) < 0 ? -1 : 1);   // (-1: the previous mode)
        else if (n == "quit") quit_ = true;
        // "live" belongs to the radio: ignored here
        force_redraw_ = true;
    }
    phone_publish();   // the remote app's screen, when a phone asks for it
    const TrackMetadata md = has_track_ ? display_metadata() : TrackMetadata{};
    char pos[32], dur[32];
    std::snprintf(pos, sizeof pos, "%.1f", has_track_ ? player_.poll_elapsed() : 0.0);
    std::snprintf(dur, sizeof dur, "%zu", has_track_ ? total_sec_ : static_cast<size_t>(0));
    remote::publish_status({{"mode", mode_name},
                            {"state", !has_track_ ? "stopped" : player_.is_paused() ? "paused" : muted_ ? "muted" : "playing"},
                            {"title", has_track_ ? md.name : std::string()},
                            {"artist", has_track_ && md.artist != "-" ? md.artist : std::string()},
                            {"album", has_track_ && md.album != "-" ? md.album : std::string()},
                            {"position_sec", pos}, {"duration_sec", dur},
                            {"volume", std::to_string(player_.volume())}, {"muted", muted_ ? "yes" : "no"},
                            {"playback_mode", play_mode_name(settings_.play_mode)}});
}

namespace { std::string fo_name_of(const std::string& p); }   // (defined with the folder overlay below)

// The phone's audio book LISTS: the library overlay's tree (folders, books, chapters), walked with phone_book_open_.
std::vector<App::FoRow> App::phone_book_rows() const {
    std::vector<FoRow> out;
    std::function<void(const std::string&, int)> emit = [&](const std::string& d, int depth) {
        FoRow r;
        r.path = d;
        r.label = fo_name_of(d);
        r.is_dir = true;
        r.depth = depth;
        r.expanded = phone_book_open_.count(d) > 0;
        const auto c = fo_count_.find(d);
        r.count = c == fo_count_.end() ? 0 : c->second;
        out.push_back(r);
        if (!r.expanded) return;
        const auto sd = fo_subdirs_.find(d);
        if (sd != fo_subdirs_.end()) for (const auto& x : sd->second) emit(x, depth + 1);
        const auto f = fo_files_.find(d);
        if (f != fo_files_.end())
            for (const auto& file : f->second) {
                FoRow fr;
                fr.path = file;
                fr.label = path_utf8(path_from_utf8(file).stem());
                fr.depth = depth + 1;
                out.push_back(fr);
            }
    };
    for (const auto& r : fo_roots_) emit(r, 0);
    return out;
}

// LISTS was opened: the tree is built again (the library may have been scanned meanwhile) and the way down to the open book
// is opened, the page is the one that shows it.
void App::phone_book_reveal() {
    folder_overlay_build_tree();
    phone_page_ = 0;
    phone_note_.clear();
    if (book_dir_.empty()) return;
    phone_book_open_.insert(book_dir_);
    std::set<std::string> roots(fo_roots_.begin(), fo_roots_.end());
    std::string k = path_utf8(path_from_utf8(book_dir_).parent_path());
    while (!k.empty()) {
        if (fo_subdirs_.count(k)) phone_book_open_.insert(k);
        if (roots.count(k)) break;
        const std::string par = path_utf8(path_from_utf8(k).parent_path());
        if (par.empty() || par == k) break;
        k = par;
    }
    int cols = 44, rows = 44;
    remote::net::screen_wanted(cols, rows);
    int per = 0, per2 = 0;
    remote::net::phone_lists_rows(rows, per, per2);
    const int page_rows = std::max(1, per + per2 - 1);
    const std::vector<FoRow> v = phone_book_rows();
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].path == book_dir_) { phone_page_ = static_cast<int>(i) / page_rows; break; }
}

void App::handle_key(int key) {
    if (key == 0) return;

    // Audio book: the note editor owns every key while it is open (the audio goes on).
    if (audiobook_ && book_note_edit_ && mode_ == Mode::Browse) {
        if (key == kKeyCtrlN && !book_note_discard_ask_) { book_note_to_overlay(); return; }   // CTRL+n: on in the big notes overlay
        book_note_key(key);
        return;
    }

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

    if (mode_ == Mode::UpdateNotice) {
        // The new-version popup: [o] opens the release page (and closes it), ESC closes it; other keys do nothing, so a
        // key meant for the player is not lost silently behind it (it is small and one key away from gone).
        const bool arrow = last_key_was_arrow();
        if (!arrow && (key == 'o' || key == 'O')) {
            if (!open_url(update_url_)) { status_line_ = "could not open the browser: " + update_url_; }
            mode_ = Mode::Browse;
        } else if (!arrow && key == 27) {
            mode_ = Mode::Browse;
        }
        force_redraw_ = true;
        return;
    }

    if (mode_ == Mode::QueueToPlaylist) {
        const bool arrow = last_key_was_arrow();
        force_redraw_ = true;
        if (qtp_stage_ == 0) {   // New playlist | Existing playlist
            if (!arrow && key == 27) { mode_ = Mode::Browse; qtp_track_ = false; return; }
            if ((arrow && (key == 'C' || key == 'D')) || (!arrow && key == 9)) { qtp_choice_ ^= 1; return; }
            if (!arrow && (key == 'n' || key == 'N')) { qtp_choice_ = 0; key = '\r'; }
            else if (!arrow && (key == 'e' || key == 'E')) { qtp_choice_ = 1; key = '\r'; }
            if (!arrow && (key == '\r' || key == '\n')) {
                if (qtp_choice_ == 0 && qtp_track_) { qtp_stage_ = 2; qtp_new_name_.clear(); edit_owner_.clear(); }
                else if (qtp_choice_ == 0) { mode_ = Mode::Browse; queue_to_playlist_apply(""); }
                else { qtp_stage_ = 1; qtp_query_.clear(); qtp_selected_ = 0; edit_owner_.clear(); qtp_refresh(); }
            }
            return;
        }
        if (qtp_stage_ == 2) {   // SHIFT+a, new playlist: its name
            if (!arrow && key == 27) { qtp_stage_ = 0; return; }
            if (!arrow && (key == '\r' || key == '\n')) {
                std::string name = qtp_new_name_;
                while (!name.empty() && name.front() == ' ') name.erase(name.begin());
                while (!name.empty() && name.back() == ' ') name.pop_back();
                if (name.empty()) { status_line_ = "type a name first"; return; }
                track_to_playlist_apply(name);
                return;
            }
            std::string st;
            edit_focus("track-to-playlist-name", qtp_new_name_);
            edit_text_key(qtp_new_name_, edit_caret_, edit_anchor_, key, 80, &st);
            return;
        }
        // stage 1: the search field (typing filters) and the list of saved playlists
        if (!arrow && key == 27) { qtp_stage_ = 0; return; }
        const int total = static_cast<int>(qtp_view_.size());
        if (arrow && key == 'A') { if (qtp_selected_ > 0) --qtp_selected_; return; }
        if (arrow && key == 'B') { if (qtp_selected_ < total - 1) ++qtp_selected_; return; }
        if (!arrow && (key == '\r' || key == '\n')) {
            if (total > 0) {
                const std::string name = qtp_view_[static_cast<size_t>(std::clamp(qtp_selected_, 0, total - 1))].name;
                if (qtp_track_) track_to_playlist_apply(name);
                else { mode_ = Mode::Browse; queue_to_playlist_apply(name); }
            }
            return;
        }
        std::string st;
        edit_focus("queue-to-playlist-search", qtp_query_);
        if (edit_text_key(qtp_query_, edit_caret_, edit_anchor_, key, 80, &st)) qtp_refresh();
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
        // the oscilloscope's overlays take over without closing this one first
        if (!arrow && act == "HKeyOscMenu") { save_settings(settings_); spectro_full_ = false; osci_menu_row_ = 0; mode_ = Mode::OsciMenu; force_redraw_ = true; return; }
        if (!arrow && act == "HKeyScopeBig") {
            save_settings(settings_);
            scope_big_ = true; spectro_full_ = false; scope_big_color_edit_ = false; scope_metrics_open_ = false;
            osci_menu_row_ = 0; mode_ = Mode::OsciMenu; force_redraw_ = true; return;
        }
        if (!arrow && (key == 27 || act == "HKeySpectroMenu")) {
            mode_ = Mode::Browse;
            save_settings(settings_);
        }
        return;
    }

    if (creator_show_ && mode_ == Mode::Browse) { creator_preview_key(key); return; }   // the creator's preview: ESC stops it

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
        // the spectrogram's overlays take over without closing this one first
        if (!arrow && osc_action == "HKeySpectroMenu") { save_settings(settings_); scope_big_ = false; spectro_menu_row_ = kSrStyle; mode_ = Mode::SpectroMenu; force_redraw_ = true; return; }
        if (!arrow && osc_action == "HKeySpectroFull") {
            save_settings(settings_);
            spectro_full_ = true; scope_big_ = false;
            spectro_menu_row_ = kSrStyle; mode_ = Mode::SpectroMenu; force_redraw_ = true; return;
        }
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

    if (mode_ == Mode::BookOverlay) { book_overlay_key(key); return; }
    if (mode_ == Mode::Creator) { creator_key(key); return; }
    if (mode_ == Mode::MenuIndex) { menu_index_key(key); return; }

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
        const bool arrow = last_key_was_arrow();
        if (cheat_searching_) {   // SHIFT+s: the search field has the keyboard
            const int n = static_cast<int>(cheat_result_rows_.size());
            if (!arrow && key == 27) {   // ESC clears the query first, then leaves the search
                if (!cheat_query_.empty()) { cheat_query_.clear(); cheat_sel_ = 0; cheat_first_ = 0; edit_owner_.clear(); }
                else cheat_searching_ = false;
            } else if (arrow && (key == 'A' || key == 'B')) {
                const int step = last_key_was_shifted() ? 5 : 1;
                cheat_sel_ = std::clamp(cheat_sel_ + (key == 'B' ? step : -step), 0, std::max(0, n - 1));
            } else if (!arrow && (key == '\r' || key == '\n')) {
                if (n > 0) cheat_jump_row_ = cheat_result_rows_[static_cast<size_t>(std::clamp(cheat_sel_, 0, n - 1))];
                cheat_searching_ = false;
            } else {
                edit_focus("cheat-search", cheat_query_);
                if (edit_text_key(cheat_query_, edit_caret_, edit_anchor_, key, 80, nullptr)) { cheat_sel_ = 0; cheat_first_ = 0; }
            }
            return;
        }
        if (!arrow && key == 'S') { cheat_searching_ = true; cheat_query_.clear(); cheat_sel_ = 0; cheat_first_ = 0; edit_owner_.clear(); }
        else if (key == 27 || key_is("HKeyCheatsheet", key)) mode_ = Mode::Browse;
        else if (key == 'A') --cheatsheet_scroll_; // up -- the table is longer than the screen
        else if (key == 'B') ++cheatsheet_scroll_; // down (clamped while rendering)
        return;
    }

    if (mode_ == Mode::Wizard) {
        // The wizard keeps its own page / part / contents state (src/wizard.cpp); here the key only has to be named.
        // Nothing reaches the player underneath: the wizard is a full-screen view like the cheat sheet, and playback
        // simply keeps running.
        const bool arrow = last_key_was_arrow();
        if (wizard_st_.searching) { wizard::search_key(wizard_st_, key, arrow, arrow && last_key_was_shifted()); return; }   // SHIFT+s: the query has the keyboard
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
        else if (key == 'S') wk = wizard::Key::Search;                          // SHIFT+s: search the whole text
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
        // The shared single-line editor: caret, SHIFT+arrows marking, HOME / END, DEL, CTRL+C / X / V. It also tells
        // a typed A-D from an arrow key (arrows collapse to those letters).
        edit_focus("bulk-add", bulk_add_buffer_);
        edit_text_key(bulk_add_buffer_, edit_caret_, edit_anchor_, key, 200, &status_line_);
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
        if (key == 9 || (key == 'B' && last_key_was_arrow())) { // Tab / down -- next field (a typed capital B goes into the text)
            idx = (idx + 1) % static_cast<int>(fields.size());
            rl_focus_ = fields[idx];
            if (rl_focus_ == RLField::Source) { rl_src_sel_ = 0; rl_fix_scroll(); }
            return;
        }
        if (key == 'A' && last_key_was_arrow()) { // up -- previous field
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
            // Caret, SHIFT+arrows marking, HOME / END, DEL, CTRL+C / X / V as in every other text box.
            edit_focus(rl_owner(rl_focus_), *t);
            edit_text_key(*t, edit_caret_, edit_anchor_, key, 200, &status_line_);
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
    if ((key == '*' || key == '_') && !last_key_was_arrow()) { request_mode_switch(key == '*' ? 1 : -1); return; }
    int lookup_key = (key == '\r') ? '\n' : key;
    std::string action = resolve_hotkey_action(lookup_key);
    if (action.empty() && lookup_key >= 'a' && lookup_key <= 'z') action = resolve_hotkey_action(lookup_key - 32);
    if (action.empty() && lookup_key >= 'A' && lookup_key <= 'Z') action = resolve_hotkey_action(lookup_key + 32);
    if (audiobook_ && book_intercept(action, key)) return;   // the audio book player's own keys (and what it does not offer)

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
    } else if (action == "HKeyBulkAdd") {           // paste a playlist link, from any pane
        bulk_add_open();
    } else if (action == "HKeyAddToPlaylist") {     // the highlighted library track -> a playlist, saved at once
        track_to_playlist();
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
    } else if (action == "HKeyCreator") { // ^: creator overlay
        creator_status_.clear();
        creator_list_open_ = false;
        if (fs::is_directory(creator_dir()) && creator_dir_prepare()) rescan_library();   // files made before the folder was marked leave the library
        creator_load();
        mode_ = Mode::Creator;
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
        act_play_pause();
    } else if (action == "HKeyIncreaseVolume") {
        act_volume_change(+5);
    } else if (action == "HKeyDecreaseVolume") {
        act_volume_change(-5);
    } else if (action == "HKeyPlayNextSong") {
        act_next();
    } else if (action == "HKeyPlayPreviousSong") {
        act_prev();
    } else if (action == "HKeyShuffleNext") {
        // Shuffle to a random next track in the current list -- a
        // manual one-off jump, independent of Play Mode
        // (settings_.play_mode). Reuses the same play_relative_random()
        // the automatic Shuffle play mode already calls on auto-advance.
        // With a queue: a random title of the queue instead (it is what plays next anyway).
        if (!queue_.empty()) play_next_from_queue(true);
        else play_relative_random();
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
        // Add hovering song to queue. Only for the library pane: with the Queue pane itself focused there is
        // nothing hovering in the list to add, so the key does nothing there (bulk add has its own key).
        if (!queue_focus_) {
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
        act_cycle_play_mode();   // one key for all five play modes
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
        act_mute(-1);
    } else if (action == "HKeyWizard") {
        wizard_open();
    } else if (action == "HKeyCheatsheet") {
        mode_ = Mode::Cheatsheet;
        cheat_searching_ = false;
    } else if (action == "HKeyMenus") {   // "<": the list of all menus and overlays
        menu_index_sel_ = 0;
        mode_ = Mode::MenuIndex;
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
        if (has_track_) {   // also with the lyrics engine off: the fetched lyrics are kept for karaoke / READ LYRICS
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
    } else if (action == "HKeyCycleSortMode") { // cycle local-list sort mode (folder order -> title A-Z -> artist A-Z -> recently added)
        local_sort_mode_ = (local_sort_mode_ + 1) % 4;
        if (local_sort_mode_ == 3) { std::lock_guard<std::mutex> lk(added_time_mutex_); added_time_cache_.clear(); }   // fresh times each time the mode is chosen
        refresh_local_view();
        log_event(std::string("sort: ") + sort_mode_name(local_sort_mode_, settings_.meta_only));
    } else if (action == "HKeyPlay") {
        if (queue_focus_) { queue_next_run_ = 0; if (!queue_.empty()) play_queue_item(std::clamp(queue_selected_, 0, static_cast<int>(queue_.size()) - 1), last_key_was_shifted()); }   // ENTER in the queue pane (small or big): play the highlighted item; SHIFT+ENTER also moves the queue along up to it
        else play_selected();
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
        // clean_tag_text(): a tag can hold malformed UTF-8 (cut in the middle of a character) or control characters,
        // which the terminal does not draw the way they are measured -- the row would come out narrower than its pane
        rm.title = clean_tag_text(tags.title);
        rm.artist = clean_tag_text(tags.artist);
        rm.album = clean_tag_text(tags.album);
        rm.year = clean_tag_text(tags.year); // TYER/TDRC (MP3), DATE (Vorbis), ©day (MP4)
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

void App::resolve_playlist_tracks_async() {
    auto paths = std::make_shared<std::vector<fs::path>>();
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        for (const auto& t : playlist_edit_tracks_) {
            if (t.missing) continue;
            auto it = row_meta_cache_.find(path_utf8(t.path));
            if (it == row_meta_cache_.end() || !it->second.tags_resolved) paths->push_back(t.path);
        }
    }
    for (const auto& q : queue_) {
        if (!q.is_local) continue;
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        auto it = row_meta_cache_.find(path_utf8(q.local_path));
        if (it == row_meta_cache_.end() || !it->second.tags_resolved) paths->push_back(q.local_path);
    }
    if (paths->empty()) return;
    std::thread([this, paths]() { run_guarded("playlist metadata", [&] {
        for (const fs::path& p : *paths) {
            std::string key = path_utf8(p);
            RowMeta rm = try_native_row_meta(p);
            if (!rm.tags_resolved) {
                RowMeta ff = probe_row_meta(p);
                if (rm.duration_sec <= 0) rm.duration_sec = ff.duration_sec;
                rm.title = ff.title;
                rm.artist = ff.artist;
                rm.album = ff.album;
                rm.tags_resolved = ff.tags_resolved;
            }
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                row_meta_cache_[key] = rm;
            }
            row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
        }
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
    p.crt = o.crt;
    p.low_rate_blank = o.low_rate;
    p.dwell = o.dwell;
    p.rough = o.rough;
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
    // The audio book's picture sits in the disk's place; its field is one column wider on the left (the other fields move to the
    // right, the meta data field is a column shorter).
    const int book_extra = 0;   // (the field was a column wider for a while; the picture keeps its place, the field is the disk's width again)
    const int disk_w = settings_.element_disk ? disk_.width() : 0;
    const int panel_h = disk_.height();
    
    std::string sep = "  " + settings_.meta_separator + "  ";
    int sep_w = display_width(sep);
    const int fixed_extra = settings_.element_disk ? sep_w : 2;

    int avail = std::max(10, inner - disk_w - fixed_extra + book_extra);   // (the extra column is taken from the meta data below)
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
    if (book_extra) meta_w = std::max(10, meta_w - book_extra);

    std::vector<std::string> disk_frame;
    bool book_pic_img = false;   // the audio book picture is a real pixel image this frame (it uses the picture slot gfx_)
    if (settings_.element_disk && audiobook_ && book_picture_gfx(2, disk_w, panel_h)) {
        book_pic_img = true;
        // placeholder cells: the main loop writes them as spaces or (Sixel) skips them so the picture stays
        for (int i = 0; i < panel_h; ++i) disk_frame.push_back(gfx_hole(disk_w));
    } else if (settings_.element_disk && audiobook_) {
        // The opening book takes the disk's place: braille dots in the colour wave. It is smaller than the disk, so it sits
        // centred between the left border and the ':' of the separator (a column to the right of the exact middle when the
        // widths do not divide), and in the upper part of the field (centred vertically).
        const std::vector<std::string> art = book_art_lines(0, book_anim_t_);
        const int art_w = BookArt::get(BookArt::Which::Book).cols();
        const int left = std::clamp((disk_w + 4 - art_w + 1) / 2 - 2 + 1 + 1, 0, std::max(0, disk_w - art_w));
        const int top = std::max(0, (panel_h - static_cast<int>(art.size())) / 2 - 2);   // two rows above the middle: the sparkles rise above the book
        for (int i = 0; i < panel_h; ++i) {
            const int ai = i - top;
            if (ai >= 0 && ai < static_cast<int>(art.size()))
                disk_frame.push_back(std::string(static_cast<size_t>(left), ' ') + art[static_cast<size_t>(ai)] +
                                     std::string(static_cast<size_t>(std::max(0, disk_w - left - art_w)), ' '));
            else disk_frame.emplace_back(std::string(static_cast<size_t>(disk_w), ' '));
        }
    } else if (settings_.element_disk && !audiobook_ && settings_.album_cover && album_cover_gfx(2, disk_w, panel_h)) {
        // Album Cover: the cover of the playing track is a pixel image in the disk's place (it uses the picture slot gfx_, so the
        // oscilloscope is drawn in braille meanwhile, like next to the audio book picture)
        book_pic_img = true;
        for (int i = 0; i < panel_h; ++i) disk_frame.push_back(gfx_hole(disk_w));
    } else if (settings_.element_disk && !audiobook_ && settings_.album_cover && gfx_proto_ == GfxProto::None &&
               !(disk_frame = album_cover_lines(disk_w, panel_h)).empty()) {
        // (no Kitty / Sixel: the cover in braille dots)
    } else if (settings_.element_disk) {
        disk_frame.clear();
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
                // the scrolling text ends one column before the next field starts
                const int mw = std::max(1, avail_v - 1);
                value_lines.push_back(marquee_or_truncate(mapped_val, mw, key,
                                                          marquee_meta_idx_[marquee_slot], marquee_meta_since_[marquee_slot]) +
                                      (avail_v > 0 ? " " : ""));
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
        if (audiobook_) {
            // The audio book's block: everything that matters for a book (tags read by book_poll(), "-" while they load).
            const bool tags_ready = book_meta_path_ == path_utf8(current_path_);
            auto tag = [&](int i) { return tags_ready && !book_meta_vals_[static_cast<size_t>(i)].empty() ? book_meta_vals_[static_cast<size_t>(i)] : std::string("-"); };
            std::string book_name = tag(1);
            if (book_name == "-" && !book_dir_.empty()) book_name = path_utf8(path_from_utf8(book_dir_).filename());
            const int cur_ch = book_current_chapter();
            std::string chapter = tag(4);
            if (chapter == "-") chapter = path_utf8(current_path_.stem());
            if (cur_ch >= 0) chapter = std::to_string(cur_ch + 1) + "/" + std::to_string(chapters_.size()) + "  " + chapter;
            double total = 0, done = 0; bool complete = false;
            book_totals(elapsed, total, done, complete);
            auto clock = [](double sec) {
                const long long t = static_cast<long long>(std::max(0.0, sec) + 0.5);
                char b[32];
                if (t >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
                else std::snprintf(b, sizeof b, "%02lld:%02lld", t / 60, t % 60);
                return std::string(b);
            };
            kv("Book", book_name, 1, 0);
            kv("Author", tag(2), 1, 1);
            kv("Narrator", tag(3), 1, 2);
            kv("Chapter", chapter);
            kv("Publisher", tag(5));
            kv("Genre", tag(6));
            if (total > 0) {
                kv("Length", clock(total) + (complete ? "" : " +"));
                kv("Heard", clock(done) + "  (" + std::to_string(static_cast<int>(100.0 * done / total + 0.5)) + " %)");
            }
            kv("Sampling", metadata_.sampling);
            kv("Format", metadata_.format);
            kv("File size", metadata_.file_size);
        } else {
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
        }

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
        } else {
            bars = fft_.compute_bars(48, viz_dt_);
        }
    } else {
        // Nothing loaded -- never started, playback stopped with no track,
        // or the file that was playing got deleted. The message sits centred
        // (both ways) in the metadata/visualizer column instead of being
        // glued to its first row, where it used to hide behind the disk.
        meta_rows[std::max(0, panel_h / 2)] = center_pad(audiobook_ ? "Currently No Narrative Loaded" : "Currently No Track Loaded", meta_w);
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
        if (audiobook_) {
            // the audio book mode fetches no lyrics: its field shows the notes (or the visualization), never a "fetching" caption
        } else if (settings_.lyrics_on() && lyrics_ready_) {
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

    if (!has_track_ && audiobook_) {
        // No book loaded: the stack of books builds itself up where the visualizations are.
        const std::vector<std::string> st = book_art_lines(1, book_idle_t_);
        const int sw = BookArt::get(BookArt::Which::Stack).cols();
        const int start = std::max(0, (panel_h - static_cast<int>(st.size())) / 2);
        const int left = std::max(0, (lyrics_w - sw) / 2);
        for (int i = 0; i < static_cast<int>(st.size()) && start + i < panel_h && lyrics_w >= sw; ++i)
            lyric_rows[static_cast<size_t>(start + i)] = std::string(static_cast<size_t>(left), ' ') + st[static_cast<size_t>(i)] +
                                                         std::string(static_cast<size_t>(std::max(0, lyrics_w - left - sw)), ' ');
    } else if (!has_track_) {
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
                if (settings_.osci_style == 1 && gfx_proto_ != GfxProto::None && lyrics_w > 0 && !book_pic_img) {
                    // image style: the terminal draws the picture; the cells stay blank here and the loop emits it
                    gfx_cell_pixels(settings_.cell_pixels, cell_w_, cell_h_);
                    const int cw0 = std::max(4, cell_w_), ch0 = std::max(8, cell_h_);
                    const int cwq = std::clamp(osci_detail_cap(settings_.image_detail, false) / lyrics_w, 4, cw0);
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
                    g.plain = false; g.direct = false; g.opaque = false; g.erase = false;   // (the album cover's picture is a plain one)
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

    // Audio book: the note that is typed or shown takes the field's place (oscilloscope / spectrogram come back when it is put away).
    if (audiobook_ && lyrics_w >= 12 && book_field_has_note()) lyric_rows = book_note_field_rows(lyrics_w, panel_h);

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
    // The filled part is drawn with the Volume Indicator of the ON/OFF tab ("#", "⠶", "⣿" or the "⡾⣽" row); every
    // position is one column wide whatever the look, so the width math below is the same for all of them.
    // angle_: the disk's angle, so "disk" follows it
    std::string vol_fill_colored;
    if (settings_.volume_gradient) {   // "gradient": every position takes its colour from the VIZ gradient, left to right, as the oscilloscope does
        const auto cells = volume_bar_cells(settings_.volume_indicator, vol_hashes, angle_);
        for (size_t i = 0; i < cells.size(); ++i)
            vol_fill_colored += gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, static_cast<float>(i) / (kVolBarCells - 1)) + cells[i];
    } else {
        vol_fill_colored = color_played + volume_bar_fill(settings_.volume_indicator, vol_hashes, angle_);
    }
    std::string vol_bar_colored = vol_fill_colored + "\x1b[0m"
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
                       : (list_source_ == ListSource::Folder) ? (audiobook_ ? "SEARCH BOOKS" : "SEARCH FOLDERS")
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
        content = (audiobook_ ? "/" : "/f:") + last_folder_query_;
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
                       : folders_mode ? (audiobook_ ? "AUDIO BOOKS (Enter: open the book)" : "LOCAL AUDIO FOLDERS (Enter: open folder)")
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
                std::string t_count = std::to_string(f.track_count) + (audiobook_ ? (f.track_count == 1 ? " chapter" : " chapters") : (f.track_count == 1 ? " track" : " tracks"));
                if (audiobook_ && f.up) { t_name = "\u2190 .."; t_count.clear(); }
                else if (audiobook_ && f.container) { t_name = "\u25b8 " + t_name; t_count = std::to_string(f.sub_count) + (f.sub_count == 1 ? " book" : " books"); }   // a folder you walk into
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + (idx == selected_ ? marquee_or_truncate(t_name, name_w, idx + 1000000, marquee_row_idx_, marquee_since_)   // (the hovered folder / book scrolls when too long)
                                            : pad_right(truncate_str(t_name, name_w), name_w)) + settings_.list_separator + " "
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

std::vector<std::string> App::build_queue_panel(int total_width, int height, bool wide_artist) const {
    if (audiobook_) return build_chapter_panel(total_width, height);   // (the audio book player has chapters instead of a queue)
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
                const int artist_w = inner >= 34 ? (wide_artist && inner >= 80 ? 32 : 16) : inner >= 24 ? 10 : 0;   // (the big overlay: twice the room for the artist)
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

    // Like the LOCAL AUDIO pane: how many titles are below the visible rows.
    std::string footer;
    const int remaining = static_cast<int>(queue_.size()) - (queue_scroll_ + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// ---------------------------------------------------------------------
// Playlist editor overlay (Mode::Playlist, HKeyPlaylist)
// ---------------------------------------------------------------------

// The library search of the playlist and meta editors: a normal search, or -- with "/f:" (or "f:") in front of the query --
// every track whose folder matches: each word of the rest must appear in "<parent folder> <folder name>", as in the main
// UI's "/f:" list. No sort here; the callers add their own.
std::vector<LocalTrack> App::filter_local_or_folders(const std::string& raw) const {
    std::string q = raw;
    size_t i = 0;
    while (i < q.size() && q[i] == ' ') ++i;
    if (q.size() >= i + 3 && q[i] == '/') ++i;
    const bool folders = q.size() >= i + 2 && (q[i] == 'f' || q[i] == 'F') && q[i + 1] == ':';
    if (!folders) return filter_and_rank_local(raw);
    std::vector<std::string> words;
    std::string cur;
    for (size_t k = i + 2; k <= q.size(); ++k) {
        if (k == q.size() || q[k] == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
        else cur += q[k];
    }
    std::vector<LocalTrack> out;
    for (const auto& t : all_local_tracks_) {
        const fs::path dir = t.path.parent_path();
        const std::string hay = path_utf8(dir.parent_path().filename()) + " " + path_utf8(dir.filename());
        bool ok = true;
        for (const auto& w : words) if (!contains_ci(hay, w)) { ok = false; break; }
        if (ok) out.push_back(t);
    }
    return out;
}

const char* App::playlist_sort_name(int mode) {
    switch (mode) {
        case 1: return "Filenames A-Z";
        case 2: return "Title A-Z";
        case 3: return "Artist A-Z";
        case 4: return "Album A-Z";
        case 5: return "Recently added";
        default: return "folder order";
    }
}

// The LIBRARY list of the playlist editor: the search result (or, with "/f:" in front of the query, every track whose
// folder matches -- each word of the rest must appear in "<parent folder> <folder name>", as in the main UI's "/f:" list),
// then the sort of Shift+T. Unlike the main list the sort also applies to a search result, so a found set can be put in
// order; sort 0 keeps the relevance ranking / folder order.
void App::playlist_refresh_lib_view() {
    playlist_edit_lib_view_ = filter_local_or_folders(playlist_edit_lib_query_);
    if (playlist_edit_sort_ == 5) {   // recently added: newest first (a stable sort: equal times keep their order)
        std::vector<std::pair<long long, size_t>> keyed;
        keyed.reserve(playlist_edit_lib_view_.size());
        for (size_t i = 0; i < playlist_edit_lib_view_.size(); ++i) keyed.emplace_back(added_time_of(playlist_edit_lib_view_[i].path), i);
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<LocalTrack> sorted;
        sorted.reserve(keyed.size());
        for (const auto& k : keyed) sorted.push_back(std::move(playlist_edit_lib_view_[k.second]));
        playlist_edit_lib_view_ = std::move(sorted);
    } else if (playlist_edit_sort_ != 0) {
        struct Key { bool empty; std::string s; };
        std::vector<std::pair<Key, size_t>> keyed;
        keyed.reserve(playlist_edit_lib_view_.size());
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            for (size_t i = 0; i < playlist_edit_lib_view_.size(); ++i) {
                const LocalTrack& t = playlist_edit_lib_view_[i];
                auto it = row_meta_cache_.find(path_utf8(t.path));
                std::string k;
                if (playlist_edit_sort_ == 1) k = t.title;
                else if (playlist_edit_sort_ == 2) k = (it != row_meta_cache_.end() && !it->second.title.empty()) ? it->second.title : t.title;
                else if (playlist_edit_sort_ == 3) k = (it != row_meta_cache_.end() && !it->second.artist.empty()) ? it->second.artist : t.folder_artist;
                else if (it != row_meta_cache_.end()) k = it->second.album;
                keyed.push_back({{k.empty(), lower(k)}, i});   // no album / artist: after the others
            }
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
            if (a.first.empty != b.first.empty) return !a.first.empty;
            return a.first.s < b.first.s;
        });
        std::vector<LocalTrack> sorted;
        sorted.reserve(keyed.size());
        for (const auto& k : keyed) sorted.push_back(std::move(playlist_edit_lib_view_[k.second]));
        playlist_edit_lib_view_ = std::move(sorted);
    }
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
    resolve_playlist_tracks_async();
    playlist_edit_track_selected_ = 0;
    playlist_tab_ = 0;
    playlist_edit_focus_ = 1;
    text_disengage();
    playlist_edit_dirty_ = false; // freshly loaded from disk -- matches what's saved, nothing to lose yet
    playlist_status_ = "editing \"" + pl->name + "\" (" + std::to_string(pl->tracks.size()) + " tracks)";
}


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
        row(legend_colored(settings_, hint), display_width(hint));
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
        row(legend_colored(settings_, hint), display_width(hint));
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
        // Shift+T (HKeyCycleSortMode) sorts the list -- while the box has not been typed into yet (a capital T typed
        // as text is a T, so start a search with a small letter, or leave the box with ESC first).
        if (!text_engaged("playlist-lib-search") && resolve_hotkey_action(key) == "HKeyCycleSortMode") {
            playlist_edit_sort_ = (playlist_edit_sort_ + 1) % 6;
            if (playlist_edit_sort_ == 5) { std::lock_guard<std::mutex> lk(added_time_mutex_); added_time_cache_.clear(); }
            playlist_refresh_lib_view();
            playlist_status_ = std::string("sort: ") + playlist_sort_name(playlist_edit_sort_);
            return;
        }
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
    out.push_back(box_top(std::string("LIBRARY") + (playlist_edit_sort_ ? std::string(" (") + playlist_sort_name(playlist_edit_sort_) + ")" : std::string())
                          + (playlist_edit_focus_ == 1 ? focus_indicator(settings_, border_ansi) : std::string()),
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
        const std::string head_sep(col_sep.size(), ' ');   // (the names only, without the "|" of the rows below)
        std::string header = pad_right(truncate_str("Filename", col1_w), col1_w) + head_sep
                           + pad_right(truncate_str("Title", col2_w), col2_w) + head_sep
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
std::pair<std::string, std::string> menu_tab_strip(int W, const std::string& title, const std::vector<std::string>& labels,
                                                   int cur, const std::string& top_ansi, const std::string& bot_ansi,
                                                   const std::string& corner_ul, const std::string& corner_ur,
                                                   const std::string& vertical, int min_inner,
                                                   const std::string& line2, int line2_cols,
                                                   const std::string& tab_cur, const std::string& tab_oth) {
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
    int panel_h = std::max(8, budget - fixed_rows - 5); // no upper cap: fills a maximised window (-5: 2 border rows + the 2-row legend + the status row)
    if (playlist_tab_ == 0) {
        // Stacked, full width: playlist box (above), LIBRARY, then TRACKS.
        // The two panes together take exactly the lines the old side-by-
        // side pair did (panel_h rows + 2 borders), so the whole screen
        // keeps its height. LIBRARY spends 3 lines on chrome (top border,
        // Filename|Title|Artist header, bottom border), TRACKS 2, which
        // leaves panel_h - 3 data rows to split roughly 1:3.
        // The SEARCH LIBRARY pane above them costs 3 more lines (top border,
        // input row, bottom border), which come out of the TRACKS pane.
        // LIBRARY has two rows more and TRACKS two fewer than the plain 1:3 split, and the third legend row ("Sort")
        // costs TRACKS one more, so the frame keeps its height.
        int data_rows = std::max(5, panel_h - 6);
        int lib_rows = std::max(2, data_rows / 4);
        int track_rows = data_rows - lib_rows;
        lib_rows += 2;
        track_rows = std::max(2, track_rows - 3);
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
        frame << "\n\n";   // the legend's two rows + the status row stay reserved (the prompt is the first of the three), so a prompt never changes the layout above it
    } else if (playlist_confirm_delete_) {
        std::string name = (playlist_manage_selected_ >= 0
                          && playlist_manage_selected_ < static_cast<int>(playlist_manage_view_.size()))
                          ? playlist_manage_view_[playlist_manage_selected_].name : std::string();
        std::string prompt = "Delete playlist \"" + name + "\"? This can't be undone.   [Y]es   [N]o   [ESC] cancel";
        frame << "\x1b[41;97m " << prompt << " \x1b[0m\n";
        frame << "\n\n";
    } else {
        // Left/Right and ESC say what they do RIGHT NOW (see
        // handle_playlist_key()): in a text box that was typed into they move
        // the caret / leave the box, everywhere else they switch the tab /
        // close the editor.
        const char* field = playlist_tab_ == 1 ? (playlist_manage_focus_ == 0 ? "playlist-manage-search" : "")
                          : playlist_edit_focus_ == 0 ? "playlist-name"
                          : playlist_edit_focus_ == 1 ? "playlist-lib-search" : "";
        const bool in_text = *field && text_engaged(field);
        // The legend is a small tree table, like the meta editor's: two rows (Move / list keys)
        // on both tabs, so the footer always has the same height and fits 120 x 30 (shorter labels on narrower terminals).
        const std::string lr = in_text ? "[\u2190\u2192] Cursor" : "[\u2190\u2192] Switch Tab\001Tab";
        const std::string esc = in_text ? "[ESC] Leave field" : "[ESC] Clear / Exit\001Exit";
        std::vector<LegendGroup> groups;
        if (playlist_tab_ == 1) {
            groups.push_back({"Move", {lr, "[TAB] Focus", "[\u2191\u2193] Navi.", "[ENTER] Load", esc}});
            groups.push_back({"Playlist", {"[DEL] Delete", "[e/i] Export/Import"}});
        } else {
            groups.push_back({"Move", {lr, "[TAB] Focus", "[\u2191\u2193] Navi.", "[ENTER] Add/Load", esc}});
            groups.push_back({"Tracks", {"[DEL] Remove", "[4/5] Move \u2191\u2193", "[CTRL+s] Save"}});
            groups.push_back({"Sort", {"[SHIFT+t] Sorting searched titles", "[/f:] Search folders"}});
        }
        {
            const auto lrows = layout_legend_tree(groups, W - 1);
            for (size_t i = 0; i < lrows.size(); ++i) frame << " " << paint_legend(settings_, lrows, i, border, 0, true) << "\n";
        }
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
std::string header_sgr(const Settings& s) {
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
std::string legend_sgr(const Settings& s) {
    return ansi_for(s.legend_color, false);
}

// A one-line key legend ("[ESC] Close | [ENTER] OK"): the keys in [square brackets] in the LEGEND colour (KEY-CMD), everything else
// (descriptions, separators) in the COMMAND colour. Each piece is closed with a reset, so the caller needs no trailing one.
std::string legend_colored(const Settings& s, const std::string& text) {
    const std::string R = "\x1b[0m", key = legend_sgr(s), cmd = ansi_for(s.legend_cmd_color, false);
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '[') {
            const size_t e = text.find(']', i);
            const size_t stop = e == std::string::npos ? text.size() : e + 1;
            out += key + text.substr(i, stop - i) + R;
            i = stop;
        } else {
            size_t e = text.find('[', i);
            if (e == std::string::npos) e = text.size();
            out += cmd + text.substr(i, e - i) + R;
            i = e;
        }
    }
    return out;
}

// One row of a legend tree table (layout_legend_tree()) in the colours of the Colors tab: the row label ("Move", "Fetch" ...)
// keeps the HEADER colour, the keys use the LEGEND colour (KEY-CMD) and their descriptions the COMMAND colour. The tree lines
// and the bar are drawn in the border colour with their glyph right behind the escape, so the smooth border gradient (BorderGradient=smooth)
// recolours them row by row with the frame they belong to. (`fade` is kept for the callers; it no longer changes anything.)
std::string paint_legend(const Settings& s, const std::vector<LegendRow>& rows, size_t i, const std::string& border, int pad_to, bool fade) {
    (void)fade;
    return paint_legend_row(rows[i], border, header_sgr(s), legend_sgr(s), pad_to, ansi_for(s.legend_cmd_color, false));
}

// SGR prefix for "this field is actively being typed into" -- every
// in-place text edit across the Settings panel (a path row, a hex colour
// value, a hotkey) and, now, the meta editor's tag fields. The Colors
// tab's CHANGE row (settings_.change_text_color / change_bg_color);
// defaults to "7"/"1" (palette white on red) -- the same white-on-red
// this used to be hardcoded as everywhere it appears, before it had a
// name of its own in the Colors tab.
std::string change_sgr(const Settings& s) {
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
    if ((meta_resort_edited_ || meta_sort_added_) && !meta_lib_view_.empty()) keep = meta_hovering_path();

    meta_lib_view_ = filter_local_or_folders(meta_query_);

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

    if (meta_sort_added_) {   // 'n': newest arrived first; 'r' (below) still lifts the edited files above this order
        std::vector<std::pair<long long, size_t>> keyed;
        keyed.reserve(meta_lib_view_.size());
        for (size_t i = 0; i < meta_lib_view_.size(); ++i) keyed.emplace_back(added_time_of(meta_lib_view_[i].path), i);
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<LocalTrack> sorted;
        sorted.reserve(keyed.size());
        for (const auto& k : keyed) sorted.push_back(std::move(meta_lib_view_[k.second]));
        meta_lib_view_ = std::move(sorted);
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
    }
    if (!keep.empty()) {
        for (size_t i = 0; i < meta_lib_view_.size(); ++i) {
            if (path_utf8(meta_lib_view_[i].path) == keep) {
                meta_lib_selected_ = static_cast<int>(i);
                break;
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
                     + (n == 1 ? "" : "s") + " on top" + meta_filter_note();
    } else {
        meta_status_ = "library order: alphabetical again" + meta_filter_note();
    }
}

// The reminder added to the status line of 'r' / 'n': a missing-tag filter (x / T / A / L / Y) that is still on hides every file
// that is not missing that tag, which looks like "the resort found nothing".
std::string App::meta_filter_note() const {
    if (meta_filter_ == 0) return "";
    return std::string("  NOTE: [") + meta_filter_label() + "] filter active, hit [ESC] to clear filter";
}

// 'n': flip the library pane between its normal order and "the files that arrived last first" (see added_time_of()).
void App::meta_toggle_sort_added() {
    meta_sort_added_ = !meta_sort_added_;
    if (meta_sort_added_) { std::lock_guard<std::mutex> lk(added_time_mutex_); added_time_cache_.clear(); }   // fresh times
    meta_refresh_lib_view();
    meta_status_ = std::string(meta_sort_added_ ? "library order: recently added first" : "library order: alphabetical again") + meta_filter_note();
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
    else meta_status_ = std::string("filter: ") + meta_filter_label() + " - " + shown + " files (press the key again or [ESC] to clear)";
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
                for (auto& t : playlist_edit_tracks_) {
                    if (path_utf8(t.path) == e.path) {
                        t.path = path_from_utf8(new_path);
                        t.title = path_utf8(t.path.stem());
                        t.missing = false;
                    }
                }
                job.renamed.emplace_back(e.path, new_path);   // saved playlists follow in meta_save_finish()
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
    if (!job.renamed.empty()) {
        // Saved playlists hold file paths: let them follow the renamed files.
        for (const fs::path& dir : playlist_dirs()) PlaylistManager::rename_paths(dir, job.renamed);
    }
    if (job.ok > 0) {
        // Names and tags changed -- rebuild the lists. rescan_library() only
        // refreshes the app's own views, so the meta menu's LIBRARY panel has
        // to be rebuilt as well: after a rename it would otherwise keep
        // showing (and keep handing out, via meta_hovering_path()) the old
        // path, and every edit made against it from there on would be filed
        // under a file that no longer exists.
        rescan_library();
        meta_refresh_lib_view();
        resolve_playlist_tracks_async();   // an open playlist editor shows the new tags
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
        } else if (p == MetaPrompt::ClearFetch) {
            // only the list: pending edits (fetched tags included) stay until CTRL+s or CTRL+SHIFT+x
            meta_prompt_paths_.clear();
            const size_t n = meta_fetch_list_.size();
            meta_fetch_list_.clear();
            meta_fetch_ok_paths_.clear();
            meta_fetch_selected_ = 0;
            meta_persist();
            say("fetch list emptied (" + std::to_string(n) + " title" + (n == 1 ? "" : "s") + "); pending edits are kept");
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

// The THEME overlays of the Settings screen (see theme_ui_ in app.h): the theme picker (also used to choose a
// ---------------------------------------------------------------------
// Console / log overlay (HKeyConsole)
// ---------------------------------------------------------------------

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
        case Mode::RetryLyrics: case Mode::ClearQueue: case Mode::UpdateNotice: case Mode::QueueToPlaylist: case Mode::OsciMenu: case Mode::SpectroMenu: case Mode::NormMenu: case Mode::Equalizer: case Mode::SleepTimer: case Mode::MenuIndex: case Mode::Creator: case Mode::LyricsEdit: case Mode::BookOverlay: return true;
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

// Key legend of the big queue overlay: a small tree table (Edit / Order / Other), see layout_legend_tree().
std::vector<LegendRow> App::queue_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kQueueOverlayChromeRows - 5, 1, 3);
    std::vector<LegendGroup> groups = {
        {"Edit", {"[" + hotkey_text("HKeyRemoveHoveringSongFromQueue", "d") + "] Delete",
                  "[" + hotkey_text("HKeyClearQueue", "X") + "] Clear all",
                  "[" + hotkey_text("HKeyQueueUndoClear", "CTRL+SHIFT+Z") + "] Undo clear"}},
        {"Order", {"[" + join_keys(hotkey_text("HKeyQueueMoveUp", "4"), hotkey_text("HKeyQueueMoveDown", "5")) + "] Move up/down",
                   "[" + join_keys(hotkey_text("HKeyQueueMoveTop", "$"), hotkey_text("HKeyQueueMoveBottom", "%")) + "] Move to top/bottom\001Top/Bottom",
                   "[" + hotkey_text("HKeyQueueLock", "!") + "] " + (queue_locked_ ? "Unlock" : "Lock")}},
        {"Other", {"[" + hotkey_text("HKeyQueueToPlaylist", "CTRL+SHIFT+U") + "] Queue to playlist\001To playlist",
                   "[SHIFT+\u2191\u2193] Next/Prev. page\001Page", "[ESC] Close"}},
    };
    return layout_legend_tree(groups, std::max(0, panel_w), max_lines);
}

namespace {
// One legend line as drawn below an overlay frame: in the legend color (the
// Colors tab's LEGEND row, `sgr` = legend_sgr()), padded to the panel width
// (so it overwrites whatever the background has under it).
std::string legend_row(const std::string& text, int panel_w, const std::string& sgr) {
    const int pad = std::max(0, panel_w - display_width(text));
    return sgr + text + std::string(pad, ' ') + "\x1b[0m";
}
// A legend with several lines hangs under the frame in the border colour, centred on the main screen's bottom border line (see
// overlay_legend_shift()); a single line keeps the key / command colours of the Colors tab.
std::string overlay_legend_line(const Settings& s, const std::string& text, int panel_w, bool several) {
    if (several) return legend_row(text, panel_w, ansi_for(s.border_color, false));
    return legend_colored(s, text) + std::string(static_cast<size_t>(std::max(0, panel_w - display_width(text))), ' ');
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
    list_rows = std::max(1, term_rows_ - 2 + overlay_legend_shift(legend) - kListOverlayChromeRows - legend);
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
        {
            const auto lines = list_overlay_legend(panel_w);
            for (const auto& l : lines) list.push_back(overlay_legend_line(settings_, l, panel_w, lines.size() > 1));
        }
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
    for (const auto& r : library_roots()) if (!r.empty()) roots.push_back(fo_normal_root(r));

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
    fo_marked_.clear();
    if (audiobook_) {   // the audio book player: the open book (else the book under the cursor), with its chapters shown
        if (!book_dir_.empty()) { target = book_dir_; fo_expanded_.insert(book_dir_); }
        else if (selected_ >= 0 && selected_ < static_cast<int>(folder_view_.size())) target = folder_view_[static_cast<size_t>(selected_)].path;
        for (const auto& kv : fo_files_) {   // chapters with a bookmark get a mark
            BookCue cue;
            if (!cue.load(BookCue::path_for(path_from_utf8(kv.first)))) continue;
            for (const std::string& raw : cue.get_all_raw("REM BOOKMARK")) {
                BookMark m;
                if (parse_bookmark(raw, m)) fo_marked_.insert(path_utf8(path_from_utf8(kv.first) / path_from_utf8(m.file)));
            }
        }
    } else if (list_source_ == ListSource::Local && selected_ >= 0 && selected_ < static_cast<int>(local_view_.size()))
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
    if (audiobook_) {   // `f`: the book opens in the chapter pane
        folder_overlay_close();
        if (!book_open_dir(dir)) { status_line_ = "that folder holds no audio files"; return; }
        if (!row.is_dir) for (size_t i = 0; i < chapters_.size(); ++i) if (path_utf8(chapters_[i].path) == row.path) { queue_selected_ = static_cast<int>(i); book_fit_scroll(); break; }
        return;
    }
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
        if (cur->is_dir) { status_line_ = "CTRL+p plays a chapter -- open the book (ENTER) and pick one"; return true; }
        if (audiobook_) {   // plays from its start at once (no questions), the overlay stays
            if (!book_open_dir(path_utf8(path_from_utf8(cur->path).parent_path()))) return true;
            for (size_t i = 0; i < chapters_.size(); ++i)
                if (path_utf8(chapters_[i].path) == cur->path) { book_prompt_paused_ = false; book_play_chapter(static_cast<int>(i)); break; }
            return true;
        }
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

    if (key == 27 || (audiobook_ && key == 'S')) { folder_overlay_close(); return true; }
    if (key == '/' || key == 9) { fo_search_ = true; return true; }

    if (key == '\r' || key == '\n') {
        if (!cur) return true;
        if (cur->is_dir) {
            if (fo_expanded_.count(cur->path)) fo_expanded_.erase(cur->path); else fo_expanded_.insert(cur->path);
            folder_overlay_rebuild_view();
        } else if (audiobook_) {   // ENTER on a chapter: its book opens and the chapter plays (with the bookmark questions)
            const std::string file = cur->path;
            folder_overlay_close();
            if (!book_play_file(file)) status_line_ = "that file no longer exists";
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
    if (audiobook_ && !fo_search_ && book_overlay_switch(key)) return true;   // SHIFT+k / m / l: the other audio book overlays
    if (action == "HKeyFolderOverlay") { folder_overlay_close(); return true; }
    if (action == "HKeyListOverlay") { folder_overlay_close(); list_overlay_open(); return true; }     // switch without closing first
    if (action == "HKeyQueueOverlay") { folder_overlay_close(); queue_overlay_open(); return true; }
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
    const int legend = audiobook_ ? static_cast<int>(folder_overlay_book_legend(panel_w).size()) : static_cast<int>(folder_overlay_legend(panel_w).size());
    rows = std::max(1, term_rows_ - 2 + overlay_legend_shift(legend) - kListOverlayChromeRows - legend);
}

// The legend of the library overlay of the audio book mode: a tree table in three groups (the legends of the meta editor look like this).
std::vector<LegendRow> App::folder_overlay_book_legend(int panel_w) const {
    const std::vector<LegendGroup> groups = {
        {"Open", {"[ENTER] Open the book / play the chapter\001Open", "[\u2190\u2192] Close/open a book\001Fold", "[" + hotkey_text("HKeyFilterForFolder", "f") + "] Book to the chapter pane\001To pane"}},
        {"Play", {"[CTRL+p] Play from the start (the overlay stays)\001Play"}},
        {"Find", {"[/] Search", "[SHIFT+\u2191\u2193] Page", "[ESC] Close"}}};
    return layout_legend_tree(groups, std::max(0, panel_w), 3);
}

std::vector<std::string> App::folder_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kListOverlayChromeRows - 5, 1, 3);
    std::vector<std::string> items = audiobook_ ? std::vector<std::string>{
        "[←→] open/close book",
        "[ENTER] open the book / play the chapter",
        "[" + hotkey_text("HKeyFilterForFolder", "f") + "] book to the chapter pane",
        "[CTRL+p] play from the start",
        "[/] search",
        "[SHIFT+↑↓] page",
        "[ESC] close",
    } : std::vector<std::string>{
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
        out.push_back(box_top(audiobook_ ? "SEARCH BOOKS" : "SEARCH LOCAL FOLDERS", search_w, border_ansi) + border_ansi + "╭───╮\x1b[0m");
        const int inner = std::max(0, search_w - 4);
        const std::string pre = audiobook_ ? "/" : "/f:";
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
    out.push_back(box_top(audiobook_ ? "AUDIO BOOKS" : "LOCAL AUDIO FILES FOLDER", panel_w, border_ansi));
    for (int row = 0; row < list_rows; ++row) {
        const int idx = fo_scroll_ + row;
        std::string content;
        bool playing = false;
        if (idx < total) {
            const FoRow& r = fo_view_[static_cast<size_t>(idx)];
            const std::string indent(static_cast<size_t>(r.depth) * 2, ' ');
            const std::string mark = r.is_dir ? (r.expanded ? "▾ " : "▸ ") : (audiobook_ && fo_marked_.count(r.path) ? "◆ " : "  ");   // (◆: a bookmark)
            const std::string head = indent + mark;
            if (r.is_dir) {
                const std::string cnt = std::to_string(r.count) + (audiobook_ ? (r.count == 1 ? " chapter" : " chapters") : (r.count == 1 ? " track" : " tracks"));
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
    {
        if (audiobook_) {
            const auto lrows = folder_overlay_book_legend(panel_w);
            for (size_t i = 0; i < lrows.size(); ++i) out.push_back(paint_legend(settings_, lrows, i, border_ansi, panel_w, false));
        } else {
            const auto lines = folder_overlay_legend(panel_w);
            for (const auto& l : lines) out.push_back(overlay_legend_line(settings_, l, panel_w, lines.size() > 1));
        }
    }
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
    for (const auto& r : library_roots()) if (!r.empty()) roots.push_back(fo_normal_root(r));
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
    rows = std::max(1, term_rows_ - 2 + overlay_legend_shift(legend) - kListOverlayChromeRows - legend - 2);   // 2 rows of the box hold the filter and status lines
}

std::vector<LegendRow> App::convert_overlay_legend(int panel_w) const {
    const int max_lines = std::clamp(term_rows_ - kListOverlayChromeRows - 5, 1, 3);
    std::vector<LegendGroup> groups = {
        {"Convert", {"[SPACE] mark", "[a] mark all", "[ENTER] convert", "[c] cancel"}},
        {"Options", {"[\u2190\u2192] target", "[t] type", "[r] min kbps", "[h] hi-res"}},
        {"Other", {"[/ , /p: , /f:] full or filtered path search\001search", "[CTRL+p] play", "[SHIFT+\u2191\u2193] page", "[ESC] close"}},
    };
    return layout_legend_tree(groups, std::max(0, panel_w), max_lines);
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
    {
        const auto lrows = convert_overlay_legend(panel_w);
        for (size_t i = 0; i < lrows.size(); ++i) out.push_back(paint_legend(settings_, lrows, i, border_ansi, panel_w, false));
    }
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
        case Mode::RetryLyrics: case Mode::ClearQueue: case Mode::UpdateNotice: case Mode::QueueToPlaylist: case Mode::OsciMenu: case Mode::SpectroMenu: case Mode::NormMenu: case Mode::Equalizer: case Mode::SleepTimer: case Mode::MenuIndex: case Mode::Creator: case Mode::LyricsEdit: case Mode::BookOverlay: return true;
        default: return false;
    }
}

void App::queue_overlay_geometry(int W, int& panel_w, int& queue_rows) const {
    panel_w = std::min(W, std::max(40, std::min(W - 6, 160)));
    // Rows 2 .. term_rows_-1 belong to the panel: queue box border (2) + the queue rows.
    // ...minus the key legend lines below the frame (see list_overlay_geometry()).
    const int legend = static_cast<int>(queue_overlay_legend(panel_w).size());
    queue_rows = std::max(1, term_rows_ - 2 + overlay_legend_shift(legend) - kQueueOverlayChromeRows - legend);
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
    std::vector<std::string> out = build_queue_panel(panel_w, queue_rows, true);
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
        {
            const auto lrows = queue_overlay_legend(panel_w);
            for (size_t i = 0; i < lrows.size(); ++i) out.push_back(paint_legend(settings_, lrows, i, ansi_for(settings_.border_color, false), panel_w, false));
        }
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
    static const std::vector<std::string> kGlyphs = {"\u2502", "\u2500", "\u256d", "\u256e", "\u2570", "\u256f", "\u251c", "\u2524", "\u252c", "\u2534", "\u253c", "\u250c", "\u2514"};
    std::vector<std::string> glyphs = kGlyphs;
    for (const std::string* g : {&settings_.box_upper_left, &settings_.box_upper_right, &settings_.box_lower_left,
                                 &settings_.box_lower_right, &settings_.box_vertical, &settings_.box_horizontal})
        if (!g->empty()) glyphs.push_back(*g);
    const std::string top = ansi_for(settings_.border_color, false), bottom = ansi_for(settings_.border_color_bottom, false);
    return border_gradient::apply(lines, top, bottom, glyphs, [&](float t) {
        return gradient_ansi(settings_.border_color, settings_.border_color_bottom, t, false);
    });
}

void App::draw_floating_panel(std::ostringstream& frame, const std::vector<std::string>& lines_in, int panel_w, int W, int fixed_col, bool exact_center, int fixed_row) const {
    const std::vector<std::string> lines = panel_gradient(lines_in);
    int panel_h = static_cast<int>(lines.size());
    int start_col = fixed_col > 0 ? fixed_col : 1 + std::max(0, (W - panel_w) / 2);
    int start_row = 1 + std::max(0, (term_rows_ - panel_h) / 2 - (exact_center ? 0 : kFloatingPanelUpShift));
    if (fixed_row > 0) start_row = fixed_row;
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
    if (bulk_add_buffer_.empty() && !bulk_add_results_ready_) {
        lines.push_back(wrap(box_line("//:paste yt playlist link here", inner_w, border)));
    } else {
        const bool mine = edit_owner_ == "bulk-add";
        const size_t cc = mine ? std::min(edit_caret_, bulk_add_buffer_.size()) : bulk_add_buffer_.size();
        const size_t aa = mine ? std::min(edit_anchor_, bulk_add_buffer_.size()) : bulk_add_buffer_.size();
        const EditPaint p = paint_edit_field(bulk_add_buffer_, cc, aa, std::max(4, inner_w - 4), "", !bulk_add_results_ready_);
        lines.push_back(wrap(box_line_field("", p.s, p.cols, inner_w, border)));
    }
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
    edit_owner_.clear();   // fresh fields: the caret starts at the end of the one in focus
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
        push_legend_block(lines, W, border_bottom, {"Type a name (max " + std::to_string(kEqNameMaxBytes) + " characters)", "An existing custom name is replaced", "[ENTER] save  [ESC] cancel"}, legend_row, legend_bottom);
    } else {
        push_legend_block(lines, W, border_bottom, {"[</>] band  [UP/DOWN] gain  [,/.] preset  [0] zero", "[S] save preset  [DEL/X] delete preset", "[SPACE] on/off  [R] flat  [" + hotkey_text("HKeyEqualizer", "E") + " / ESC] close"}, legend_row, legend_bottom);
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
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + "\x1b[0m" + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[UP/DOWN] select  [LEFT/RIGHT] change", "[R] reset  [" + hotkey_text("HKeyOscMenu", "O") + " / ESC] close"}, leg_in, leg_bot);
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
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + "\x1b[0m" + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[UP/DOWN] select   [LEFT/RIGHT] change", "[SPACE] on/off  [R] reset  [" + hotkey_text("HKeyNormMenu", "V") + " / ESC] close"}, leg_in, leg_bot);
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
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + R + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(out, W, border_bottom, {"[LEFT/RIGHT] -/+ 0.1 s   [DOWN/UP] -/+ 0.5 s", "[SHIFT+LEFT/RIGHT] seek the track -/+ 5 s", "[R] reset  [ENTER] save  [ESC] cancel"}, leg_in, leg_bot);
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
    lines.push_back(centred_bottom(settings_, W, "[UP/DOWN] [ENTER] set  [ESC] close", border_bottom));
    return lines;
}

// ---------------------------------------------------------------------
// MENUS index (HKeyMenus, "<"): every menu and overlay of the player by category, with its key. A floating panel in two
// columns; ENTER closes it and presses the key of the highlighted entry (as bound now), so a rebinding is followed.
// ---------------------------------------------------------------------
namespace {
struct MenuIdxEntry { const char* cat; const char* title; const char* action; const char* fallback; };
const MenuIdxEntry kMenuIdxPlayer[] = {
    {"LYRICS",           "Karaoke",                   "HKeyKaraoke",        "k"},
    {"LYRICS",           "Read lyrics",               "HKeyReadLyrics",     "CTRL+L"},
    {"LYRICS",           "Lyrics timing",             "HKeyLyricsTiming",   "ALT+L"},
    {"LYRICS",           "Retry given lyrics",              "HKeyRetryLyrics",    "l"},
    {"LISTS & LIBRARY",  "Big list overlay",          "HKeyListOverlay",    "L"},
    {"LISTS & LIBRARY",  "Big queue overlay",         "HKeyQueueOverlay",   "K"},
    {"LISTS & LIBRARY",  "Big folder overlay",        "HKeyFolderOverlay",  "F"},
    {"LISTS & LIBRARY",  "File conversion",           "HKeyConvertOverlay", "C"},
    {"LISTS & LIBRARY",  "Playlist editor",           "HKeyPlaylist",       "P"},
    {"LISTS & LIBRARY",  "Meta data editor",          "HKeyMetaEditor",     "M"},
    {"LISTS & LIBRARY",  "Listening history",         "HKeyHistory",        "H"},
    {"SOUND",            "Equalizer",                 "HKeyEqualizer",      "E"},
    {"SOUND",            "Normalization",             "HKeyNormMenu",       "V"},
    {"SOUND",            "Sleep timer",               "HKeySleepTimer",     "Z"},
    {"VISUALS",          "Oscilloscope options",      "HKeyOscMenu",        "O"},
    {"VISUALS",          "Oscilloscope full screen",  "HKeyScopeBig",       "="},
    {"VISUALS",          "Spectrogram options",       "HKeySpectroMenu",    "I"},
    {"VISUALS",          "Spectrogram full screen",   "HKeySpectroFull",    "U"},
    {"VISUALS",          "Scope window",              "HKeyScopeWindow",    ")"},
    {"VISUALS",          "Spectrogram window",        "HKeySpectroWindow",  "("},
    {"CREATE",           "Creator",                   "HKeyCreator",        "^"},
    {"SYSTEM",           "Settings",                  "HKeySetting",        "s"},
    {"SYSTEM",           "Console / logs",            "HKeyConsole",        "t"},
    {"SYSTEM",           "Help wizard",               "HKeyWizard",         "W"},
    {"SYSTEM",           "Cheat sheet",               "HKeyCheatsheet",     "?"},
};
// The audio book mode: only what it can do (the lyrics, playlist and creator entries are not there). An action starting with '#'
// is a fixed key (the fallback), not a bound one.
const MenuIdxEntry kMenuIdxBook[] = {
    {"BOOKS & NOTES",    "Library overlay",           "#",                  "S"},
    {"BOOKS & NOTES",    "Notes overlay",             "HKeyListOverlay",    "L"},
    {"BOOKS & NOTES",    "Bookmark list",             "HKeyQueueOverlay",   "K"},
    {"BOOKS & NOTES",    "New note",                  "HKeyToggleMetaOnly", "N"},
    {"BOOKS & NOTES",    "Set bookmark",              "HKeyFetchTags",      "B"},
    {"BOOKS & NOTES",    "Chapter marks",             "HKeyLyricsTiming",   "ALT+L"},
    {"BOOKS & NOTES",    "Tag editor",                "HKeyMetaEditor",     "M"},
    {"BOOKS & NOTES",    "Reading history",           "HKeyHistory",        "H"},
    {"BOOKS & NOTES",    "File conversion",           "HKeyConvertOverlay", "C"},
    {"SOUND",            "Equalizer",                 "HKeyEqualizer",      "E"},
    {"SOUND",            "Normalization",             "HKeyNormMenu",       "V"},
    {"SOUND",            "Sleep timer",               "HKeySleepTimer",     "Z"},
    {"VISUALS",          "Oscilloscope options",      "HKeyOscMenu",        "O"},
    {"VISUALS",          "Oscilloscope full screen",  "HKeyScopeBig",       "="},
    {"VISUALS",          "Spectrogram options",       "HKeySpectroMenu",    "I"},
    {"VISUALS",          "Spectrogram full screen",   "HKeySpectroFull",    "U"},
    {"SYSTEM",           "Settings",                  "HKeySetting",        "s"},
    {"SYSTEM",           "Console / logs",            "HKeyConsole",        "t"},
    {"SYSTEM",           "Help wizard",               "HKeyWizard",         "W"},
    {"SYSTEM",           "Cheat sheet",               "HKeyCheatsheet",     "?"},
};
struct MenuIdxTable { const MenuIdxEntry* e; int n; };
MenuIdxTable menu_idx_table(bool book) {
    return book ? MenuIdxTable{kMenuIdxBook, static_cast<int>(sizeof(kMenuIdxBook) / sizeof(kMenuIdxBook[0]))}
                : MenuIdxTable{kMenuIdxPlayer, static_cast<int>(sizeof(kMenuIdxPlayer) / sizeof(kMenuIdxPlayer[0]))};
}

// Two columns of cells (entry index, or -1 for a header / blank line), split at the category boundary that balances them.
struct MenuIdxCell { int entry = -1; const char* header = nullptr; };
std::vector<std::vector<MenuIdxCell>> menu_idx_layout(bool book) {
    const MenuIdxTable tab = menu_idx_table(book);
    const MenuIdxEntry* kMenuIdx = tab.e;
    const int kMenuIdxCount = tab.n;
    std::vector<std::vector<MenuIdxCell>> cats;   // one cell list per category
    for (int i = 0; i < kMenuIdxCount; ++i) {
        if (i == 0 || std::string(kMenuIdx[i].cat) != kMenuIdx[i - 1].cat) cats.push_back({MenuIdxCell{-1, kMenuIdx[i].cat}});
        cats.back().push_back(MenuIdxCell{i, nullptr});
    }
    int total = 0;
    for (const auto& c : cats) total += static_cast<int>(c.size()) + 1;
    int best = 1, best_diff = 1 << 20, acc = 0;
    for (size_t k = 0; k + 1 < cats.size(); ++k) {
        acc += static_cast<int>(cats[k].size()) + 1;
        const int diff = std::abs(acc - (total - acc));
        if (diff < best_diff) { best_diff = diff; best = static_cast<int>(k) + 1; }
    }
    std::vector<std::vector<MenuIdxCell>> cols(2);
    for (size_t k = 0; k < cats.size(); ++k) {
        auto& col = cols[static_cast<int>(k) < best ? 0 : 1];
        if (!col.empty()) col.push_back(MenuIdxCell{});   // blank line between categories
        col.insert(col.end(), cats[k].begin(), cats[k].end());
    }
    return cols;
}
} // namespace

std::vector<std::string> App::build_menu_index_panel() const {
    const int W = kMenuIndexPanelWidth;
    const int inner = W - 4;
    const int cw = (inner - 3) / 2, cw2 = inner - 3 - cw;   // the right column takes the odd column
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    const MenuIdxTable tab = menu_idx_table(audiobook_);
    const MenuIdxEntry* kMenuIdx = tab.e;
    const int kMenuIdxCount = tab.n;
    const auto cols = menu_idx_layout(audiobook_);
    const size_t rows = std::max(cols[0].size(), cols[1].size());
    const int sel = std::clamp(menu_index_sel_, 0, kMenuIdxCount - 1);

    // every cell keeps two empty columns on its left and right (the highlight covers them too)
    auto cell = [&](const std::vector<MenuIdxCell>& col, size_t r, int cw_full) {
        const int cw = cw_full - 4;
        if (r >= col.size()) return std::string(static_cast<size_t>(cw_full), ' ');
        const MenuIdxCell& c = col[r];
        if (c.header) return "  " + header_sgr(settings_) + pad_right(c.header, cw) + R + "  ";
        if (c.entry < 0) return std::string(static_cast<size_t>(cw_full), ' ');
        const MenuIdxEntry& e = kMenuIdx[c.entry];
        const std::string key = "[" + (e.action[0] == '#' ? std::string(e.fallback) : hotkey_text(e.action, e.fallback)) + "]";
        const std::string left = std::string(c.entry == sel ? "> " : "  ") + e.title;
        const int gap = std::max(1, cw - display_width(left) - display_width(key));
        const std::string plain = truncate_str(left + std::string(static_cast<size_t>(gap), ' ') + key, cw);
        if (c.entry == sel) return HI + "  " + pad_right(plain, cw) + "  " + R;
        const std::string lead = pad_right(left, cw - display_width(key));
        return "  " + lead + legend_sgr(settings_) + key + R + "  ";
    };

    std::vector<std::string> lines;
    lines.push_back(box_top("MENUS & OVERLAYS", W, border));
    lines.push_back(bar + " " + pad_right("", inner) + " " + bar);
    for (size_t r = 0; r < rows; ++r)
        lines.push_back(bar + " " + cell(cols[0], r, cw) + " " + border + settings_.box_vertical + R + " " + cell(cols[1], r, cw2) + " " + bar);
    lines.push_back(bar + " " + pad_right("", inner) + " " + bar);
    lines.push_back(box_bottom(W, "[↑↓←→] Select  [ENTER] Open  [ESC / " + hotkey_text("HKeyMenus", "<") + "] Close", border_bottom));
    return lines;
}

void App::menu_index_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (!arrow && (key == 27 || key_is("HKeyMenus", key))) { mode_ = Mode::Browse; return; }
    const MenuIdxTable tab = menu_idx_table(audiobook_);
    const MenuIdxEntry* kMenuIdx = tab.e;
    const int kMenuIdxCount = tab.n;
    const auto cols = menu_idx_layout(audiobook_);
    menu_index_sel_ = std::clamp(menu_index_sel_, 0, kMenuIdxCount - 1);
    int col = 0; size_t row = 0;
    for (int c = 0; c < 2; ++c)
        for (size_t r = 0; r < cols[c].size(); ++r) if (cols[c][r].entry == menu_index_sel_) { col = c; row = r; }
    auto entry_at = [&](int c, size_t r) { return r < cols[c].size() ? cols[c][r].entry : -1; };
    if (arrow && (key == 'A' || key == 'B')) {   // up / down: the previous / next entry, wrapping
        menu_index_sel_ = (menu_index_sel_ + (key == 'A' ? kMenuIdxCount - 1 : 1)) % kMenuIdxCount;
        return;
    }
    if (arrow && (key == 'C' || key == 'D')) {   // right / left: the other column, the entry nearest to this row
        const int oc = col == 0 ? 1 : 0;
        for (size_t d = 0; d < std::max(cols[0].size(), cols[1].size()); ++d) {
            if (row >= d && entry_at(oc, row - d) >= 0) { menu_index_sel_ = entry_at(oc, row - d); return; }
            if (entry_at(oc, row + d) >= 0) { menu_index_sel_ = entry_at(oc, row + d); return; }
        }
        return;
    }
    if (!arrow && (key == '\r' || key == '\n')) {   // open it: close the index, then press the key the entry is bound to
        const MenuIdxEntry& e = kMenuIdx[std::clamp(menu_index_sel_, 0, kMenuIdxCount - 1)];
        const auto it = settings_.hotkeys.find(e.action);
        mode_ = Mode::Browse;
        if (e.action[0] == '#') {   // a fixed key
            handle_key(static_cast<unsigned char>(e.fallback[0]));
        } else if (it != settings_.hotkeys.end()) {
            const int k = hotkey_string_to_key(it->second);
            if (k > 0) handle_key(k);
        }
    }
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

// Ctrl+Shift+U: "New playlist / Existing playlist"; the second step has a search field and the list of saved playlists.
std::vector<std::string> App::build_queue_to_playlist_panel() const {
    const int W = kQtpPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain) {
        const std::string t = truncate_str(plain, inner);
        return bar + " " + t + std::string(static_cast<size_t>(std::max(0, inner - display_width(t))), ' ') + " " + bar;
    };
    int skipped = 0;
    const int n = qtp_track_ ? 1 : static_cast<int>(queue_local_tracks(skipped).size());
    std::vector<std::string> lines;
    if (qtp_stage_ == 2) {   // SHIFT+a, a new playlist: its name
        lines.push_back(box_top("New playlist", W, border));
        lines.push_back(row(""));
        lines.push_back(row("Add \"" + atp_track_.title + "\" to a new playlist."));
        lines.push_back(row(""));
        {
            const std::string prefix = "Name: ";
            const int field_w = std::max(1, inner - static_cast<int>(prefix.size()));
            const bool typed = !qtp_new_name_.empty();
            EditPaint p = paint_edit_field(qtp_new_name_, edit_caret_, edit_anchor_, field_w, typed ? search_sgr(settings_) : search_idle_sgr(settings_), true);
            lines.push_back(box_line_field(prefix, search_run(settings_, p.s, typed), p.cols, W, border, search_idle_sgr(settings_)));
        }
        lines.push_back(row(""));
        lines.push_back(box_bottom(W, "[ENTER] Add + save  [ESC] Back", border_bottom));
        return lines;
    }
    if (qtp_stage_ == 0) {
        auto centred = [&](const std::string& plain) {
            const int fill = std::max(0, inner - display_width(plain));
            return bar + " " + std::string(static_cast<size_t>(fill / 2), ' ') + plain + std::string(static_cast<size_t>(fill - fill / 2), ' ') + " " + bar;
        };
        const std::string a = "  New playlist  ", b = "  Existing playlist  ", gap = "    ";
        const int btn_w = display_width(a) + display_width(gap) + display_width(b);
        const int fill = std::max(0, inner - btn_w);
        lines.push_back(box_top(qtp_track_ ? "Track to playlist" : "Queue to playlist", W, border));
        lines.push_back(centred(""));
        lines.push_back(centred(qtp_track_ ? truncate_str("Add \"" + atp_track_.title + "\" to:", inner)
                                           : "Add the " + std::to_string(n) + " track" + (n == 1 ? "" : "s") + " of the queue to:"));
        lines.push_back(centred(""));
        lines.push_back(bar + " " + std::string(static_cast<size_t>(fill / 2), ' ') + (qtp_choice_ == 0 ? HI + a + R : a) + gap
                        + (qtp_choice_ == 1 ? HI + b + R : b) + std::string(static_cast<size_t>(fill - fill / 2), ' ') + " " + bar);
        lines.push_back(centred(""));
        lines.push_back(box_bottom(W, "[n / e]  [ENTER] choose  [ESC] Close", border_bottom));
        return lines;
    }
    lines.push_back(box_top("Existing playlist", W, border));
    {
        const std::string prefix = "Search: ";
        const int field_w = std::max(1, inner - static_cast<int>(prefix.size()));
        const bool typed = !qtp_query_.empty();
        EditPaint p = paint_edit_field(qtp_query_, edit_caret_, edit_anchor_, field_w, typed ? search_sgr(settings_) : search_idle_sgr(settings_), true);
        lines.push_back(box_line_field(prefix, search_run(settings_, p.s, typed), p.cols, W, border, search_idle_sgr(settings_)));
    }
    const int total = static_cast<int>(qtp_view_.size());
    const int scroll = std::clamp(qtp_selected_ - kQtpListRows / 2, 0, std::max(0, total - kQtpListRows));
    for (int r = 0; r < kQtpListRows; ++r) {
        const int idx = scroll + r;
        if (idx >= total) { lines.push_back(row(total == 0 && r == 0 ? "(no saved playlists match)" : "")); continue; }
        const std::string cnt = std::to_string(qtp_view_[static_cast<size_t>(idx)].track_count) + " tracks";
        const int name_w = std::max(1, inner - display_width(cnt) - 2);
        std::string content = pad_right(truncate_str(qtp_view_[static_cast<size_t>(idx)].name, name_w), name_w) + "  " + cnt;
        content = pad_right(truncate_str(content, inner), inner);
        if (idx == qtp_selected_) lines.push_back(bar + " " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + content + R + " " + bar);
        else lines.push_back(bar + " " + content + " " + bar);
    }
    lines.push_back(box_bottom(W, total > kQtpListRows ? std::to_string(total) + " playlists  [\u2191\u2193] [ENTER] Add  [ESC] Back" : "[\u2191\u2193] [ENTER] Add  [ESC] Back", border_bottom));
    return lines;
}

// "A newer version is out": version, the version in use and the address of the release page; [ESC] closes, [o] opens it.
std::vector<std::string> App::build_update_panel() const {
    const int W = kUpdatePanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    auto row = [&](const std::string& plain) {
        const std::string t = truncate_str(plain, inner);
        return bar + " " + t + std::string(static_cast<size_t>(std::max(0, inner - display_width(t))), ' ') + " " + bar;
    };
    std::string ver = update_latest_;
    while (!ver.empty() && (ver[0] == 'v' || ver[0] == 'V')) ver.erase(0, 1);
    std::string shown_url = update_url_;
    if (shown_url.compare(0, 8, "https://") == 0) shown_url.erase(0, 8);
    std::vector<std::string> lines;
    lines.push_back(box_top("New version", W, border));
    lines.push_back(row(""));
    lines.push_back(row("Mousiki " + ver + " is out."));
    lines.push_back(row("You are using " + update_current_ + "."));
    lines.push_back(row(""));
    lines.push_back(row(shown_url));
    lines.push_back(row(""));
    for (const std::string& l : wrap_lines("Your configuration stays the same when you update by installing the newer version. New commands get their default key; if you already use that key for something else, yours is kept and the new command is left unbound (set it under Settings > REFERENCE).", inner, 6))
        lines.push_back(row(l));
    lines.push_back(row(""));
    for (const std::string& l : wrap_lines("Note for those using portable versions: unzip the new version over the old folder, or copy your data folder into the new one, and keep portable.txt next to the launcher. Without portable.txt your configs are not found.", inner, 6))
        lines.push_back(row(l));
    lines.push_back(row(""));
    lines.push_back(box_bottom(W, "[ESC] Close  [o] Go to release", border_bottom));
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
        if (focused) {   // caret, marked range and block cursor from the shared single-line editor
            const bool mine = edit_owner_ == rl_owner(f);
            const size_t cc = mine ? std::min(edit_caret_, value.size()) : value.size();
            const size_t aa = mine ? std::min(edit_anchor_, value.size()) : value.size();
            const EditPaint p = paint_edit_field(value, cc, aa, std::max(4, box_w - 4), "", true);
            out.push_back(wrap(label(lbl), box_line_field("", p.s, p.cols, box_w, border)));
        } else {
            out.push_back(wrap(label(lbl), box_line((value.empty() ? placeholder : value), box_w, border)));
        }
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
            lines.push_back(note_row(leg, settings_.lyrics_on() ? "older version, so saved timing is never overwritten."
                                                               : "older version. Lyrics engine is off: see karaoke / READ LYRICS."));
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
    // SHIFT+p in the theme picker: the normal main screen with the highlighted theme, kept still (the first frame is stored and
    // handed out again; the caller skips identical frames) until a key closes it, the theme changes or the window is resized.
    if (theme_peek_ && !peek_busy_ && theme_sel_ >= 0 && theme_sel_ < static_cast<int>(theme_list_.size())) {
        if (peek_frame_.empty() || peek_sel_ != theme_sel_ || peek_cols_ != term.cols() || peek_rows_ != term.rows()) {
            const Settings saved = settings_;
            const Mode saved_mode = mode_;
            const int saved_ui = theme_ui_;
            const std::string saved_status = status_line_;
            theme_apply(settings_, theme_list_[static_cast<size_t>(theme_sel_)]);
            mode_ = Mode::Browse;
            theme_ui_ = 0;
            status_line_ = "THEME PREVIEW: " + theme_list_[static_cast<size_t>(theme_sel_)].name + "  (any key goes back)";
            peek_busy_ = true;
            force_redraw_ = true;
            try { peek_frame_ = render_frame(term); } catch (...) { peek_frame_.clear(); }
            peek_busy_ = false;
            settings_ = saved; mode_ = saved_mode; theme_ui_ = saved_ui; status_line_ = saved_status;
            peek_sel_ = theme_sel_; peek_cols_ = term.cols(); peek_rows_ = term.rows();
        }
        return peek_frame_;
    }
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
            case Mode::Browse: case Mode::Search: case Mode::BulkAdd: case Mode::RetryLyrics: case Mode::ClearQueue: case Mode::UpdateNotice: case Mode::QueueToPlaylist: case Mode::OsciMenu: case Mode::SpectroMenu: case Mode::NormMenu: case Mode::Equalizer: case Mode::SleepTimer: case Mode::MenuIndex: case Mode::Creator: case Mode::LyricsEdit: case Mode::BookOverlay: return 0;
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
    // (the audio book's full-screen faces - notes, bookmarks, chapter marks - count too: closing them clears the whole screen, so no row of
    // their window or legend can stay behind in the main screen)
    const bool book_big_overlay = audiobook_ && mode_ == Mode::BookOverlay &&
        (book_ov_kind_ == BookOv::Notes || book_ov_kind_ == BookOv::Marks || book_ov_kind_ == BookOv::Chapters);
    const bool big_overlay_now = list_overlay_active() || convert_overlay_active() || folder_overlay_active() || queue_overlay_active() || book_big_overlay;
    const bool overlay_closed = last_big_overlay_ && !big_overlay_now;
    last_big_overlay_ = big_overlay_now;
    bool hard_clear = overlay_closed || (W != last_render_w_) || (term_rows_ != last_render_rows_) || (mode_family(mode_) != mode_family(last_render_mode_)) || force_redraw_;
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

    // The creator's preview (after "Create"): the oscilloscope music or the spectrogram of the new file over the whole screen
    if (creator_show_ && mode_ == Mode::Browse) {
        std::string out = build_creator_preview(W, clear_prefix);
        frame_main_len_ = out.size();
        gfx_ok_ = gfx_.active;
        return out;
    }

    // SHIFT+0: the oscilloscope over the whole screen, YT left and XY right (the options overlay SHIFT+o can sit on top of it)
    if (scope_big_ && (mode_ == Mode::Browse || mode_ == Mode::OsciMenu)) {
        std::string out = build_scope_big(W, clear_prefix);
        frame_main_len_ = out.size();
        if (mode_ == Mode::OsciMenu) {
            std::ostringstream floating;
            draw_floating_panel(floating, build_osci_menu_panel(), kOsciMenuPanelWidth, W, 0, false, 2);   // starts on row 2 and only grows downwards (braille / image style have different row counts)
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
        if (remote_ov_.open) remote_overlay_draw(frame, W);
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
        build_karaoke_screen(frame, W, term_rows_);
        // The legend occupies the very last terminal row (it has no line break after it, so nothing scrolls): the clamp
        // is told about one more row than usual so it does not cut that row off.
        std::string out = clamp_output_rows(frame.str(), term_rows_ + 1);
        if (!hard_clear) out = soften_fullscreen_frame(out, term_cols);
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
            if (row > term_rows_) break;   // the legend block may reach the last terminal row (see overlay_legend_shift())
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    } else if (convert_overlay_active()) {
        const auto lines = panel_gradient(build_convert_overlay_panel(overlay_panel_w, cv_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_) break;   // the legend block may reach the last terminal row (see overlay_legend_shift())
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    } else if (folder_overlay_active()) {
        const auto lines = panel_gradient(build_folder_overlay_panel(overlay_panel_w, fo_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_) break;   // the legend block may reach the last terminal row (see overlay_legend_shift())
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    } else if (queue_overlay_active()) {
        // Same placement as the list overlay; the queue pane's own rows live in it.
        const auto lines = panel_gradient(build_queue_overlay_panel(overlay_panel_w, overlay_queue_rows_));
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2;
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_) break;   // the legend block may reach the last terminal row (see overlay_legend_shift())
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    }
    if (mode_ == Mode::BulkAdd) {
        draw_floating_panel(floating, build_bulk_add_panel(), kBulkAddPanelWidth, W);
    } else if (mode_ == Mode::RetryLyrics) {
        draw_floating_panel(floating, build_retry_lyrics_panel(), kRetryLyricsPanelWidth, W, 0, true);
    } else if (mode_ == Mode::ClearQueue) {
        draw_floating_panel(floating, build_clear_queue_panel(), kClearQueuePanelWidth, W);
    } else if (mode_ == Mode::QueueToPlaylist) {
        draw_floating_panel(floating, build_queue_to_playlist_panel(), kQtpPanelWidth, W);
    } else if (mode_ == Mode::UpdateNotice) {
        draw_floating_panel(floating, build_update_panel(), kUpdatePanelWidth, W);
    } else if (mode_ == Mode::OsciMenu) {
        // Centred like the other overlays; a scope image under it is cropped to the columns that stay free.
        draw_floating_panel(floating, build_osci_menu_panel(), kOsciMenuPanelWidth, W, 0, false, 2);   // starts on row 2 and only grows downwards (braille / image style have different row counts)
    } else if (mode_ == Mode::SpectroMenu) {
        draw_floating_panel(floating, build_spectro_menu_panel(), kSpectroMenuPanelWidth, W);
    } else if (mode_ == Mode::NormMenu) {
        draw_floating_panel(floating, build_norm_menu_panel(), kNormMenuPanelWidth, W);
    } else if (mode_ == Mode::LyricsEdit) {
        draw_floating_panel(floating, build_lyrics_edit_panel(), kLyricsEditPanelWidth, W);
    } else if (mode_ == Mode::MenuIndex) {
        draw_floating_panel(floating, build_menu_index_panel(), kMenuIndexPanelWidth, W, 0, true);
    } else if (mode_ == Mode::SleepTimer) {
        draw_floating_panel(floating, build_sleep_timer_panel(), kSleepTimerPanelWidth, W);
    } else if (mode_ == Mode::BookOverlay) {
        { const int bw = book_ov_width(W); draw_floating_panel(floating, build_book_overlay_panel(bw), bw, W); }
    } else if (mode_ == Mode::Creator) {
        draw_floating_panel(floating, build_creator_panel(), kCreatorPanelWidth, W);
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
            if (gfx_.plain) {
                // The audio book picture stays under a floating panel (a question, the "<" menu): only the pixels the panel covers
                // are left out. `gfx_full_level_` is the picture as made; the hole is cut into the copy that is sent.
                int cut[4] = {0, 0, 0, 0};
                if (floating_mode && fw > 0 && fy < gfx_.row + gfx_.rows && fy + fh > gfx_.row && fx < gfx_.col + gfx_.cols && fx + fw > gfx_.col &&
                    gfx_.cols > 0 && gfx_.rows > 0) {
                    cut[0] = std::clamp((fx - gfx_.col) * gfx_.w / gfx_.cols, 0, gfx_.w);
                    cut[2] = std::clamp((fx + fw - gfx_.col) * gfx_.w / gfx_.cols + 1, 0, gfx_.w);
                    cut[1] = std::clamp((fy - gfx_.row) * gfx_.h / gfx_.rows, 0, gfx_.h);
                    cut[3] = std::clamp((fy + fh - gfx_.row) * gfx_.h / gfx_.rows + 1, 0, gfx_.h);
                }
                // the same hole in cells, for Sixel pictures that paint or blank their cells (opaque / erase): not under the panel
                gfx_.cut_w = 0;
                if (cut[2] > cut[0] && cut[3] > cut[1]) {
                    const int c0 = std::max(fx, gfx_.col), c1 = std::min(fx + fw, gfx_.col + gfx_.cols);
                    const int r0 = std::max(fy, gfx_.row), r1 = std::min(fy + fh, gfx_.row + gfx_.rows);
                    gfx_.cut_c = c0 - gfx_.col; gfx_.cut_r = r0 - gfx_.row; gfx_.cut_w = c1 - c0; gfx_.cut_h = r1 - r0;
                }
                const bool changed = std::memcmp(cut, gfx_last_cut_, sizeof(cut)) != 0;
                const bool new_picture = gfx_.fresh || gfx_full_level_.size() != gfx_.level.size();
                if (new_picture) gfx_full_level_ = gfx_.level;
                else if (changed) gfx_.level = gfx_full_level_;
                if (new_picture || changed) {
                    for (int y = cut[1]; y < cut[3]; ++y)
                        std::fill(gfx_.level.begin() + static_cast<long>(y) * gfx_.w + cut[0], gfx_.level.begin() + static_cast<long>(y) * gfx_.w + cut[2], 0);
                    if (changed) gfx_.fresh = true;
                }
                std::memcpy(gfx_last_cut_, cut, sizeof(cut));
            } else
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
        static const std::vector<std::string> kGlyphs = {"\u2502", "\u2500", "\u256d", "\u256e", "\u2570", "\u256f", "\u251c", "\u2524", "\u252c", "\u2534", "\u253c", "\u250c", "\u2514"};
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
    g_settings_book_layout = audiobook_;   // (the settings screen shows this mode's rows)
    remote::start("player");
    if (resuming) sync_after_radio();
    remote_app_apply();   // the remote app's web server (Settings -> ON/OFF -> MODES -> Remote App)   // the radio's settings may have changed some of ours meanwhile
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
    if (settings_.autosave_enabled && !audiobook_) {   // (the audio book player keeps its place in the .cue files instead)
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
    if (!restored && !local_view_.empty() && !audiobook_) {
        selected_ = 0;
        start_local_track(local_view_[0]);
    }
    // First start after an install: greet with the help wizard. Closing it
    // writes WizardSeen=true, so this happens exactly once -- and only on a
    // cold start, never when coming back from the radio (which keeps the
    // player suspended, not restarted).
    if (wizard::kEnabled && !settings_.wizard_seen && !audiobook_) wizard_open();
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
        if ((settings_.osci_style == 1 || settings_.spectro.style == 1 || audiobook_) && gfx_probed_pref_ != settings_.gfx_protocol) {
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
                       (mode_ == Mode::BulkAdd && !bulk_add_results_ready_) ||
                       (mode_ == Mode::QueueToPlaylist && qtp_stage_ >= 1) ||
                       (mode_ == Mode::RetryLyrics && rl_text_ptr(rl_focus_) != nullptr) ||
                       (audiobook_ && book_note_edit_ && book_note_typing_ && !book_note_discard_ask_) ||   // the note editor (CTRL+c copies)
                       (audiobook_ && book_note_shown_ && !book_note_edit_) ||                            // a note in the field: CTRL+c copies it
                       (audiobook_ && mode_ == Mode::BookOverlay && book_ov_kind_ == BookOv::Notes) ||     // the notes overlay (list, path fields, editor)
                       (mode_ == Mode::Cheatsheet && cheat_searching_) ||
                       (mode_ == Mode::Wizard && wizard_st_.searching) ||
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
        poll_pending_atp();
        poll_pending_row_meta_tags();
        poll_pending_meta_fetch(); // AcoustID results -> pending edits
        poll_meta_save();          // the next slice of a running save
        if (audiobook_) book_poll();   // the loaded chapter's tags, a running tag save
        readlyrics_poll();         // a finished lyrics translation
        maybe_autosave();
        remote_poll();             // `mousiki --cmd ...` and the status file

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time_).count();
        last_frame_time_ = now;
        viz_dt_ = dt;

        if (has_track_) {
            player_.poll_elapsed();
            // No auto-advance while a device handoff is still in flight:
            // finished_ belongs to the previous track until play() swaps the
            // new one in (see device_play_pending_gen_).
            if (!advancing_ && device_play_pending_gen_.load() == 0 && player_.finished()) {
                if (creator_show_) creator_preview_loop(); else advance_track();
            }
        }
        creator_preview_tick();
        sleep_timer_tick();
        creator_resume_tick();
        player_.set_scope_raw(settings_.osci().music);   // oscilloscope music mode: the scopes get the unprocessed signal
        scope_window_sync();
        spectro_window_sync();
        enforce_visual_switches();
        // Disk only spins while something is actually playing — frozen
        // when idle or paused, per instruction.
        if (audiobook_) {   // the audio book's own animations: the book moves while a chapter plays, the stack while none is loaded
            if (has_track_ && !player_.is_paused()) book_anim_t_ += dt;
            if (!has_track_) book_idle_t_ += dt;
        }
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
        terminal_black_background(settings_.black_background);   // Settings -> ON/OFF -> MODES (only writes when it changes)
        terminal_default_text(settings_.list_color);              // text without a colour of its own: the list text colour
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
        const bool sp_creator = sp_img && creator_show_;   // the creator's preview has its own spectrogram, settings and picture
        if (!spectro_used_ || !main_frame) spectro().set_visible(false);
        if (frame_str.find("\x1b[2J") != std::string::npos) { spectro_gfx_.invalidate(); creator_spectro_gfx_.invalidate(); }
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
            // A floating panel (a question, the "<" menu, a small overlay) came or went: the text it left over the picture's cells
            // must be wiped. Sixel keeps its picture only while the cells are skipped, so then the cells are written as spaces and
            // the picture is sent again in the same write (nothing shows in between).
            const bool floating_changed = floating != last_floating;
            if (sixel_on && floating_changed && gfx_.active) gfx_.fresh = true;
            last_floating = floating;
            last_frame_str = std::move(frame_str);
            last_frame_written_at = frame_now;
            gfx_fill_holes(wire, sixel_on && !(floating_changed && gfx_.active));
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
            SpectroGfx& sgfx = sp_creator ? creator_spectro_gfx_ : spectro_gfx_;
            wire += sgfx.emit(sp_proto, sp_creator ? creator_spectro_set_ : settings_.spectro, sp_creator ? creator_spectro_ : spectro(), spectro_area_[0], spectro_area_[1], spectro_area_[2], spectro_area_[3],
                                      cw, chh, true, floating ? float_rect_[1] : caption ? caption_rect_[0] : 0,
                                      floating ? float_rect_[0] : caption ? caption_rect_[1] : 0,
                                      floating ? float_rect_[3] : caption ? 1 : 0, floating ? float_rect_[2] : caption ? caption_rect_[2] : 0);
        } else {
            if (spectro_gfx_.shown()) wire = spectro_gfx_.clear(sp_proto) + wire;   // before the new screen's text
            if (creator_spectro_gfx_.shown()) wire = creator_spectro_gfx_.clear(sp_proto) + wire;
        }
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
    if (creator_spectro_gfx_.shown()) write_frame(creator_spectro_gfx_.clear(gfx_proto_ == GfxProto::Kitty ? 1 : 2));
    if (switch_mode_) {
        // Switching to the radio: the player is only SUSPENDED (kept in memory, so coming back is instant and shows
        // the very same screen). Playback is paused, the terminal is handed over without a flash of the shell.
        if (has_track_ && !player_.is_paused()) player_.pause();
        terminal_hold_alt_screen();
        term.restore();
        save_settings(settings_);
        if (settings_.autosave_enabled && !audiobook_) save_snapshot(build_snapshot());
        suspended_ = true;
        remote::stop();
        return kExitSwitchMode;
    }
    remote::stop();
    term.restore();
    shutdown();
    std::cout << "\nbye.\n";
    return 0;
}

// Everything that has to happen when the player really ends (quit, or quit from the radio while the player was
// suspended): close the play in the history, stop the audio, save, join the threads.
void App::shutdown() {
    if (suspended_) sync_after_radio();   // quit from the radio: its rows may have changed them
    suspended_ = false;
    history_end_current_play();
    history_.save();
    stop_device_worker();
    {
        std::lock_guard<std::mutex> lk(player_mutex_);
        player_.stop();
    }
    save_settings(settings_);
    if (settings_.autosave_enabled && !audiobook_) {
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
    c.crt = o.crt ? 1 : 0;
    c.low_rate = o.low_rate ? 1 : 0;
    c.dwell = o.dwell ? 1 : 0;
    c.rough = o.rough ? 1 : 0;
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
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + "\x1b[0m" + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[UP/DOWN] select  [LEFT/RIGHT] change", "[R] Audacity defaults  [" + hotkey_text("HKeySpectroFull", "U") + "] full screen", "[" + hotkey_text("HKeySpectroMenu", "I") + " / ESC] close"}, leg_in, leg_bot);
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
        const int cwq = std::clamp(osci_detail_cap(settings_.image_detail, true) / g.cols, 4, cw0);
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
    info.samples = std::clamp(os.trace, 128, 8192);
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

// ---------------------------------------------------------------------
// Creator (HKeyCreator = ^) -- see app.h. The sound is made by creator.h; this is the panel around it.
// ---------------------------------------------------------------------
namespace {
constexpr int kCreatorLens[5] = {5, 10, 20, 30, 60};
constexpr int kCreatorFps[4] = {30, 60, 90, 120};
constexpr const char* kCreatorMotionNames[4] = {"None", "Spin", "Tumble", "Pulse"};
constexpr const char* kCreatorDetailNames[3] = {"Low", "Medium", "High"};
constexpr const char* kCreatorReverbNames[3] = {"Off", "Light", "Hall"};
// The rows of the creator panel (the order on the screen depends on the creator mode, see creator_rows()).
enum CreatorRow { RMode, RText, RSound, RLen, RInvert, ROpt1, ROpt2, ROpt3, RGrid, RCreate, RFiles, RView, RStart };
constexpr const char* kCreatorViewNames[3] = {"Spectrogram", "Oscilloscope (XY)", "Oscilloscope (YT)"};
constexpr int kCreatorViewKind[3] = {1, 0, 2};   // preview kind of each view: 0 XY scope, 1 spectrogram, 2 YT scope
constexpr size_t kCreatorMaxRecordSeconds = 300;

}  // namespace

bool App::creator_load() {
    using namespace creator;
    const std::string key = "T:" + creator_text_;
    auto gray = std::static_pointer_cast<Gray>(creator_gray_);
    if (!gray || creator_loaded_key_.compare(0, key.size() + 1, key + "|") != 0) {
        auto g = std::make_shared<Gray>();
        std::string t = creator_text_;
        std::replace(t.begin(), t.end(), '|', '\n');
        *g = gray_from_text(t, 6);
        if (g->empty()) { creator_status_ = "Type some text first"; creator_gray_.reset(); creator_preview_.clear(); creator_loaded_key_.clear(); return false; }
        creator_gray_ = g;
        gray = g;
    }
    creator_loaded_key_ = key + "|n";
    creator_preview_ = braille_preview(*gray, 40, 6, 128, false);
    return true;
}

fs::path App::creator_dir() const {
    fs::path dir;
    const std::string cache_dir = path_utf8(cache_.cache_dir());
    for (const auto& p : settings_.local_music_paths)
        if (!p.empty() && p != cache_dir) { dir = path_from_utf8(p); break; }
    if (dir.empty()) {
        const char* home = std::getenv("HOME");
        if (!home) home = std::getenv("USERPROFILE");
        dir = fs::path(home ? home : ".") / "Music";
    }
    return dir / "creator";
}

bool App::creator_dir_prepare() {
    const fs::path dir = creator_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path marker = dir / kCreatorMarker;
    if (fs::exists(marker, ec)) return false;
    std::ofstream(marker) << "Mousiki Plus creator folder: the files in here are not part of the library; open them from the creator overlay (^), Created files.\n";
    return true;
}

void App::creator_list_refresh() {
    creator_files_.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(creator_dir(), ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".wav") creator_files_.push_back(e.path());
    }
    std::sort(creator_files_.begin(), creator_files_.end(), [](const fs::path& x, const fs::path& y) {
        std::error_code e1, e2;
        return fs::last_write_time(x, e1) > fs::last_write_time(y, e2);   // newest first
    });
    creator_list_sel_ = std::clamp(creator_list_sel_, 0, std::max(0, static_cast<int>(creator_files_.size()) - 1));
}

void App::creator_open_file(const fs::path& wav) {
    auto audio = std::make_shared<creator::Stereo>();
    if (!creator::read_wav16(path_utf8(wav), *audio)) { creator_status_ = "Could not read " + path_utf8(wav.filename()); return; }
    LocalTrack t;
    t.title = path_utf8(wav.stem());
    t.path = wav;
    t.folder_artist = "creator";
    mode_ = Mode::Browse;
    creator_remember_previous();
    start_local_track(t);
    creator_track_ = t;
    creator_audio_ = audio;
    creator_preview_kind_ = t.title.find("_spectrogram") != std::string::npos ? 1 : 0;   // as the creator names its files
    creator_preview_open();
}

void App::creator_render_and_play() {
    using namespace creator;
    if (!creator_load()) return;
    const auto gray = std::static_pointer_cast<Gray>(creator_gray_);
    const bool inv = creator_invert_ == 1 || (creator_invert_ == 0 && gray_is_mostly_bright(*gray));
    const double seconds = kCreatorLens[std::clamp(creator_len_, 0, 4)];
    Stereo audio;
    std::string name = creator_text_;
    if (creator_target_ == 0) {
        TraceOptions to;
        to.invert = inv;
        to.simplify = creator_detail_ == 0 ? 1.6f : creator_detail_ == 1 ? 0.8f : 0.4f;
        auto paths = trace_outlines(*gray, to);
        if (paths.empty()) { creator_status_ = "Nothing to draw: no clear shapes (try Invert)"; return; }
        XyOptions xo;
        xo.rate = 48000;
        xo.seconds = seconds;
        xo.fps = kCreatorFps[std::clamp(creator_fps_, 0, 3)];
        xo.motion = static_cast<Motion>(std::clamp(creator_motion_, 0, 3));
        fit_to_unit(paths);
        limit_length(paths, (xo.rate / xo.fps) * 0.03);
        paths = order_paths(std::move(paths));
        if (!render_xy(paths, xo, audio)) { creator_status_ = "Nothing to draw"; return; }
    } else {
        SpecOptions so;
        so.seconds = seconds;
        so.scale = static_cast<Scale>(std::clamp(creator_scale_, 0, 6));
        so.octaves = std::clamp(creator_octaves_, 3, 6);
        so.reverb = creator_reverb_ == 0 ? 0.0 : creator_reverb_ == 1 ? 0.25 : 0.5;
        so.invert = inv;
        if (!render_spectrogram_sound(*gray, so, audio)) { creator_status_ = "Silent: the picture has no bright part (try Invert)"; return; }
    }
    // <first music folder>/creator, or ~/Music/creator
    const fs::path dir = creator_dir();
    std::error_code ec;
    creator_dir_prepare();
    std::string base;
    for (size_t i = 0; i < name.size() && base.size() < 24;) {
        const size_t len = static_cast<size_t>(utf8_seq_len(static_cast<unsigned char>(name[i])));
        const std::string ch = name.substr(i, len);
        i += len;
        if (len == 1) base += (std::isalnum(static_cast<unsigned char>(ch[0])) ? ch[0] : '_');
        else base += ch;
    }
    if (base.empty()) base = "creation";
    base += creator_target_ == 0 ? "_scope" : "_spectrogram";
    fs::path out = dir / (base + ".wav");
    for (int n = 2; fs::exists(out, ec) && n < 1000; ++n) out = dir / (base + "_" + std::to_string(n) + ".wav");
    std::string err;
    if (!write_wav16(path_utf8(out), audio, &err)) { creator_status_ = "Could not save: " + err; return; }
    log_event("Creator: saved " + path_utf8(out));
    rescan_library();
    LocalTrack t;
    t.title = path_utf8(out.stem());
    t.path = out;
    t.folder_artist = "creator";
    mode_ = Mode::Browse;
    creator_remember_previous();
    start_local_track(t);
    creator_track_ = t;
    creator_audio_ = std::make_shared<Stereo>(std::move(audio));
    creator_preview_kind_ = creator_target_;
    creator_preview_open();
    status_line_ = std::string("Created ") + (creator_target_ == 0 ? "oscilloscope music" : "spectrogram sound") +
                   " (" + std::to_string(static_cast<int>(seconds)) + " s): " + path_utf8(out);
}

void App::creator_key(int key) {
    using namespace creator;
    const bool arrow = last_key_was_arrow();
    // The list of the created files
    if (creator_list_open_) {
        const int n = static_cast<int>(creator_files_.size());
        if (arrow && key == 'A') { if (n) creator_list_sel_ = (creator_list_sel_ + n - 1) % n; return; }
        if (arrow && key == 'B') { if (n) creator_list_sel_ = (creator_list_sel_ + 1) % n; return; }
        if (!arrow && (key == 27 || key == 127 || key == 8)) { creator_list_open_ = false; return; }
        if (!arrow && (key == '\r' || key == '\n') && n > 0) { creator_list_open_ = false; creator_open_file(creator_files_[static_cast<size_t>(creator_list_sel_)]); return; }
        return;   // SPACE and everything else do nothing
    }
    const std::vector<int> ids = creator_rows();
    const int rows = static_cast<int>(ids.size());
    creator_row_ = std::clamp(creator_row_, 0, rows - 1);
    const int id = ids[static_cast<size_t>(creator_row_)];
    const bool on_field = id == RText;
    auto cycle = [](int& v, int n, int dir) { v = ((v + dir) % n + n) % n; };
    // The three options of the sound depend on it: scope (frame rate, motion, detail) or spectrogram (scale, octaves, reverb).
    auto change = [&](int dir) {
        switch (id) {
            case RMode: cycle(creator_mode_, 2, dir); break;
            case RSound: cycle(creator_target_, 2, dir); break;
            case RLen: cycle(creator_len_, 5, dir); break;
            case RInvert: cycle(creator_invert_, 3, dir); break;
            case ROpt1: if (creator_target_ == 0) cycle(creator_fps_, 4, dir); else cycle(creator_scale_, 7, dir); break;
            case ROpt2: if (creator_target_ == 0) cycle(creator_motion_, 4, dir); else creator_octaves_ = std::clamp(creator_octaves_ + dir, 3, 6); break;
            case ROpt3: if (creator_target_ == 0) cycle(creator_detail_, 3, dir); else cycle(creator_reverb_, 3, dir); break;
            case RGrid: creator_grid_ = !creator_grid_; break;
            case RView: cycle(creator_live_view_, 3, dir); break;
            default: break;
        }
    };
    if (arrow && key == 'A') { creator_row_ = (creator_row_ + rows - 1) % rows; return; }
    if (arrow && key == 'B') { creator_row_ = (creator_row_ + 1) % rows; return; }
    if (!arrow && key == 9) { creator_row_ = (creator_row_ + 1) % rows; return; }   // TAB
    if (!arrow && key == 27) { mode_ = Mode::Browse; return; }
    if (on_field) {
        if (!arrow && (key == '\r' || key == '\n')) { creator_status_.clear(); if (creator_load()) creator_row_ = std::min(rows - 1, creator_row_ + 1); return; }
        // SPACE types a space in the text (needed for words); on every other row it does nothing
        edit_focus("creator-text", creator_text_);
        if (edit_text_key(creator_text_, edit_caret_, edit_anchor_, key, 80, &creator_status_)) {
            creator_status_.clear();
            creator_load();
        }
        return;
    }
    if (arrow && key == 'D') { change(-1); return; }
    if (arrow && key == 'C') { change(+1); return; }
    if (!arrow && (key == '\r' || key == '\n')) {
        if (id == RCreate) { creator_render_and_play(); return; }
        if (id == RStart) { creator_status_.clear(); creator_live_start(); return; }
        if (id == RFiles) { creator_list_refresh(); creator_list_open_ = true; creator_status_.clear(); return; }
        change(+1);
        return;
    }
    if (!arrow && key == ' ') return;   // SPACE is off here (a stray space on a German keyboard must not create anything)
    std::string act = resolve_hotkey_action(key);
    if (act == "HKeyCreator") mode_ = Mode::Browse;
}

std::vector<int> App::creator_rows() const {
    if (creator_mode_ == 1) {
        std::vector<int> v{RMode, RView};
        if (kCreatorViewKind[std::clamp(creator_live_view_, 0, 2)] != 1) v.push_back(RGrid);
        v.push_back(RStart);
        v.push_back(RFiles);
        return v;
    }
    std::vector<int> v{RMode, RText, RSound, RLen, RInvert, ROpt1, ROpt2, ROpt3};
    if (creator_target_ == 0) v.push_back(RGrid);
    v.push_back(RCreate);
    v.push_back(RFiles);
    return v;
}

bool App::creator_on_text_field() const {
    const std::vector<int> ids = creator_rows();
    return creator_mode_ == 0 && creator_row_ >= 0 && creator_row_ < static_cast<int>(ids.size()) && ids[static_cast<size_t>(creator_row_)] == RText;
}

std::vector<std::string> App::build_creator_panel() const {
    const int W = kCreatorPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    auto two = [&](bool cursor, const std::string& left, const std::string& right) {
        const std::string l = std::string(cursor ? "> " : "  ") + left;
        const int gap = std::max(1, inner - display_width(l) - display_width(right));
        return l + std::string(static_cast<size_t>(gap), ' ') + right;
    };
    constexpr int kTotalLines = 21;   // the panel keeps its height in both views
    std::vector<std::string> lines;
    if (creator_list_open_) {
        lines.push_back(box_top("Creator: created files", W, border));
        const int n = static_cast<int>(creator_files_.size());
        const int visible = kTotalLines - 4;
        const int first = std::clamp(creator_list_sel_ - visible / 2, 0, std::max(0, n - visible));
        for (int i = 0; i < visible; ++i) {
            const int k = first + i;
            if (k < n) lines.push_back(row(std::string(k == creator_list_sel_ ? "> " : "  ") + path_utf8(creator_files_[static_cast<size_t>(k)].filename()), k == creator_list_sel_));
            else lines.push_back(row(n == 0 && i == 0 ? "  (nothing created yet)" : "", false));
        }
        lines.push_back(row(n ? "Newest first. ENTER plays the file and opens its preview." : "Create something first.", false));
        lines.push_back(box_bottom(W, "[UP/DOWN] [ENTER] preview  [ESC] back", border_bottom));
        return lines;
    }
    const std::vector<int> ids = creator_rows();
    const int cur = std::clamp(creator_row_, 0, static_cast<int>(ids.size()) - 1);
    const bool mic = creator_mode_ == 1;
    const bool scope = creator_target_ == 0;
    lines.push_back(box_top("Creator", W, border));
    for (int idx = 0; idx < static_cast<int>(ids.size()); ++idx) {
        const int id = ids[static_cast<size_t>(idx)];
        const bool at = idx == cur;
        switch (id) {
        case RMode: lines.push_back(row(two(at, "Creator mode", mic ? "< Microphone >" : "< Text to sound >"), at)); break;
        case RText: {   // the text field, with the caret while it has the keyboard
            const std::string prefix = std::string(at ? "> " : "  ") + "Text: ";
            if (at) {
                const EditPaint p = paint_edit_field(creator_text_, edit_caret_, edit_anchor_, std::max(1, inner - display_width(prefix) - 1), "", true);
                const int fill = std::max(0, inner - display_width(prefix) - p.cols);
                lines.push_back(bar + " " + prefix + p.s + std::string(static_cast<size_t>(fill), ' ') + " " + bar);
            } else {
                lines.push_back(row(prefix + (creator_text_.empty() ? std::string("(type a text, | = new line)") : creator_text_), false));
            }
            break;
        }
        case RSound: lines.push_back(row(two(at, "Sound", scope ? "< Oscilloscope (XY) >" : "< Spectrogram >"), at)); break;
        case RLen: lines.push_back(row(two(at, "Length", "< " + std::to_string(kCreatorLens[std::clamp(creator_len_, 0, 4)]) + " s >"), at)); break;
        case RInvert: lines.push_back(row(two(at, "Invert", creator_invert_ == 0 ? "< Auto >" : creator_invert_ == 1 ? "< On >" : "< Off >"), at)); break;
        case ROpt1: lines.push_back(row(scope ? two(at, "Frame rate", "< " + std::to_string(kCreatorFps[std::clamp(creator_fps_, 0, 3)]) + " per second >")
                                              : two(at, "Scale", std::string("< ") + creator::scale_name(static_cast<creator::Scale>(std::clamp(creator_scale_, 0, 6))) + " >"), at)); break;
        case ROpt2: lines.push_back(row(scope ? two(at, "Motion", std::string("< ") + kCreatorMotionNames[std::clamp(creator_motion_, 0, 3)] + " >")
                                              : two(at, "Octaves", "< " + std::to_string(std::clamp(creator_octaves_, 3, 6)) + " >"), at)); break;
        case ROpt3: lines.push_back(row(scope ? two(at, "Detail", std::string("< ") + kCreatorDetailNames[std::clamp(creator_detail_, 0, 2)] + " >")
                                              : two(at, "Reverb", std::string("< ") + kCreatorReverbNames[std::clamp(creator_reverb_, 0, 2)] + " >"), at)); break;
        case RGrid: lines.push_back(row(two(at, "Scope grid", creator_grid_ ? "< On >" : "< Off >"), at)); break;
        case RView: lines.push_back(row(two(at, "Live view", std::string("< ") + kCreatorViewNames[std::clamp(creator_live_view_, 0, 2)] + " >"), at)); break;
        case RCreate: lines.push_back(row(two(at, "Create, save and preview", ""), at)); break;
        case RStart: lines.push_back(row(two(at, "Start the microphone preview", ""), at)); break;
        case RFiles: lines.push_back(row(two(at, "Created files...", ""), at)); break;
        default: break;
        }
    }
    if (!mic) {
        for (int i = 0; i < 6; ++i) {   // a braille preview of the text (6 rows, centred)
            if (i < static_cast<int>(creator_preview_.size())) {
                const int pad = std::max(0, (inner - 40) / 2);
                lines.push_back(bar + " " + std::string(static_cast<size_t>(pad), ' ') + creator_preview_[static_cast<size_t>(i)] +
                                std::string(static_cast<size_t>(std::max(0, inner - pad - 40)), ' ') + " " + bar);
            } else {
                lines.push_back(row(i == 2 && creator_preview_.empty() ? "      (the text appears here)" : "", false));
            }
        }
    }
    // the rest of the height: the hints of the microphone mode, or empty rows
    {
        std::vector<std::string> hint;
        if (mic) {
            hint = {"  The big preview shows your microphone live.", "  [ENTER] there starts / stops a recording (a WAV file);",
                    "  [V] changes the view, [G] the scope grid; [ESC] closes it.", "  Recordings appear under \"Created files...\"."};
        }
        while (static_cast<int>(lines.size()) < kTotalLines - 3) {
            const size_t k = lines.size() - (ids.size() + 1);
            lines.push_back(row(mic && k < hint.size() ? hint[k] : "", false));
        }
    }
    {   // the note / the last message, on two rows (wrapped at a space)
        const std::string note = creator_status_.empty()
            ? (mic ? "Watch your microphone as a spectrogram or an oscilloscope, and record it. Allow microphone access if asked."
               : scope ? "Left channel = x, right channel = y: the XY scope draws the text. A preview opens after creation."
                     : "The text is the spectrogram: time runs left to right, pitch bottom to top. A preview opens after creation.")
            : creator_status_;
        std::string l1 = note, l2;
        if (display_width(note) > inner) {
            size_t cut = std::string::npos;
            for (size_t k = 0; k < note.size(); ++k)
                if (note[k] == ' ' && display_width(note.substr(0, k)) <= inner) cut = k;
            if (cut != std::string::npos) { l1 = note.substr(0, cut); l2 = note.substr(cut + 1); }
        }
        lines.push_back(row(l1, false));
        lines.push_back(row(l2, false));
    }
    lines.push_back(box_bottom(W, "[UP/DOWN] [LEFT/RIGHT] change  [ENTER] go  [ESC] close", border_bottom));
    return lines;
}

// ---------------------------------------------------------------------
// The creator's preview (opens after "Create", loops until ESC) -- see app.h.
// ---------------------------------------------------------------------
void App::creator_preview_open() {
    if (!creator_audio_ || creator_audio_->l.empty()) return;
    creator_fed_ = 0;
    creator_last_pos_ = 0.0;
    creator_scope_.reset();
    creator_spectro_.reset();
    creator_spectro_gfx_.invalidate();
    if (creator_preview_kind_ == 1) {
        // the spectrogram's own settings: the picture as it was drawn (log pitch, the rows of the scale), one channel, one page
        // as long as the clip
        const auto freqs = creator::scale_frequencies(static_cast<creator::Scale>(std::clamp(creator_scale_, 0, 6)), 36, std::clamp(creator_octaves_, 3, 6));
        SpectroSettings sp;
        sp.style = 1;
        sp.motion = 0;
        sp.scale = 1;
        sp.min_freq = std::max(20, static_cast<int>(freqs.front() * 0.85));
        sp.max_freq = static_cast<int>(freqs.back() * 1.15);
        sp.channels = 1;
        sp.scheme = 0;
        const double secs = creator_audio_->seconds();
        sp.span = std::clamp(static_cast<int>(std::ceil(secs / 5.0)) * 5, 5, 60);
        creator_spectro_set_ = sp;
        creator_spectro_.set_visible(true);
    } else {
        creator_spectro_.set_visible(false);
    }
    creator_show_ = true;
    scope_big_ = false;
    spectro_full_ = false;
    force_redraw_ = true;
}

void App::creator_remember_previous() {
    if (creator_show_) return;   // already in a preview: what was playing before it stays the song to come back to
    creator_prev_valid_ = false;
    if (has_track_ && current_is_local_ && !current_path_.empty()) {
        creator_prev_.path = current_path_;
        creator_prev_.title = path_utf8(current_path_.stem());
        creator_prev_.folder_artist = path_utf8(current_path_.parent_path().filename());
        creator_prev_valid_ = true;
    }
}

void App::creator_resume_tick() {
    if (!creator_resume_ || load_in_progress_.load()) return;
    creator_resume_ = false;
    if (!creator_prev_valid_) return;
    creator_prev_valid_ = false;
    start_local_track(creator_prev_);
    status_line_ = "Preview closed: back to " + creator_prev_.title;
}

void App::creator_preview_close() {
    if (creator_live_) {
        if (creator_rec_on_) creator_record_save();
        if (creator_mic_) creator_mic_->stop();
        creator_live_ = false;
    }
    creator_show_ = false;
    creator_spectro_.set_visible(false);
    creator_audio_.reset();
    if (has_track_) {
        player_.stop();
        has_track_ = false;
        history_end_current_play();
    }
    creator_resume_ = creator_prev_valid_;   // the song that played before comes back (creator_resume_tick)
    mode_ = Mode::Creator;                   // back to the creator overlay; ESC there closes it
    creator_list_open_ = false;
    status_line_ = "Preview closed";
    force_redraw_ = true;
}

// ---------------------------------------------------------------------
// The microphone preview (creator mode "Microphone"): the big preview made from the live microphone; ENTER records.
// ---------------------------------------------------------------------
void App::creator_live_start() {
    if (!creator_mic_) creator_mic_ = std::make_unique<MicCapture>();
    creator_remember_previous();                 // a playing song would be heard by the microphone: it stops and comes back afterwards
    std::string err;
    if (!creator_mic_->start(44100, err)) { creator_status_ = "Microphone: " + err; return; }
    if (has_track_) {
        player_.stop();
        has_track_ = false;
        history_end_current_play();
    }
    creator_live_ = true;
    creator_rec_on_ = false;
    creator_rec_.clear();
    creator_rec_rate_ = creator_mic_->rate();
    creator_audio_.reset();
    creator_track_ = LocalTrack{};
    creator_preview_kind_ = kCreatorViewKind[std::clamp(creator_live_view_, 0, 2)];
    creator_scope_.reset();
    creator_spectro_.reset();
    creator_spectro_gfx_.invalidate();
    SpectroSettings sp;
    sp.style = 1;
    sp.motion = 1;       // scrolling, as a live picture
    sp.scale = 1;
    sp.min_freq = 50;
    sp.max_freq = 12000;
    sp.channels = 1;
    sp.scheme = 0;
    sp.span = 10;
    creator_spectro_set_ = sp;
    creator_spectro_.set_visible(creator_preview_kind_ == 1);
    creator_show_ = true;
    mode_ = Mode::Browse;
    scope_big_ = false;
    spectro_full_ = false;
    force_redraw_ = true;
    status_line_ = "Microphone: " + creator_mic_->device_name();
}

void App::creator_live_tick() {
    if (!creator_mic_ || !creator_mic_->running()) return;
    std::vector<float> lr;
    creator_mic_->read(lr);
    if (lr.empty()) return;
    const size_t n = lr.size() / 2;
    if (creator_preview_kind_ == 1) creator_spectro_.push(lr.data(), n, creator_rec_rate_);
    else creator_scope_.push_frames(lr.data(), n);
    if (creator_rec_on_) {
        creator_rec_.insert(creator_rec_.end(), lr.begin(), lr.end());
        if (creator_rec_.size() / 2 >= kCreatorMaxRecordSeconds * static_cast<size_t>(creator_rec_rate_)) {
            creator_record_save();
            status_line_ += " (the 5 minute limit was reached)";
        }
    }
}

void App::creator_record_toggle() {
    if (creator_rec_on_) { creator_record_save(); return; }
    creator_rec_.clear();
    creator_rec_on_ = true;
    status_line_ = "Recording ... [ENTER] stops and saves";
    force_redraw_ = true;
}

void App::creator_record_save() {
    creator_rec_on_ = false;
    force_redraw_ = true;
    if (creator_rec_.size() < 2 * static_cast<size_t>(creator_rec_rate_) / 20) { status_line_ = "Recording too short: not saved"; creator_rec_.clear(); return; }
    creator::Stereo st;
    st.rate = creator_rec_rate_;
    const size_t n = creator_rec_.size() / 2;
    st.l.resize(n);
    st.r.resize(n);
    for (size_t i = 0; i < n; ++i) { st.l[i] = creator_rec_[2 * i]; st.r[i] = creator_rec_[2 * i + 1]; }
    creator_rec_.clear();
    std::error_code ec;
    fs::create_directories(creator_dir(), ec);
    const bool newly = creator_dir_prepare();
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tmv);
    fs::path out = creator_dir() / (std::string("mic_") + stamp + ".wav");
    for (int k = 2; fs::exists(out, ec) && k < 1000; ++k) out = creator_dir() / (std::string("mic_") + stamp + "_" + std::to_string(k) + ".wav");
    std::string err;
    if (!creator::write_wav16(path_utf8(out), st, &err)) { status_line_ = "Could not save the recording: " + err; return; }
    log_event("Creator: saved " + path_utf8(out));
    if (newly) rescan_library();
    status_line_ = "Recording saved: " + path_utf8(out) + " (" + std::to_string(static_cast<int>(st.seconds())) + " s)";
}

void App::creator_preview_loop() {
    // the same as the play mode "loop": the track is already decoded, so it starts again at once
    launch_device_play_async();
    has_track_ = true;
    history_end_current_play();
    player_.clear_finished();
    history_begin_current_play();
    creator_fed_ = 0;
    creator_last_pos_ = 0.0;
    creator_scope_.reset();
    creator_spectro_.reset();
    creator_spectro_gfx_.invalidate();
}

void App::creator_preview_tick() {
    if (creator_live_) { creator_live_tick(); return; }
    if (!creator_show_ || !creator_audio_ || !has_track_) return;
    const creator::Stereo& a = *creator_audio_;
    const double pos = player_.poll_elapsed();
    if (pos + 0.3 < creator_last_pos_) {   // the file started again (or was sought back)
        creator_fed_ = 0;
        creator_scope_.reset();
        creator_spectro_.reset();
        creator_spectro_gfx_.invalidate();
    }
    creator_last_pos_ = pos;
    const size_t target = std::min(a.l.size(), static_cast<size_t>(std::max(0.0, pos) * a.rate));
    std::vector<float> lr;
    while (creator_fed_ < target) {
        const size_t n = std::min<size_t>(4096, target - creator_fed_);
        lr.resize(n * 2);
        for (size_t i = 0; i < n; ++i) { lr[2 * i] = a.l[creator_fed_ + i]; lr[2 * i + 1] = a.r[creator_fed_ + i]; }
        if (creator_preview_kind_ == 0) creator_scope_.push_frames(lr.data(), n);
        else creator_spectro_.push(lr.data(), n, a.rate);
        creator_fed_ += n;
    }
}

void App::creator_preview_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (!arrow && key == 27) { creator_preview_close(); return; }
    if (!arrow && (key == 'g' || key == 'G') && creator_preview_kind_ != 1) { creator_grid_ = !creator_grid_; force_redraw_ = true; return; }
    if (creator_live_) {
        if (!arrow && (key == '\r' || key == '\n')) { creator_record_toggle(); return; }
        if (!arrow && (key == 'v' || key == 'V')) {
            creator_live_view_ = (creator_live_view_ + 1) % 3;
            creator_preview_kind_ = kCreatorViewKind[creator_live_view_];
            creator_scope_.reset();
            creator_spectro_.reset();
            creator_spectro_.set_visible(creator_preview_kind_ == 1);
            creator_spectro_gfx_.invalidate();
            force_redraw_ = true;
        }
        return;
    }
    if (!arrow && key == ' ') {
        if (player_.is_paused()) player_.resume(); else player_.pause();
        return;
    }
}

std::string App::build_creator_preview(int W, const char* clear_prefix) const {
    const int H = std::max(6, term_rows_ - 1);
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string legend = ansi_for(settings_.legend_color, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const bool scope = creator_preview_kind_ != 1;
    const bool live = creator_live_;
    std::string title = live ? std::string("CREATOR - MICROPHONE - ") + kCreatorViewNames[std::clamp(creator_live_view_, 0, 2)]
                             : std::string("CREATOR PREVIEW - ") + (scope ? "oscilloscope music" : "spectrogram");
    if (!creator_track_.title.empty()) title += " - " + creator_track_.title;
    if (live && creator_rec_on_) {
        const int secs = static_cast<int>(creator_rec_.size() / 2 / static_cast<size_t>(std::max(1, creator_rec_rate_)));
        char t[40];
        std::snprintf(t, sizeof t, " - REC %d:%02d", secs / 60, secs % 60);
        title += t;
    }
    std::ostringstream f;
    f << clear_prefix << "\x1b[?25l";
    f << box_top(truncate_str(title, W - 6), W, border) << "\n";
    const int rows = std::max(2, H - 2);
    const bool image = gfx_proto_ != GfxProto::None;
    if (scope) {
        // a square picture in the middle; its own look: the CRT beam in music mode (the exact figure), classic green phosphor
        int cw0 = 10, ch0 = 20;
        if (image) gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
        cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
        int cols = image ? static_cast<int>(std::lround(static_cast<double>(rows) * ch0 / cw0)) : rows * 2;
        if (creator_preview_kind_ == 2) cols = W - 4;   // the time trace uses the full width
        cols = std::clamp(cols, 4, W - 4);
        const int margin = std::max(0, (W - 2 - cols) / 2);
        OscilloscopeVisualizer::Params p;
        p.decay = std::pow(0.80f, static_cast<float>(std::clamp(viz_dt_, 0.004, 0.15) * 30.0));
        p.fixed_gain = !live;          // a file keeps its exact figure; the microphone is levelled automatically
        p.mono_phase = false;
        p.crt = true;
        p.dwell = !live;
        p.low_rate_blank = !live;
        p.trace = 1024;
        p.glow = 0.60f;
        p.mode = creator_preview_kind_ == 2 ? 1 : 0;
        const int gkind = creator_preview_kind_ == 2 ? 1 : 0;
        std::vector<std::string> pane;
        if (image) {
            const int scale = 2;
            const int cws = std::max(2, std::min(std::clamp(640 / std::max(1, cols), 4, cw0), cw0 / scale));
            const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
            GfxFrame& F = gfx_;
            F.id = 0; F.scale = scale; F.out_cw = cw0; F.out_ch = ch0; F.crop = 0; F.cut_w = 0;
            F.cols = cols; F.rows = rows; F.w = cols * cws; F.h = rows * chs;
            F.row = 1; F.col = 1 + margin;
            creator_scope_.render_image(F.w, F.h, p, F.level, F.hue);
            for (auto& c : F.pal) c = {90, 255, 130};
            std::fill(F.hue.begin(), F.hue.end(), static_cast<uint8_t>(0));
            if (creator_grid_) F.grid = scope_grid_pixels(F.w, F.h, cols, rows, gkind, 4, 4); else F.grid.clear();
            int gr = 128, gg = 128, gb = 128;
            try_parse_rgb(settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color, gr, gg, gb);
            F.grid_rgb = {static_cast<uint8_t>(gr), static_cast<uint8_t>(gg), static_cast<uint8_t>(gb)};
            F.active = true;
            F.fresh = gfx_due_;
            gfx2_.active = false;
            pane.assign(static_cast<size_t>(rows), gfx_hole(cols));
        } else {
            const std::string grid_ansi = ansi_for(settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color, false);
            auto color_of = [&](int, int, uint8_t, float k) { return osci_rgb_seq(90, 255, 130, k); };
            pane = scope_big_pane(creator_scope_, p, cols, rows, creator_grid_, gkind, grid_ansi, color_of, 4, 4);
        }
        for (int r = 0; r < rows; ++r)
            f << bar << std::string(static_cast<size_t>(margin), ' ') << (r < static_cast<int>(pane.size()) ? pane[static_cast<size_t>(r)] : std::string(static_cast<size_t>(cols), ' '))
              << std::string(static_cast<size_t>(std::max(0, W - 2 - margin - cols)), ' ') << bar << "\n";
    } else {
        SpectroAnalyzer& a = creator_spectro_;
        a.update(creator_spectro_set_);
        const SpectroSettings& sp = creator_spectro_set_;
        const int label_w = 6;
        const int cols = std::max(4, W - 2 - label_w - 1);
        const int srows = std::max(2, rows - 1);
        int cw = 10, chh = 20;
        if (image) gfx_cell_pixels(settings_.cell_pixels, cw, chh);
        const int unit = image ? std::max(1, chh) : 4;
        const auto labels = spectro_axis(sp, a.rate(), srows * unit, unit);
        std::vector<std::string> lab(static_cast<size_t>(srows));
        for (const auto& l : labels) if (l.first >= 0 && l.first < srows) lab[static_cast<size_t>(l.first)] = l.second;
        std::string ruler(static_cast<size_t>(cols), ' ');
        const int step = sp.span <= 5 ? 1 : sp.span <= 10 ? 2 : sp.span <= 30 ? 5 : 10;
        for (int t = 0; t <= sp.span; t += step) {
            const int x = static_cast<int>(static_cast<long long>(t) * (cols - 1) / std::max(1, sp.span));
            const std::string txt = std::to_string(t) + "s";
            const int x0 = std::clamp(x - (t == 0 ? 0 : t == sp.span ? static_cast<int>(txt.size()) - 1 : static_cast<int>(txt.size()) / 2), 0, std::max(0, cols - static_cast<int>(txt.size())));
            for (size_t k = 0; k < txt.size() && x0 + static_cast<int>(k) < cols; ++k) ruler[static_cast<size_t>(x0) + k] = txt[k];
        }
        f << bar << legend << std::string(static_cast<size_t>(label_w + 1), ' ') << ruler << R << bar << "\n";
        std::vector<std::string> body;
        if (image) {
            spectro_area_[0] = 2;
            spectro_area_[1] = 1 + label_w + 1;
            spectro_area_[2] = cols;
            spectro_area_[3] = srows;
        } else body = a.braille(sp, cols, srows);
        for (int r = 0; r < srows; ++r) {
            f << bar << legend << pad_left(lab[static_cast<size_t>(r)], label_w) << R << " ";
            if (image) f << gfx_hole(cols);
            else f << (r < static_cast<int>(body.size()) ? body[static_cast<size_t>(r)] : std::string(static_cast<size_t>(cols), ' '));
            f << bar << "\n";
        }
    }
    const std::string grid_hint = scope ? "  [G] grid " + std::string(creator_grid_ ? "off" : "on") : std::string();
    if (live) f << box_bottom(W, std::string("[ENTER] ") + (creator_rec_on_ ? "stop and save" : "record") + "  [V] view" + grid_hint + "  [ESC] close", border_bottom) << "\n";
    else f << box_bottom(W, "[ESC] stop and close  [SPACE] pause" + grid_hint + "  -- loops; your scope / spectrogram settings are not used", border_bottom) << "\n";
    return clamp_output_rows(f.str(), term_rows_);
}

} // namespace muisc
