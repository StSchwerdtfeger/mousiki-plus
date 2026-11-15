// radio_milkdrop.cpp
// The MilkDrop visuals (src/milkdrop.h, projectM loaded at run time) in the radio: the fifth view of the scope block (OsciSphere=milkdrop,
// the "." cycle), drawn as a picture (Kitty: the picture's own colours; Sixel: 216 colours) or as half blocks on other terminals, and the
// visuals overlay (":", key action VisualsMenu, overlay 11): the preset and the MilkDrop options of ANIMA/VIZ -> MILKDROP. The sound comes
// from the radio's audio callback (radio_engine.cpp, milkdrop_feed()).
#include "radio_ui_internal.h"
#include "milkdrop.h"
#include "path_utf8.h"
#include "settings.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace muisc::radio {

namespace {
// The favourites are the player's file (milkdrop_favourites.txt in the config folder): one preset path a line.
std::vector<std::string>& favs() { static std::vector<std::string> f; return f; }
bool& favs_loaded() { static bool l = false; return l; }
fs::path favs_file() { return radio_default_dir() / "milkdrop_favourites.txt"; }
void favs_load() {
    if (favs_loaded()) return;
    favs_loaded() = true;
    favs().clear();
    std::ifstream in(favs_file(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (std::find(favs().begin(), favs().end(), line) == favs().end()) favs().push_back(line);
    }
}
void favs_save() {
    std::ostringstream o;
    o << "# MilkDrop favourites (Mousiki): one preset file per line, full paths. [a] / [d] in the preset overlay.\n";
    for (const auto& f : favs()) o << f << "\n";
    write_text_atomic(favs_file(), o.str());
}
} // namespace

void md_favs_reload() { favs_loaded() = false; }

// ---- the preset overlay: folders, Favourites, Recommended, search ------------------------------------------------------
namespace {
struct Idx { std::string path, name, lname, lwhere; };
std::vector<Idx>& index_of() { static std::vector<Idx> v; return v; }
std::string index_key;   // what the index was built for (the folders)
std::string lower(std::string t) { for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return t; }
bool is_preset(const fs::path& p) { const std::string e = lower(path_utf8(p.extension())); return e == ".milk" || e == ".prjm"; }

std::vector<std::string> roots_of() {
    std::vector<std::string> r;
    const std::string root = Milkdrop::instance().find_presets_dir();
    if (!root.empty()) r.push_back(root);
    for (const std::string& e : Milkdrop::instance().extra_preset_dirs()) r.push_back(e);
    return r;
}
bool is_extra_root(const fs::path& d) {
    for (const std::string& e : Milkdrop::instance().extra_preset_dirs()) if (path_from_utf8(e).lexically_normal() == d.lexically_normal()) return true;
    return false;
}
bool inside(const MdBrowse& b, const std::string& file) {
    const std::string pr = path_utf8(path_from_utf8(file).parent_path().lexically_normal());
    for (const std::string& root : roots_of()) {
        const std::string rr = path_utf8(path_from_utf8(root).lexically_normal());
        if (!rr.empty() && pr.size() >= rr.size() && pr.compare(0, rr.size(), rr) == 0) return true;
    }
    (void)b;
    return false;
}
void build_index() {
    const std::vector<std::string> roots = roots_of();
    std::string key;
    for (const auto& r : roots) key += r + "\n";
    if (key == index_key && !index_of().empty()) return;
    index_key = key;
    index_of().clear();
    for (const std::string& r : roots) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(path_from_utf8(r), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2) || !is_preset(it->path())) continue;
            Idx x;
            x.path = path_utf8(it->path());
            x.name = path_utf8(it->path().stem());
            x.lname = lower(x.name);
            x.lwhere = lower(path_utf8(it->path().parent_path().filename()));
            index_of().push_back(std::move(x));
        }
    }
    std::sort(index_of().begin(), index_of().end(), [](const Idx& a, const Idx& b) { return a.lname < b.lname; });
}

// milkdrop_recommended.txt: the list that ships with Mousiki (recommended.txt beside the preset folder) and the user's own in the config folder;
// a line is a path inside the collection or only a file name, looked up in the presets that are installed
std::vector<std::string> recommended() {
    const std::string root = Milkdrop::instance().find_presets_dir();
    if (root.empty()) return {};
    build_index();
    std::vector<std::string> lowered;
    for (const Idx& e : index_of()) { std::string l = lower(e.path); std::replace(l.begin(), l.end(), '\\', '/'); lowered.push_back(std::move(l)); }
    std::vector<fs::path> files = {path_from_utf8(root).parent_path() / "recommended.txt", radio_default_dir() / "milkdrop_recommended.txt"};
    std::vector<std::string> out;
    for (const fs::path& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
            first = false;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t b = 0;
            while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
            line.erase(0, b);
            if (line.empty() || line[0] == '#') continue;
            std::replace(line.begin(), line.end(), '\\', '/');
            while (line.compare(0, 2, "./") == 0) line.erase(0, 2);
            const std::string want = lower(line);
            const bool has_ext = want.size() > 5 && want.compare(want.size() - 5, 5, ".milk") == 0;
            for (size_t k = 0; k < lowered.size(); ++k) {
                bool hit = false;
                for (const std::string& w : has_ext ? std::vector<std::string>{want} : std::vector<std::string>{want + ".milk", want + ".milk2"}) {
                    const std::string& cand = lowered[k];
                    if (cand.size() < w.size() || cand.compare(cand.size() - w.size(), w.size(), w) != 0) continue;
                    if (cand.size() == w.size() || cand[cand.size() - w.size() - 1] == '/') { hit = true; break; }
                }
                if (!hit) continue;
                if (std::find(out.begin(), out.end(), index_of()[k].path) == out.end()) out.push_back(index_of()[k].path);
                break;
            }
        }
    }
    return out;
}

void list_dir(MdBrowse& b, const std::string& dir, bool in_favs, const std::string& select_path) {
    b.items.clear();
    b.in_favs = in_favs;
    b.in_recs = !in_favs && dir == "<recommended>";
    b.dir = dir;
    const fs::path root = path_from_utf8(b.root);
    if (b.in_recs) {
        b.items.push_back({0, "..", b.root, ""});
        for (const std::string& f : recommended()) b.items.push_back({3, path_utf8(path_from_utf8(f).stem()), f, ""});
    } else if (in_favs) {
        favs_load();
        b.items.push_back({0, "..", b.root, ""});
        for (const std::string& f : favs()) {
            std::error_code ec;
            b.items.push_back({fs::exists(path_from_utf8(f), ec) ? 3 : 4, path_utf8(path_from_utf8(f).stem()), f, ""});   // (4: the file is gone)
        }
    } else {
        const fs::path d = path_from_utf8(dir.empty() ? b.root : dir);
        if (d.lexically_normal() != root.lexically_normal()) b.items.push_back({0, "..", is_extra_root(d) ? b.root : path_utf8(d.parent_path()), ""});
        std::vector<MdBrowseItem> folders, presets;
        std::error_code ec;
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2)) folders.push_back({2, path_utf8(it->path().filename()), path_utf8(it->path()), ""});
            else if (it->is_regular_file(e2) && is_preset(it->path())) presets.push_back({3, path_utf8(it->path().stem()), path_utf8(it->path()), ""});
        }
        auto by_name = [](const MdBrowseItem& x, const MdBrowseItem& y) { return lower(x.name) < lower(y.name); };
        std::sort(folders.begin(), folders.end(), by_name);
        std::sort(presets.begin(), presets.end(), by_name);
        if (d.lexically_normal() == root.lexically_normal()) {
            b.items.push_back({1, "Favourites", "", ""});
            const size_t nrec = recommended().size();
            if (nrec > 0) b.items.push_back({5, "Recommended", "<recommended>", std::to_string(nrec)});
            for (const std::string& e : Milkdrop::instance().extra_preset_dirs()) b.items.push_back({2, path_utf8(path_from_utf8(e).filename()), e, "extra path"});
        }
        for (auto& f : folders) b.items.push_back(std::move(f));
        for (auto& p : presets) b.items.push_back(std::move(p));
    }
    b.sel = 0;
    if (!select_path.empty())
        for (size_t k = 0; k < b.items.size(); ++k) if (b.items[k].path == select_path) { b.sel = static_cast<int>(k); break; }
    if (b.sel == 0 && b.items.size() > 1 && b.items[0].kind == 0) b.sel = 1;
    b.top = std::max(0, b.sel - 3);
}

void search_run(MdBrowse& b) {
    build_index();
    b.items.clear();
    b.sel = b.top = 0;
    std::vector<std::string> words;
    { std::istringstream is(lower(b.query)); std::string w; while (is >> w) words.push_back(w); }
    if (words.empty()) return;
    std::vector<std::pair<int, const Idx*>> hits;
    for (const Idx& e : index_of()) {
        int score = 0;
        bool all = true;
        for (const std::string& w : words) {
            const size_t at = e.lname.find(w);
            if (at != std::string::npos) score += at == 0 ? 3 : 2;
            else if (e.lwhere.find(w) != std::string::npos) score += 1;
            else { all = false; break; }
        }
        if (all) hits.push_back({score, &e});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (size_t i = 0; i < hits.size() && i < 500; ++i) b.items.push_back({3, hits[i].second->name, hits[i].second->path, path_utf8(path_from_utf8(hits[i].second->path).parent_path().filename())});
}

void play_selected(MdBrowse& b) {   // the preset under the cursor plays at once (the overlay locks the preset while it is open)
    if (b.sel < 0 || b.sel >= static_cast<int>(b.items.size()) || b.items[static_cast<size_t>(b.sel)].kind != 3) return;
    Milkdrop::instance().play_path(b.items[static_cast<size_t>(b.sel)].path);
    b.moved = true;
}
} // namespace

void radio_milkdrop_apply(const RadioSettings& c) {
    static bool lock_given = false;   // (the saved lock is put on once)
    if (!lock_given) { lock_given = true; Milkdrop::instance().set_locked(c.md_lock); }
    MilkdropOptions o;
    o.presets_dir = c.md_presets;
    o.extra_dirs = c.md_extra;
    o.fps = std::clamp(c.md_fps, 10, 60);
    o.max_width = std::clamp(c.md_res, 160, 1920);
    o.preset_seconds = std::max(0, c.md_secs);
    o.shuffle = c.md_shuffle;
    o.cut_sens = static_cast<float>(std::clamp(c.md_cut_sens, 5, 60)) / 10.0f;
    o.hard_cuts = c.md_hard_cuts;
    o.transition = std::clamp(c.md_transition, 0, 10);
    o.gpu = c.md_gpu;
    o.hard_cut_secs = std::clamp(c.md_hcd, 3, 120);
    o.aspect = c.md_aspect;
    { static const int mw[] = {16, 32, 64, 96}, mh[] = {12, 24, 48, 72}; const int mi = std::clamp(c.md_mesh, 0, 3); o.mesh_w = mw[mi]; o.mesh_h = mh[mi]; }
    o.variance = static_cast<float>(std::clamp(c.md_var, 0, 30));
    o.clean_start = c.md_clean;
    o.favourites_only = c.md_fav_shuffle;
    if (o.favourites_only) { favs_load(); o.favourites = favs(); }
    Milkdrop::instance().configure(o);
}

bool radio_milkdrop_usable(const RadioSettings& c, std::string* why) {
    if (!c.use_milkdrop) { if (why) *why = "switched off (Settings -> ANIMA/VIZ -> Use MilkDrop)"; return false; }
    radio_milkdrop_apply(c);
    return Milkdrop::instance().available(why);
}

namespace detail {

namespace {
// what the scope block's picture was made from (a new size, place or name box makes it again even without a new frame)
struct MdPic {
    std::string key;
    uint64_t frames = 0;
};
MdPic& md_pic() { static MdPic p; return p; }

// nearest-neighbour: the frame (any size) onto w x h pixels, centred in iw pixels across (the rest black: the bars of a place wider than 16:9)
void md_scale(const VideoFrame& f, int w, int h, std::vector<uint8_t>& out, int iw = 0) {
    out.assign(static_cast<size_t>(w) * h * 3, 0);
    if (f.w <= 0 || f.h <= 0) return;
    if (iw <= 0 || iw > w) iw = w;
    const int x0 = (w - iw) / 2;
    for (int y = 0; y < h; ++y) {
        const int sy = std::min(f.h - 1, static_cast<int>((static_cast<int64_t>(y) * f.h) / h));
        const uint8_t* srow = f.rgb.data() + static_cast<size_t>(sy) * f.w * 3;
        uint8_t* d = out.data() + (static_cast<size_t>(y) * w + x0) * 3;
        for (int x = 0; x < iw; ++x) {
            const int sx = std::min(f.w - 1, static_cast<int>((static_cast<int64_t>(x) * f.w) / iw));
            const uint8_t* sp = srow + static_cast<size_t>(sx) * 3;
            d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d += 3;
        }
    }
}
// How many of `total` units across the picture takes: no wider than 16:9 (Aspect Correction); a unit is par times as wide as high.
int md_inner_w(int total, int h, double par, bool cap) {
    if (!cap || h <= 0) return total;
    const int iw = static_cast<int>(std::lround(h * (16.0 / 9.0) / std::max(0.01, par)));
    return std::clamp(iw, 2, total);
}
} // namespace

bool build_milkdrop_block(const Style& s, const UiModel& m, int scope_w, std::vector<std::string>& rows) {
    if (scope_w < 8 || !radio_milkdrop_usable(s.c)) return false;
    Milkdrop& md = Milkdrop::instance();
    if (milkdrop_window_running()) {   // the pictures are in the extra window: nothing is drawn or sent here
        rows.assign(kPanelH, spaces(scope_w));
        const std::string t = truncate_str("MilkDrop is in its own window", scope_w - 2);
        const int l = std::max(0, (scope_w - display_width(t)) / 2);
        rows[kPanelH / 2] = spaces(l) + s.header + t + kReset + spaces(std::max(0, scope_w - l - display_width(t)));
        return true;
    }
    const bool pic = m.gfx_proto != GfxProto::None;
    const int cw0 = std::max(4, m.cell_w), ch0 = std::max(8, m.cell_h);
    // a big block is drawn at half the pixels (Kitty scales it up, Sixel repeats them): the visuals are soft anyway
    const int scale = pic && scope_w * cw0 > 1280 ? 2 : 1;
    const int cws = std::max(2, cw0 / scale);
    const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
    const int pw = scope_w * cws, ph = kPanelH * chs;
    // Aspect Correction: the picture is no wider than 16:9 (a wide block gets bars at the sides, not a cut-off preset)
    const int ipw = md_inner_w(pw, ph, 1.0, s.c.md_aspect);
    bool fresh = false;
    const VideoFrame* f = md.frame(ipw, ph, fresh);   // (the render thread starts here and learns the size)
    // the preset's name for 3 s after a change, in a small box on the bottom row
    std::string name;
    if (s.c.md_show_name && md.since_change() <= 3.0) name = truncate_str(md.preset_name(), std::max(4, scope_w - 4));
    const int nw = name.empty() ? 0 : display_width(name) + 2;
    const int nl = std::max(0, (scope_w - nw) / 2);
    auto name_row = [&](const std::string& left, const std::string& right) {
        return left + "\x1b[0;97;40m " + name + " " + kReset + right;
    };
    if (!f) {   // starting: the status in the middle of the block
        rows.assign(kPanelH, spaces(scope_w));
        const std::string t = truncate_str(md.status().empty() ? std::string("milkdrop: starting ...") : md.status(), scope_w - 2);
        const int l = std::max(0, (scope_w - display_width(t)) / 2);
        rows[kPanelH / 2] = spaces(l) + s.header + t + kReset + spaces(std::max(0, scope_w - l - display_width(t)));
        return true;
    }
    MdPic& st = md_pic();
    if (fresh) ++st.frames;
    if (pic) {
        GfxFrame& g = m.gfx;
        const bool tc = m.gfx_proto == GfxProto::Kitty;
        const std::string key = std::to_string(pw) + "x" + std::to_string(ph) + "|" + std::to_string(ipw) + "|" + std::to_string(scope_w) + "|" + std::to_string(nl) + "," +
                                std::to_string(nw) + (tc ? "|t" : "|p") + (s.c.black_background ? "|b" : "|-");
        const bool remake = key != st.key || !g.plain || g.w != pw || g.h != ph || g.level.size() != static_cast<size_t>(pw) * ph;
        g.plain = true; g.direct = true; g.truecolor = tc;
        g.scale = scale; g.out_cw = cw0; g.out_ch = ch0;
        g.cols = scope_w; g.rows = kPanelH; g.w = pw; g.h = ph;
        g.id = 0; g.grid.clear(); g.cut2_w = 0;
        g.cut_c = nl; g.cut_r = kPanelH - 1; g.cut_w = nw; g.cut_h = nw > 0 ? 1 : 0;
        g.opaque = s.c.black_background;
        g.erase = !s.c.black_background;
        g.active = true;
        g.fresh = fresh || remake;
        if (g.fresh) {
            std::vector<uint8_t> px;
            md_scale(*f, pw, ph, px, ipw);
            g.level.assign(static_cast<size_t>(pw) * ph, 255);
            if (tc) g.rgb.swap(px);
            else {   // Sixel: a 6 x 6 x 6 colour cube (216 colours of the 240 there are)
                g.hue.resize(static_cast<size_t>(pw) * ph);
                for (size_t i = 0, n = g.hue.size(); i < n; ++i) {
                    const int r = (px[i * 3] * 5 + 127) / 255, gg = (px[i * 3 + 1] * 5 + 127) / 255, b = (px[i * 3 + 2] * 5 + 127) / 255;
                    g.hue[i] = static_cast<uint8_t>(r * 36 + gg * 6 + b);
                }
                for (int i = 0; i < 216; ++i)
                    g.pal[static_cast<size_t>(i)] = {static_cast<uint8_t>(i / 36 * 51), static_cast<uint8_t>(i / 6 % 6 * 51), static_cast<uint8_t>(i % 6 * 51)};
            }
            if (nw > 0)   // the name box's cells stay free (their pixels transparent)
                for (int y = (kPanelH - 1) * chs; y < ph; ++y)
                    for (int x = nl * cws; x < (nl + nw) * cws && x < pw; ++x) g.level[static_cast<size_t>(y) * pw + x] = 0;
        }
        st.key = key;
        rows.assign(kPanelH, gfx_hole(scope_w));
        if (nw > 0) rows[kPanelH - 1] = name_row(gfx_hole(nl), gfx_hole(std::max(0, scope_w - nl - nw)));
        return true;
    }
    // other terminals: half blocks, two pixels a cell (the upper one the text colour, the lower one the background)
    std::vector<uint8_t> px;
    const int icells = md_inner_w(scope_w, kPanelH * 2, 2.0 * cw0 / ch0, s.c.md_aspect);   // (a half block is cw0 wide, ch0 / 2 high)
    md_scale(*f, scope_w, kPanelH * 2, px, icells);
    rows.assign(kPanelH, std::string());
    for (int r = 0; r < kPanelH; ++r) {
        std::string line;
        line.reserve(static_cast<size_t>(scope_w) * 40);
        const int last = r == kPanelH - 1 && nw > 0 ? nl : scope_w;
        for (int x = 0; x < last; ++x) {
            const uint8_t* a = px.data() + (static_cast<size_t>(2 * r) * scope_w + x) * 3;
            const uint8_t* b = px.data() + (static_cast<size_t>(2 * r + 1) * scope_w + x) * 3;
            char buf[64];
            std::snprintf(buf, sizeof buf, "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xE2\x96\x80", a[0], a[1], a[2], b[0], b[1], b[2]);
            line += buf;
        }
        line += kReset;
        if (last < scope_w) line = name_row(line, spaces(std::max(0, scope_w - nl - nw)));
        rows[static_cast<size_t>(r)] = line;
    }
    return true;
}

// ---- the visuals overlay (overlay 11) ----

std::vector<std::string> build_md_overlay(const Style& s, const UiModel& m) {
    const RadioSettings& c = s.c;
    const int W = kMdOverlayW, inner = W - 4;
    auto row = [&](const std::string& plain, bool hi, bool dim = false) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return box_line(s, (hi ? std::string("\x1b[7m") : dim ? std::string("\x1b[90m") : std::string()) + body + kReset, s.border);
    };
    Milkdrop& md = Milkdrop::instance();
    std::string why;
    const bool ok = radio_milkdrop_usable(c, &why);
    std::string preset = md.preset_name();
    if (preset.empty()) preset = md.running() ? "(starting ...)" : "(shows when the visuals run)";
    char beat[8];
    std::snprintf(beat, sizeof beat, "%.1f", std::clamp(c.md_cut_sens, 5, 60) / 10.0);
    const std::string labels[kMdRowCount] = {"Preset", "Preset Overlay", "Random Preset", "Lock Preset", "Preset Duration", "Shuffle Presets", "Shuffle Favourites",
                                             "Hard Cut Sensitivity", "Hard Cuts", "Preset Transition", "Frame Rate", "Resolution", "GPU", "Hard Cut Interval",
                                             "Duration Variance", "Mesh Detail", "Aspect Correction", "Clean Start", "Show Preset Name", "Show in the Scope Block"};
    const std::string vals[kMdRowCount] = {
        preset, "[ENTER]", "[ENTER]", md.locked() ? "on" : "off",
        c.md_secs <= 0 ? std::string("off") : std::to_string(c.md_secs) + " s",
        c.md_shuffle ? "on" : "off", c.md_fav_shuffle ? "on" : "off", beat, c.md_hard_cuts ? "on" : "off",
        c.md_transition <= 0 ? std::string("cut") : std::to_string(c.md_transition) + " s",
        std::to_string(c.md_fps) + " fps", std::to_string(c.md_res) + " px", milkdrop_gpu_label(c.md_gpu),
        std::to_string(std::clamp(c.md_hcd, 3, 120)) + " s", c.md_var <= 0 ? std::string("off") : std::to_string(c.md_var) + " s", mesh_name(c.md_mesh),
        c.md_aspect ? "on" : "off", c.md_clean ? "on" : "off",
        c.md_show_name ? "on" : "off", c.scope_mode == 4 ? "on" : "off",
    };
    // a small terminal shows a window of the rows, following the selection
    const int vis = std::clamp(m.rows - 8, 6, kMdRowCount);
    const int top = std::clamp(m.overlay_row - vis / 2, 0, kMdRowCount - vis);
    std::vector<std::string> lines;
    lines.push_back(box_top(s, W, vis < kMdRowCount ? "Visuals: MilkDrop (" + std::to_string(m.overlay_row + 1) + "/" + std::to_string(kMdRowCount) + ")" : std::string("Visuals: MilkDrop"), s.border));
    for (int i = top; i < top + vis; ++i) {
        const bool sel = i == m.overlay_row;
        std::string plain;
        if (i == kMdPreset) {
            const std::string lab = std::string(sel ? "> " : "  ") + pad_right(labels[i], 10);
            plain = lab + "< " + truncate_str(vals[i], std::max(4, inner - display_width(lab) - 4)) + " >";
        } else {
            plain = std::string(sel ? "> " : "  ") + pad_right(labels[i], 24) + pad_left(vals[i], std::max(1, inner - 26));
        }
        lines.push_back(row(plain, sel, !ok));
    }
    lines.push_back(row("", false));
    if (!ok) {
        lines.push_back(row("Not available: " + why, false, true));
        lines.push_back(row("Install projectM 4 and a preset pack (see the manual).", false, true));
    } else {
        char info[160];
        const int idx = md.preset_index(), cnt = md.preset_count();
        if (cnt > 0 && idx >= 0) std::snprintf(info, sizeof info, "preset %d of %d   one picture %.0f ms", idx + 1, cnt, md.frame_ms());
        else if (cnt > 0) std::snprintf(info, sizeof info, "a preset outside the list (%d in it)   one picture %.0f ms", cnt, md.frame_ms());
        else std::snprintf(info, sizeof info, "%s", md.running() ? "starting ..." : "not running (nothing shows it)");
        lines.push_back(row(info, false, true));
        const std::string r = md.renderer();
        if (!r.empty()) lines.push_back(row("drawn by " + r + (r.find("llvmpipe") != std::string::npos || r.find("softpipe") != std::string::npos ? " (software)" : ""), false, true));
    }
    push_legend_block(s, lines, W, {"[UP/DOWN] select   [LEFT/RIGHT] change   [ENTER] change / open",
                                    "[N] / [B] next / previous   [L] lock   [P] preset overlay   [" + menus_key_label(c, "VisualsMenu") + " / ESC] close"});
    return lines;
}


std::vector<std::string> build_mdb_overlay(const Style& s, const UiModel& m) {
    const MdBrowse& b = m.mdb;
    const int W = kMdbOverlayW, inner = W - 4;
    const int vis = std::clamp(m.rows - 10, 6, 18);
    auto row = [&](const std::string& plain, bool hi, bool dim = false) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return box_line(s, (hi ? std::string("\x1b[7m") : dim ? std::string("\x1b[90m") : std::string()) + body + kReset, s.border);
    };
    std::string title = "MilkDrop presets";
    if (b.searching || b.typing) title += ": search";
    else if (b.in_favs) title += ": Favourites";
    else if (b.in_recs) title += ": Recommended";
    std::vector<std::string> lines;
    lines.push_back(box_top(s, W, title, s.border));
    const int n = static_cast<int>(b.items.size());
    const int top = std::clamp(b.top, 0, std::max(0, n - vis));
    for (int i = top; i < top + vis; ++i) {
        if (i >= n) { lines.push_back(row("", false)); continue; }
        const MdBrowseItem& it = b.items[static_cast<size_t>(i)];
        const bool sel = i == b.sel;
        const std::string mark = it.kind == 3 || it.kind == 4 ? "" : it.kind == 0 ? "" : "/";
        std::string label = it.kind == 0 ? std::string("..") : it.name + mark;
        std::string extra = it.kind == 5 ? it.extra : it.kind == 3 ? it.extra : it.kind == 4 ? "(missing)" : it.kind == 2 ? it.extra : "";
        const int room = std::max(8, inner - 4 - (extra.empty() ? 0 : display_width(extra) + 2));
        label = truncate_str(label, room);
        std::string plain = std::string(sel ? "> " : "  ") + label;
        if (!extra.empty()) plain = pad_right(plain, inner - display_width(extra) - 1) + extra;
        lines.push_back(row(plain, sel, it.kind == 4));
    }
    lines.push_back(row(b.typing || b.searching ? "search: " + b.query + (b.typing ? "_" : "") : "", false, true));
    const int count = b.in_recs ? std::max(0, n - 1) : b.in_favs ? static_cast<int>(favs().size()) : Milkdrop::instance().preset_count();
    lines.push_back(row(std::to_string(count) + (b.in_recs ? " recommended" : b.in_favs ? " favourites" : " presets") + (b.searching ? "   " + std::to_string(n) + " found" : ""), false, true));
    const std::string keys = b.typing ? "[ENTER] done  [ESC] clear" : b.in_favs ? "[ENTER] choose  [D] remove  [/] search  [LEFT] back  [ESC] close"
                                                                      : "[ENTER] choose / open  [F] favourites  [A] add  [/] search  [LEFT] up  [ESC] close";
    push_legend_block(s, lines, W, {keys});
    return lines;
}

} // namespace detail

void md_overlay_adjust(RadioSettings& c, int row, int dir, std::string& notice) {
    Milkdrop& md = Milkdrop::instance();
    switch (row) {
        case kMdPreset: if (dir > 0) md.next_preset(); else md.prev_preset(); break;
        case kMdBrowse: break;   // (opened by the key handler: it needs the overlay state)
        case kMdRandom: md.random_preset(); break;
        case kMdLock: md.set_locked(!md.locked()); c.md_lock = md.locked(); notice = md.locked() ? "milkdrop: the preset stays" : "milkdrop: the presets change again"; break;
        case kMdDuration: c.md_secs = radio_md_step(2, c.md_secs, dir); break;
        case kMdShuffle: c.md_shuffle = !c.md_shuffle; break;
        case kMdFavShuffle:
            c.md_fav_shuffle = !c.md_fav_shuffle;
            favs_load();
            notice = !c.md_fav_shuffle ? "milkdrop: all presets" : favs().empty() ? "milkdrop: only favourites -- there are none yet ([a] in the preset overlay): all presets until then"
                                                                            : "milkdrop: only the " + std::to_string(favs().size()) + " favourites";
            break;
        case kMdCutSens: c.md_cut_sens = std::clamp(c.md_cut_sens + dir, 5, 60); break;
        case kMdHardCuts: c.md_hard_cuts = !c.md_hard_cuts; break;
        case kMdTransition: c.md_transition = std::clamp(c.md_transition + dir, 0, 10); break;
        case kMdFps: c.md_fps = radio_md_step(0, c.md_fps, dir); break;
        case kMdRes: c.md_res = radio_md_step(1, c.md_res, dir); break;
        case kMdGpu: c.md_gpu = radio_md_gpu_step(c.md_gpu, dir); notice = "milkdrop GPU: " + milkdrop_gpu_label(c.md_gpu) + " -- the visuals restart on it"; break;
        case kMdHcd: c.md_hcd = radio_md_step(3, c.md_hcd, dir); break;
        case kMdVar: c.md_var = radio_md_step(4, c.md_var, dir); break;
        case kMdMesh: c.md_mesh = std::clamp(c.md_mesh + dir, 0, 3); notice = "milkdrop mesh: " + mesh_name(c.md_mesh) + " -- the visuals restart"; break;
        case kMdAspect: c.md_aspect = !c.md_aspect; break;
        case kMdClean: c.md_clean = !c.md_clean; break;
        case kMdName: c.md_show_name = !c.md_show_name; break;
        case kMdScope: {
            std::string why;
            if (c.scope_mode == 4) { c.scope_mode = c.use_osci ? 0 : 1; notice = "scope block: back to the " + scope_mode_name(c); }
            else if (!radio_milkdrop_usable(c, &why)) notice = "milkdrop: cannot run here -- " + why;
            else { c.scope_mode = 4; notice = "scope block: milkdrop"; }
            break;
        }
        default: break;
    }
    radio_milkdrop_apply(c);
}


bool md_browse_open(RadioSettings& c, UiModel& m, std::string& notice) {
    std::string why;
    if (!radio_milkdrop_usable(c, &why)) { notice = "milkdrop: cannot run here -- " + why; return false; }
    MdBrowse& b = m.mdb;
    b = MdBrowse{};
    Milkdrop& md = Milkdrop::instance();
    b.root = md.find_presets_dir();
    if (b.root.empty()) { notice = "milkdrop: no preset folder"; return false; }
    b.prev_path = md.preset_path();
    b.prev_locked = md.locked();
    md.set_locked(true);   // (the preset stays while one is looked at)
    std::string dir = b.root;
    if (!b.prev_path.empty() && inside(b, b.prev_path)) dir = path_utf8(path_from_utf8(b.prev_path).parent_path());
    list_dir(b, dir, false, b.prev_path);
    m.overlay = 12;
    return true;
}

bool md_browse_key(RadioSettings& c, UiModel& m, int k, bool arrow, std::string& notice, bool& close) {
    MdBrowse& b = m.mdb;
    Milkdrop& md = Milkdrop::instance();
    const int n = static_cast<int>(b.items.size());
    auto finish = [&](bool keep) {   // leave: keep the preset chosen, or put the one before back
        if (!keep && b.moved && !b.prev_path.empty()) md.play_path(b.prev_path);
        md.set_locked(b.prev_locked);
        c.md_lock = md.locked();
        close = true;
        m.overlay = 11;
    };
    auto current = [&]() -> const MdBrowseItem* { return b.sel >= 0 && b.sel < n ? &b.items[static_cast<size_t>(b.sel)] : nullptr; };
    auto up = [&]() {
        if (b.searching) { b.searching = false; b.query.clear(); list_dir(b, b.root, false, ""); return; }
        if (b.in_favs || b.in_recs) { list_dir(b, b.root, false, ""); return; }
        const fs::path d = path_from_utf8(b.dir.empty() ? b.root : b.dir);
        if (d.lexically_normal() == path_from_utf8(b.root).lexically_normal()) return;
        const std::string from = path_utf8(d);
        list_dir(b, is_extra_root(d) ? b.root : path_utf8(d.parent_path()), false, from);
    };
    auto move_to = [&](int sel) {
        if (n == 0) return;
        b.sel = std::clamp(sel, 0, n - 1);
        const int vis = 12;
        if (b.sel < b.top) b.top = b.sel;
        if (b.sel >= b.top + vis) b.top = b.sel - vis + 1;
        play_selected(b);
    };
    if (b.typing) {
        if (k == 13 || k == 10) { b.typing = false; if (b.items.empty()) { b.searching = false; list_dir(b, b.root, false, ""); } else play_selected(b); }
        else if (k == 27 && !arrow) { b.typing = b.searching = false; b.query.clear(); list_dir(b, b.root, false, ""); }
        else if (k == 127 || k == 8) { if (!b.query.empty()) b.query.pop_back(); search_run(b); }
        else if (arrow && (k == 'A' || k == 'B')) { b.typing = false; move_to(b.sel + (k == 'B' ? 1 : -1)); }
        else if (!arrow && k >= 32 && k < 127) { b.query.push_back(static_cast<char>(k)); search_run(b); }
        return true;
    }
    if (arrow) {
        if (k == 'A') move_to(b.sel - 1);
        else if (k == 'B') move_to(b.sel + 1);
        else if (k == 'D') up();
        else if (k == 'C') { const MdBrowseItem* it = current(); if (it && it->kind != 3 && it->kind != 4) k = 13; }
        if (k != 13) return true;
    }
    if (k == 27) { if (b.searching || b.in_favs || b.in_recs) up(); else finish(false); return true; }
    if (k == '/') { b.typing = b.searching = true; b.query.clear(); b.items.clear(); b.sel = b.top = 0; return true; }
    if (k == 'f' || k == 'F') { list_dir(b, "", true, ""); return true; }
    if (k == 'a' || k == 'A') {
        const MdBrowseItem* it = current();
        if (!it || it->kind != 3) { notice = "milkdrop: [A] adds a preset (not a folder) to the favourites"; return true; }
        favs_load();
        if (std::find(favs().begin(), favs().end(), it->path) != favs().end()) { notice = "milkdrop: " + it->name + " is a favourite already"; return true; }
        favs().push_back(it->path);
        favs_save();
        notice = "milkdrop: " + it->name + " added to the favourites (" + std::to_string(favs().size()) + ")";
        radio_milkdrop_apply(c);
        return true;
    }
    if (k == 'd' || k == 'D') {
        const MdBrowseItem* it = current();
        if (!b.in_favs || !it || it->kind == 0 || it->kind == 1) { notice = "milkdrop: [D] takes a preset out of the favourites (in the Favourites folder)"; return true; }
        favs_load();
        const std::string name = it->name, path = it->path;
        favs().erase(std::remove(favs().begin(), favs().end(), path), favs().end());
        favs_save();
        list_dir(b, "", true, "");
        b.sel = std::clamp(b.sel, 0, std::max(0, static_cast<int>(b.items.size()) - 1));
        notice = "milkdrop: " + name + " taken out of the favourites (" + std::to_string(favs().size()) + " left)";
        radio_milkdrop_apply(c);
        return true;
    }
    if (k == 13 || k == 10) {
        const MdBrowseItem* it = current();
        if (!it) return true;
        const MdBrowseItem item = *it;
        if (item.kind == 0) { if (b.searching || b.in_favs || b.in_recs) up(); else list_dir(b, item.path, false, b.dir); }
        else if (item.kind == 1) list_dir(b, "", true, "");
        else if (item.kind == 5) list_dir(b, "<recommended>", false, "");
        else if (item.kind == 2) list_dir(b, item.path, false, "");
        else if (item.kind == 3) { md.play_path(item.path); notice = "milkdrop: " + item.name; finish(true); }
    }
    return true;
}

} // namespace muisc::radio
