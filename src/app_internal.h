#pragma once
// Helpers that app.cpp and the files split out of it (app_meta.cpp, app_history.cpp, app_readlyrics.cpp) both use.
// Only for those files: nothing here is part of the program's interface. The functions are defined in app.cpp
// (n_files_label in app_history.cpp).
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include "console_log.h"
#include "lyrics_fetcher.h"
#include "settings.h"
#include "themes.h"
#include "terminal_ui.h"

namespace muisc {

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
// "12 files" / "1 file" (defined in app_history.cpp)
std::string n_files_label(size_t n);

// scripts/<filename> next to the program (the same search as for the lyrics helper)
std::filesystem::path locate_script(const std::string& filename);

void theme_apply(Settings& s, const themes::Theme& t);   // sets the colours of a theme (app_settings.cpp)

// Hotkey strings (defined in app.cpp, used by the Settings screen)
std::string combo_key_name(int key);                  // "" when `key` is not a bindable combination
std::string normalize_hotkey_string(std::string s);   // what was typed into a key field, cleaned up

} // namespace muisc
