#include "creator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <unordered_map>

#include "glyph_raster.h"
#include "karaoke_font.h"
#include "path_utf8.h"
#include "utf8_util.h"

namespace muisc::creator {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

// ===========================================================================================================
// pictures
// ===========================================================================================================

bool parse_pgm(const std::string& data, Gray& out) {
    out = Gray();
    size_t i = 0;
    auto skip_ws = [&]() {
        while (i < data.size()) {
            if (data[i] == '#') { while (i < data.size() && data[i] != '\n') ++i; }
            else if (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n') ++i;
            else break;
        }
    };
    auto number = [&](long& v) {
        skip_ws();
        if (i >= data.size() || data[i] < '0' || data[i] > '9') return false;
        v = 0;
        while (i < data.size() && data[i] >= '0' && data[i] <= '9') {
            v = v * 10 + (data[i] - '0');
            if (v > 100000) return false;
            ++i;
        }
        return true;
    };
    if (data.size() < 9 || data[0] != 'P' || data[1] != '5') return false;
    i = 2;
    long w = 0, h = 0, maxv = 0;
    if (!number(w) || !number(h) || !number(maxv)) return false;
    if (w <= 0 || h <= 0 || maxv <= 0 || maxv > 255) return false;
    ++i;   // the single white space after the maximum value
    const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (data.size() < i + need) return false;
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.px.assign(need, 0);
    for (size_t k = 0; k < need; ++k) {
        const unsigned v = static_cast<unsigned char>(data[i + k]);
        out.px[k] = static_cast<uint8_t>(maxv == 255 ? v : std::min<unsigned>(255, v * 255 / static_cast<unsigned>(maxv)));
    }
    return true;
}

namespace {

struct TextGlyph {
    bool pixel = false;            // from the 5x7 font (5 columns, 8 rows) -- otherwise from `raster`
    const uint8_t* rows = nullptr;
    RasterGlyph raster;
    bool space = false;
};

int glyph_advance(const TextGlyph& g, int scale) {
    if (g.space) return 4 * scale;
    if (g.pixel) return 6 * scale;
    return std::max(1, g.raster.w) + scale;
}

std::vector<std::vector<char32_t>> split_lines(const std::string& utf8) {
    std::vector<std::vector<char32_t>> lines(1);
    size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_decode(utf8, i);
        if (cp == '\n') lines.emplace_back();
        else if (cp == '\r' || cp == '\t') lines.back().push_back(U' ');
        else if (cp >= 32) lines.back().push_back(cp);
    }
    return lines;
}

}  // namespace

Gray gray_from_text(const std::string& utf8, int scale) {
    scale = std::clamp(scale, 1, 16);
    const auto lines = split_lines(utf8);
    for (int sc = scale; sc >= 1; --sc) {
        // glyphs for this scale
        std::vector<std::vector<TextGlyph>> glyphs;
        int max_w = 0;
        bool any = false;
        for (const auto& line : lines) {
            std::vector<TextGlyph> gl;
            int w = 0;
            for (char32_t cp : line) {
                TextGlyph g;
                if (cp == U' ') {
                    g.space = true;
                } else if (const uint8_t* rows = karaoke_glyph(cp)) {
                    g.pixel = true;
                    g.rows = rows;
                    any = true;
                } else if (glyph_rasterize(cp, 8 * sc, g.raster) && g.raster.w > 0) {
                    any = true;
                } else {
                    continue;                     // no font has it: left out
                }
                w += glyph_advance(g, sc);
                gl.push_back(std::move(g));
            }
            max_w = std::max(max_w, w);
            glyphs.push_back(std::move(gl));
        }
        if (!any) return Gray();
        const int margin = 2 * sc;
        const int width = max_w + 2 * margin;
        if (width > 3000 && sc > 1) continue;     // too wide: a smaller scale
        const int line_h = 10 * sc;
        const int height = static_cast<int>(glyphs.size()) * line_h + 2 * margin - 2 * sc;
        Gray out;
        out.w = width;
        out.h = std::max(1, height);
        out.px.assign(static_cast<size_t>(out.w) * static_cast<size_t>(out.h), 0);
        auto put = [&](int x, int y) {
            if (x >= 0 && y >= 0 && x < out.w && y < out.h) out.px[static_cast<size_t>(y) * static_cast<size_t>(out.w) + static_cast<size_t>(x)] = 255;
        };
        for (size_t li = 0; li < glyphs.size(); ++li) {
            int x = margin;
            const int y0 = margin + static_cast<int>(li) * line_h;
            for (const TextGlyph& g : glyphs[li]) {
                if (g.pixel) {
                    for (int row = 0; row < 8; ++row)
                        for (int col = 0; col < 5; ++col)
                            if (g.rows[row] & (1 << (4 - col)))
                                for (int dy = 0; dy < sc; ++dy)
                                    for (int dx = 0; dx < sc; ++dx) put(x + col * sc + dx, y0 + row * sc + dy);
                } else if (!g.space) {
                    for (int yy = 0; yy < g.raster.h; ++yy)
                        for (int xx = 0; xx < g.raster.w; ++xx)
                            if (g.raster.dots[static_cast<size_t>(yy) * static_cast<size_t>(g.raster.w) + static_cast<size_t>(xx)]) put(x + xx, y0 + yy);
                }
                x += glyph_advance(g, sc);
            }
        }
        return out;
    }
    return Gray();
}

Gray gray_resized(const Gray& g, int W, int H) {
    Gray out;
    if (g.empty() || W <= 0 || H <= 0 || W > 20000 || H > 20000) return out;
    out.w = W;
    out.h = H;
    out.px.assign(static_cast<size_t>(W) * static_cast<size_t>(H), 0);
    const double sx = static_cast<double>(g.w) / W, sy = static_cast<double>(g.h) / H;
    for (int y = 0; y < H; ++y) {
        const double y0 = y * sy, y1 = (y + 1) * sy;
        const int iy0 = static_cast<int>(std::floor(y0)), iy1 = std::min(g.h - 1, static_cast<int>(std::ceil(y1)) - 1);
        for (int x = 0; x < W; ++x) {
            const double x0 = x * sx, x1 = (x + 1) * sx;
            const int ix0 = static_cast<int>(std::floor(x0)), ix1 = std::min(g.w - 1, static_cast<int>(std::ceil(x1)) - 1);
            double sum = 0, wsum = 0;
            for (int yy = iy0; yy <= iy1; ++yy) {
                const double wy = std::min(y1, yy + 1.0) - std::max(y0, static_cast<double>(yy));
                if (wy <= 0) continue;
                for (int xx = ix0; xx <= ix1; ++xx) {
                    const double wx = std::min(x1, xx + 1.0) - std::max(x0, static_cast<double>(xx));
                    if (wx <= 0) continue;
                    sum += g.at(xx, yy) * wx * wy;
                    wsum += wx * wy;
                }
            }
            out.px[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(wsum > 0 ? std::clamp(sum / wsum + 0.5, 0.0, 255.0) : 0);
        }
    }
    return out;
}

Gray gray_inverted(const Gray& g) {
    Gray out = g;
    for (auto& v : out.px) v = static_cast<uint8_t>(255 - v);
    return out;
}

bool gray_is_mostly_bright(const Gray& g) {
    if (g.empty()) return false;
    unsigned long long sum = 0;
    for (uint8_t v : g.px) sum += v;
    return static_cast<double>(sum) / static_cast<double>(g.px.size()) > 127.5;
}

int otsu_threshold(const Gray& g) {
    if (g.empty()) return 128;
    std::vector<double> hist(256, 0.0);
    for (uint8_t v : g.px) hist[v] += 1.0;
    const double total = static_cast<double>(g.px.size());
    double sum_all = 0;
    for (int t = 0; t < 256; ++t) sum_all += t * hist[static_cast<size_t>(t)];
    double w_b = 0, sum_b = 0, best = -1;
    int best_t = 128;
    for (int t = 0; t < 256; ++t) {
        w_b += hist[static_cast<size_t>(t)];
        if (w_b <= 0) continue;
        const double w_f = total - w_b;
        if (w_f <= 0) break;
        sum_b += t * hist[static_cast<size_t>(t)];
        const double m_b = sum_b / w_b, m_f = (sum_all - sum_b) / w_f;
        const double between = w_b * w_f * (m_b - m_f) * (m_b - m_f);
        if (between > best) { best = between; best_t = t; }
    }
    return best_t;
}

std::vector<std::string> braille_preview(const Gray& g, int cols, int rows, int threshold, bool invert) {
    std::vector<std::string> out;
    if (g.empty() || cols < 1 || rows < 1) return out;
    const int DW = cols * 2, DH = rows * 4;
    const double scale = std::min(static_cast<double>(DW) / g.w, static_cast<double>(DH) / g.h);
    const int sw = std::max(1, std::min(DW, static_cast<int>(std::lround(g.w * scale))));
    const int sh = std::max(1, std::min(DH, static_cast<int>(std::lround(g.h * scale))));
    const Gray small = gray_resized(g, sw, sh);
    const int ox = (DW - sw) / 2, oy = (DH - sh) / 2;
    auto dot = [&](int x, int y) {
        const int sx = x - ox, sy = y - oy;
        if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) return false;
        const int v = invert ? 255 - small.at(sx, sy) : small.at(sx, sy);
        return v > threshold;
    };
    static const int bit[2][4] = {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}};
    for (int cy = 0; cy < rows; ++cy) {
        std::string line;
        for (int cx = 0; cx < cols; ++cx) {
            int mask = 0;
            for (int dx = 0; dx < 2; ++dx)
                for (int dy = 0; dy < 4; ++dy)
                    if (dot(cx * 2 + dx, cy * 4 + dy)) mask |= bit[dx][dy];
            const unsigned cp = 0x2800u + static_cast<unsigned>(mask);
            line += static_cast<char>(0xE0 | (cp >> 12));
            line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            line += static_cast<char>(0x80 | (cp & 0x3F));
        }
        out.push_back(std::move(line));
    }
    return out;
}

// ===========================================================================================================
// outlines
// ===========================================================================================================

namespace {

// The marching-squares cell, see trace_outlines().
struct Seg { int a, b; };

double dist2(const Pt& a, const Pt& b) {
    const double dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// Distance from p to the line a-b (to the point a when a == b).
double point_line_dist(const Pt& p, const Pt& a, const Pt& b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    if (len2 < 1e-12) return std::sqrt(dist2(p, a));
    const double cross = std::fabs((p.x - a.x) * dy - (p.y - a.y) * dx);
    return cross / std::sqrt(len2);
}

// Douglas-Peucker on the open polyline pts[lo..hi]; marks the points that stay.
void dp_mark(const std::vector<Pt>& pts, size_t lo, size_t hi, double eps, std::vector<char>& keep) {
    std::vector<std::pair<size_t, size_t>> stack;
    stack.emplace_back(lo, hi);
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        double best = -1;
        size_t bi = a;
        for (size_t k = a + 1; k < b; ++k) {
            const double d = point_line_dist(pts[k], pts[a], pts[b]);
            if (d > best) { best = d; bi = k; }
        }
        if (best > eps) {
            keep[bi] = 1;
            stack.emplace_back(a, bi);
            stack.emplace_back(bi, b);
        }
    }
}

std::vector<Pt> simplify_closed(const std::vector<Pt>& pts, double eps) {
    const size_t n = pts.size();
    if (n < 4 || eps <= 0) return pts;
    size_t far = 0;
    double best = -1;
    for (size_t k = 1; k < n; ++k) {
        const double d = dist2(pts[k], pts[0]);
        if (d > best) { best = d; far = k; }
    }
    std::vector<char> keep(n, 0);
    keep[0] = 1;
    keep[far] = 1;
    dp_mark(pts, 0, far, eps, keep);
    // the second half runs from `far` round to the start again: duplicate the start as the last point
    std::vector<Pt> second(pts.begin() + static_cast<std::ptrdiff_t>(far), pts.end());
    second.push_back(pts[0]);
    std::vector<char> keep2(second.size(), 0);
    dp_mark(second, 0, second.size() - 1, eps, keep2);
    std::vector<Pt> out;
    for (size_t k = 0; k <= far; ++k) if (keep[k]) out.push_back(pts[k]);
    for (size_t k = 1; k + 1 < second.size(); ++k) if (keep2[k]) out.push_back(second[k]);
    return out;
}

}  // namespace

double path_length(const Path& p) {
    double len = 0;
    for (size_t k = 1; k < p.pts.size(); ++k) len += std::sqrt(dist2(p.pts[k - 1], p.pts[k]));
    if (p.closed && p.pts.size() > 2) len += std::sqrt(dist2(p.pts.back(), p.pts.front()));
    return len;
}

double total_length(const std::vector<Path>& paths) {
    double t = 0;
    for (const auto& p : paths) t += path_length(p);
    return t;
}

std::vector<Path> trace_outlines(const Gray& g, const TraceOptions& opt) {
    std::vector<Path> result;
    if (g.empty() || g.w > 8000 || g.h > 8000) return result;
    const int w = g.w, h = g.h;
    float level_orig;
    if (opt.threshold >= 0) {
        level_orig = static_cast<float>(std::min(255, opt.threshold)) + 0.5f;
    } else {
        // halfway between the average dark and the average bright pixel: a clean picture gets its outline halfway
        // between the two colours (and not on the edge of one)
        const int t = otsu_threshold(g);
        double lo = 0, hi = 0, nl = 0, nh = 0;
        for (uint8_t v : g.px) { if (v <= t) { lo += v; nl += 1; } else { hi += v; nh += 1; } }
        level_orig = (nl > 0 && nh > 0) ? static_cast<float>((lo / nl + hi / nh) * 0.5) : static_cast<float>(t) + 0.5f;
    }
    // shape = the pixels whose (possibly inverted) value is above the level; outside the picture is never shape
    const float level = opt.invert ? 255.0f - level_orig : level_orig;
    auto val = [&](int x, int y) -> float {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0.0f;
        const float v = g.at(x, y);
        return opt.invert ? 255.0f - v : v;
    };
    const int stride = w + 2;
    auto hid = [&](int x, int y) { return (((y + 1) * stride) + (x + 1)) * 2; };       // edge from pixel (x,y) to (x+1,y)
    auto vid = [&](int x, int y) { return (((y + 1) * stride) + (x + 1)) * 2 + 1; };   // edge from pixel (x,y) to (x,y+1)
    auto frac = [&](float a, float b) {
        const float d = b - a;
        if (std::fabs(d) < 1e-6f) return 0.5f;
        return std::clamp((level - a) / d, 0.0f, 1.0f);
    };

    std::unordered_map<int, Pt> pos;
    std::vector<Seg> segs;
    std::unordered_map<int, int> next;      // start edge -> segment index
    pos.reserve(static_cast<size_t>(w + h) * 4);

    for (int j = -1; j < h; ++j) {
        for (int i = -1; i < w; ++i) {
            const float tl = val(i, j), tr = val(i + 1, j), br = val(i + 1, j + 1), bl = val(i, j + 1);
            const int idx = (tl > level ? 1 : 0) | (tr > level ? 2 : 0) | (br > level ? 4 : 0) | (bl > level ? 8 : 0);
            if (idx == 0 || idx == 15) continue;

            // the four edge midpoints of this cell: id, picture position, doubled integer position (for the side test)
            struct Edge { int id; Pt p; int dx, dy; };
            const Edge T{hid(i, j), {i + frac(tl, tr), static_cast<float>(j)}, 2 * i + 1, 2 * j};
            const Edge R{vid(i + 1, j), {static_cast<float>(i + 1), j + frac(tr, br)}, 2 * i + 2, 2 * j + 1};
            const Edge B{hid(i, j + 1), {i + frac(bl, br), static_cast<float>(j + 1)}, 2 * i + 1, 2 * j + 2};
            const Edge L{vid(i, j), {static_cast<float>(i), j + frac(tl, bl)}, 2 * i, 2 * j + 1};
            const int cx[4] = {2 * i, 2 * i + 2, 2 * i + 2, 2 * i};        // corners tl, tr, br, bl (doubled)
            const int cy[4] = {2 * j, 2 * j, 2 * j + 2, 2 * j + 2};

            struct Spec { const Edge* p; const Edge* q; int corner; bool inside; };
            Spec spec[2];
            int n = 0;
            switch (idx) {
                case 1:  spec[n++] = {&T, &L, 0, true}; break;
                case 2:  spec[n++] = {&T, &R, 1, true}; break;
                case 3:  spec[n++] = {&L, &R, 0, true}; break;
                case 4:  spec[n++] = {&R, &B, 2, true}; break;
                case 5:  spec[n++] = {&T, &L, 0, true}; spec[n++] = {&R, &B, 2, true}; break;
                case 6:  spec[n++] = {&T, &B, 1, true}; break;
                case 7:  spec[n++] = {&L, &B, 3, false}; break;
                case 8:  spec[n++] = {&L, &B, 3, true}; break;
                case 9:  spec[n++] = {&T, &B, 0, true}; break;
                case 10: spec[n++] = {&T, &R, 1, true}; spec[n++] = {&L, &B, 3, true}; break;
                case 11: spec[n++] = {&R, &B, 2, false}; break;
                case 12: spec[n++] = {&L, &R, 3, true}; break;
                case 13: spec[n++] = {&T, &R, 1, false}; break;
                case 14: spec[n++] = {&T, &L, 0, false}; break;
                default: break;
            }
            for (int s = 0; s < n; ++s) {
                const Edge* p = spec[s].p;
                const Edge* q = spec[s].q;
                // which side of p->q is the reference corner on? (screen coordinates, y down: cross > 0 = right-hand side)
                const long ux = q->dx - p->dx, uy = q->dy - p->dy;
                const long vx = cx[spec[s].corner] - p->dx, vy = cy[spec[s].corner] - p->dy;
                const long cross = ux * vy - uy * vx;
                const bool corner_on_right = cross > 0;
                if (corner_on_right != spec[s].inside) std::swap(p, q);   // the shape stays on the right-hand side
                pos[p->id] = p->p;
                pos[q->id] = q->p;
                next[p->id] = static_cast<int>(segs.size());
                segs.push_back({p->id, q->id});
            }
        }
    }

    std::unordered_map<int, char> done;
    done.reserve(segs.size() * 2);
    for (const Seg& start : segs) {
        if (done.count(start.a)) continue;
        std::vector<Pt> pts;
        int cur = start.a;
        bool ok = false;
        for (size_t guard = 0; guard <= segs.size(); ++guard) {
            done[cur] = 1;
            pts.push_back(pos[cur]);
            const auto it = next.find(cur);
            if (it == next.end()) break;
            cur = segs[static_cast<size_t>(it->second)].b;
            if (cur == start.a) { ok = true; break; }
            if (done.count(cur)) break;
        }
        if (!ok || pts.size() < 3) continue;
        Path path;
        path.closed = true;
        path.pts = simplify_closed(pts, opt.simplify);
        if (path.pts.size() < 3) continue;
        if (path_length(path) < opt.min_perimeter) continue;
        result.push_back(std::move(path));
    }
    return result;
}

void limit_length(std::vector<Path>& paths, double max_len) {
    if (max_len <= 0) { paths.clear(); return; }
    if (total_length(paths) <= max_len) return;
    std::stable_sort(paths.begin(), paths.end(), [](const Path& a, const Path& b) { return path_length(a) > path_length(b); });
    std::vector<Path> kept;
    double used = 0;
    for (auto& p : paths) {
        const double len = path_length(p);
        if (used + len > max_len && !kept.empty()) continue;   // a smaller one may still fit
        used += len;
        kept.push_back(std::move(p));
    }
    paths = std::move(kept);
}

void fit_to_unit(std::vector<Path>& paths, float margin) {
    bool any = false;
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    for (const auto& p : paths)
        for (const auto& q : p.pts) {
            if (!any) { x0 = x1 = q.x; y0 = y1 = q.y; any = true; }
            x0 = std::min(x0, q.x); x1 = std::max(x1, q.x);
            y0 = std::min(y0, q.y); y1 = std::max(y1, q.y);
        }
    if (!any) return;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float span = std::max({x1 - x0, y1 - y0, 1e-6f});
    const float s = 2.0f * margin / span;
    for (auto& p : paths)
        for (auto& q : p.pts) {
            q.x = (q.x - cx) * s;
            q.y = -(q.y - cy) * s;
        }
}

std::vector<Path> order_paths(std::vector<Path> paths) {
    std::vector<Path> out;
    out.reserve(paths.size());
    std::vector<char> used(paths.size(), 0);
    Pt pen{0, 0};
    for (size_t round = 0; round < paths.size(); ++round) {
        double best = 1e300;
        size_t bi = 0, bk = 0;
        bool rev = false;
        for (size_t i = 0; i < paths.size(); ++i) {
            if (used[i] || paths[i].pts.empty()) continue;
            const auto& pts = paths[i].pts;
            if (paths[i].closed) {
                for (size_t k = 0; k < pts.size(); ++k) {
                    const double d = dist2(pen, pts[k]);
                    if (d < best) { best = d; bi = i; bk = k; rev = false; }
                }
            } else {
                const double d0 = dist2(pen, pts.front()), d1 = dist2(pen, pts.back());
                if (d0 < best) { best = d0; bi = i; bk = 0; rev = false; }
                if (d1 < best) { best = d1; bi = i; bk = 0; rev = true; }
            }
        }
        if (best > 1e299) break;
        used[bi] = 1;
        Path p = std::move(paths[bi]);
        if (p.closed) std::rotate(p.pts.begin(), p.pts.begin() + static_cast<std::ptrdiff_t>(bk), p.pts.end());
        else if (rev) std::reverse(p.pts.begin(), p.pts.end());
        pen = p.closed ? p.pts.front() : p.pts.back();
        out.push_back(std::move(p));
    }
    return out;
}

// ===========================================================================================================
// oscilloscope audio
// ===========================================================================================================

std::vector<float> xy_frame(const std::vector<Path>& paths, int n, double jump_speed) {
    std::vector<float> out;
    if (n < 2) return out;
    struct V { Pt p; double t; };
    std::vector<V> vs;
    double t = 0;
    const double js = std::max(1.0, jump_speed);
    auto add = [&](const Pt& p, bool jump) {
        if (!vs.empty()) {
            const double len = std::sqrt(dist2(vs.back().p, p));
            t += jump ? std::max(len / js, 1e-5) : len;
        }
        vs.push_back({p, t});
    };
    bool first = true;
    Pt first_pt{0, 0};
    for (const auto& path : paths) {
        if (path.pts.empty()) continue;
        if (first) { first_pt = path.pts.front(); first = false; }
        add(path.pts.front(), true);
        for (size_t k = 1; k < path.pts.size(); ++k) add(path.pts[k], false);
        if (path.closed && path.pts.size() > 2) add(path.pts.front(), false);
    }
    if (vs.empty()) return out;
    add(first_pt, true);                      // back to the start: the frame repeats
    if (t < 1e-9) t = 1e-9;
    out.resize(static_cast<size_t>(n) * 2);
    size_t seg = 1;
    for (int k = 0; k < n; ++k) {
        const double tk = t * k / n;
        while (seg + 1 < vs.size() && vs[seg].t < tk) ++seg;
        if (vs.size() == 1) { out[2 * k] = vs[0].p.x; out[2 * k + 1] = vs[0].p.y; continue; }
        const V& a = vs[seg - 1];
        const V& b = vs[seg];
        const double span = b.t - a.t;
        const double u = span > 1e-12 ? std::clamp((tk - a.t) / span, 0.0, 1.0) : 0.0;
        out[2 * k] = static_cast<float>(a.p.x + (b.p.x - a.p.x) * u);
        out[2 * k + 1] = static_cast<float>(a.p.y + (b.p.y - a.p.y) * u);
    }
    return out;
}

bool render_xy(const std::vector<Path>& unit_paths, const XyOptions& opt, Stereo& out) {
    out = Stereo();
    out.rate = std::clamp(opt.rate, 8000, 192000);
    bool any = false;
    for (const auto& p : unit_paths) if (!p.pts.empty()) any = true;
    if (!any) return false;
    const double seconds = std::clamp(opt.seconds, 0.5, 600.0);
    const double fps = std::clamp(opt.fps, 5.0, 2000.0);
    const long total = static_cast<long>(std::lround(seconds * out.rate));
    const long frames = std::max(1L, static_cast<long>(std::lround(seconds * fps)));
    out.l.reserve(static_cast<size_t>(total));
    out.r.reserve(static_cast<size_t>(total));

    std::unordered_map<int, std::vector<float>> cache;   // motion none: one frame per length
    for (long f = 0; f < frames && static_cast<long>(out.l.size()) < total; ++f) {
        const long s0 = std::lround(static_cast<double>(f) * out.rate / fps);
        const long s1 = std::min(total, std::lround(static_cast<double>(f + 1) * out.rate / fps));
        const int n = static_cast<int>(std::max(2L, s1 - s0));
        const std::vector<float>* frame = nullptr;
        std::vector<float> local;
        if (opt.motion == Motion::None) {
            auto it = cache.find(n);
            if (it == cache.end()) it = cache.emplace(n, xy_frame(unit_paths, n, opt.jump_speed)).first;
            frame = &it->second;
        } else {
            const double tt = static_cast<double>(f) / fps;
            std::vector<Path> moved = unit_paths;
            double rot = 0, sx = 1, sy = 1, scale = 1;
            if (opt.motion == Motion::Spin) { rot = 2 * kPi * tt / 8.0; scale = 0.70710678; }
            else if (opt.motion == Motion::Tumble) { sx = std::cos(2 * kPi * tt / 6.0); }
            else if (opt.motion == Motion::Pulse) { scale = 0.72 + 0.28 * (0.5 + 0.5 * std::sin(2 * kPi * tt / 2.5)); }
            const double cr = std::cos(rot), sr = std::sin(rot);
            for (auto& p : moved)
                for (auto& q : p.pts) {
                    const double x = q.x, y = q.y;
                    q.x = static_cast<float>(scale * sx * (x * cr - y * sr));
                    q.y = static_cast<float>(scale * sy * (x * sr + y * cr));
                }
            local = xy_frame(moved, n, opt.jump_speed);
            frame = &local;
        }
        for (int k = 0; k < n && static_cast<long>(out.l.size()) < total; ++k) {
            out.l.push_back((*frame)[static_cast<size_t>(2 * k)]);
            out.r.push_back((*frame)[static_cast<size_t>(2 * k + 1)]);
        }
    }
    // short fades: no click at the start and the end
    const size_t fade = std::min<size_t>(static_cast<size_t>(out.rate / 50), out.l.size() / 2);
    for (size_t k = 0; k < fade; ++k) {
        const float g = static_cast<float>(0.5 - 0.5 * std::cos(kPi * static_cast<double>(k) / static_cast<double>(fade)));
        out.l[k] *= g; out.r[k] *= g;
        out.l[out.l.size() - 1 - k] *= g; out.r[out.r.size() - 1 - k] *= g;
    }
    for (size_t k = 0; k < out.l.size(); ++k) {
        out.l[k] = std::clamp(out.l[k], -0.9f, 0.9f);
        out.r[k] = std::clamp(out.r[k], -0.9f, 0.9f);
    }
    return true;
}

// ===========================================================================================================
// spectrogram sound
// ===========================================================================================================

const char* scale_name(Scale s) {
    switch (s) {
        case Scale::PentatonicMinor: return "Pentatonic minor";
        case Scale::PentatonicMajor: return "Pentatonic major";
        case Scale::Major:           return "Major";
        case Scale::Minor:           return "Minor";
        case Scale::WholeTone:       return "Whole tone";
        case Scale::Chromatic:       return "Chromatic";
        case Scale::Exact:           return "Exact (every pitch)";
    }
    return "";
}

std::vector<double> scale_frequencies(Scale scale, int low_note, int octaves) {
    octaves = std::clamp(octaves, 1, 8);
    low_note = std::clamp(low_note, 12, 96);
    std::vector<double> f;
    auto hz = [](double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); };
    if (scale == Scale::Exact) {
        const int rows = 72;
        for (int r = 0; r < rows; ++r) f.push_back(hz(low_note + 12.0 * octaves * r / (rows - 1)));
        return f;
    }
    std::vector<int> iv;
    switch (scale) {
        case Scale::PentatonicMinor: iv = {0, 3, 5, 7, 10}; break;
        case Scale::PentatonicMajor: iv = {0, 2, 4, 7, 9}; break;
        case Scale::Major:           iv = {0, 2, 4, 5, 7, 9, 11}; break;
        case Scale::Minor:           iv = {0, 2, 3, 5, 7, 8, 10}; break;
        case Scale::WholeTone:       iv = {0, 2, 4, 6, 8, 10}; break;
        default:                     iv = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}; break;
    }
    for (int o = 0; o < octaves; ++o)
        for (int i : iv) f.push_back(hz(low_note + 12 * o + i));
    f.push_back(hz(low_note + 12 * octaves));
    return f;
}

namespace {

// A small Schroeder reverb (four combs, two all-passes) for one channel. `detune` shifts the delays a little so the
// two channels differ.
std::vector<float> reverb_channel(const std::vector<float>& in, int rate, int detune) {
    const double k = rate / 44100.0;
    const int comb_ms[4] = {1557, 1617, 1491, 1422};   // in samples at 44.1 kHz
    const int ap_ms[2] = {225, 556};
    std::vector<float> out(in.size(), 0.0f);
    for (int c = 0; c < 4; ++c) {
        const size_t d = std::max<size_t>(8, static_cast<size_t>(std::lround((comb_ms[c] + detune) * k)));
        std::vector<float> buf(d, 0.0f);
        size_t p = 0;
        float lp = 0;
        for (size_t n = 0; n < in.size(); ++n) {
            const float y = buf[p];
            lp = y * 0.75f + lp * 0.25f;                // damping
            buf[p] = in[n] + lp * 0.80f;
            p = (p + 1) % d;
            out[n] += y * 0.25f;
        }
    }
    for (int a = 0; a < 2; ++a) {
        const size_t d = std::max<size_t>(4, static_cast<size_t>(std::lround((ap_ms[a] + detune / 2) * k)));
        std::vector<float> buf(d, 0.0f);
        size_t p = 0;
        for (size_t n = 0; n < out.size(); ++n) {
            const float x = out[n];
            const float b = buf[p];
            const float y = -x + b;
            buf[p] = x + b * 0.5f;
            p = (p + 1) % d;
            out[n] = y;
        }
    }
    return out;
}

}  // namespace

bool render_spectrogram_sound(const Gray& g, const SpecOptions& opt, Stereo& out) {
    out = Stereo();
    out.rate = std::clamp(opt.rate, 8000, 96000);
    if (g.empty() || g.w < 2 || g.h < 2) return false;
    const double seconds = std::clamp(opt.seconds, 1.0, 300.0);
    const std::vector<double> freqs = scale_frequencies(opt.scale, opt.low_note, opt.octaves);
    const int R = static_cast<int>(freqs.size());
    const int C = std::clamp(static_cast<int>(std::lround(seconds * 14.0)), 8, std::max(8, std::min(g.w, 1200)));
    const Gray small = gray_resized(g, std::min(C, std::max(2, g.w)), R);
    if (small.empty()) return false;
    const int cols = small.w;

    // loudness per column and row
    std::vector<float> amp(static_cast<size_t>(cols) * static_cast<size_t>(R), 0.0f);   // [c * R + r], r = 0 lowest pitch
    float peak_amp = 0;
    for (int c = 0; c < cols; ++c)
        for (int r = 0; r < R; ++r) {
            const int y = R - 1 - r;                                    // the top of the picture is the highest pitch
            double v = small.at(c, y) / 255.0;
            if (opt.invert) v = 1.0 - v;
            v = std::max(0.0, (v - 0.12) / 0.88);                       // a faint haze (noise, JPEG blocks) stays silent
            const float a = static_cast<float>(std::pow(v, std::max(0.2, opt.gamma)));
            amp[static_cast<size_t>(c) * static_cast<size_t>(R) + static_cast<size_t>(r)] = a;
            peak_amp = std::max(peak_amp, a);
        }
    if (peak_amp <= 0.0f) return false;
    // at most `polyphony` rows per column, and an even loudness whatever the picture
    for (int c = 0; c < cols; ++c) {
        float* col = &amp[static_cast<size_t>(c) * static_cast<size_t>(R)];
        if (opt.polyphony > 0 && opt.polyphony < R) {
            std::vector<float> sorted(col, col + R);
            std::nth_element(sorted.begin(), sorted.begin() + (opt.polyphony - 1), sorted.end(), std::greater<float>());
            const float cut = sorted[static_cast<size_t>(opt.polyphony - 1)];
            int kept = 0;
            for (int r = R - 1; r >= 0; --r) {                         // ties: the higher rows win
                if (col[r] > cut || (col[r] == cut && cut > 0 && kept < opt.polyphony)) ++kept; else col[r] = 0.0f;
            }
        }
        double p = 0;
        for (int r = 0; r < R; ++r) p += static_cast<double>(col[r]) * col[r];
        const float norm = static_cast<float>(1.0 / std::max(1.0, std::sqrt(p)));
        for (int r = 0; r < R; ++r) col[r] *= norm;
    }

    const double reverb = std::clamp(opt.reverb, 0.0, 1.0);
    const long body = static_cast<long>(std::lround(seconds * out.rate));
    const long tail = reverb > 0.01 ? static_cast<long>(out.rate * 1.5) : 0;
    std::vector<float> L(static_cast<size_t>(body + tail), 0.0f), Rr(static_cast<size_t>(body + tail), 0.0f);

    // oscillators as rotating (sin, cos) pairs: no sin() per sample
    struct Osc { double s, c, ds, dc; };
    std::vector<Osc> main_osc(static_cast<size_t>(R)), det_osc(static_cast<size_t>(R));
    std::vector<float> pan_l(static_cast<size_t>(R)), pan_r(static_cast<size_t>(R));
    unsigned lcg = 12345u;
    for (int r = 0; r < R; ++r) {
        auto init = [&](Osc& o, double f) {
            lcg = lcg * 1664525u + 1013904223u;
            const double ph = (lcg >> 8) * (2 * kPi / 16777216.0);
            const double w = 2 * kPi * f / out.rate;
            o = {std::sin(ph), std::cos(ph), std::sin(w), std::cos(w)};
        };
        init(main_osc[static_cast<size_t>(r)], freqs[static_cast<size_t>(r)]);
        init(det_osc[static_cast<size_t>(r)], freqs[static_cast<size_t>(r)] * 1.003);
        const double pos = R > 1 ? static_cast<double>(r) / (R - 1) : 0.5;      // low = a little left, high = a little right
        const double ang = (0.5 + (pos - 0.5) * 0.7) * kPi * 0.5;
        pan_l[static_cast<size_t>(r)] = static_cast<float>(std::cos(ang));
        pan_r[static_cast<size_t>(r)] = static_cast<float>(std::sin(ang));
    }

    const double col_len = static_cast<double>(body) / cols;            // samples per column
    std::vector<int> active;
    active.reserve(static_cast<size_t>(R));
    long active_c0 = -2;
    for (long n = 0; n < body; ++n) {
        // control points sit in the middles of the columns; between two of them the loudness glides on a cosine
        const double pos = (static_cast<double>(n) + 0.5) / col_len - 0.5;
        long c0 = static_cast<long>(std::floor(pos));
        double u = pos - static_cast<double>(c0);
        long c1 = c0 + 1;
        if (c0 < 0) { c0 = 0; c1 = 0; u = 0; }
        if (c1 >= cols) { c1 = cols - 1; if (c0 >= cols) c0 = cols - 1; }
        const float w = static_cast<float>(0.5 - 0.5 * std::cos(kPi * u));
        if (c0 != active_c0) {
            active_c0 = c0;
            active.clear();
            for (int r = 0; r < R; ++r)
                if (amp[static_cast<size_t>(c0) * static_cast<size_t>(R) + static_cast<size_t>(r)] > 0.0f ||
                    amp[static_cast<size_t>(c1) * static_cast<size_t>(R) + static_cast<size_t>(r)] > 0.0f) active.push_back(r);
        }
        float sl = 0, sr = 0;
        for (int r : active) {
            const float a0 = amp[static_cast<size_t>(c0) * static_cast<size_t>(R) + static_cast<size_t>(r)];
            const float a1 = amp[static_cast<size_t>(c1) * static_cast<size_t>(R) + static_cast<size_t>(r)];
            const float a = a0 + (a1 - a0) * w;
            Osc& m = main_osc[static_cast<size_t>(r)];
            Osc& d = det_osc[static_cast<size_t>(r)];
            const double ns = m.s * m.dc + m.c * m.ds, nc = m.c * m.dc - m.s * m.ds;
            m.s = ns; m.c = nc;
            const double ds2 = d.s * d.dc + d.c * d.ds, dc2 = d.c * d.dc - d.s * d.ds;
            d.s = ds2; d.c = dc2;
            // a soft tone: the note, a quiet octave above it, and a slightly detuned copy for a gentle chorus
            const float tone = static_cast<float>(m.s + 0.22 * (2.0 * m.s * m.c) + 0.5 * d.s);
            sl += a * tone * pan_l[static_cast<size_t>(r)];
            sr += a * tone * pan_r[static_cast<size_t>(r)];
        }
        L[static_cast<size_t>(n)] = sl;
        Rr[static_cast<size_t>(n)] = sr;
        if ((n & 4095) == 0) {                                          // keep the rotating pairs at length 1
            for (int r : active) {
                for (Osc* o : {&main_osc[static_cast<size_t>(r)], &det_osc[static_cast<size_t>(r)]}) {
                    const double len = std::sqrt(o->s * o->s + o->c * o->c);
                    if (len > 0) { o->s /= len; o->c /= len; }
                }
            }
        }
    }

    auto peak_of = [&](const std::vector<float>& a, const std::vector<float>& b) {
        float p = 0;
        for (size_t i = 0; i < a.size(); ++i) p = std::max({p, std::fabs(a[i]), std::fabs(b[i])});
        return p;
    };
    // level, then a soft limiter: the sum of many notes can never clip
    float p = peak_of(L, Rr);
    if (p <= 0) return false;
    const float pre = 0.9f / p;
    const float knee = 1.2f, knee_norm = 1.0f / std::tanh(knee);
    for (size_t i = 0; i < L.size(); ++i) {
        L[i] = std::tanh(L[i] * pre * knee) * knee_norm * 0.85f;
        Rr[i] = std::tanh(Rr[i] * pre * knee) * knee_norm * 0.85f;
    }
    if (reverb > 0.01) {
        const std::vector<float> wl = reverb_channel(L, out.rate, 0), wr = reverb_channel(Rr, out.rate, 23);
        const float mix = static_cast<float>(reverb * 0.6);
        for (size_t i = 0; i < L.size(); ++i) {
            L[i] += wl[i] * mix;
            Rr[i] += wr[i] * mix;
        }
    }
    p = peak_of(L, Rr);
    const float final_gain = p > 0 ? 0.89f / p : 1.0f;
    const size_t fade_in = static_cast<size_t>(out.rate / 40);
    const size_t fade_out = std::min(L.size() / 2, static_cast<size_t>(out.rate / 2));
    for (size_t i = 0; i < L.size(); ++i) {
        float g = final_gain;
        if (i < fade_in) g *= static_cast<float>(0.5 - 0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(fade_in)));
        const size_t from_end = L.size() - 1 - i;
        if (from_end < fade_out) g *= static_cast<float>(0.5 - 0.5 * std::cos(kPi * static_cast<double>(from_end) / static_cast<double>(fade_out)));
        L[i] *= g; Rr[i] *= g;
    }
    out.l = std::move(L);
    out.r = std::move(Rr);
    return true;
}

// ===========================================================================================================
// WAV
// ===========================================================================================================

bool read_wav16(const std::string& path_utf8, Stereo& out) {
    out = Stereo();
    std::ifstream f(path_from_utf8(path_utf8), std::ios::binary);
    if (!f) return false;
    auto rd32 = [&](uint32_t& v) { unsigned char b[4]; if (!f.read(reinterpret_cast<char*>(b), 4)) return false; v = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24); return true; };
    // the chunk sizes come from the file: they are checked against what the file really holds before anything is allocated
    f.seekg(0, std::ios::end);
    const std::streamoff file_len = f.tellg();
    f.seekg(0, std::ios::beg);
    auto left = [&]() { const std::streamoff at = f.tellg(); return at < 0 || at > file_len ? static_cast<uint64_t>(0) : static_cast<uint64_t>(file_len - at); };
    constexpr uint64_t kMaxData = 1ull << 30;   // 1 GiB of samples is far more than the creator makes (5 minutes of recording is ~50 MB)
    char tag[4];
    uint32_t size = 0;
    if (!f.read(tag, 4) || std::memcmp(tag, "RIFF", 4) != 0 || !rd32(size)) return false;
    if (!f.read(tag, 4) || std::memcmp(tag, "WAVE", 4) != 0) return false;
    int channels = 0, bits = 0, rate = 0, format = 0;
    while (f.read(tag, 4) && rd32(size)) {
        if (std::memcmp(tag, "fmt ", 4) == 0) {
            if (size < 16 || size > 4096 || size > left()) return false;
            std::vector<unsigned char> b(size);
            if (!f.read(reinterpret_cast<char*>(b.data()), size)) return false;
            format = b[0] | (b[1] << 8);
            channels = b[2] | (b[3] << 8);
            rate = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);
            bits = b[14] | (b[15] << 8);
            if (size & 1) f.ignore(1);
        } else if (std::memcmp(tag, "data", 4) == 0) {
            if (format != 1 || bits != 16 || (channels != 1 && channels != 2) || rate < 8000 || rate > 192000) return false;
            const uint64_t take = std::min<uint64_t>({static_cast<uint64_t>(size), left(), kMaxData});   // a streamed file may say 0xFFFFFFFF
            if (take == 0) return false;
            std::vector<unsigned char> b(static_cast<size_t>(take));
            f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(take));
            const size_t got = static_cast<size_t>(f.gcount());
            const size_t frame = static_cast<size_t>(channels) * 2;
            const size_t n = got / frame;
            out.rate = rate;
            out.l.resize(n);
            out.r.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const int16_t a = static_cast<int16_t>(b[i * frame] | (b[i * frame + 1] << 8));
                const int16_t c = channels == 2 ? static_cast<int16_t>(b[i * frame + 2] | (b[i * frame + 3] << 8)) : a;
                out.l[i] = a / 32768.0f;
                out.r[i] = c / 32768.0f;
            }
            return n > 0;
        } else {
            if (static_cast<uint64_t>(size) + (size & 1) > left()) return false;
            f.ignore(size + (size & 1));
        }
    }
    return false;
}

bool write_wav16(const std::string& path_utf8, const Stereo& s, std::string* error) {
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    if (s.l.empty() || s.l.size() != s.r.size()) return fail("nothing to write");
    std::ofstream f(path_from_utf8(path_utf8), std::ios::binary | std::ios::trunc);
    if (!f) return fail("cannot create " + path_utf8);
    const uint32_t frames = static_cast<uint32_t>(s.l.size());
    const uint32_t data = frames * 4;
    auto u32 = [&](uint32_t v) { const char b[4] = {char(v & 255), char((v >> 8) & 255), char((v >> 16) & 255), char((v >> 24) & 255)}; f.write(b, 4); };
    auto u16 = [&](uint16_t v) { const char b[2] = {char(v & 255), char((v >> 8) & 255)}; f.write(b, 2); };
    f.write("RIFF", 4); u32(36 + data); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(2); u32(static_cast<uint32_t>(s.rate)); u32(static_cast<uint32_t>(s.rate) * 4); u16(4); u16(16);
    f.write("data", 4); u32(data);
    std::vector<char> buf;
    buf.reserve(static_cast<size_t>(frames) * 4);
    for (uint32_t i = 0; i < frames; ++i) {
        for (float v : {s.l[i], s.r[i]}) {
            const int q = static_cast<int>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
            const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(q));
            buf.push_back(static_cast<char>(u & 255));
            buf.push_back(static_cast<char>(u >> 8));
        }
    }
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!f) return fail("write failed");
    return true;
}

}  // namespace muisc::creator
