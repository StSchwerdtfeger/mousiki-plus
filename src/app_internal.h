#pragma once
// Helpers that app.cpp and the files split out of it (app_meta.cpp, app_history.cpp, app_readlyrics.cpp) both use.
// Only for those files: nothing here is part of the program's interface. The functions are defined in app.cpp
// (n_files_label in app_history.cpp).
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "console_log.h"
#include "path_utf8.h"
#include "lyrics_fetcher.h"
#include "settings.h"
#include "themes.h"
#include "terminal_ui.h"

namespace muisc {

// When a file arrived in the library (later of modification and creation / change time); 0 when unreadable. app_history.cpp.
long long file_added_time(const fs::path& p);

// Worker-thread exception guard: whatever a background task throws is turned into a log line and that task alone fails
// (an exception that escapes a std::thread would call std::terminate() and end the whole program).
template <class F>
inline void run_guarded(const char* what, F&& body) noexcept {
    try {
        body();
    } catch (const std::exception& e) {
        ConsoleLog::instance().log_basic(std::string("internal error in ") + what + ": " + e.what());
    } catch (...) {
        ConsoleLog::instance().log_basic(std::string("internal error in ") + what + " (unknown exception)");
    }
}

// Removes exactly one full UTF-8 codepoint from the end of a text-entry buffer (not just its last byte).
inline void pop_utf8_char(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.erase(i);
}

// SGR for a "cursor" (hovering) row; reverse video when both colours are empty, so the cursor is never invisible.
inline std::string cursor_sgr(const std::string& fg, const std::string& bg) {
    std::string s = ansi_for(fg) + bg_ansi_for(bg);
    return s.empty() ? std::string("\x1b[7m") : s;
}

// ---- colours of the shared screen elements (defined in app.cpp) ----
std::string search_sgr(const Settings& s);        // the typed query in a search bar
std::string search_idle_sgr(const Settings& s);   // everything else in the bar
std::string search_run(const Settings& s, const std::string& text, bool nonempty);
std::string focus_sgr(const Settings& s);
std::string focus_indicator(const Settings& s, const std::string& border_ansi);
std::string strip_csi(const std::string& s);
std::string header_sgr(const Settings& s);
std::string legend_sgr(const Settings& s);
std::string legend_colored(const Settings& s, const std::string& text);
std::string paint_legend(const Settings& s, const std::vector<LegendRow>& rows, size_t i, const std::string& border, int pad_to, bool fade);
std::string change_sgr(const Settings& s);
// The tab strip of a menu overlay: two lines (the top border with the title and the tabs, the line below).
std::pair<std::string, std::string> menu_tab_strip(int W, const std::string& title, const std::vector<std::string>& labels,
                                                   int cur, const std::string& top_ansi, const std::string& bot_ansi,
                                                   const std::string& corner_ul, const std::string& corner_ur,
                                                   const std::string& vertical, int min_inner = 0,
                                                   const std::string& line2 = "", int line2_cols = 0,
                                                   const std::string& tab_cur = "", const std::string& tab_oth = "");
// Word-wraps one lyric line to `width` columns with the words already sung highlighted (defined in app.cpp).
std::vector<std::string> render_lyric_line_wrapped(const LyricLine& line, double elapsed, int width, bool is_active, const Settings& settings);
// A key as it is shown to the user ("ALT+L" -> the label of this platform), and word wrapping for the help texts (app.cpp).
std::string pretty_key(const std::string& k);
std::vector<std::string> wrap_words(const std::string& text, int width);
// "Chapter 2" before "Chapter 10" (audio book mode, app_audiobook.cpp)
bool natural_less(const std::string& a, const std::string& b);
// "12 files" / "1 file" (defined in app_history.cpp)
std::string n_files_label(size_t n);

// scripts/<filename> next to the program (the same search as for the lyrics helper)
std::filesystem::path locate_script(const std::string& filename);

void theme_apply(Settings& s, const themes::Theme& t);   // sets the colours of a theme (app_settings.cpp)

// Hotkey strings (defined in app.cpp, used by the Settings screen)
std::string combo_key_name(int key);                  // "" when `key` is not a bindable combination
std::string normalize_hotkey_string(std::string s);   // what was typed into a key field, cleaned up

// Floating-panel legends (defined here so every panel file uses the same ones; used by the EQ / oscilloscope / spectrogram panels and the
// audio book tag editor).
// A bottom border with its text centred in it (the dashes are split evenly on both sides).
inline std::string centred_bottom(const Settings& s, int W, const std::string& text, const std::string& colour) {
    const std::string t = " " + truncate_str(text, W - 6) + " ";
    const int dashes = std::max(0, W - 2 - display_width(t));
    const int left = dashes / 2, right = dashes - left;
    std::string out = s.box_lower_left;
    for (int i = 0; i < left; ++i) out += s.box_horizontal;
    out += t;
    for (int i = 0; i < right; ++i) out += s.box_horizontal;
    out += s.box_lower_right;
    return colour.empty() ? out : colour + out + "\x1b[0m";
}

// The legend of a floating option panel, all rows in the bottom border colour. With several rows the middle one sits ON the bottom
// border line (rows/2 rows inside the panel, the rest hanging below it), so the block is centred on that line. `inside(text)` makes
// a framed row, `bottom(text)` the bottom border carrying a text.
template <class Inside, class Bottom>
void push_legend_block(std::vector<std::string>& lines, int W, const std::string& colour, const std::vector<std::string>& rows,
                       Inside inside, Bottom bottom) {
    const size_t before = rows.size() / 2;
    for (size_t i = 0; i < before; ++i) lines.push_back(inside(rows[i]));
    lines.push_back(bottom(rows[before]));
    for (size_t i = before + 1; i < rows.size(); ++i) {
        const std::string tx = truncate_str(rows[i], W - 4);
        const int left = std::max(0, (W - display_width(tx)) / 2);
        lines.push_back(colour + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, W - left - display_width(tx))), ' ') + "\x1b[0m");
    }
}

// --- the audio book panels (app_audiobook_notes.cpp, app_audiobook_marks.cpp, app_audiobook_chapters.cpp) ---
// "Chapter 03.mp3" -> "Chapter 03"
inline std::string stem_of(const std::string& file) { return path_utf8(path_from_utf8(file).stem()); }

// One framed line: the exact `inner` columns of already coloured content between the two border bars.
struct Framer {
    std::string bar;
    int inner;
    std::string operator()(const std::string& body, int used) const {
        return bar + " " + body + std::string(static_cast<size_t>(std::max(0, inner - used)), ' ') + "\x1b[0m " + bar;
    }
};

// mm:ss, or h:mm:ss from an hour on
inline std::string clock_text(double sec) {
    const long long t = static_cast<long long>(std::max(0.0, sec) + 0.5);
    char b[32];
    if (t >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else std::snprintf(b, sizeof b, "%02lld:%02lld", t / 60, t % 60);
    return b;
}

} // namespace muisc
