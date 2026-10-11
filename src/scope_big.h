#pragma once
// The big oscilloscope overlay (SHIFT+0 in the player and the radio): the YT trace on the left, the XY figure on the right,
// each in its own phosphor buffer, with an optional grid. Header only and templated on the scope class, because the player
// (OscilloscopeVisualizer) and the radio (RadioScope) each have their own and the drawing is exactly the same.
//
// A pane is drawn in braille cells like the scope in the lyrics area. Where a cell has trace dots it takes the trace's colour
// (and brightness); an empty cell shows the grid's dots in the grid colour, so the grid never changes how the trace looks.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "scope_metrics.h"

namespace muisc {

// kind 0 = XY, 1 = YT. The braille pattern of the grid for every cell of a cols x rows pane. `xdiv` / `ydiv` = the divisions
// across the width / height (the lines between them are dotted); both pictures are drawn as a box with a solid frame:
//   XY: a square like the figure itself (side = the smaller of width and height, the beam uses 94 % of it): the frame around
//       +-100 %, the two axes through the centre (solid) and dotted lines at every division.
//   YT: the same frame over the whole width (top and bottom line straight), the zero line (solid), dotted lines at every
//       division of the amplitude and a dotted vertical line at every time division.
inline std::vector<std::vector<uint8_t>> scope_grid_cells(int cols, int rows, int kind, int xdiv = 4, int ydiv = 4) {
    static const uint8_t kMap[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    std::vector<std::vector<uint8_t>> g(static_cast<size_t>(std::max(0, rows)), std::vector<uint8_t>(static_cast<size_t>(std::max(0, cols)), 0));
    const int pw = cols * 2, ph = rows * 4;
    if (pw <= 0 || ph <= 0) return g;
    xdiv = std::max(1, xdiv);
    ydiv = std::max(1, ydiv);
    auto dot = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= pw || y >= ph) return;
        g[static_cast<size_t>(y / 4)][static_cast<size_t>(x / 2)] |= kMap[y % 4][x % 2];
    };
    const float cx = (pw - 1) * 0.5f, cy = (ph - 1) * 0.5f;
    auto hline = [&](float y, int x0, int x1, bool solid) {
        const int yy = static_cast<int>(std::lround(y));
        for (int x = x0; x <= x1; ++x) if (solid || (x % 4) == 0) dot(x, yy);
    };
    auto vline = [&](float x, int y0, int y1, bool solid) {
        const int xx = static_cast<int>(std::lround(x));
        for (int y = y0; y <= y1; ++y) if (solid || (y % 2) == 0) dot(xx, y);
    };
    const float amp_y = (kind == 0 ? (std::min(pw, ph) - 1) : (ph - 1)) * 0.5f * 0.94f;
    float amp_x;
    if (kind == 0) amp_x = amp_y; else amp_x = cx;   // XY: the square; YT: the whole width
    const int x0 = static_cast<int>(std::lround(cx - amp_x)), x1 = static_cast<int>(std::lround(cx + amp_x));
    const int y0 = static_cast<int>(std::lround(cy - amp_y)), y1 = static_cast<int>(std::lround(cy + amp_y));
    for (int i = 1; i < ydiv; ++i) hline(cy - amp_y + 2.0f * amp_y * static_cast<float>(i) / static_cast<float>(ydiv), x0, x1, false);
    for (int i = 1; i < xdiv; ++i) vline(cx - amp_x + 2.0f * amp_x * static_cast<float>(i) / static_cast<float>(xdiv), y0, y1, false);
    hline(cy, x0, x1, true);                  // the axes
    if (kind == 0) vline(cx, y0, y1, true);
    hline(static_cast<float>(y0), x0, x1, true);   // the frame
    hline(static_cast<float>(y1), x0, x1, true);
    vline(static_cast<float>(x0), y0, y1, true);
    vline(static_cast<float>(x1), y0, y1, true);
    return g;
}

// The same grid as scope_grid_cells(), as pixels, for the image style: a w x h mask (1 = grid) of a picture that covers
// cols x rows cells. The geometry is that of the braille grid (2 x 4 dots per cell), scaled to the pixels; the dotted lines
// keep their rhythm (one dot lit, three dark / one lit, one dark), the frame and the axes are solid (2 pixels thick on a big picture).
inline std::vector<uint8_t> scope_grid_pixels(int w, int h, int cols, int rows, int kind, int xdiv = 4, int ydiv = 4) {
    std::vector<uint8_t> g(static_cast<size_t>(std::max(0, w)) * static_cast<size_t>(std::max(0, h)), 0);
    const int pw = cols * 2, ph = rows * 4;
    if (w <= 0 || h <= 0 || pw <= 0 || ph <= 0) return g;
    xdiv = std::max(1, xdiv);
    ydiv = std::max(1, ydiv);
    const float sx = static_cast<float>(w) / static_cast<float>(pw), sy = static_cast<float>(h) / static_cast<float>(ph);   // pixels per dot
    auto px_of = [&](float dot) { return static_cast<int>(std::lround((dot + 0.5f) * sx - 0.5f)); };
    auto py_of = [&](float dot) { return static_cast<int>(std::lround((dot + 0.5f) * sy - 0.5f)); };
    const int thick = h >= 240 ? 2 : 1;
    auto put = [&](int x, int y) { if (x >= 0 && y >= 0 && x < w && y < h) g[static_cast<size_t>(y) * w + x] = 1; };
    auto hline = [&](int y, int x0, int x1, bool solid) {
        const int t = solid ? thick : 1;
        for (int x = x0; x <= x1; ++x) {
            if (!solid && (static_cast<int>(std::floor(static_cast<float>(x) / sx)) % 4) != 0) continue;
            for (int k = 0; k < t; ++k) put(x, y - t / 2 + k);
        }
    };
    auto vline = [&](int x, int y0, int y1, bool solid) {
        const int t = solid ? thick : 1;
        for (int y = y0; y <= y1; ++y) {
            if (!solid && (static_cast<int>(std::floor(static_cast<float>(y) / sy)) % 2) != 0) continue;
            for (int k = 0; k < t; ++k) put(x - t / 2 + k, y);
        }
    };
    const float cx = (pw - 1) * 0.5f, cy = (ph - 1) * 0.5f;
    const float amp_y = (kind == 0 ? (std::min(pw, ph) - 1) : (ph - 1)) * 0.5f * 0.94f;
    const float amp_x = kind == 0 ? amp_y : cx;
    const int x0 = px_of(cx - amp_x), x1 = px_of(cx + amp_x), y0 = py_of(cy - amp_y), y1 = py_of(cy + amp_y);
    for (int i = 1; i < ydiv; ++i) hline(py_of(cy - amp_y + 2.0f * amp_y * static_cast<float>(i) / static_cast<float>(ydiv)), x0, x1, false);
    for (int i = 1; i < xdiv; ++i) vline(px_of(cx - amp_x + 2.0f * amp_x * static_cast<float>(i) / static_cast<float>(xdiv)), y0, y1, false);
    hline(py_of(cy), x0, x1, true);
    if (kind == 0) vline(px_of(cx), y0, y1, true);
    hline(y0 + thick / 2, x0, x1, true);   // the frame, kept inside the picture
    hline(y1 - thick / 2, x0, x1, true);
    vline(x0 + thick / 2, y0, y1, true);
    vline(x1 - thick / 2, y0, y1, true);
    return g;
}

inline void scope_put_braille(std::string& out, uint8_t pattern) {
    const int cp = 0x2800 + pattern;
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
}

// One pane as `rows` lines of exactly `cols` columns. `p.mode` decides what it shows (the caller sets 1 for the YT pane, 0 for XY).
// color_of(cell_x, cols, hue, k) returns the escape sequence for a trace cell: k = 0.30 .. 1.00 is its brightness.
template <class Scope, class ColorFn>
std::vector<std::string> scope_big_pane(Scope& sc, const typename Scope::Params& p, int cols, int rows, bool grid, int kind,
                                        const std::string& grid_ansi, ColorFn color_of, int xdiv = 4, int ydiv = 4) {
    std::vector<std::string> out;
    if (cols <= 0 || rows <= 0) return out;
    const auto cells = sc.render(cols, rows, p);
    const auto gc = grid ? scope_grid_cells(cols, rows, kind, xdiv, ydiv) : std::vector<std::vector<uint8_t>>();
    for (int ty = 0; ty < rows; ++ty) {
        std::string line, last;
        for (int tx = 0; tx < cols; ++tx) {
            const auto& c = cells[static_cast<size_t>(ty)][static_cast<size_t>(tx)];
            if (c.braille != 0) {
                const float lvl = std::round(c.level / 255.0f * 7.0f) / 7.0f;   // 8 brightness steps keep the escape traffic small
                const std::string a = color_of(tx, cols, c.hue, 0.30f + 0.70f * lvl);
                if (a != last) { line += a; last = a; }
                scope_put_braille(line, c.braille);
            } else if (grid && gc[static_cast<size_t>(ty)][static_cast<size_t>(tx)] != 0) {
                if (grid_ansi != last) { line += grid_ansi; last = grid_ansi; }
                scope_put_braille(line, gc[static_cast<size_t>(ty)][static_cast<size_t>(tx)]);
            } else line += ' ';
        }
        out.push_back(line + "\x1b[0m");
    }
    return out;
}

// Geometry of the overlay. Both pictures are "boxes" of exactly the same size, wide enough that they look square on screen
// (a cell is about twice as high as wide, so a box has twice as many columns as rows and its dot grid is square: 4*rows dots
// high, 2*cols = 4*rows dots wide). The XY figure fills its box, the YT trace gets the same box, so the horizontal grid lines of
// both pictures sit on the same screen rows. The box shrinks with the terminal and keeps its shape; what is left over is margin.
// With the axis values on, every box gets a column of y values on its left (`lab`) and a row of x values below (`xrow`).
struct ScopeBigGeom {
    int rows = 2, cols = 4;   // size of each box in cells
    int lab = 0;              // width of the y value column left of each box (0 = values off)
    int xrow = 0;             // 1 = a row of x values below the boxes
    int note = 0;             // 1 = a row below that explaining what 100 % means
    int gap = 1;              // columns between the left block and the right block
    int margin = 0;           // blank columns left of the left block
    int top = 0;              // blank rows above the header row
};

// `inner` = columns between the frame's side bars, `avail` = rows between the top and the bottom border (header row included).
inline ScopeBigGeom scope_big_geometry(int inner, int avail, bool values) {
    ScopeBigGeom g;
    g.lab = values ? 7 : 0;
    g.xrow = values ? 1 : 0;
    g.note = values ? 1 : 0;
    g.gap = values ? 1 : 3;
    const int by_height = avail - 1 - g.xrow - g.note;                // header row, box rows, value row, note row
    const int by_width = (inner - 2 * g.lab - g.gap) / 4;              // two boxes of 2*rows columns
    g.rows = std::max(2, std::min(by_height, by_width));
    g.cols = g.rows * 2;
    const int used_w = 2 * g.lab + 2 * g.cols + g.gap;
    g.margin = std::max(0, (inner - used_w) / 2);
    g.top = std::max(0, (avail - (1 + g.rows + g.xrow + g.note)) / 2);
    return g;
}

// `text` cut to `w` columns and centred in a field of exactly `w` columns (ASCII only).
inline std::string scope_centered(const std::string& text, int w) {
    if (w <= 0) return std::string();
    const std::string t = static_cast<int>(text.size()) > w ? text.substr(0, static_cast<size_t>(w)) : text;
    const int left = (w - static_cast<int>(t.size())) / 2;
    return std::string(static_cast<size_t>(left), ' ') + t + std::string(static_cast<size_t>(w - left - static_cast<int>(t.size())), ' ');
}

// The axis values of a box, always percent of the picture's scale (100 % = the loudest part, or full scale in music mode).
// The y values (+100 % at the top .. -100 % at the bottom, `div` divisions): one string of exactly `lab` columns for every box row
// (blank where none; where two values would share a row the upper one stays).
inline std::vector<std::string> scope_ylabels(int rows, int lab, int div = 4) {
    std::vector<std::string> out(static_cast<size_t>(std::max(0, rows)), std::string(static_cast<size_t>(std::max(0, lab)), ' '));
    const int ph = rows * 4;
    if (ph <= 0 || lab < 2) return out;
    div = std::max(1, div);
    const float cy = (ph - 1) * 0.5f, amp = (ph - 1) * 0.5f * 0.94f;
    for (int i = 0; i <= div; ++i) {
        const double pct = 100.0 - 200.0 * i / div;
        const int ty = static_cast<int>(std::lround(cy - static_cast<float>(pct / 100.0) * amp)) / 4;
        if (ty < 0 || ty >= rows) continue;
        if (out[static_cast<size_t>(ty)].find_first_not_of(' ') != std::string::npos) continue;
        std::string t = scope_value_text(pct);
        if (static_cast<int>(t.size()) > lab - 1) t = t.substr(0, static_cast<size_t>(lab - 1));
        out[static_cast<size_t>(ty)] = std::string(static_cast<size_t>(lab - 1 - static_cast<int>(t.size())), ' ') + t + " ";
    }
    return out;
}

// The row of x values below a box (exactly `cols` columns), at as many divisions as fit with a blank column between the texts.
// kind 0 = XY (the horizontal axis, `xdiv` divisions, percent); kind 1 = YT (`xdiv` time divisions;
// tunit 0 = milliseconds, 1 = samples, 2 = divisions; `ms` = the time the whole width shows, `samples` = its samples).
inline std::string scope_xlabels(int cols, int kind, double ms, int xdiv = 8, int tunit = 0, int samples = 1024) {
    std::string line(static_cast<size_t>(std::max(0, cols)), ' ');
    const int pw = cols * 2;
    if (pw <= 0) return line;
    xdiv = std::max(1, xdiv);
    const float cx = (pw - 1) * 0.5f;
    const float amp = kind == 0 ? (pw - 1) * 0.5f * 0.94f : cx;   // the XY box is square: its side is the width
    auto text = [&](int i) {
        if (kind == 0) return scope_value_text(-100.0 + 200.0 * i / xdiv);
        char b[24];
        if (i == 0) return std::string("0");
        if (tunit == 1) std::snprintf(b, sizeof b, "%.0f", static_cast<double>(samples) * i / xdiv);
        else if (tunit == 2) std::snprintf(b, sizeof b, "%d", i);
        else std::snprintf(b, sizeof b, "%.1f", ms * i / xdiv);
        return std::string(b);
    };
    for (int step = 1; step <= xdiv; ++step) {
        if (xdiv % step != 0) continue;
        std::vector<std::pair<int, std::string>> marks;   // first column, text
        bool fit = true;
        int free_from = 0;
        for (int i = 0; i <= xdiv && fit; i += step) {
            const std::string t = text(i);
            const int len = static_cast<int>(t.size());
            const int px = static_cast<int>(std::lround(cx - amp + 2.0f * amp * static_cast<float>(i) / static_cast<float>(xdiv)));
            const int start = std::clamp(px / 2 - len / 2, 0, std::max(0, cols - len));
            if (start < free_from || start + len > cols) fit = false;
            else { marks.emplace_back(start, t); free_from = start + len + 1; }
        }
        if (!fit && step < xdiv) continue;
        if (!fit) marks.clear();
        for (const auto& m : marks) line.replace(static_cast<size_t>(m.first), m.second.size(), m.second);
        break;
    }
    return line;
}

// What the overlay shows besides the pictures: the metrics, the scales the 100 % marks stand for and the measurements.
struct ScopeBigInfo {
    ScopeMetrics m;
    bool rotate = false;            // the XY figure shows mid / side instead of left / right
    bool music = false;             // Osci music mode: 100 % is full scale (else the loudest part of the signal)
    double ms = 0.0;                // the time the YT picture's width shows
    int samples = 1024;             // ... and its samples
    double rms_db = -120.0, peak_db = -120.0, corr = 0.0, bal_db = 0.0;   // the steady readouts (see the scope's measure())
};

// The first of `cands` (longest first) that fits in `w` columns; the last one cut when none does.
inline std::string scope_pick_head(const std::vector<std::string>& cands, int w) {
    for (const std::string& c : cands) if (static_cast<int>(c.size()) <= w) return c;
    return cands.empty() ? std::string() : cands.back().substr(0, static_cast<size_t>(std::max(0, w)));
}

// The picture names above the boxes, as short as the width needs ("samp.", "horiz.", "vert."), with the chosen readouts.
inline std::string scope_head_yt(const ScopeBigInfo& in, int w) {
    char b[64];
    std::string main_t, paren;
    if (in.m.yt_time_unit == 1) {
        main_t = std::to_string(in.samples) + " samp.";
        std::snprintf(b, sizeof b, " (%.1f ms)", in.ms); paren = b;
    } else if (in.m.yt_time_unit == 2) {
        std::snprintf(b, sizeof b, "%.2f ms/div", in.ms / std::max(1, in.m.yt_time_div)); main_t = b;
        paren = " (" + std::to_string(in.m.yt_time_div) + " div)";
    } else {
        std::snprintf(b, sizeof b, "%.1f ms", in.ms); main_t = b;
        paren = " (" + std::to_string(in.samples) + " samp.)";
    }
    std::string info;
    if (in.m.yt_info & 1) info += "  RMS " + scope_db_text(in.rms_db);
    if (in.m.yt_info & 2) info += "  peak " + scope_db_text(in.peak_db);
    return scope_pick_head({"YT  time -> " + main_t + paren + info, "YT  " + main_t + paren + info, "YT  " + main_t + info, "YT  " + main_t, "YT" + info}, w);
}

inline std::string scope_head_xy(const ScopeBigInfo& in, int w) {
    std::string info;
    char b[48];
    if (in.m.xy_info & 1) { std::snprintf(b, sizeof b, "  corr %+.1f", in.corr); info += b; }
    if (in.m.xy_info & 2) {
        if (in.bal_db > 0.5) std::snprintf(b, sizeof b, "  bal L+%.0f dB", in.bal_db);
        else if (in.bal_db < -0.5) std::snprintf(b, sizeof b, "  bal R+%.0f dB", -in.bal_db);
        else std::snprintf(b, sizeof b, "  bal 0 dB");
        info += b;
    }
    const std::string axes = in.rotate ? "side = horiz., mid = vert." : "L = horiz., R = vert.";
    const std::string axes2 = in.rotate ? "side horiz., mid vert." : "L horiz., R vert.";
    return scope_pick_head({"XY  " + axes + info, "XY  " + axes2 + info, "XY" + info, "XY"}, w);
}

// The line below a box that says what the percentages mean.
inline std::string scope_note_text(const ScopeBigInfo& in, int kind, int w) {
    const std::string what = in.music ? "full scale" : "loudest part";
    return scope_pick_head({std::string(kind == 1 ? "vert. " : "horiz. + vert. ") + "100% = " + what, "100% = " + what, "100% = max."}, w);
}

// The body of the overlay: `avail` lines of exactly `inner` visible columns (blank rows, the picture names centred above their
// boxes, the boxes with their y values, the row of x values). left/right are the two panes from scope_big_pane() at g.cols x g.rows.
inline std::vector<std::string> scope_big_compose(const ScopeBigGeom& g, int inner, int avail, const std::vector<std::string>& left,
                                                  const std::vector<std::string>& right, const std::string& head_ansi,
                                                  const std::string& label_ansi, const ScopeBigInfo& in) {
    const std::string R = "\x1b[0m";
    const std::string blank(static_cast<size_t>(std::max(0, inner)), ' ');
    const std::string gap(static_cast<size_t>(g.gap), ' ');
    const std::string margin(static_cast<size_t>(g.margin), ' ');
    const int used_w = 2 * g.lab + 2 * g.cols + g.gap;
    const std::string rest(static_cast<size_t>(std::max(0, inner - g.margin - used_w)), ' ');
    const std::string pad_lab(static_cast<size_t>(g.lab), ' ');
    std::vector<std::string> out;
    auto at = [&](const std::vector<std::string>& v, int i) { return i < static_cast<int>(v.size()) ? v[static_cast<size_t>(i)] : std::string(static_cast<size_t>(g.cols), ' '); };
    for (int i = 0; i < g.top; ++i) out.push_back(blank);
    out.push_back(margin + pad_lab + head_ansi + scope_centered(scope_head_yt(in, g.cols), g.cols) + R + gap + pad_lab + head_ansi +
                  scope_centered(scope_head_xy(in, g.cols), g.cols) + R + rest);
    const auto yl_l = scope_ylabels(g.rows, g.lab, in.m.yt_value_div);
    const auto yl_r = scope_ylabels(g.rows, g.lab, in.m.xy_div);
    for (int r = 0; r < g.rows; ++r) {
        const std::string ly = g.lab > 0 ? label_ansi + yl_l[static_cast<size_t>(r)] + R : std::string();
        const std::string ry = g.lab > 0 ? label_ansi + yl_r[static_cast<size_t>(r)] + R : std::string();
        out.push_back(margin + ly + at(left, r) + gap + ry + at(right, r) + rest);
    }
    if (g.xrow) {
        out.push_back(margin + pad_lab + label_ansi +
                      scope_xlabels(g.cols, 1, in.ms, in.m.yt_time_div, in.m.yt_time_unit, in.samples) + R + gap +
                      pad_lab + label_ansi + scope_xlabels(g.cols, 0, in.ms, in.m.xy_div) + R + rest);
    }
    if (g.note) {
        out.push_back(margin + pad_lab + label_ansi + scope_centered(scope_note_text(in, 1, g.cols), g.cols) + R + gap +
                      pad_lab + label_ansi + scope_centered(scope_note_text(in, 0, g.cols), g.cols) + R + rest);
    }
    while (static_cast<int>(out.size()) < avail) out.push_back(blank);
    if (static_cast<int>(out.size()) > avail) out.resize(static_cast<size_t>(avail));
    return out;
}

} // namespace muisc
