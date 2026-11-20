// radio_milkdrop.cpp
// The MilkDrop visuals (src/milkdrop.h, projectM loaded at run time) in the radio: the fifth view of the scope block (OsciSphere=milkdrop,
// the "." cycle), drawn as a picture (Kitty: the picture's own colours; Sixel: 216 colours) or as half blocks on other terminals, and the
// visuals overlay (":", key action VisualsMenu, overlay 11): the preset and the MilkDrop options of ANIMA/VIZ -> MILKDROP. The sound comes
// from the radio's audio callback (radio_engine.cpp, milkdrop_feed()).
#include "radio_ui_internal.h"
#include "milkdrop.h"
#include "milkdrop_folders.h"
#include "path_utf8.h"
#include "settings.h"
#include "video_fx.h"
#include "terminal_ui.h"
#include "library_search.h"
#include "text_edit.h"   // normalize_for_search()
#include "video_pipe.h"
#include "video_player.h"
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
struct Idx { std::string path, name, lname, lwhere, name_n, where_n, all_n; };   // (*_n: normalised for the search, as the player's)
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
            x.name_n = normalize_for_search(x.name);
            x.where_n = normalize_for_search(path_utf8(it->path().parent_path().filename()));
            x.all_n = x.name_n + " " + x.where_n;
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
    std::vector<fs::path> files = {path_from_utf8(root).parent_path() / "recommended.txt", radio_default_dir() / "milkdrop_recommended.txt"};
    // Asked for on every redraw of the MilkDrop options (the values show how many presets a folder has) and resolving it walks the whole preset
    // index for every line of the lists: made once, kept until the preset folders or the two lists change.
    static std::string cache_key;
    static std::vector<std::string> cache;
    std::string ckey = root + "|" + std::to_string(index_of().size());
    for (const fs::path& f : files) {
        std::error_code ec;
        const auto t = fs::last_write_time(f, ec);
        ckey += "|" + (ec ? std::string("-") : std::to_string(t.time_since_epoch().count()));
    }
    if (!cache_key.empty() && cache_key == ckey) return cache;
    std::vector<std::string> lowered;
    for (const Idx& e : index_of()) { std::string l = lower(e.path); std::replace(l.begin(), l.end(), '\\', '/'); lowered.push_back(std::move(l)); }
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
    cache_key = ckey;
    cache = out;
    return out;
}

void md_undo_push(MdBrowse& b, const MdUndo& u) {
    b.undo.push_back(u);
    if (b.undo.size() > 30) b.undo.erase(b.undo.begin());
}

void list_dir(MdBrowse& b, const std::string& dir, bool in_favs, const std::string& select_path) {
    b.items.clear();
    b.in_favs = in_favs;
    b.in_recs = !in_favs && dir == "<recommended>";
    b.in_user = (!in_favs && dir.compare(0, 9, "<folder>:") == 0) ? dir.substr(9) : std::string();
    b.dir = dir;
    const fs::path root = path_from_utf8(b.root);
    if (b.in_recs) {
        b.items.push_back({0, "..", b.root, ""});
        for (const std::string& f : recommended()) b.items.push_back({3, path_utf8(path_from_utf8(f).stem()), f, ""});
    } else if (!b.in_user.empty()) {
        b.items.push_back({0, "..", b.root, ""});
        for (const std::string& f : muisc::mdf::load(radio_default_dir(), b.in_user)) {
            std::error_code ec;
            b.items.push_back({fs::exists(path_from_utf8(f), ec) ? 3 : 4, path_utf8(path_from_utf8(f).stem()), f, ""});
        }
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
            for (const std::string& n : muisc::mdf::names(radio_default_dir())) b.items.push_back({6, n, "<folder>:" + n, std::to_string(muisc::mdf::load(radio_default_dir(), n).size())});
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
    if (b.query.empty()) return;
    // the player's match: the name first (exact part, earlier = better), then the folder, then fuzzy over both (a close word, an abbreviation)
    const PreparedQuery pq = prepare_query(b.query);
    std::vector<std::pair<double, const Idx*>> hits;
    for (const Idx& e : index_of()) {
        double sc = substring_score(pq, e.name_n);
        if (sc < 0.0) { sc = substring_score(pq, e.where_n); if (sc >= 0.0) sc *= 0.5; }
        if (sc < 0.0) sc = word_fuzzy_score(pq, e.all_n);
        if (sc > 0.0) hits.push_back({sc, &e});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (size_t i = 0; i < hits.size() && i < 2000; ++i) b.items.push_back({3, hits[i].second->name, hits[i].second->path, path_utf8(path_from_utf8(hits[i].second->path).parent_path().filename())});
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
    o.variance = std::clamp(c.md_var, 0, 30);
    o.clean_start = c.md_clean;
    o.favourites_only = c.md_fav_shuffle;
    if (o.favourites_only) o.favourites = radio_md_shuffle_paths(c);
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

// nearest-neighbour: the frame (any size) onto w x h pixels, centred as iw x ih pixels (the rest black: the bars of a place that is not the picture's shape)
void md_scale(const VideoFrame& f, int w, int h, std::vector<uint8_t>& out, int iw = 0, int ih = 0) {
    out.assign(static_cast<size_t>(w) * h * 3, 0);
    if (f.w <= 0 || f.h <= 0) return;
    if (iw <= 0 || iw > w) iw = w;
    if (ih <= 0 || ih > h) ih = h;
    const int x0 = (w - iw) / 2, y0 = (h - ih) / 2;
    for (int y = 0; y < ih; ++y) {
        const int sy = std::min(f.h - 1, static_cast<int>((static_cast<int64_t>(y) * f.h) / ih));
        const uint8_t* srow = f.rgb.data() + static_cast<size_t>(sy) * f.w * 3;
        uint8_t* d = out.data() + (static_cast<size_t>(y + y0) * w + x0) * 3;
        for (int x = 0; x < iw; ++x) {
            const int sx = std::min(f.w - 1, static_cast<int>((static_cast<int64_t>(x) * f.w) / iw));
            const uint8_t* sp = srow + static_cast<size_t>(sx) * 3;
            d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d += 3;
        }
    }
}
// The effect over the pictures (Use Effect, MilkDrop Effect): the radio has no video settings of its own, so the video defaults (dither, base, contrast, gradient).
VideoLook md_look(const RadioSettings& c) {
    static const Settings dflt;
    VideoLook l;
    l.fx = c.md_fx_on ? video_fx_canon(std::clamp(c.md_fx, 0, kVideoFxCount - 1)) : 0;
    l.dither = dflt.video_dither; l.base = dflt.video_base; l.fxc = dflt.video_fxc;
    l.brightness = std::clamp(c.md_bright, 0, 100); l.colors = std::clamp(c.md_sat, 0, 100); l.contrast = std::clamp(c.md_contrast, 0, 100);
    if (video_fx_uses_gradient(l.fx)) {   // (the gradient of the player's default colours: left, middle halfway, right)
        int r0, g0, b0, r1, g1, b1;
        l.grad.sat = std::clamp(dflt.video_grad_sat, 0, 100);
        if (try_parse_rgb(dflt.visualizer_color, r0, g0, b0) && try_parse_rgb(dflt.visualizer_color_end, r1, g1, b1)) {
            const int v[3][3] = {{r0, g0, b0}, {(r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2}, {r1, g1, b1}};
            for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) l.grad.c[i][k] = static_cast<unsigned char>(std::clamp(v[i][k], 0, 255));
        }
    }
    return l;
}
// The size of the picture inside w x h units (a unit is par times as wide as high), by Aspect Correction / Aspect Ratio: the whole place (off), the
// place's own shape but no wider than 16:9 (auto), or exactly the chosen shape; never cropped, the rest is bars.
void md_inner(int w, int h, double par, bool aspect, int ratio_idx, int& iw, int& ih) {
    iw = w; ih = h;
    const MilkShape m = milkdrop_shape(aspect, ratio_idx);
    if (m.ratio <= 0 || h <= 0 || w <= 0) return;
    const double real = w * par / h;
    if (real > m.ratio) iw = std::clamp(static_cast<int>(std::lround(h * m.ratio / std::max(0.01, par))), 2, w);
    else if (m.fixed && real < m.ratio) ih = std::clamp(static_cast<int>(std::lround(w * par / m.ratio)), 2, h);
}
} // namespace

bool build_milkdrop_block(const Style& s, const UiModel& m, int scope_w, std::vector<std::string>& rows, int prows) {
    if (scope_w < 8 || !radio_milkdrop_usable(s.c)) return false;
    Milkdrop& md = Milkdrop::instance();
    if (milkdrop_window_running()) {   // the pictures are in the extra window: nothing is drawn or sent here
        rows.assign(prows, spaces(scope_w));
        const std::string t = truncate_str("MilkDrop is in its own window", scope_w - 2);
        const int l = std::max(0, (scope_w - display_width(t)) / 2);
        rows[prows / 2] = spaces(l) + s.header + t + kReset + spaces(std::max(0, scope_w - l - display_width(t)));
        return true;
    }
    const bool pic = m.gfx_proto != GfxProto::None;
    const int cw0 = std::max(4, m.cell_w), ch0 = std::max(8, m.cell_h);
    // a big block is drawn at half the pixels (Kitty scales it up, Sixel repeats them): the visuals are soft anyway
    const int scale = pic && scope_w * cw0 > 1280 ? 2 : 1;
    const int cws = std::max(2, cw0 / scale);
    const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
    const int pw = scope_w * cws, ph = prows * chs;
    // Aspect Correction: the picture is no wider than 16:9 (a wide block gets bars at the sides, not a cut-off preset)
    int ipw = pw, iph = ph;
    md_inner(pw, ph, 1.0, s.c.md_aspect, s.c.md_ratio, ipw, iph);
    const VideoLook look = md_look(s.c);
    bool fresh = false;
    const VideoFrame* f = md.frame(ipw, iph, fresh);   // (the render thread starts here and learns the size)
    // the preset's name for 3 s after a change, in a small box on the bottom row
    std::string name;
    if (s.c.md_show_name && md.since_change() <= 3.0) name = truncate_str(md.preset_name(), std::max(4, scope_w - 4));
    const int nw = name.empty() ? 0 : display_width(name) + 2;
    const int nl = std::max(0, (scope_w - nw) / 2);
    auto name_row = [&](const std::string& left, const std::string& right) {
        return left + "\x1b[0;97;40m " + name + " " + kReset + right;
    };
    if (!f) {   // starting: the status in the middle of the block
        rows.assign(prows, spaces(scope_w));
        const std::string t = truncate_str(md.status().empty() ? std::string("milkdrop: starting ...") : md.status(), scope_w - 2);
        const int l = std::max(0, (scope_w - display_width(t)) / 2);
        rows[prows / 2] = spaces(l) + s.header + t + kReset + spaces(std::max(0, scope_w - l - display_width(t)));
        return true;
    }
    MdPic& st = md_pic();
    if (fresh) ++st.frames;
    if (pic) {
        GfxFrame& g = m.gfx;
        const bool tc = m.gfx_proto == GfxProto::Kitty;
        const std::string key = std::to_string(pw) + "x" + std::to_string(ph) + "|" + std::to_string(ipw) + "x" + std::to_string(iph) + "|" + std::to_string(scope_w) + "|" + std::to_string(nl) + "," +
                                std::to_string(nw) + (tc ? "|t" : s.c.md_fixed_colors ? "|p" : "|a") + (s.c.black_background ? "|b" : "|-") + look.sig();
        const bool remake = key != st.key || !g.plain || g.w != pw || g.h != ph || g.level.size() != static_cast<size_t>(pw) * ph;
        g.plain = true; g.direct = true; g.truecolor = tc;
        g.scale = scale; g.out_cw = cw0; g.out_ch = ch0;
        g.cols = scope_w; g.rows = prows; g.w = pw; g.h = ph;
        g.id = 0; g.grid.clear(); g.cut2_w = 0;
        g.cut_c = nl; g.cut_r = prows - 1; g.cut_w = nw; g.cut_h = nw > 0 ? 1 : 0;
        g.opaque = s.c.black_background;
        g.erase = !s.c.black_background;
        g.active = true;
        g.fresh = fresh || remake;
        if (g.fresh) {
            std::vector<uint8_t> px;
            md_scale(*f, pw, ph, px, ipw, iph);
            g.level.assign(static_cast<size_t>(pw) * ph, 255);
            const bool adaptive = !tc && !s.c.md_fixed_colors;   // (Sixel: up to 240 colours taken from each picture)
            if (look.active() || adaptive) {   // the player's way: look, then the colours, the name box cut out
                VideoFrame vf;
                vf.w = pw; vf.h = ph; vf.rgb.swap(px);
                VideoPicParams pp;
                pp.look = look; pp.t = m.t_sec; pp.truecolor = tc; pp.adaptive = adaptive;
                pp.vcols = scope_w; pp.vrows = prows;
                pp.cut_c = nl; pp.cut_r = prows - 1; pp.cut_w = nw; pp.cut_h = nw > 0 ? 1 : 0;
                VideoPic pic;
                video_make_picture(vf, nullptr, pp, pic);
                g.level.swap(pic.level);
                if (tc) g.rgb.swap(pic.rgb);
                else {
                    g.hue.swap(pic.hue);
                    if (pic.own_pal) g.pal = pic.pal;
                    else { const auto& pal = VideoPlayer::palette(); for (size_t i = 0; i < pal.size() && i < g.pal.size(); ++i) g.pal[i] = pal[i]; }
                }
            } else if (tc) g.rgb.swap(px);
            else {   // Sixel: a 6 x 6 x 6 colour cube (216 colours of the 240 there are)
                g.hue.resize(static_cast<size_t>(pw) * ph);
                for (size_t i = 0, n = g.hue.size(); i < n; ++i) {
                    const int r = (px[i * 3] * 5 + 127) / 255, gg = (px[i * 3 + 1] * 5 + 127) / 255, b = (px[i * 3 + 2] * 5 + 127) / 255;
                    g.hue[i] = static_cast<uint8_t>(r * 36 + gg * 6 + b);
                }
                for (int i = 0; i < 216; ++i)
                    g.pal[static_cast<size_t>(i)] = {static_cast<uint8_t>(i / 36 * 51), static_cast<uint8_t>(i / 6 % 6 * 51), static_cast<uint8_t>(i % 6 * 51)};
            }
            {   // the gaps like the player's: one empty column at the left (to the rightmost spectrum bar), half a cell at the right (as wide as the one at the top and bottom)
                const int lcut = std::min(pw, cws), rcut = std::max(1, cws / 2);
                for (int y = 0; y < ph; ++y) {
                    uint8_t* rowp = g.level.data() + static_cast<size_t>(y) * pw;
                    for (int x = 0; x < lcut; ++x) rowp[x] = 0;
                    for (int x = std::max(0, pw - rcut); x < pw; ++x) rowp[x] = 0;
                }
            }
            if (nw > 0)   // the name box's cells stay free (their pixels transparent)
                for (int y = (prows - 1) * chs; y < ph; ++y)
                    for (int x = nl * cws; x < (nl + nw) * cws && x < pw; ++x) g.level[static_cast<size_t>(y) * pw + x] = 0;
        }
        st.key = key;
        rows.assign(prows, gfx_hole(scope_w));
        if (nw > 0) rows[prows - 1] = name_row(gfx_hole(nl), gfx_hole(std::max(0, scope_w - nl - nw)));
        return true;
    }
    // other terminals: half blocks, two pixels a cell (the upper one the text colour, the lower one the background)
    std::vector<uint8_t> px;
    int icw = scope_w, ich = prows * 2;
    md_inner(scope_w, prows * 2, 2.0 * cw0 / ch0, s.c.md_aspect, s.c.md_ratio, icw, ich);   // (a half block is cw0 wide, ch0 / 2 high)
    md_scale(*f, scope_w, prows * 2, px, icw, ich);
    if (look.active()) {
        VideoFrame vf;
        vf.w = scope_w; vf.h = prows * 2; vf.rgb.swap(px);
        VideoPlayer::apply_look(vf, look, m.t_sec, false, nullptr);
        px.swap(vf.rgb);
    }
    rows.assign(prows, std::string());
    for (int r = 0; r < prows; ++r) {
        std::string line;
        line.reserve(static_cast<size_t>(scope_w) * 40);
        const int last = r == prows - 1 && nw > 0 ? nl : scope_w;
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
    std::string gpu = milkdrop_gpu_label(c.md_gpu);
    { const std::string r = md.renderer(); if (c.md_gpu.empty() && !r.empty()) gpu += " (" + r + ")"; }   // (what draws now, next to "automatic")
    const std::string folder = radio_md_shuffle_label(c);
    auto pct = [](int v) { return std::to_string(std::clamp(v, 0, 100)) + "%"; };
    auto secs = [](int v) { return v <= 0 ? std::string("off") : std::to_string(v) + " s"; };
    const size_t nfold = radio_md_shuffle_paths(c).size();
    const std::string fav_val = !c.md_fav_shuffle ? std::string("off") : nfold == 0 ? std::string("on (none yet: all)") : "on (" + std::to_string(nfold) + ")";
    std::string labels[kMdRowCount], vals[kMdRowCount];
    auto set = [&](int i, const std::string& l, const std::string& v) { labels[i] = l; vals[i] = v; };
    set(kMdPreset, "Preset", preset); set(kMdBrowse, "Preset Overlay", "[ENTER]"); set(kMdRandom, "Random Preset", "[ENTER]");
    set(kMdLock, "Lock Preset", md.locked() ? "on" : "off"); set(kMdName, "Show Preset Name", c.md_show_name ? "on" : "off");
    set(kMdUseFx, "Use Effect", c.md_fx_on ? "on" : "off"); set(kMdFx, "MilkDrop Effect", video_fx_name(video_fx_canon(c.md_fx)));
    set(kMdScope, "Show in the Scope Block", c.scope_mode == 4 ? "on" : "off");
    set(kMdBright, "Brightness", pct(c.md_bright)); set(kMdSat, "Saturation", pct(c.md_sat)); set(kMdContrast, "Contrast", pct(c.md_contrast));
    set(kMdDuration, "Preset Duration", secs(c.md_secs)); set(kMdShuffle, "Shuffle All Presets", c.md_shuffle ? "on" : "off");
    set(kMdFolder, "Shuffle Folder", folder + " (" + std::to_string(nfold) + ")"); set(kMdFavShuffle, "Shuffle " + folder, fav_val);
    set(kMdHardCuts, "Hard Cuts", c.md_hard_cuts ? "on" : "off"); set(kMdCutSens, "Hard Cut Sensitivity", beat);
    set(kMdTransition, "Preset Transition", c.md_transition <= 0 ? std::string("cut") : std::to_string(c.md_transition) + " s");
    set(kMdHcd, "Hard Cut Interval", std::to_string(std::clamp(c.md_hcd, 3, 120)) + " s"); set(kMdVar, "Duration Variance", secs(c.md_var));
    set(kMdFps, "Frame Rate", std::to_string(c.md_fps) + " fps"); set(kMdRes, "Resolution", std::to_string(c.md_res) + " px"); set(kMdGpu, "GPU", gpu);
    set(kMdColors, "Sixel Colours", c.md_fixed_colors ? "fixed" : "adaptive"); set(kMdMesh, "Mesh Detail", mesh_name(c.md_mesh));
    set(kMdAspect, "Aspect Correction", c.md_aspect ? "on" : "off"); set(kMdRatio, "Aspect Ratio", milkdrop_ratio_name(c.md_ratio)); set(kMdClean, "Clean Start", c.md_clean ? "on" : "off");
    auto category = [](int i) -> const char* { return i == kMdPreset ? "GENERAL" : i == kMdBright ? "PICTURE" : i == kMdDuration ? "SHUFFLE / TRANSITION" : i == kMdFps ? "GRAPHICS / PERFORMANCE" : nullptr; };
    // the rows (with the category headings between them); a small terminal shows a window of the lines, following the selection
    std::vector<std::string> body;
    int sel_line = 0;
    for (int i = 0; i < kMdRowCount; ++i) {
        if (const char* h = category(i)) {
            if (i > 0) body.push_back(row("", false));
            body.push_back(box_line(s, s.header + pad_right(h, inner) + kReset, s.border));
        }
        const bool sel = i == m.overlay_row;
        if (sel) sel_line = static_cast<int>(body.size());
        std::string plain;
        if (i == kMdPreset) {
            const std::string lab = std::string(sel ? "> " : "  ") + pad_right(labels[i], 10);
            plain = lab + "< " + truncate_str(vals[i], std::max(4, inner - display_width(lab) - 4)) + " >";
        } else {
            plain = std::string(sel ? "> " : "  ") + pad_right(labels[i], 24) + pad_left(vals[i], std::max(1, inner - 26));
        }
        body.push_back(row(plain, sel, !ok));
    }
    const int extra = ok ? 0 : 2;
    const int vis = std::clamp(m.rows - 8 - extra, 6, static_cast<int>(body.size()));
    const int top = std::clamp(sel_line - vis / 2, 0, static_cast<int>(body.size()) - vis);
    std::vector<std::string> lines;
    lines.push_back(box_top(s, W, vis < static_cast<int>(body.size()) ? "Visuals: MilkDrop (" + std::to_string(m.overlay_row + 1) + "/" + std::to_string(kMdRowCount) + ")" : std::string("Visuals: MilkDrop"), s.border));
    for (int i = top; i < top + vis; ++i) lines.push_back(body[static_cast<size_t>(i)]);
    if (!ok) {
        lines.push_back(row("Not available: " + why, false, true));
        lines.push_back(row("Install projectM 4 and a preset pack (see the manual).", false, true));
    }
    push_legend_block(s, lines, W, {"[UP/DOWN] select   [LEFT/RIGHT] change   [ENTER] change / open",
                                    "[N] / [B] next / previous   [L] lock   [P] preset overlay   [" + menus_key_label(c, "VisualsMenu") + " / ESC] close"});
    return lines;
}


// overlay 12, the preset overlay: the whole screen like the player's -- the list at the left, the visuals as a live preview at the right
// (the preset under the cursor plays at once)
std::vector<std::string> build_mdb_screen(const Style& s, const UiModel& m, int W, int rows_total) {
    const MdBrowse& b = m.mdb;
    const int ph = std::max(4, rows_total - 3);   // the rows between the top border and the bottom border (its legend line 1 sits on it, line 2 below the box)
    int left_w = std::clamp(W * 2 / 5, 30, 64);
    const int mg = 1;
    int pw = W - 3 - left_w - 2 * mg;
    if (pw < 20) { left_w = W - 2; pw = 0; }   // (too narrow for the preview: the list alone)
    const std::string bar = s.border + s.c.box_vertical + kReset;
    std::vector<std::string> pic;
    if (pw > 0) {
        if (!build_milkdrop_block(s, m, pw, pic, ph)) {
            std::string why;
            radio_milkdrop_usable(s.c, &why);
            pic.assign(static_cast<size_t>(ph), spaces(pw));
            const std::string t = truncate_str(why.empty() ? std::string("starting ...") : why, pw);
            const int l = std::max(0, (pw - display_width(t)) / 2);
            pic[static_cast<size_t>(ph / 2)] = spaces(l) + t + spaces(std::max(0, pw - l - display_width(t)));
        }
        if (m.gfx.active) { m.gfx.col = 1 + left_w + 1 + mg; m.gfx.row = 1; m.gfx.crop = 0; }
    }
    const int inner = left_w - 2;
    std::vector<std::string> left;
    {   // where we are
        std::string where;
        if (b.typing || b.searching) {
            where = "Search: " + b.query + (b.typing ? "█" : "");
            if (!b.query.empty()) where += "  (" + std::to_string(b.items.size()) + (b.items.size() >= 2000 ? "+" : "") + ")";
        } else if (b.in_favs) where = "Favourites";
        else if (!b.in_user.empty()) where = "Folder: " + b.in_user;
        else if (b.in_recs) where = "Recommended";
        else {   // Presets / folder / folder (inside a further folder: its name first)
            fs::path rel = path_from_utf8(b.dir).lexically_relative(path_from_utf8(b.root));
            where = "Presets";
            for (const std::string& x : Milkdrop::instance().extra_preset_dirs()) {
                const fs::path xr = path_from_utf8(x), r2 = path_from_utf8(b.dir).lexically_relative(xr);
                if (!r2.empty() && *r2.begin() != "..") { where += " / " + path_utf8(xr.filename()); rel = r2; break; }
            }
            if (!rel.empty() && rel != "." && *rel.begin() != "..") for (const auto& part : rel) where += " / " + path_utf8(part);
        }
        left.push_back(" " + s.header + pad_right(truncate_str(where, inner), inner) + kReset + " ");
        left.push_back(spaces(left_w));
    }
    const int list_rows = std::max(1, ph - static_cast<int>(left.size()));
    const int n = static_cast<int>(b.items.size());
    int top = b.top;
    if (b.sel < top) top = b.sel;
    if (b.sel >= top + list_rows) top = b.sel - list_rows + 1;
    top = std::clamp(top, 0, std::max(0, n - list_rows));
    const std::string shown = Milkdrop::instance().preset_path();
    for (int r = 0; r < list_rows; ++r) {
        const int k = top + r;
        if (k >= n) { left.push_back(spaces(left_w)); continue; }
        const MdBrowseItem& it = b.items[static_cast<size_t>(k)];
        const bool sel = k == b.sel;
        const bool is_fav = it.kind == 3 && !b.in_favs && std::find(favs().begin(), favs().end(), it.path) != favs().end();
        std::string text = it.kind == 0 ? std::string("\xE2\x97\x82 ..")
                         : it.kind == 1 ? "\xE2\x98\x85 Favourites (" + std::to_string(favs().size()) + ")"
                         : it.kind == 5 ? "\xE2\x9C\x94 Recommended (" + it.extra + ")"
                         : it.kind == 6 ? "\xE2\x96\xA4 " + it.name + " (" + it.extra + ")"
                         : it.kind == 2 ? "\xE2\x96\xB8 " + it.name
                         : it.kind == 4 ? "  " + it.name + "  (file gone)"
                         : (is_fav ? "\xE2\x98\x85 " : "  ") + it.name;
        std::string body = pad_right(truncate_str(text, inner), inner);
        if (it.kind == 3 && !it.extra.empty() && display_width(text) + 4 < inner) {   // the folder of a search result, in the room the name leaves
            const std::string w = truncate_str("  " + it.extra, inner - display_width(text));
            body = text + (sel ? w : "\x1b[90m" + w + kReset) + spaces(std::max(0, inner - display_width(text) - display_width(w)));
        }
        const bool dim = it.kind == 4;
        const bool playing = it.kind == 3 && it.path == shown;
        const std::string ink = sel ? std::string("\x1b[7m") : dim ? std::string("\x1b[90m") : (it.kind == 1 || it.kind == 5 || it.kind == 6) ? s.header : playing ? s.list_playing : std::string();
        left.push_back(" " + ink + body + kReset + " ");
    }
    const int count = b.in_recs || !b.in_user.empty() ? std::max(0, n - 1) : b.in_favs ? static_cast<int>(favs().size()) : Milkdrop::instance().preset_count();
    const std::string counts = std::to_string(count) + (b.in_recs ? " recommended" : !b.in_user.empty() ? " in " + b.in_user : b.in_favs ? " favourites" : " presets");
    const std::string fx_now = s.c.md_fx_on ? std::string(video_fx_name(video_fx_canon(s.c.md_fx))) : std::string("off");
    std::vector<std::string> out;
    out.push_back(box_top(s, W, "MILKDROP PRESETS  " + counts + "  |  effect: " + fx_now, s.border));
    for (int r = 0; r < ph; ++r) {
        std::string line = bar + left[static_cast<size_t>(r)];
        if (pw > 0) line += bar + spaces(mg) + (r < static_cast<int>(pic.size()) ? pic[static_cast<size_t>(r)] : spaces(pw)) + kReset + spaces(mg);
        out.push_back(line + bar);
    }
    const int in_w = W - 2;
    std::string leg1, leg2;
    if (b.typing) { leg1 = "type to search (fuzzy)  [UP/DOWN] select"; leg2 = "[ENTER] to the list  [ESC] clear"; }
    else if (b.searching) { leg1 = "[UP/DOWN] select  [ENTER] choose  [a] add to favourites  [A] add to a folder"; leg2 = "[/] search  [ESC / LEFT] clear"; }
    else if (b.in_favs) { leg1 = "[UP/DOWN] select  [ENTER] choose  [D] remove from favourites  [U] undo remove"; leg2 = "[A] add to a folder  [/] search  [E] effect  [LEFT] back  [ESC] close"; }
    else if (!b.in_user.empty()) { leg1 = "[UP/DOWN] select  [ENTER] choose  [D] remove from this folder  [U] undo remove"; leg2 = "[A] add to another folder  [/] search  [E] effect  [LEFT] back  [ESC] close"; }
    else { leg1 = "[ENTER] choose / open  [F] favourites  [a] add to favourites  [A] add to a folder  [N] new folder"; leg2 = "[D] delete folder  [U] undo delete  [/] search  [E] effect  [LEFT] up  [ESC] close"; }
    // (SHIFT+a / SHIFT+u are the capitals here) The legend as in the player: line 1 centred on the bottom border, line 2 below the box, both in the border colour.
    out.push_back(box_bottom_centred(s, W, truncate_str(leg1, W - 6), s.border_bottom));
    {
        const std::string lt = truncate_str(leg2, in_w);
        out.push_back(spaces(std::max(0, (W - display_width(lt)) / 2)) + s.border_bottom + lt + kReset);
    }
    if (b.naming || b.pick_open || !b.del_ask.empty()) {   // the small overlays: the name of a new folder, SHIFT+A (the folder a preset goes into)
        const int PW = b.del_ask.empty() ? 44 : 56, pin = PW - 4;
        std::vector<std::string> panel;
        if (!b.del_ask.empty()) {
            panel.push_back(box_top(s, PW, "DELETE FOLDER?", s.border));
            panel.push_back(box_line(s, s.header + pad_right(truncate_str("\"" + b.del_ask + "\"", pin), pin) + kReset, s.border));
            const size_t cnt = muisc::mdf::load(radio_default_dir(), b.del_ask).size();
            for (const std::string& t : wrap_lines("The folder and its list of " + std::to_string(cnt) + " preset" + (cnt == 1 ? "" : "s") + " are deleted. The presets themselves stay where they are. SHIFT+U brings the folder back.", pin, 6))
                panel.push_back(box_line(s, pad_right(t, pin), s.border));
            panel.push_back(box_bottom(s, PW, "[ENTER/Y] delete  [ESC/N] keep", s.border_bottom));
        } else if (b.naming) {
            panel.push_back(box_top(s, PW, "NEW FOLDER", s.border));
            panel.push_back(box_line(s, s.header + pad_right("The name of the folder:", pin) + kReset, s.border));
            panel.push_back(box_line(s, pad_right(truncate_str(b.new_name + "\xE2\x96\x88", pin), pin), s.border));
            panel.push_back(box_bottom(s, PW, b.new_add ? "[ENTER] make it, add the preset  [ESC] cancel" : "[ENTER] make it  [ESC] cancel", s.border_bottom));
        } else {
            panel.push_back(box_top(s, PW, "ADD TO A FOLDER", s.border));
            panel.push_back(box_line(s, s.header + pad_right(truncate_str(b.pick_title, pin), pin) + kReset, s.border));
            const int np = static_cast<int>(b.pick_names.size());
            const int pvis = std::clamp(rows_total - 12, 3, 12);
            const int ptop = std::clamp(b.pick_sel - pvis / 2, 0, std::max(0, np - pvis));
            for (int i = ptop; i < std::min(np, ptop + pvis); ++i) {
                const bool psel = i == b.pick_sel;
                panel.push_back(box_line(s, (psel ? std::string("\x1b[7m") : std::string()) + pad_right(std::string(psel ? "> " : "  ") + truncate_str(b.pick_names[static_cast<size_t>(i)], pin - 2), pin) + kReset, s.border));
            }
            panel.push_back(box_bottom(s, PW, "[ENTER] add  [ESC] cancel", s.border_bottom));
        }
        const int ph2 = static_cast<int>(panel.size());
        const int px = std::max(0, (W - PW) / 2), py = std::clamp((static_cast<int>(out.size()) - ph2) / 2, 0, std::max(0, static_cast<int>(out.size()) - ph2));
        for (int i = 0; i < ph2; ++i) out[static_cast<size_t>(py + i)] = splice_line(out[static_cast<size_t>(py + i)], px, panel[static_cast<size_t>(i)], PW);
    }
    return out;
}

} // namespace detail

VideoLook radio_milkdrop_look(const RadioSettings& c) { return detail::md_look(c); }

std::vector<std::string> radio_md_folder_choices() {
    std::vector<std::string> v = {"Favourites", "Recommended"};
    for (const std::string& n : muisc::mdf::names(radio_default_dir())) v.push_back(n);
    return v;
}
std::string radio_md_shuffle_label(const RadioSettings& c) {
    const std::string& f = c.md_shuffle_folder;
    if (f.empty() || f == "Favourites") return "Favourites";
    if (f == "Recommended") return f;
    return muisc::mdf::exists(radio_default_dir(), f) ? f : std::string("Favourites");   // (a folder that is gone: the Favourites)
}
std::vector<std::string> radio_md_shuffle_paths(const RadioSettings& c) {
    const std::string l = radio_md_shuffle_label(c);
    if (l == "Favourites") { favs_load(); return favs(); }
    if (l == "Recommended") return recommended();
    return muisc::mdf::load(radio_default_dir(), l);
}
std::string radio_md_folder_step(const RadioSettings& c, int dir) {
    const std::vector<std::string> ch = radio_md_folder_choices();
    const std::string now = radio_md_shuffle_label(c);
    size_t at = 0;
    for (size_t i = 0; i < ch.size(); ++i) if (ch[i] == now) at = i;
    at = (at + (dir < 0 ? ch.size() - 1 : 1)) % ch.size();
    return at == 0 ? std::string() : ch[at];
}

void md_overlay_adjust(RadioSettings& c, int row, int dir, std::string& notice) {
    Milkdrop& md = Milkdrop::instance();
    switch (row) {
        case kMdPreset: if (dir > 0) md.next_preset(); else md.prev_preset(); break;
        case kMdBrowse: break;   // (opened by the key handler: it needs the overlay state)
        case kMdRandom: md.random_preset(); break;
        case kMdLock: md.set_locked(!md.locked()); c.md_lock = md.locked(); notice = md.locked() ? "milkdrop: the preset stays" : "milkdrop: the presets change again"; break;
        case kMdDuration: c.md_secs = radio_md_step(2, c.md_secs, dir); break;
        case kMdShuffle: c.md_shuffle = !c.md_shuffle; break;
        case kMdFolder: c.md_shuffle_folder = radio_md_folder_step(c, dir); notice = "milkdrop: the shuffle plays from " + radio_md_shuffle_label(c) + " (" + std::to_string(radio_md_shuffle_paths(c).size()) + " presets)"; break;
        case kMdBright: c.md_bright = std::clamp(c.md_bright + 5 * dir, 0, 100); break;
        case kMdSat: c.md_sat = std::clamp(c.md_sat + 5 * dir, 0, 100); break;
        case kMdContrast: c.md_contrast = std::clamp(c.md_contrast + 5 * dir, 0, 100); break;
        case kMdFavShuffle:
            c.md_fav_shuffle = !c.md_fav_shuffle;
            notice = !c.md_fav_shuffle ? "milkdrop: all presets" : radio_md_shuffle_paths(c).empty() ? "milkdrop: only " + radio_md_shuffle_label(c) + " -- there are no presets in it yet ([A] / SHIFT+A in the preset overlay): all presets until then"
                                                                                   : "milkdrop: only the " + std::to_string(radio_md_shuffle_paths(c).size()) + " presets of " + radio_md_shuffle_label(c);
            break;
        case kMdCutSens: c.md_cut_sens = std::clamp(c.md_cut_sens + dir, 5, 60); break;
        case kMdHardCuts: c.md_hard_cuts = !c.md_hard_cuts; break;
        case kMdTransition: c.md_transition = std::clamp(c.md_transition + dir, 0, 10); break;
        case kMdFps: c.md_fps = radio_md_step(0, c.md_fps, dir); break;
        case kMdRes: c.md_res = radio_md_step(1, c.md_res, dir); break;
        case kMdGpu: c.md_gpu = radio_md_gpu_step(c.md_gpu, dir); notice = "milkdrop GPU: " + milkdrop_gpu_label(c.md_gpu) + " -- the visuals restart on it"; break;
        case kMdUseFx: c.md_fx_on = !c.md_fx_on; notice = c.md_fx_on ? "milkdrop: the effect is on again" : "milkdrop: no effect on the visuals"; break;
        case kMdFx: c.md_fx = video_fx_step(c.md_fx, dir); notice = std::string("milkdrop effect: ") + video_fx_name(video_fx_canon(c.md_fx)); break;
        case kMdColors: c.md_fixed_colors = !c.md_fixed_colors; notice = c.md_fixed_colors ? "milkdrop: the fixed 256 colours (Sixel)" : "milkdrop: the colours taken from each picture (Sixel)"; break;
        case kMdHcd: c.md_hcd = radio_md_step(3, c.md_hcd, dir); break;
        case kMdVar: c.md_var = radio_md_step(4, c.md_var, dir); break;
        case kMdMesh: c.md_mesh = std::clamp(c.md_mesh + dir, 0, 3); notice = "milkdrop mesh: " + mesh_name(c.md_mesh) + " -- the visuals restart"; break;
        case kMdAspect: c.md_aspect = !c.md_aspect; break;
        case kMdRatio: c.md_ratio = (c.md_ratio + (dir < 0 ? kMilkRatioCount - 1 : 1)) % kMilkRatioCount; notice = std::string("milkdrop aspect ratio: ") + milkdrop_ratio_name(c.md_ratio) + (c.md_aspect ? "" : " (Aspect Correction is off)"); break;
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
    { std::vector<MdUndo> keep_undo = std::move(b.undo); b = MdBrowse{}; b.undo = std::move(keep_undo); }   // (SHIFT+U still knows what was deleted before)
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
        if (b.in_favs || b.in_recs || !b.in_user.empty()) { list_dir(b, b.root, false, ""); return; }
        const fs::path d = path_from_utf8(b.dir.empty() ? b.root : b.dir);
        if (d.lexically_normal() == path_from_utf8(b.root).lexically_normal()) return;
        const std::string from = path_utf8(d);
        list_dir(b, is_extra_root(d) ? b.root : path_utf8(d.parent_path()), false, from);
    };
    auto move_to = [&](int sel) {
        if (n == 0) return;
        b.sel = std::clamp(sel, 0, n - 1);
        const int vis = std::max(3, m.rows - 5);   // (the list rows of the full-screen overlay: the rows less the two borders, the legend row below the box and the two lines on top)
        if (b.sel < b.top) b.top = b.sel;
        if (b.sel >= b.top + vis) b.top = b.sel - vis + 1;
        play_selected(b);
    };
    if (!b.del_ask.empty()) {   // the warning before a folder is deleted
        if (arrow) return true;
        if (k == 'y' || k == 'Y' || k == 13 || k == 10) {
            const std::string name = b.del_ask;
            b.del_ask.clear();
            MdUndo u; u.kind = 1; u.folder = name; u.paths = muisc::mdf::load(radio_default_dir(), name); u.was_shuffle = c.md_shuffle_folder == name;
            muisc::mdf::remove_folder(radio_default_dir(), name);
            md_undo_push(b, u);
            if (c.md_shuffle_folder == name) c.md_shuffle_folder.clear();
            notice = "milkdrop: the folder \"" + name + "\" is deleted (SHIFT+U brings it back)";
            list_dir(b, b.dir, false, "");
            radio_milkdrop_apply(c);
        } else if (k == 27 || k == 'n' || k == 'N') {
            b.del_ask.clear();
            notice = "milkdrop: the folder is kept";
        }
        return true;
    }
    if (b.naming) {   // the name of a new folder
        if (arrow) return true;
        if (k == 27) { b.naming = false; b.new_add = false; notice = "milkdrop: no folder made"; return true; }
        if (k == 13 || k == 10) {
            std::string made;
            if (!muisc::mdf::create(radio_default_dir(), b.new_name, &made)) notice = b.new_name.empty() ? "milkdrop: a folder needs a name" : "milkdrop: no folder made -- \"" + b.new_name + "\" is there already or is a name of its own (Favourites, Recommended)";
            else {
                notice = "milkdrop: the folder \"" + made + "\" is made";
                if (b.new_add && !b.pick_preset.empty()) { muisc::mdf::add(radio_default_dir(), made, b.pick_preset); notice += ", the preset is in it"; }
                if (!b.in_favs && !b.in_recs && b.in_user.empty() && !b.searching) list_dir(b, b.dir, false, "");
            }
            b.naming = false; b.new_add = false;
            return true;
        }
        if (k == 127 || k == 8) { while (!b.new_name.empty()) { const unsigned char ch = static_cast<unsigned char>(b.new_name.back()); b.new_name.pop_back(); if ((ch & 0xC0) != 0x80) break; } return true; }
        if (((k >= 32 && k < 127) || (k >= 128 && k < 256)) && b.new_name.size() < 60) b.new_name.push_back(static_cast<char>(k));
        return true;
    }
    if (b.pick_open) {   // SHIFT+A: the folder the preset goes into
        const int np = static_cast<int>(b.pick_names.size());
        if (arrow) {
            if (k == 'A') b.pick_sel = (b.pick_sel + np - 1) % np;
            else if (k == 'B') b.pick_sel = (b.pick_sel + 1) % np;
            else if (k == 'D') b.pick_open = false;
            return true;
        }
        if (k == 27) { b.pick_open = false; return true; }
        if (k == 13 || k == 10 || k == ' ') {
            const std::string name = b.pick_names[static_cast<size_t>(b.pick_sel)];
            b.pick_open = false;
            if (b.pick_sel == np - 1) { b.naming = true; b.new_name.clear(); b.new_add = true; return true; }
            if (name == "Favourites") {
                favs_load();
                if (std::find(favs().begin(), favs().end(), b.pick_preset) != favs().end()) notice = "milkdrop: " + b.pick_title + " is a favourite already";
                else { favs().push_back(b.pick_preset); favs_save(); notice = "milkdrop: " + b.pick_title + " added to the favourites (" + std::to_string(favs().size()) + ")"; }
            } else {
                const int r = muisc::mdf::add(radio_default_dir(), name, b.pick_preset);
                notice = r == 0 ? "milkdrop: " + b.pick_title + " added to \"" + name + "\"" : r == 1 ? "milkdrop: " + b.pick_title + " is in \"" + name + "\" already" : "milkdrop: the folder \"" + name + "\" could not be written";
                if (b.in_user == name) list_dir(b, b.dir, false, b.pick_preset);
            }
            radio_milkdrop_apply(c);
            return true;
        }
        return true;
    }
    if (b.typing) {
        if (k == 13 || k == 10) { b.typing = false; if (b.items.empty()) { b.searching = false; list_dir(b, b.root, false, ""); } else play_selected(b); }
        else if (k == 27 && !arrow) { b.typing = b.searching = false; b.query.clear(); list_dir(b, b.root, false, ""); }
        else if (k == 127 || k == 8) {
            if (b.query.empty()) { b.typing = b.searching = false; list_dir(b, b.root, false, ""); }
            else { do b.query.pop_back(); while (!b.query.empty() && (static_cast<unsigned char>(b.query.back()) & 0xC0) == 0x80); search_run(b); play_selected(b); }
        }
        else if (arrow && (k == 'A' || k == 'B')) { b.typing = false; move_to(b.sel + (k == 'B' ? 1 : -1)); }
        else if (!arrow && ((k >= 32 && k < 127) || (k >= 128 && k < 256))) { b.query.push_back(static_cast<char>(k)); if (k < 128 || (k & 0xC0) == 0x80) { search_run(b); play_selected(b); } }
        return true;
    }
    if (!arrow && k == kKeyHome) { move_to(0); return true; }
    if (!arrow && k == kKeyEnd) { move_to(n - 1); return true; }
    if (arrow) {
        const int step = last_key_was_shifted() ? 12 : 1;   // (SHIFT: a page)
        if (k == 'A') move_to(b.sel - step);
        else if (k == 'B') move_to(b.sel + step);
        else if (k == 'D') up();
        else if (k == 'C') { const MdBrowseItem* it = current(); if (it && it->kind != 3 && it->kind != 4) k = 13; }
        if (k != 13) return true;
    }
    if (k == 27) { if (b.searching || b.in_favs || b.in_recs || !b.in_user.empty()) up(); else finish(false); return true; }
    if (k == 127 || k == 8) { const MdBrowseItem* it0 = n > 0 ? &b.items[0] : nullptr; if (it0 && it0->kind == 0) up(); return true; }   // (BACKSPACE: up a folder)
    if (k == 'e' || k == 'E') { c.md_fx_on = !c.md_fx_on; notice = c.md_fx_on ? "milkdrop: the effect is on" : "milkdrop: no effect on the visuals"; radio_milkdrop_apply(c); return true; }
    if (k == 'g' || k == 'G' || k == 'h' || k == 'H') {   // [g] / [h]: the next / previous effect
        c.md_fx = video_fx_step(c.md_fx, k == 'h' || k == 'H' ? -1 : 1); c.md_fx_on = true;
        notice = std::string("milkdrop effect: ") + video_fx_name(video_fx_canon(c.md_fx));
        radio_milkdrop_apply(c);
        return true;
    }
    if (k == '/') { b.typing = b.searching = true; b.query.clear(); b.items.clear(); b.sel = b.top = 0; return true; }
    if (k == 'f' || k == 'F') { list_dir(b, "", true, ""); return true; }
    if ((k == 'n' || k == 'N') && !arrow) {   // a new folder of presets
        if (b.searching || b.in_favs || b.in_recs || !b.in_user.empty()) { notice = "milkdrop: [N] makes a new folder (in the list of the preset folders: LEFT goes up)"; return true; }
        b.naming = true; b.new_name.clear(); b.new_add = false; b.pick_preset.clear();
        return true;
    }
    if (k == 'U' && !arrow) {   // SHIFT+U: takes back the last delete
        if (b.undo.empty()) { notice = "milkdrop: nothing to undo"; return true; }
        const MdUndo u = b.undo.back();
        b.undo.pop_back();
        if (u.kind == 1) {
            if (muisc::mdf::exists(radio_default_dir(), u.folder)) { notice = "milkdrop: the folder \"" + u.folder + "\" is there again already"; return true; }
            muisc::mdf::save(radio_default_dir(), u.folder, u.paths);
            if (u.was_shuffle) c.md_shuffle_folder = u.folder;
            notice = "milkdrop: the folder \"" + u.folder + "\" is back (" + std::to_string(u.paths.size()) + " presets)";
            if (!b.in_favs && !b.in_recs && b.in_user.empty() && !b.searching) list_dir(b, b.dir, false, "");
        } else if (u.kind == 2) {
            std::vector<std::string> v = muisc::mdf::load(radio_default_dir(), u.folder);
            if (std::find(v.begin(), v.end(), u.preset) == v.end()) v.insert(v.begin() + static_cast<std::ptrdiff_t>(std::min(u.index, v.size())), u.preset);
            muisc::mdf::save(radio_default_dir(), u.folder, v);
            notice = "milkdrop: the preset is back in \"" + u.folder + "\"";
            if (b.in_user == u.folder) list_dir(b, b.dir, false, u.preset);
        } else {
            favs_load();
            if (std::find(favs().begin(), favs().end(), u.preset) == favs().end()) favs().insert(favs().begin() + static_cast<std::ptrdiff_t>(std::min(u.index, favs().size())), u.preset);
            favs_save();
            notice = "milkdrop: the preset is back in the favourites (" + std::to_string(favs().size()) + ")";
            if (b.in_favs) list_dir(b, "", true, u.preset);
        }
        radio_milkdrop_apply(c);
        return true;
    }
    if (k == 'A') {   // SHIFT+A: the folder this preset goes into
        const MdBrowseItem* it = current();
        if (!it || it->kind != 3) { notice = "milkdrop: SHIFT+A adds a preset (not a folder) to a folder of your choice"; return true; }
        b.pick_names.clear();
        b.pick_names.push_back("Favourites");
        for (const std::string& nm : muisc::mdf::names(radio_default_dir())) b.pick_names.push_back(nm);
        b.pick_names.push_back("+ New folder ...");
        b.pick_sel = 0; b.pick_preset = it->path; b.pick_title = it->name; b.pick_open = true;
        return true;
    }
    if ((k == 'd' || k == 'D' || k == kKeyDelete) && !b.in_user.empty()) {
        const MdBrowseItem* it = current();
        if (!it || (it->kind != 3 && it->kind != 4)) return true;
        const std::string name = it->name, path = it->path, folder = b.in_user;
        {
            MdUndo u; u.kind = 2; u.folder = folder; u.preset = path;
            const std::vector<std::string> before = muisc::mdf::load(radio_default_dir(), folder);
            u.index = static_cast<size_t>(std::find(before.begin(), before.end(), path) - before.begin());
            md_undo_push(b, u);
        }
        muisc::mdf::remove_preset(radio_default_dir(), folder, path);
        list_dir(b, b.dir, false, "");
        b.sel = std::clamp(b.sel, 0, std::max(0, static_cast<int>(b.items.size()) - 1));
        notice = "milkdrop: " + name + " taken out of \"" + folder + "\" (SHIFT+U brings it back)";
        radio_milkdrop_apply(c);
        return true;
    }
    if ((k == 'd' || k == 'D' || k == kKeyDelete) && current() && current()->kind == 6) {   // a folder of the user: a second press deletes it (the presets stay where they are)
        const std::string name = current()->name;
        b.del_ask = name;   // (a warning first: [ENTER] / [Y] deletes it, SHIFT+U brings it back)
        return true;
    }
    if (k == 'a') {
        const MdBrowseItem* it = current();
        if (!it || it->kind != 3) { notice = "milkdrop: [a] adds a preset (not a folder) to the favourites"; return true; }
        favs_load();
        if (std::find(favs().begin(), favs().end(), it->path) != favs().end()) { notice = "milkdrop: " + it->name + " is a favourite already"; return true; }
        favs().push_back(it->path);
        favs_save();
        notice = "milkdrop: " + it->name + " added to the favourites (" + std::to_string(favs().size()) + ")";
        radio_milkdrop_apply(c);
        return true;
    }
    if (k == 'd' || k == 'D' || k == kKeyDelete) {
        const MdBrowseItem* it = current();
        if (!b.in_favs || !it || it->kind == 0 || it->kind == 1) { notice = "milkdrop: [D] takes a preset out of the favourites (in the Favourites folder)"; return true; }
        favs_load();
        const std::string name = it->name, path = it->path;
        {
            MdUndo u; u.kind = 3; u.preset = path;
            u.index = static_cast<size_t>(std::find(favs().begin(), favs().end(), path) - favs().begin());
            md_undo_push(b, u);
        }
        favs().erase(std::remove(favs().begin(), favs().end(), path), favs().end());
        favs_save();
        list_dir(b, "", true, "");
        b.sel = std::clamp(b.sel, 0, std::max(0, static_cast<int>(b.items.size()) - 1));
        notice = "milkdrop: " + name + " taken out of the favourites (" + std::to_string(favs().size()) + " left, SHIFT+U brings it back)";
        radio_milkdrop_apply(c);
        return true;
    }
    if (k == 13 || k == 10) {
        const MdBrowseItem* it = current();
        if (!it) return true;
        const MdBrowseItem item = *it;
        if (item.kind == 0) { if (b.searching || b.in_favs || b.in_recs || !b.in_user.empty()) up(); else list_dir(b, item.path, false, b.dir); }
        else if (item.kind == 1) list_dir(b, "", true, "");
        else if (item.kind == 5) list_dir(b, "<recommended>", false, "");
        else if (item.kind == 6) list_dir(b, item.path, false, "");
        else if (item.kind == 2) list_dir(b, item.path, false, "");
        else if (item.kind == 3) { md.play_path(item.path); notice = "milkdrop: " + item.name; finish(true); }
        else if (item.kind == 4) notice = "milkdrop: this favourite's file is gone ([D] takes it out)";
    }
    return true;
}

} // namespace muisc::radio
