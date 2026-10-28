// The album cover in the disk's place (music player). Settings -> GENERAL -> AUDIO: Album Cover (on / off), Cover Mode
// (Just Cover / Cover with Gradient / Cover behind Disk), Cover Search (where the cover is looked for; cover_art.cpp).
//
//   Just Cover           the cover in its own colours (a real picture: Sixel keeps 240 colours, Kitty all of them)
//   Cover with Gradient  a grey version of the cover under the disk's gradient (disk colour -> disk colour end, top to bottom)
//   Cover behind Disk (Full Anima)  the same with the disk's exact braille animation: its dots come and go as it turns
//   Cover behind Disk    the same grey cover filling the whole field, hidden behind a black background: every dot of the
//                        rotating disk is a pinhole through which the cover shines at full brightness -- the animation stays.
// Without Kitty / Sixel the same three looks are drawn with braille dots (the cover dithered; behind the disk: the turning disk
// with every cell lit by the cover under it).
#include "app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "cover_art.h"
#include "cover_store.h"
#include "miniz.h"
#include "path_utf8.h"
#include "settings.h"
#include "terminal_gfx.h"
#include "terminal_ui.h"

namespace muisc {

namespace {

constexpr double kBacking = 0.55;   // Cover behind Disk: the grey level (of 255) a pinhole shows where the cover is transparent

// The disk's colours: disk_color -> disk_color_end (one colour when the gradient is off or a colour has no RGB value).
struct DiskColours {
    int sr = 255, sg = 255, sb = 255, er = 255, eg = 255, eb = 255;
    explicit DiskColours(const Settings& st) {
        if (!try_parse_rgb(st.disk_color, sr, sg, sb)) { sr = sg = sb = 255; }
        er = sr; eg = sg; eb = sb;
        if (st.disk_color_gradient && !st.disk_color_end.empty() && !try_parse_rgb(st.disk_color_end, er, eg, eb)) { er = sr; eg = sg; eb = sb; }
    }
    std::array<uint8_t, 3> at(double u) const {
        return {static_cast<uint8_t>(std::lround(sr + (er - sr) * u)), static_cast<uint8_t>(std::lround(sg + (eg - sg) * u)),
                static_cast<uint8_t>(std::lround(sb + (eb - sb) * u))};
    }
};


// Cover with Gradient: the gradient is a slow wave that runs over the picture diagonally, from the upper left to the lower right, like the audio book's colour wave: u (0..1, the place in the gradient) of a place d (0..1) along the diagonal at wave phase `phase`.
double wave_u(double d, double phase) {
    const double ripple = 0.5 + 0.5 * std::sin(6.283185307179586 * (d * 1.5 - phase));
    return std::clamp(0.5 * d + 0.5 * ripple, 0.0, 1.0);
}
// the phase of the wave now (it never stops; 0.15 cycles per second, as in the audio book)
double wave_phase_now() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 0.15;
}

} // namespace

// The look of the cover mode: the ASCII modes (5 Cover ASCII, 6 Cover ASCII with Gradient) look like Just Cover (0) / Cover with Gradient (1), in braille dots.
int App::cover_mode_look() const {
    const int m = std::clamp(settings_.cover_mode, 0, 6);
    return m >= 5 ? m - 5 : m;
}

const CoverImage* App::player_cover() const {
    if (audiobook_ || !settings_.element_disk || !settings_.album_cover || !has_track_ || !current_is_local_ || current_path_.empty()) return nullptr;
    const TrackMetadata md = display_metadata();
    CoverRequest rq;
    rq.path = path_utf8(current_path_);
    rq.title = md.name.empty() ? path_utf8(current_path_.stem()) : md.name;
    rq.album = md.album == "-" ? std::string() : md.album;
    rq.search = settings_.cover_search;
    rq.store_rev = cover_store_revision();
    return track_cover(rq);
}

bool App::album_cover_gfx(int col, int cols, int rows) const {
    if (gfx_proto_ == GfxProto::None || cols < 8 || rows < 4 || settings_.cover_mode >= 5) return false;   // (5, 6: the ASCII looks are braille dots)
    const CoverImage* cover = player_cover();
    if (!cover) return false;
    int cw0 = 10, ch0 = 20;
    gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
    cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
    // the cover is shown at the terminal's full resolution (Image Resolution does not apply: it is only sent when it changes, and
    // the cover itself has at most 640 pixels in either direction)
    const int scale = 1;
    const int cws = std::min(cw0, std::max(2, 640 / cols));
    const int chs = std::min(ch0, std::max(4, 640 / rows));
    const int w = cols * cws, h = rows * chs;
    const int mode = std::clamp(settings_.cover_mode, 0, 4);
    GfxFrame& g = gfx_;

    // the cover is only built again when something it shows changes (the behind-the-disk picture turns, so that one is due again
    // every few frames)
    const std::string key = cover->key + "|" + std::to_string(mode) + "|" + std::to_string(settings_.cover_contrast) + "|" + std::to_string(w) + "x" + std::to_string(h) + "|" +
                            std::to_string(col) + "|" + settings_.disk_color + "|" + (settings_.disk_color_gradient ? settings_.disk_color_end : std::string());
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const bool resized = key != cover_pic_key_ || !g.plain || g.w != w || g.h != h || g.level.size() != static_cast<size_t>(w) * h || g.id != 0 ||
                         g.col != col || g.cols != cols;
    const auto gap = std::chrono::milliseconds(gfx_proto_ == GfxProto::Sixel ? 110 : 40);
    const bool turning = mode >= 1;   // (the gradient wave of mode 1 moves too)
    const bool due = resized || (turning && now - cover_pic_at_ >= gap);
    g.plain = true;
    g.direct = mode == 0;
    g.scale = scale;
    g.out_cw = cw0; g.out_ch = ch0;
    g.cols = cols; g.rows = rows; g.w = w; g.h = h;
    g.col = col; g.row = 1;
    g.id = 0; g.grid.clear(); g.cut_w = 0;
    g.opaque = settings_.black_background;    // Sixel: black behind the picture (the terminal is black) ...
    g.erase = !settings_.black_background;    // ... or the cells are blanked first (the terminal keeps its own colour)
    g.active = true;
    g.fresh = due;
    if (!due) return true;
    cover_pic_key_ = key;
    cover_pic_at_ = now;
    cover_fill(g, *cover, mode, mode == 1 ? wave_phase_now() : angle_, w, h, cws, chs);
    return true;
}

namespace {
// "Smart" lift of dark parts for the behind-the-disk looks: the disk's dots show the cover through pinholes, so where the cover is dark the
// disk's silhouette would vanish. The local brightness (a coarse grid of block averages, interpolated smoothly) decides how much a spot is
// lifted: a dark area gets up to ~35% of the way to white added, a bright area (also a bright part at an edge of an otherwise dark cover)
// is left exactly as it is, and the lift fades smoothly between the two, so no seams or halos appear. Values 0..255 in, 0..1 strength out.
std::vector<float> dark_lift_map(const std::vector<unsigned char>& v, int w, int h, std::vector<float>* local_mean = nullptr) {
    std::vector<float> out(static_cast<size_t>(w) * h, 0.f);
    if (local_mean) local_mean->assign(static_cast<size_t>(w) * h, 0.f);
    if (w < 2 || h < 2) return out;
    const int gx = std::min(w, 6), gy = std::min(h, 6);
    std::vector<float> mean(static_cast<size_t>(gx) * gy, 0.f);
    for (int by = 0; by < gy; ++by)
        for (int bx = 0; bx < gx; ++bx) {
            const int x0 = bx * w / gx, x1 = std::max(x0 + 1, (bx + 1) * w / gx), y0 = by * h / gy, y1 = std::max(y0 + 1, (by + 1) * h / gy);
            double sum = 0; int n = 0;
            for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { sum += v[static_cast<size_t>(y) * w + x]; ++n; }
            mean[static_cast<size_t>(by) * gx + bx] = n ? static_cast<float>(sum / n) : 0.f;
        }
    auto need = [](float m) {   // 1 for a dark block (mean <= 40), 0 for a bright one (mean >= 120), smooth between
        const float t = std::clamp((120.f - m) / 80.f, 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    };
    for (int y = 0; y < h; ++y) {
        const float fy = std::clamp((y + 0.5f) * gy / h - 0.5f, 0.f, static_cast<float>(gy - 1));
        const int y0 = static_cast<int>(fy), y1 = std::min(gy - 1, y0 + 1);
        const float ty = fy - y0;
        for (int x = 0; x < w; ++x) {
            const float fx = std::clamp((x + 0.5f) * gx / w - 0.5f, 0.f, static_cast<float>(gx - 1));
            const int x0 = static_cast<int>(fx), x1 = std::min(gx - 1, x0 + 1);
            const float tx = fx - x0;
            const float a = mean[static_cast<size_t>(y0) * gx + x0] * (1 - tx) + mean[static_cast<size_t>(y0) * gx + x1] * tx;
            const float b = mean[static_cast<size_t>(y1) * gx + x0] * (1 - tx) + mean[static_cast<size_t>(y1) * gx + x1] * tx;
            const float m = a * (1 - ty) + b * ty;
            out[static_cast<size_t>(y) * w + x] = need(m);
            if (local_mean) (*local_mean)[static_cast<size_t>(y) * w + x] = m;
        }
    }
    return out;
}
constexpr float kDarkLift = 0.21f;       // how far (of the way to white) the darkest areas are lifted
constexpr float kDarkContrast = 0.9f;    // extra local contrast in those areas (0.6: features differ 1.6 times as much from their surroundings), so the lift does not grey them out
// a value with the local contrast added (around the local mean) and the dark lift on top
inline float dark_enhance(float v, float mean, float strength) {
    const float c = std::clamp(mean + (v - mean) * (1.f + kDarkContrast * strength), 0.f, 255.f);
    return c + kDarkLift * strength * (255.f - c);
}
}  // namespace


// Builds the cover picture (w x h pixels, cws x chs pixels per cell) of `mode` into g.level / g.hue / g.pal; the disk turned to `angle`
// (for mode 1: the phase of the gradient wave).
void App::cover_fill(GfxFrame& g, const CoverImage& cover_ref, int mode, double angle, int w, int h, int cws, int chs) const {
    const CoverImage* cover = &cover_ref;
    const DiskColours dc(settings_);
    g.level.assign(static_cast<size_t>(w) * h, 0);
    g.hue.assign(static_cast<size_t>(w) * h, 0);

    if (mode == 0) {   // just the cover, in its own colours: fitted into the field with a little room around it
        const CoverScaled& cs = cover_scaled(*cover, std::max(8, w - 2 * cws), std::max(8, h - chs), true, 0);   // (Cover Contrast only goes with the gradient looks)
        g.pal = cs.pal;
        const int ox = (w - cs.w) / 2, oy = (h - cs.h) / 2;
        for (int y = 0; y < cs.h; ++y)
            for (int x = 0; x < cs.w; ++x) {
                const size_t from = static_cast<size_t>(y) * cs.w + x, at = static_cast<size_t>(y + oy) * w + x + ox;
                if (x + ox < 0 || x + ox >= w || y + oy < 0 || y + oy >= h) continue;
                g.level[at] = cs.level[from];
                g.hue[at] = cs.hue[from];
            }
        return;
    }

    // the disk's gradient, top to bottom: hue 0..255 along it
    for (int i = 0; i < 256; ++i) g.pal[static_cast<size_t>(i)] = dc.at(i / 255.0);
    auto row_hue = [&](int y) { return static_cast<uint8_t>(std::lround(255.0 * y / std::max(1, h - 1))); };

    if (mode == 1) {   // grey cover under the gradient
        const CoverScaled& cs = cover_scaled(*cover, std::max(8, w - 2 * cws), std::max(8, h - chs), false, settings_.cover_contrast);
        const int ox = (w - cs.w) / 2, oy = (h - cs.h) / 2;
        std::vector<uint8_t> hue_d(static_cast<size_t>(w + h), 0);   // the wave over the diagonal of the field (x + y)
        for (int k = 0; k < w + h; ++k) hue_d[static_cast<size_t>(k)] = static_cast<uint8_t>(std::lround(255.0 * wave_u(k / std::max(1.0, w + h - 2.0), angle)));
        for (int y = 0; y < cs.h; ++y) {
            if (y + oy < 0 || y + oy >= h) continue;
            for (int x = 0; x < cs.w; ++x) {
                if (x + ox < 0 || x + ox >= w) continue;
                const unsigned char v = cs.grey[static_cast<size_t>(y) * cs.w + x];
                if (!v) continue;
                const size_t at = static_cast<size_t>(y + oy) * w + x + ox;
                g.level[at] = std::max<unsigned char>(v, 4);
                g.hue[at] = hue_d[static_cast<size_t>(x + ox + y + oy)];
            }
        }
        return;
    }

    // modes 2, 3 and 4: the cover fills the whole field (centre crop) but is only visible through the disk's dots (pinholes); the rest stays black
    const double aspect = static_cast<double>(cover->w) / std::max(1, cover->h);
    const int mw = aspect >= 1.0 ? static_cast<int>(std::ceil(h * aspect)) : w;
    const int mh = aspect >= 1.0 ? h : static_cast<int>(std::ceil(w / aspect));
    const CoverScaled& cs = cover_scaled(*cover, std::max(8, mw), std::max(8, mh), false, settings_.cover_contrast);
    const int cx0 = (cs.w - w) / 2, cy0 = (cs.h - h) / 2;
    // the pinholes: a soft-edged disc at every lit dot of the disk (dot units -> pixels)
    std::vector<unsigned char> hole(static_cast<size_t>(w) * h, 0);
    const double dpw = disk_.width() * 2.0, dph = disk_.height() * 4.0;   // the disk's field in dots
    const double px_x = w / dpw, px_y = h / dph;                          // pixels per dot
    const double radius = 0.40 * std::min(px_x, px_y);
    // mode 2: the dot grid stays and only the disk's pattern turns. mode 3 (v2): the dots themselves turn smoothly. mode 4 (Full
    // Anima): the dots of the braille disk exactly -- rotated and rounded to the dot grid, so some of them come and go as it
    // turns, like the disk and the volume bar's disk look
    std::vector<DiskArt::RotatedDot> dots;
    if (mode == 2 || mode == 4) {
        const std::vector<unsigned char> grid = disk_.dots(angle, mode == 2);
        const int gw = disk_.width() * 2;
        for (size_t i = 0; i < grid.size(); ++i)
            if (grid[i]) dots.push_back({static_cast<float>(i % static_cast<size_t>(gw)), static_cast<float>(i / static_cast<size_t>(gw))});
    } else dots = disk_.rotated_dots(angle);
    for (const DiskArt::RotatedDot& d : dots) {
        const double cx = (d.x + 0.5) * px_x, cy = (d.y + 0.5) * px_y;
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius - 1))), x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + radius + 1)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius - 1))), y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + radius + 1)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double dist = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                const double cov = std::clamp(radius + 0.5 - dist, 0.0, 1.0);
                unsigned char& m = hole[static_cast<size_t>(y) * w + x];
                m = std::max<unsigned char>(m, static_cast<unsigned char>(std::lround(cov * 255.0)));
            }
    }
    // the brightness the pinholes would show, and from it where the picture is dark enough to need a lift
    std::vector<unsigned char> field(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x + cx0, sy = y + cy0;
            if (sx < 0 || sy < 0 || sx >= cs.w || sy >= cs.h) continue;
            const size_t si = static_cast<size_t>(sy) * cs.w + sx;
            field[static_cast<size_t>(y) * w + x] = static_cast<unsigned char>(std::min(255, cs.grey[si] + static_cast<int>(kBacking * (255 - cs.alpha[si]))));
        }
    std::vector<float> lmean;
    const std::vector<float> lift = dark_lift_map(field, w, h, &lmean);
    for (int y = 0; y < h; ++y) {
        const int sy = y + cy0;
        const uint8_t hu = row_hue(y);
        for (int x = 0; x < w; ++x) {
            const int sx = x + cx0;
            if (sx < 0 || sy < 0 || sx >= cs.w || sy >= cs.h) { continue; }
            const size_t si = static_cast<size_t>(sy) * cs.w + sx;
            // a cover with a removed background (transparent parts): the pinholes there show a plain grey, so the disk keeps its
            // whole shape and not just the silhouette of the picture
            const size_t at = static_cast<size_t>(y) * w + x;
            const int v0 = field[at];
            const int v = std::clamp(static_cast<int>(std::lround(dark_enhance(static_cast<float>(v0), lmean[at], lift[at]))), 0, 255);   // (black dots in dark areas stay visible)
            const int lv = static_cast<int>(std::lround(v * (hole[at] / 255.0)));
            if (lv < 4) continue;   // no pinhole here: black (transparent)
            g.level[at] = static_cast<unsigned char>(std::min(lv, 255));
            g.hue[at] = hu;
        }
    }
}

// The remote app's picture of the album cover: the same looks as on the computer as a PNG with a transparent background (the phone
// draws it over the rows of the disk). The still looks (Just Cover, Cover with Gradient) are made once per key; the turning ones
// (Cover behind Disk ...) are made small (300 pixels) and again every few degrees of the turn. nullptr: no cover (the disk is shown).
const std::string* App::player_phone_png(std::string& key) const {
    static std::string cache_key, cache_png;
    const CoverImage* cover = player_cover();
    if (!cover) return nullptr;
    const int mode = cover_mode_look();
    const bool turning = mode >= 1;
    // the step of the picture: the turn of the disk (modes 2 to 4) or the phase of the gradient wave (mode 1)
    const double unit = mode == 1 ? 0.04 : 0.06;
    const int step = turning ? static_cast<int>(std::lround((mode == 1 ? wave_phase_now() : phone_angle_) / unit)) : 0;
    key = "cover|" + cover->key + "|" + std::to_string(settings_.cover_contrast) + "|" + std::to_string(mode) + "|" + std::to_string(step) + "|" + settings_.disk_color + "|" +
          (settings_.disk_color_gradient ? settings_.disk_color_end : std::string());
    if (key == cache_key && !cache_png.empty()) return &cache_png;
    // the disk's field is 30 x 15 cells: a cell is 1 : 2, so the picture is square
    const int cols = disk_.width(), rows = disk_.height();
    const int cws = turning ? 10 : 21, chs = turning ? 20 : 42;
    const int w = cols * cws, h = rows * chs;
    GfxFrame g;
    cover_fill(g, *cover, mode, turning ? static_cast<double>(step) * unit : 0.0, w, h, cws, chs);
    if (g.level.size() < static_cast<size_t>(w) * h || g.hue.size() < g.level.size()) return nullptr;
    std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4, 0);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        if (!g.level[i]) continue;
        const auto& c = g.pal[g.hue[i]];
        rgba[i * 4] = c[0]; rgba[i * 4 + 1] = c[1]; rgba[i * 4 + 2] = c[2]; rgba[i * 4 + 3] = g.level[i];
    }
    size_t n = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(rgba.data(), w, h, 4, &n);
    if (!png) return nullptr;
    cache_png.assign(static_cast<const char*>(png), n);
    mz_free(png);
    cache_key = key;
    return &cache_png;
}

// The same looks in braille dots (terminals without Kitty / Sixel): cols x rows cells, "" lines = no cover.
std::vector<std::string> App::album_cover_lines(int cols, int rows) const {
    const CoverImage* cover = player_cover();
    if (!cover || cols < 8 || rows < 4) return {};
    const int mode = cover_mode_look();
    const DiskColours dc(settings_);
    const int fw = cols * 2, fh = rows * 4;   // the dot field
    auto sgr = [](int r, int g, int b) {
        char c[40];
        std::snprintf(c, sizeof c, "\x1b[38;2;%d;%d;%dm", std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
        return std::string(c);
    };
    auto braille = [](std::string& line, int bits) {
        const unsigned cp = 0x2800u + static_cast<unsigned>(bits);
        line += static_cast<char>(0xE0 | (cp >> 12));
        line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        line += static_cast<char>(0x80 | (cp & 0x3F));
    };
    std::vector<std::string> lines;
    lines.reserve(static_cast<size_t>(rows));

    if (mode >= 2) {   // the turning disk, every cell lit by the cover under it
        const double aspect = static_cast<double>(cover->w) / std::max(1, cover->h);
        const int mw = aspect >= 1.0 ? static_cast<int>(std::ceil(fh * aspect)) : fw;
        const int mh = aspect >= 1.0 ? fh : static_cast<int>(std::ceil(fw / aspect));
        const CoverScaled& cs = cover_scaled(*cover, std::max(8, mw), std::max(8, mh), false, settings_.cover_contrast);
        const int cx0 = (cs.w - fw) / 2, cy0 = (cs.h - fh) / 2;
        const std::vector<unsigned char> bits = disk_.cells(angle_, mode == 2);
        std::vector<unsigned char> cell_lum(static_cast<size_t>(rows) * cols, 0);   // the brightness under every cell, and from it the lift of the dark areas
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                int sum = 0, n = 0;
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = c * 2 + dx + cx0, sy = r * 4 + dy + cy0;
                        if (sx < 0 || sy < 0 || sx >= cs.w || sy >= cs.h) continue;
                        sum += std::min(255, cs.grey[static_cast<size_t>(sy) * cs.w + sx] + static_cast<int>(kBacking * (255 - cs.alpha[static_cast<size_t>(sy) * cs.w + sx])));
                        ++n;
                    }
                cell_lum[static_cast<size_t>(r) * cols + c] = static_cast<unsigned char>(n ? sum / n : 0);
            }
        std::vector<float> cell_mean;
        const std::vector<float> cell_lift = dark_lift_map(cell_lum, cols, rows, &cell_mean);
        for (int r = 0; r < rows; ++r) {
            std::string line, last;
            const auto base = dc.at(rows > 1 ? static_cast<double>(r) / (rows - 1) : 0.0);
            for (int c = 0; c < cols; ++c) {
                const int b = bits[static_cast<size_t>(r) * disk_.width() + c];
                if (!b) { line += ' '; continue; }
                const double lum0 = cell_lum[static_cast<size_t>(r) * cols + c] / 255.0;
                const double lum = dark_enhance(static_cast<float>(lum0 * 255.0), cell_mean[static_cast<size_t>(r) * cols + c], cell_lift[static_cast<size_t>(r) * cols + c]) / 255.0;
                const double f = std::max(0.22, std::pow(lum, 0.8));
                const std::string col = sgr(static_cast<int>(base[0] * f + 0.5), static_cast<int>(base[1] * f + 0.5), static_cast<int>(base[2] * f + 0.5));
                if (col != last) { line += col; last = col; }
                braille(line, b);
            }
            line += "\x1b[0m";
            lines.push_back(std::move(line));
        }
        return lines;
    }

    // Just Cover / Cover with Gradient: the cover as dithered dots (a dot is lit where the brightness beats an ordered-dither
    // threshold); a cell takes the average colour of the pixels in it, or the disk's gradient dimmed by their brightness
    const bool own = mode == 0;
    const double wphase = wave_phase_now();
    const CoverScaled& cs = cover_scaled(*cover, fw, fh, own, own ? 0 : settings_.cover_contrast);
    const int ox = std::max(0, (fw - cs.w) / 2), oy = std::max(0, (fh - cs.h) / 2);
    static const int kBayer[4][2] = {{0, 4}, {6, 2}, {1, 5}, {7, 3}};   // thresholds 0..7 over the 2 x 4 dots of a cell
    static const int kBit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    for (int r = 0; r < rows; ++r) {
        std::string line, last;
        auto base = dc.at(rows > 1 ? static_cast<double>(r) / (rows - 1) : 0.0);
        for (int c = 0; c < cols; ++c) {
            if (mode == 1) base = dc.at(wave_u((c + 2.0 * r) / std::max(1.0, cols + 2.0 * rows - 3.0), wphase));   // the gradient wave runs diagonally, from the upper left to the lower right (a cell is twice as tall as wide)
            int bits = 0, n = 0, rs = 0, gs = 0, bs = 0;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = c * 2 + dx - ox, y = r * 4 + dy - oy;
                    if (x < 0 || y < 0 || x >= cs.w || y >= cs.h) continue;
                    const size_t at = static_cast<size_t>(y) * cs.w + x;
                    int lum, pr, pg, pb;
                    if (own) {
                        if (cs.level[at] < 128) continue;
                        const auto& p = cs.pal[cs.hue[at]];
                        pr = p[0]; pg = p[1]; pb = p[2];
                        lum = (pr * 30 + pg * 59 + pb * 11) / 100;
                    } else {
                        lum = cs.grey[at];
                        pr = pg = pb = lum;
                        if (lum == 0) continue;
                    }
                    ++n; rs += pr; gs += pg; bs += pb;
                    if (std::pow(lum / 255.0, 0.7) * 8.0 > kBayer[dy][dx] + 0.5) bits |= kBit[dy][dx];
                }
            if (!bits || !n) { line += ' '; continue; }
            std::string col;
            if (own) col = sgr(rs / n, gs / n, bs / n);
            else {
                const double f = std::max(0.25, std::pow(rs / n / 255.0, 1.4));
                col = sgr(static_cast<int>(base[0] * f + 0.5), static_cast<int>(base[1] * f + 0.5), static_cast<int>(base[2] * f + 0.5));
            }
            if (col != last) { line += col; last = col; }
            braille(line, bits);
        }
        line += "\x1b[0m";
        lines.push_back(std::move(line));
    }
    return lines;
}

// A picture file (or the picture inside an audio file) as braille dots, `cols` x `rows` cells, centred, for the overlays' previews
// (meta editor, cover fetch). Every line is exactly `cols` columns wide; the picture is read in the background ("reading ..." until
// then). `msg` replaces the picture (no picture to show).
std::vector<std::string> App::cover_preview_lines(const std::string& file, bool embedded, int cols, int rows, const std::string& msg) const {
    cols = std::max(cols, 1); rows = std::max(rows, 1);
    std::vector<std::string> lines(static_cast<size_t>(rows), std::string(static_cast<size_t>(cols), ' '));
    auto centred_text = [&](const std::string& t) {
        const std::string shown = truncate_str(t, cols);
        const int left = std::max(0, (cols - display_width(shown)) / 2);
        lines[static_cast<size_t>(rows / 2)] = std::string(static_cast<size_t>(left), ' ') + shown + std::string(static_cast<size_t>(std::max(0, cols - left - display_width(shown))), ' ');
    };
    if (!msg.empty() || file.empty()) { centred_text(msg.empty() ? "no picture" : msg); return lines; }
    const CoverImage* img = preview_cover(file, embedded);
    if (!img) { centred_text(preview_cover_loading(file, embedded) ? "reading ..." : "(the picture cannot be read)"); return lines; }
    const int fw = cols * 2, fh = rows * 4;
    const CoverScaled& cs = cover_scaled(*img, fw, fh, true, 0);   // (previews show the picture as it is: no contrast)
    const int ox = std::max(0, (fw - cs.w) / 2), oy = std::max(0, (fh - cs.h) / 2);
    static const int kBayer[4][2] = {{0, 4}, {6, 2}, {1, 5}, {7, 3}};
    static const int kBit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    for (int r = 0; r < rows; ++r) {
        std::string line, last;
        int used = 0;
        for (int c = 0; c < cols; ++c) {
            int bits = 0, n = 0, rs = 0, gs = 0, bs = 0;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = c * 2 + dx - ox, y = r * 4 + dy - oy;
                    if (x < 0 || y < 0 || x >= cs.w || y >= cs.h) continue;
                    const size_t at = static_cast<size_t>(y) * cs.w + x;
                    if (cs.level[at] < 128) continue;
                    const auto& p = cs.pal[cs.hue[at]];
                    const int lum = (p[0] * 30 + p[1] * 59 + p[2] * 11) / 100;
                    ++n; rs += p[0]; gs += p[1]; bs += p[2];
                    if (std::pow(lum / 255.0, 0.7) * 8.0 > kBayer[dy][dx] + 0.5) bits |= kBit[dy][dx];
                }
            ++used;
            if (!bits || !n) { line += ' '; continue; }
            char col[40];
            std::snprintf(col, sizeof col, "\x1b[38;2;%d;%d;%dm", rs / n, gs / n, bs / n);
            if (last != col) { line += col; last = col; }
            const unsigned cp = 0x2800u + static_cast<unsigned>(bits);
            line += static_cast<char>(0xE0 | (cp >> 12));
            line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            line += static_cast<char>(0x80 | (cp & 0x3F));
        }
        line += "\x1b[0m";
        lines[static_cast<size_t>(r)] = std::move(line);
        (void)used;
    }
    return lines;
}

} // namespace muisc
