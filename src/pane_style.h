// Settings -> GENERAL -> MAIN UI PANE STYLE: the frame of the upper pane (disk | meta data | lyrics / viz field).
//   0 one box (default): one rounded box round all three.
//   1 three panes I:     boxes that share their walls: a wall is the right edge of the pane before it (its top line ends in a corner and the wall
//                        runs down) and turns right at the bottom into the bottom line of the next pane; blanks after the corner on top and
//                        before the corner at the bottom:
//                            ╭────────╮ ───────╮ ──────╮
//                            │        │        │       │
//                            ╰──────── ╰─────── ╰─────╯
//   2 three panes II:    the middle pane is a box on top (╭──╮); at the bottom the first wall turns left into the first pane's line and the
//                        second turns right into the last pane's line; the middle pane's bottom line floats between them.
//   4 two panes:         like I, with one wall only: the disk, and the meta data + lyrics / viz field together in the second box.
//   5 gap:               one box with four gaps in its lines (two on top, two at the bottom) where the walls would be; no walls inside.
//   6 three panes T:     one box (as 0) with the two walls running through it, joined to the top line with a T (┬) and to the bottom line with
//                        an upside-down T (┴):
//                            ╭──────┬───────┬──────╮
//                            │      │       │      │
//                            ╰──────┴───────┴──────╯
//   3 three panes III:   steps: every wall runs from the top corner of the pane after it (╭) down to the bottom corner of the pane before it (╯);
//                        where the outer lines do not meet (top left, bottom right) the vertical line has one more row.
// The helpers make the rows from the columns of the walls (0-based, in a row of `w` columns), so the real panel and the preview in Settings
// draw exactly the same.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "settings.h"

namespace panestyle {

constexpr int kStyles = 7;

inline bool split(int style) { return style >= 1 && style <= 6; }
inline bool has_walls(int style) { return style != 5; }

struct Glyphs { std::string ul, ur, ll, lr, h, v; };
inline Glyphs glyphs(const muisc::Settings& s) { return {s.box_upper_left, s.box_upper_right, s.box_lower_left, s.box_lower_right, s.box_horizontal, s.box_vertical}; }

// half-height wall stub ending at the level of the horizontal line: down (top row) or up (bottom row)
inline std::string half(const std::string& v, bool down) {
    if (v == "\xE2\x94\x82") return down ? "\xE2\x95\xB7" : "\xE2\x95\xB5";   // │ -> ╷ / ╵
    if (v == "\xE2\x94\x83") return down ? "\xE2\x95\xBB" : "\xE2\x95\xB9";   // ┃ -> ╻ / ╹
    return v;
}

// the T piece where a wall meets the top (┬) or the bottom line (┴), in the weight of the box's lines (a double or a heavy box gets its own)
inline std::string tee(const Glyphs& g, bool top) {
    if (g.h == "\xE2\x95\x90") return top ? "\xE2\x95\xA6" : "\xE2\x95\xA9";   // ═ -> ╦ / ╩
    if (g.h == "\xE2\x94\x81") return top ? "\xE2\x94\xB3" : "\xE2\x94\xBB";   // ━ -> ┳ / ┻
    return top ? "\xE2\x94\xAC" : "\xE2\x94\xB4";                                    // ─ -> ┬ / ┴
}

// the top (top == true) or bottom border row; `walls`: the columns of the walls between the panes
inline std::string row(const Glyphs& g, int style, bool top, int w, const std::vector<int>& walls, const std::string& ansi) {
    std::vector<std::string> cell(static_cast<size_t>(std::max(0, w)), g.h);
    auto put = [&](int c, const std::string& s) { if (c >= 0 && c < w) cell[static_cast<size_t>(c)] = s; };
    if (style == 0) {
        put(0, top ? g.ul : g.ll); put(w - 1, top ? g.ur : g.lr);
    } else if (style == 6) {
        put(0, top ? g.ul : g.ll); put(w - 1, top ? g.ur : g.lr);
        for (int c : walls) put(c, tee(g, top));
    } else if (style == 5) {
        put(0, top ? g.ul : g.ll); put(w - 1, top ? g.ur : g.lr);
        // one blank each: at the left wall the disk's line reaches over the wall (the blank is after it), at the right wall the line of the
        // field reaches back over it (the blank is on it)
        for (size_t i = 0; i < walls.size(); ++i) put(walls[i] + ((walls.size() == 2 && i == 0) ? 1 : 0), " ");
    } else if (style == 1 || style == 4) {
        put(0, top ? g.ul : g.ll); put(w - 1, top ? g.ur : g.lr);
        for (int c : walls) { put(c, top ? g.ur : g.ll); put(top ? c + 1 : c - 1, " "); }
    } else if (style == 2) {
        put(0, top ? g.ul : g.ll); put(w - 1, top ? g.ur : g.lr);
        int k = 0;
        for (int c : walls) {
            const bool odd = (k++ % 2) == 0;   // (the first wall: ╭ on top and ╯ at the bottom; the second: ╮ and ╰)
            put(c, top ? (odd ? g.ul : g.ur) : (odd ? g.lr : g.ll));
            if (top) put(odd ? c - 1 : c + 1, " "); else put(odd ? c + 1 : c - 1, " ");
        }
    } else {
        if (top) { put(0, half(g.v, true)); put(1, " "); put(w - 1, g.ur); } else { put(0, g.ll); put(w - 1, half(g.v, false)); put(w - 2, " "); }
        for (int c : walls) { put(c, top ? g.ul : g.lr); put(top ? c - 1 : c + 1, " "); }
    }
    std::string r = ansi;
    for (const std::string& s : cell) r += s;
    return r + "\x1b[0m";
}

inline std::string wall(const Glyphs& g, const std::string& ansi) { return ansi + g.v + "\x1b[0m"; }
// the cell of a wall in a middle row: the wall, or (gap) a blank
inline std::string wall(const Glyphs& g, int style, const std::string& ansi) { return has_walls(style) ? wall(g, ansi) : std::string(" "); }

}  // namespace panestyle
