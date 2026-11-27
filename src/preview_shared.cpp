#include "preview_shared.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace muisc {

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

void preview_chord_frames(double& t, size_t n, std::vector<float>& lr) {
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr double rate = 48000.0;
    lr.resize(n * 2);
    for (size_t i = 0; i < n; ++i, t += 1.0 / rate) {
        const double l = 0.30 * std::sin(kTwoPi * 220.0 * t + 2.0 * std::sin(kTwoPi * 0.20 * t)) + 0.20 * std::sin(kTwoPi * 277.18 * t);
        const double r = 0.30 * std::sin(kTwoPi * 329.63 * t + 2.0 * std::sin(kTwoPi * 0.13 * t)) + 0.20 * std::sin(kTwoPi * 220.0 * t);
        lr[2 * i] = static_cast<float>(l);
        lr[2 * i + 1] = static_cast<float>(r);
    }
}

PanePreview pane_preview(int style, int w, const char* name1, const char* name2, const char* name3, const panestyle::Glyphs& g,
                         const std::string& border, const std::string& border_bottom, const std::string& label_on, const std::string& label_off) {
    PanePreview out;
    style = std::clamp(style, 0, panestyle::kStyles - 1);
    const bool split = panestyle::split(style);
    const int c1 = split ? (style == 4 ? w / 3 : w / 4) : -1, c2 = split && style != 4 ? w * 5 / 8 : -1;   // (two panes: one wall)
    std::vector<int> walls;
    if (c1 >= 0) walls.push_back(c1);
    if (c2 >= 0) walls.push_back(c2);
    const std::string bar = panestyle::wall(g, border), mid = panestyle::wall(g, style, border);
    const int b1 = split ? c1 : w / 4, b2 = c2 >= 0 ? c2 : (c1 >= 0 ? c1 + (w - 1 - c1) / 2 : w * 5 / 8);   // (where the names are centred: between the walls; two panes: meta data and lyrics / viz share the second box)
    const struct { int from, to; const char* text; } names[] = {{1, b1, name1}, {b1 + 1, b2, name2}, {b2 + 1, w - 1, name3}};
    out.rows.push_back(panestyle::row(g, style, true, w, walls, border));
    for (int r = 0; r < 3; ++r) {
        std::string line = bar;
        for (int c = 1; c < w - 1; ++c) {
            if (c == c1 || c == c2) { line += mid; continue; }
            bool placed = false;
            if (r == 1) for (const auto& n : names) {
                const int len = static_cast<int>(std::strlen(n.text)), start = n.from + std::max(0, (n.to - n.from - len) / 2);
                if (c == start && start + len <= n.to) { line += label_on + n.text + label_off; c += len - 1; placed = true; break; }
            }
            if (!placed) line += ' ';
        }
        out.rows.push_back(line + bar);
    }
    out.rows.push_back(panestyle::row(g, style, false, w, walls, border_bottom));
    static const char* const kCaptions[] = {"one box", "three panes I", "three panes II", "three panes III", "two panes", "gap", "three panes T"};
    out.caption = kCaptions[style];
    return out;
}

}  // namespace muisc
