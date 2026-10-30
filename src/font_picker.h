#pragma once
// The FONT overlay (Settings -> GENERAL -> TEXT AND TRANSLITERATION -> Font, player and radio): Terminal Default, Cascadia
// Mono (Mousiki's default) and every monospace font installed on the computer; ENTER uses the highlighted one. One
// implementation for both modes, drawn with the REMOTE APP overlay's style (remote_overlay.h). What a font choice does is
// terminal_font.h.
#include "remote_overlay.h"
#include <string>
#include <vector>

namespace muisc::font_ui {

struct State {
    bool open = false;
    int sel = 0;
    std::vector<std::string> items;   // [0] Terminal Default, [1] Cascadia Mono, then the installed fonts
};

// Opens the overlay on `current` (the fonts are listed now: on macOS / Linux that can take a moment).
void open(State& st, const std::string& current);
// Fills the list without asking the system (tests, and a computer where listing fails): the two fixed rows plus `installed`.
void set_items(State& st, const std::string& current, const std::vector<std::string>& installed);

// One key. Returns true when a font was chosen (`picked`; the overlay closes). `close`: ESC. `page_rows`: rows of the list
// (SHIFT+arrows / PgUp / PgDn move by that much). A letter or digit jumps to the next font starting with it.
bool key(State& st, int k, bool arrow, bool shifted, int page_rows, std::string& picked, bool& close);

// The panel as lines of equal visible width (`width` returns it) that fit into max_w x max_h. `note`: one line about what
// the font does in this terminal (terminal_font::apply's answer, or what it would do).
std::vector<std::string> build(const State& st, const remote_ui::Style& s, const std::string& current, const std::string& note,
                               int max_w, int max_h, int& width);
// The number of list rows build() shows for max_h (for key()'s paging).
int list_rows(int max_h);

} // namespace muisc::font_ui
