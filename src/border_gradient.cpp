// Colours the frame borders with a gradient: finds the border glyphs in a rendered line (box
// characters and their markers) and recolours them, keeping the text as it is.
#include "border_gradient.h"

#include <algorithm>

namespace border_gradient {

namespace {

struct Hit { size_t pos; size_t len; };

bool glyph_follows(const std::string& line, size_t at, const std::vector<std::string>& glyphs) {
    for (const std::string& g : glyphs) if (!g.empty() && line.compare(at, g.size(), g) == 0) return true;
    return false;
}

std::vector<Hit> find_hits(const std::string& line, const std::string& esc, const std::vector<std::string>& glyphs) {
    std::vector<Hit> hits;
    if (esc.empty()) return hits;
    for (size_t p = line.find(esc); p != std::string::npos; p = line.find(esc, p + 1))
        if (glyph_follows(line, p + esc.size(), glyphs)) hits.push_back({p, esc.size()});
    return hits;
}

} // namespace

std::vector<std::string> apply(const std::vector<std::string>& lines, const std::string& top_esc, const std::string& bottom_esc,
                               const std::vector<std::string>& glyphs, const std::function<std::string(float)>& color_at) {
    if (top_esc.empty() || bottom_esc.empty() || top_esc == bottom_esc) return lines;
    std::vector<std::vector<Hit>> hits(lines.size());
    int first = -1, last = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::vector<Hit> h = find_hits(lines[i], top_esc, glyphs);
        std::vector<Hit> b = find_hits(lines[i], bottom_esc, glyphs);
        h.insert(h.end(), b.begin(), b.end());
        if (h.empty()) continue;
        std::sort(h.begin(), h.end(), [](const Hit& x, const Hit& y) { return x.pos < y.pos; });
        hits[i] = std::move(h);
        if (first < 0) first = static_cast<int>(i);
        last = static_cast<int>(i);
    }
    if (first < 0) return lines;
    std::vector<std::string> out = lines;
    for (int i = first; i <= last; ++i) {
        const auto& h = hits[static_cast<size_t>(i)];
        if (h.empty()) continue;
        const float t = last > first ? static_cast<float>(i - first) / static_cast<float>(last - first) : 0.0f;
        const std::string col = color_at(t);
        std::string res;
        size_t from = 0;
        for (const Hit& x : h) {
            res.append(lines[static_cast<size_t>(i)], from, x.pos - from);
            res += col;
            from = x.pos + x.len;
        }
        res.append(lines[static_cast<size_t>(i)], from, std::string::npos);
        out[static_cast<size_t>(i)] = std::move(res);
    }
    return out;
}

std::string apply(const std::string& frame, const std::string& top_esc, const std::string& bottom_esc,
                  const std::vector<std::string>& glyphs, const std::function<std::string(float)>& color_at) {
    if (top_esc.empty() || bottom_esc.empty() || top_esc == bottom_esc) return frame;
    std::vector<std::string> lines;
    size_t from = 0;
    while (true) {
        const size_t nl = frame.find('\n', from);
        if (nl == std::string::npos) { lines.push_back(frame.substr(from)); break; }
        lines.push_back(frame.substr(from, nl - from));
        from = nl + 1;
    }
    lines = apply(lines, top_esc, bottom_esc, glyphs, color_at);
    std::string out;
    out.reserve(frame.size() + 64);
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) out += '\n';
    }
    return out;
}

} // namespace border_gradient
