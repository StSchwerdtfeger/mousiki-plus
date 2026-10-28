// Font glyphs for the karaoke and big-lyric views: finds a system font that covers the characters and
// rasterises single glyphs into dot grids.
#include "glyph_raster.h"
#include "utf8_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "path_utf8.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace muisc {
namespace fs = std::filesystem;

namespace {

struct Face {
    std::string path;
    std::vector<unsigned char> data;   // stbtt_fontinfo points into this
    stbtt_fontinfo info{};
};

struct State {
    std::mutex mu;
    bool searched = false;
    std::vector<std::string> candidates;          // best first
    size_t next_candidate = 0;                    // first one not tried yet
    std::vector<std::unique_ptr<Face>> faces;     // loaded, in lookup order
    std::unordered_map<uint64_t, RasterGlyph> cache;
    std::unordered_map<uint64_t, bool> missing;   // (cp) no loaded or loadable face has it
};

State& st() { static State s; return s; }


// Higher = better. Bold sans faces first; serif / thin / mono faces last.
int score_font(const std::string& file_lower) {
    int s = 0;
    if (file_lower.find("bold") != std::string::npos || file_lower.find("-w6") != std::string::npos ||
        file_lower.find("yugothb") != std::string::npos || file_lower.find("msyhbd") != std::string::npos ||
        file_lower.find("meiryob") != std::string::npos || file_lower.find("malgunbd") != std::string::npos) s += 4;
    if (file_lower.find("medium") != std::string::npos || file_lower.find("yugothm") != std::string::npos) s += 3;
    if (file_lower.find("regular") != std::string::npos) s += 2;
    if (file_lower.find("sans") != std::string::npos || file_lower.find("gothic") != std::string::npos) s += 1;
    for (const char* bad : {"serif", "mincho", "ming", "song", "light", "thin", "mono", "italic", "oblique", "black"})
        if (file_lower.find(bad) != std::string::npos) s -= 5;
    return s;
}

bool wanted_font_name(const std::string& n) {
    for (const char* k : {"notosanscjk", "notosansjp", "notosanssc", "notosanskr", "notosanstc", "sourcehansans", "sourcehan",
                          "wqy", "droidsansfallback", "unifont", "ipag", "ipaex", "takao", "uming", "gothic", "yugoth",
                          "msyh", "meiryo", "malgun", "simhei", "simsun", "hiragino", "pingfang", "applesdgothic",
                          "stheiti", "arialuni", "arial unicode", "mingliu", "kozgo", "vlgothic", "sazanami", "fireflysung"})
        if (n.find(k) != std::string::npos) return true;
    return false;
}

void scan_dir(const fs::path& dir, int depth, std::vector<std::pair<int, std::string>>& found) {
    std::error_code ec;
    if (depth < 0 || !fs::is_directory(dir, ec)) return;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        std::error_code e2;
        if (it->is_directory(e2)) { scan_dir(p, depth - 1, found); continue; }
        const std::string ext = ascii_lower_str(path_utf8(p.extension()));
        if (ext != ".ttc" && ext != ".otf" && ext != ".ttf" && ext != ".otc") continue;
        const std::string name = ascii_lower_str(path_utf8(p.filename()));
        if (!wanted_font_name(name)) continue;
        found.emplace_back(score_font(name), path_utf8(p));
    }
}

void build_candidates(State& s) {
    std::vector<std::pair<int, std::string>> found;
    if (const char* env = std::getenv("MOUSIKI_CJK_FONT")) {
        if (*env) s.candidates.push_back(env);
    }
#ifdef _WIN32
    const char* wd = std::getenv("WINDIR");
    const std::string fonts = std::string(wd && *wd ? wd : "C:\\Windows") + "\\Fonts\\";
    for (const char* f : {"YuGothB.ttc", "YuGothM.ttc", "meiryob.ttc", "msyhbd.ttc", "msyh.ttc", "meiryo.ttc", "msgothic.ttc",
                          "malgunbd.ttf", "malgun.ttf", "simhei.ttf", "simsun.ttc"})
        s.candidates.push_back(fonts + f);
#elif defined(__APPLE__)
    for (const char* f : {"/System/Library/Fonts/\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W6.ttc",
                          "/System/Library/Fonts/\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W3.ttc",
                          "/System/Library/Fonts/Hiragino Sans GB.ttc", "/System/Library/Fonts/PingFang.ttc",
                          "/System/Library/Fonts/AppleSDGothicNeo.ttc", "/System/Library/Fonts/STHeiti Medium.ttc",
                          "/Library/Fonts/Arial Unicode.ttf", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"})
        s.candidates.push_back(f);
#endif
#ifndef _WIN32
    std::vector<fs::path> dirs = {"/usr/share/fonts", "/usr/local/share/fonts", "/usr/X11R6/lib/X11/fonts", "/run/current-system/sw/share/X11/fonts"};
    if (const char* home = std::getenv("HOME")) {
        dirs.push_back(fs::path(home) / ".local/share/fonts");
        dirs.push_back(fs::path(home) / ".fonts");
    }
    for (const auto& d : dirs) scan_dir(d, 5, found);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    for (const auto& f : found) s.candidates.push_back(f.second);
#endif
}

// Loads candidate `idx`. Null when the file is missing or no usable font.
std::unique_ptr<Face> load_face(const std::string& path) {
    std::error_code ec;
    const fs::path p = path_from_utf8(path);
    if (!fs::is_regular_file(p, ec)) return nullptr;
    const auto size = fs::file_size(p, ec);
    if (ec || size < 1024 || size > 80u * 1024u * 1024u) return nullptr;
    auto f = std::make_unique<Face>();
    f->path = path;
    {
        std::ifstream in(p, std::ios::binary);
        if (!in) return nullptr;
        f->data.resize(static_cast<size_t>(size));
        in.read(reinterpret_cast<char*>(f->data.data()), static_cast<std::streamsize>(size));
        if (!in) return nullptr;
    }
    const int off = stbtt_GetFontOffsetForIndex(f->data.data(), 0);
    if (off < 0 || !stbtt_InitFont(&f->info, f->data.data(), off)) return nullptr;
    // A font is only useful if it draws Han characters (the reason for all this).
    if (stbtt_FindGlyphIndex(&f->info, 0x4E2D) == 0 && stbtt_FindGlyphIndex(&f->info, 0x3042) == 0 &&
        stbtt_FindGlyphIndex(&f->info, 0xD55C) == 0) return nullptr;
    return f;
}

// Loads the next usable candidate that is not loaded yet. False when there is none left (max 3 faces).
bool load_next_face(State& s) {
    if (s.faces.size() >= 3) return false;
    while (s.next_candidate < s.candidates.size()) {
        const std::string& path = s.candidates[s.next_candidate++];
        bool already = false;
        for (const auto& f : s.faces) if (f->path == path) already = true;
        if (already) continue;
        if (auto f = load_face(path)) { s.faces.push_back(std::move(f)); return true; }
    }
    return false;
}

void ensure_searched(State& s) {
    if (s.searched) return;
    s.searched = true;
    build_candidates(s);
    load_next_face(s);
}

// A character the further faces are worth loading for: East Asian blocks only, so an emoji
// or a symbol line does not make the program read three 20 MB font files.
bool east_asian(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xA960 && cp <= 0xA97F) ||
           (cp >= 0xAC00 && cp <= 0xD7FF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FFFF);
}

Face* face_for(State& s, char32_t cp) {
    for (;;) {
        for (auto& f : s.faces)
            if (stbtt_FindGlyphIndex(&f->info, static_cast<int>(cp)) != 0) return f.get();
        if (!east_asian(cp) || !load_next_face(s)) return nullptr;
    }
}

}  // namespace

bool glyph_font_available() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    ensure_searched(s);
    return !s.faces.empty();
}

std::string glyph_font_path() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    ensure_searched(s);
    return s.faces.empty() ? std::string() : s.faces.front()->path;
}

bool glyph_rasterize(char32_t cp, int h, RasterGlyph& out) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    ensure_searched(s);
    if (s.faces.empty() || h < 4) return false;
    const uint64_t key = (static_cast<uint64_t>(cp) << 16) | static_cast<uint64_t>(h);
    if (auto it = s.cache.find(key); it != s.cache.end()) { out = it->second; return true; }
    if (s.missing.count(cp)) return false;
    Face* f = face_for(s, cp);
    if (!f) { s.missing[cp] = true; return false; }

    // The em square fills the cell, so a full-width character is h x h dots.
    const float scale = stbtt_ScaleForMappingEmToPixels(&f->info, static_cast<float>(h));
    int asc = 0, desc = 0, gap = 0, adv = 0, lsb = 0;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    stbtt_GetCodepointHMetrics(&f->info, static_cast<int>(cp), &adv, &lsb);
    RasterGlyph g;
    g.h = h;
    g.w = std::max(2, static_cast<int>(std::lround(adv * scale)));
    // Vertical: centre the ascent..descent box in the cell, the baseline falls out of that.
    const float box = static_cast<float>(asc - desc) * scale;
    const int baseline = static_cast<int>(std::lround((static_cast<float>(h) - box) / 2.0f + static_cast<float>(asc) * scale));
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBox(&f->info, static_cast<int>(cp), scale, scale, &x0, &y0, &x1, &y1);
    const int bw = x1 - x0, bh = y1 - y0;
    g.dots.assign(static_cast<size_t>(g.w) * static_cast<size_t>(h), 0);
    if (bw > 0 && bh > 0) {
        std::vector<unsigned char> cov(static_cast<size_t>(bw) * static_cast<size_t>(bh), 0);
        stbtt_MakeCodepointBitmap(&f->info, cov.data(), bw, bh, bw, scale, scale, static_cast<int>(cp));
        // Thin strokes (a 16 dot kanji has 1-dot lines) must not drop out: a dot is set from ~35 % coverage on.
        constexpr int kThreshold = 90;
        for (int y = 0; y < bh; ++y) {
            const int dy = baseline + y0 + y;
            if (dy < 0 || dy >= h) continue;
            for (int x = 0; x < bw; ++x) {
                const int dx = x0 + x;
                if (dx < 0 || dx >= g.w) continue;
                if (cov[static_cast<size_t>(y) * static_cast<size_t>(bw) + static_cast<size_t>(x)] >= kThreshold)
                    g.dots[static_cast<size_t>(dy) * static_cast<size_t>(g.w) + static_cast<size_t>(dx)] = 1;
            }
        }
    }
    s.cache[key] = g;
    out = std::move(g);
    return true;
}

}  // namespace muisc
