// app_milkdrop.cpp
// Part of class App (src/app.h): the MilkDrop visuals (milkdrop.h) in the visualization field, and the visuals overlay (":" by default,
// HKeyVisualsMenu): the preset (previous / next / random / lock) and the MilkDrop options, which are the same values as Settings ->
// ANIMA/VIZ -> MILKDROP.
#include "app.h"
#include "app_internal.h"
#include "cover_store.h"
#include "meta_state.h"
#include "milkdrop.h"
#include "path_utf8.h"
#include "settings.h"
#include "terminal_ui.h"
#include "video_fx.h"
#include "video_timing.h"
#include "library_search.h"
#include "text_edit.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace muisc {

namespace {
const int kMdFpsSteps[] = {15, 20, 25, 30, 45, 60};
const int kMdResSteps[] = {320, 480, 640, 960, 1280};
const int kMdDurSteps[] = {0, 10, 15, 20, 25, 30, 45, 60, 90, 120, 180, 300};
template <size_t N> int md_step(const int (&a)[N], int cur, int dir) {
    // the nearest step, then one on (a value from config.txt between two steps snaps onto them)
    size_t k = 0;
    for (size_t i = 0; i < N; ++i) if (std::abs(a[i] - cur) < std::abs(a[k] - cur)) k = i;
    const long n = std::clamp<long>(static_cast<long>(k) + dir, 0, static_cast<long>(N) - 1);
    return a[static_cast<size_t>(n)];
}
enum MdRow { kMdPreset, kMdBrowse, kMdRandom, kMdLock, kMdDuration, kMdShuffle, kMdFavShuffle, kMdBeat, kMdHardCuts, kMdTransition, kMdFps, kMdRes, kMdFx, kMdColors, kMdName, kMdView, kMdRows };
} // namespace

void App::milkdrop_apply_settings() const {
    MilkdropOptions o;
    o.presets_dir = settings_.milkdrop_presets;
    o.fps = std::clamp(settings_.milkdrop_fps, 10, 60);
    o.max_width = std::clamp(settings_.milkdrop_res, 160, 1920);
    o.preset_seconds = std::max(0, settings_.milkdrop_preset_secs);
    o.shuffle = settings_.milkdrop_shuffle;
    o.beat = static_cast<float>(std::clamp(settings_.milkdrop_beat, 0, 20)) / 10.0f;
    o.hard_cuts = settings_.milkdrop_hard_cuts;
    o.transition = std::clamp(settings_.milkdrop_transition, 0, 10);
    o.favourites_only = settings_.milkdrop_fav_shuffle;
    if (o.favourites_only) { md_favs_load(); o.favourites = md_favs_; }
    Milkdrop::instance().configure(o);
}

bool App::milkdrop_usable() const {
    if (!settings_.use_milkdrop) return false;
    milkdrop_apply_settings();   // (the preset folder decides whether it can run)
    return Milkdrop::instance().available();
}

std::string App::visuals_key_text() const {
    const std::string k = hotkey_text("HKeyVisualsMenu", ":");
    return k == "-" ? "no key: Settings > REFERENCE" : k;   // (unbound: a bare "-" read like the volume key)
}

std::vector<std::string> App::milkdrop_name_box(int width) const {
    Milkdrop& m = Milkdrop::instance();
    if (!settings_.milkdrop_show_name || m.since_change() > 3.0) return {};
    const std::string n = m.preset_name();
    if (n.empty()) return {};
    // centred in the width of the place (width + 2 = the ov_w the callers pass): the box is measured on the row's padding, and a row
    // without it came out wider than the place, so the box was left out (or, with a lyric line's sizes, drawn the wrong size)
    const std::string t = truncate_str(n, std::max(4, width - 4));
    const int tw = display_width(t), all = width + 2;
    const int l = std::max(1, (all - tw) / 2), r = std::max(1, all - tw - l);
    return {std::string(static_cast<size_t>(l), ' ') + "\x1b[97m" + t + "\x1b[0m" + std::string(static_cast<size_t>(r), ' ')};
}

bool App::milkdrop_field_rows(std::vector<std::string>& rows, int W, int H, int area_col, bool second_slot, bool trim_right) const {
    if (W < 8 || H < 3) return false;
    const std::vector<std::string> box = milkdrop_name_box(W - 2);
    // the whole field, like the full-field video; the preset's name in a small box over its bottom
    return milkdrop_draw(rows, W, H, area_col, 1, second_slot, 2, box, box.empty() ? 0 : W, false, trim_right);
}

void App::milkdrop_track_apply() {
    MilkdropTrackOpts mo;
    if (!audiobook_ && has_track_ && current_.is_local && !current_.path.empty()) mo = cover_track_milkdrop(path_utf8(current_.path));
    // first give back what was done for the track before (unless the view was changed by hand meanwhile)
    // (the lock as it was before the track before locked its preset: what was set by hand comes back, not "unlocked")
    if (md_track_locked_ && (mo.preset.empty() || mo.keep == 0)) { Milkdrop::instance().set_locked(md_user_locked_); md_track_locked_ = false; }
    md_track_fx_ = mo.fx >= 0 && mo.fx < kVideoFxCount ? mo.fx : -1;
    if (md_track_view_ && mo.on != 1) {
        md_track_view_ = false;
        if (settings_.milkdrop_view) {
            settings_.milkdrop_view = false;
            settings_.element_lyrics = md_prev_lyrics_;
            settings_.video_view = md_prev_video_ && settings_.video_mode;
            settings_.lyric_viz = md_prev_viz_;
            if (settings_.element_lyrics && has_track_ && !(lyrics_.ready && lyrics_.path == current_.path) &&
                !(settings_.video_view && settings_.novideo_viz != 4 && video_shown_for(current_.path).empty())) {
                last_lyrics_status_.clear();
                launch_lyrics_fetch(metadata_.name, metadata_.artist == "-" ? "" : metadata_.artist, current_.path);
            }
        }
    }
    if (md_track_off_ && mo.on != 0) {
        md_track_off_ = false;
        if (!settings_.milkdrop_view && !settings_.element_lyrics && !settings_.video_view && milkdrop_usable()) settings_.milkdrop_view = true;
    }
    if (mo.on < 0 && mo.preset.empty()) return;   // (nothing set for this track: nothing is loaded)
    if (mo.on == 1 && !settings_.milkdrop_view && settings_.element_vizfield && milkdrop_usable()) {
        md_track_view_ = true;
        md_prev_lyrics_ = settings_.element_lyrics; md_prev_video_ = settings_.video_view; md_prev_viz_ = settings_.lyric_viz;
        settings_.element_lyrics = false;
        settings_.video_view = false;
        settings_.milkdrop_view = true;
    } else if (mo.on == 0 && settings_.milkdrop_view) {
        md_track_off_ = true;
        settings_.milkdrop_view = false;   // (the visual under it shows: the sphere, the oscilloscope or the spectrogram)
    }
    if (!mo.preset.empty() && settings_.use_milkdrop && milkdrop_usable()) {
        Milkdrop& m = Milkdrop::instance();
        // a path (chosen in More Options) is played by its file; an older record has only the name (looked up in the list)
        if (mo.preset.find('/') != std::string::npos || mo.preset.find('\\') != std::string::npos) m.play_path(mo.preset);
        else m.play_named(mo.preset);
        if (mo.keep != 0) {   // (kept while the track plays)
            if (!md_track_locked_) md_user_locked_ = m.locked();
            m.set_locked(true);
            md_track_locked_ = true;
        }
    }
}

// Auto MilkDrop: the field shows the visuals for every track. Auto Video would take the field for a track with a video, so it is
// switched off meanwhile -- and back on when Auto MilkDrop goes off, if it was on (remembered in config.txt: MilkDropAutoVideoWas=).
void App::milkdrop_auto_set(bool on) {
    if (on == settings_.milkdrop_auto) return;
    if (on && !milkdrop_usable()) {
        std::string why;
        Milkdrop::instance().available(&why);
        status_line_ = settings_.use_milkdrop ? "milkdrop: cannot run here -- " + why : "milkdrop: switched off (Settings -> ANIMA/VIZ -> Use MilkDrop)";
        return;
    }
    settings_.milkdrop_auto = on;
    if (on) {
        if (settings_.video_auto) { settings_.video_auto = false; settings_.milkdrop_auto_video_was = true; }
        settings_.video_view = false;
        settings_.milkdrop_view = true;
        status_line_ = std::string("auto milkdrop: the visuals for every track") + (settings_.milkdrop_auto_video_was ? " (Auto Video is off meanwhile)" : "");
    } else {
        settings_.milkdrop_view = false;
        const bool back = settings_.milkdrop_auto_video_was;
        settings_.milkdrop_auto_video_was = false;
        status_line_ = std::string("auto milkdrop off") + (back ? ": Auto Video is on again" : "");
        if (back) { settings_.video_auto = true; video_auto_apply(); }
    }
    settings_dirty_ = true;
    force_redraw_ = true;
}

void App::milkdrop_auto_apply() {
    if (!settings_.milkdrop_auto || audiobook_ || !milkdrop_usable()) return;
    settings_.video_view = false;
    settings_.milkdrop_view = true;
}

// ---- the overlay ----

std::vector<std::string> App::build_milkdrop_menu_panel() const {
    const int W = kMdMenuPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m", DIM = "\x1b[90m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi, bool dim = false) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : dim ? DIM + body + R : body) + " " + bar;
    };
    Milkdrop& m = Milkdrop::instance();
    milkdrop_apply_settings();
    std::string why;
    const bool ok = m.available(&why);
    char beat[8];
    std::snprintf(beat, sizeof beat, "%.1f", std::clamp(settings_.milkdrop_beat, 0, 20) / 10.0);
    std::string preset = m.preset_name();
    if (preset.empty()) preset = m.running() ? "(starting ...)" : "(shows when the visuals run)";
    md_favs_load();
    const std::string labels[kMdRows] = {"Preset", "Preset Overlay", "Random Preset", "Lock Preset", "Preset Duration", "Shuffle Presets", "Shuffle Favourites", "Beat Sensitivity",
                                         "Hard Cuts", "Preset Transition", "Frame Rate", "Resolution", "Effect", "Sixel Colours", "Show Preset Name", "Auto MilkDrop"};
    const std::string vals[kMdRows] = {
        preset,
        "[ENTER]",
        "[ENTER]",
        m.locked() ? "on" : "off",
        settings_.milkdrop_preset_secs <= 0 ? std::string("off") : std::to_string(settings_.milkdrop_preset_secs) + " s",
        settings_.milkdrop_shuffle ? "on" : "off",
        !settings_.milkdrop_fav_shuffle ? std::string("off") : md_favs_.empty() ? std::string("on (none yet: all)") : "on (" + std::to_string(md_favs_.size()) + ")",
        beat,
        settings_.milkdrop_hard_cuts ? "on" : "off",
        settings_.milkdrop_transition <= 0 ? std::string("cut") : std::to_string(settings_.milkdrop_transition) + " s",
        std::to_string(settings_.milkdrop_fps) + " fps",
        std::to_string(settings_.milkdrop_res) + " px",
        video_fx_name(video_fx_canon(settings_.milkdrop_fx)),
        settings_.milkdrop_fixed_colors ? "fixed" : "adaptive",
        settings_.milkdrop_show_name ? "on" : "off",
        settings_.milkdrop_auto ? "on" : "off",
    };
    std::vector<std::string> lines;
    lines.push_back(box_top("Visuals: MilkDrop", W, border));
    for (int i = 0; i < kMdRows; ++i) {
        const bool sel = i == md_menu_row_;
        std::string plain;
        if (i == kMdPreset) {   // the preset's name gets the room the label leaves
            const std::string lab = std::string(sel ? "> " : "  ") + pad_right(labels[i], 10);
            plain = lab + "< " + truncate_str(vals[i], std::max(4, inner - display_width(lab) - 4)) + " >";
        } else {
            plain = std::string(sel ? "> " : "  ") + pad_right(labels[i], 22) + pad_left(vals[i], std::max(1, inner - 24));
        }
        lines.push_back(row(plain, sel, !ok));
    }
    lines.push_back(row("", false));
    if (!ok) {
        lines.push_back(row("Not available: " + why, false, true));
        lines.push_back(row("Install projectM 4 and a preset pack (see the manual).", false, true));
    } else {
        char info[160];
        const int idx = m.preset_index(), cnt = m.preset_count();
        if (cnt > 0 && idx >= 0) std::snprintf(info, sizeof info, "preset %d of %d   one picture %.0f ms", idx + 1, cnt, m.frame_ms());
        else if (cnt > 0) std::snprintf(info, sizeof info, "a preset outside the list (%d in it)   one picture %.0f ms", cnt, m.frame_ms());
        else std::snprintf(info, sizeof info, "%s", m.running() ? "starting ..." : "not running (nothing shows it)");
        lines.push_back(row(info, false, true));
        if (md_pic_w_ > 0 && gfxs_.proto != GfxProto::None) {   // (what each picture costs on its way to the terminal: the big overlay sends the most)
            char pi[160];
            std::snprintf(pi, sizeof pi, "picture %dx%d px: colours %.0f ms, to the terminal %.0f ms", md_pic_w_, md_pic_h_,
                          video_timing().quant_ms.load(), video_timing().encode_ms.load());
            lines.push_back(row(pi, false, true));
        }
        const std::string r = m.renderer();
        if (!r.empty()) lines.push_back(row("drawn by " + r + (r.find("llvmpipe") != std::string::npos || r.find("softpipe") != std::string::npos ? " (software)" : ""), false, true));
        const std::string d = m.presets_dir();
        if (!d.empty()) lines.push_back(row("presets: " + d, false, true));
    }
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + "\x1b[0m" + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[UP/DOWN] select   [LEFT/RIGHT] change   [ENTER] random", "[N] / [B] next / previous   [L] lock   [" + (hotkey_text("HKeyVisualsMenu", ":") == "-" ? std::string() : hotkey_text("HKeyVisualsMenu", ":") + " / ") + "ESC] close"}, leg_in, leg_bot);
    return lines;
}

void App::milkdrop_menu_adjust(int row, int dir) {
    Milkdrop& m = Milkdrop::instance();
    switch (row) {
        case kMdPreset: if (dir > 0) m.next_preset(); else m.prev_preset(); break;
        case kMdRandom: m.random_preset(); break;
        case kMdLock: m.set_locked(!m.locked()); status_line_ = m.locked() ? "milkdrop: the preset stays" : "milkdrop: the presets change again"; break;
        case kMdDuration: settings_.milkdrop_preset_secs = md_step(kMdDurSteps, settings_.milkdrop_preset_secs, dir); break;
        case kMdBrowse: if (dir > 0) mdb_open(); return;
        case kMdShuffle: settings_.milkdrop_shuffle = !settings_.milkdrop_shuffle; break;
        case kMdFavShuffle:
            settings_.milkdrop_fav_shuffle = !settings_.milkdrop_fav_shuffle;
            md_favs_load();
            status_line_ = !settings_.milkdrop_fav_shuffle ? "milkdrop: all presets again"
                         : md_favs_.empty() ? "milkdrop: only favourites -- there are none yet ([a] in the Preset Overlay): all presets until then"
                         : "milkdrop: only the " + std::to_string(md_favs_.size()) + " favourites";
            break;
        case kMdBeat: settings_.milkdrop_beat = std::clamp(settings_.milkdrop_beat + dir, 0, 20); break;
        case kMdHardCuts: settings_.milkdrop_hard_cuts = !settings_.milkdrop_hard_cuts; break;
        case kMdTransition: settings_.milkdrop_transition = std::clamp(settings_.milkdrop_transition + dir, 0, 10); break;
        case kMdFps: settings_.milkdrop_fps = md_step(kMdFpsSteps, settings_.milkdrop_fps, dir); break;
        case kMdRes: settings_.milkdrop_res = md_step(kMdResSteps, settings_.milkdrop_res, dir); milkdrop_pic_key_.clear(); break;
        case kMdFx:
            settings_.milkdrop_fx = video_fx_step(settings_.milkdrop_fx, dir);
            milkdrop_pic_key_.clear();
            status_line_ = std::string("milkdrop effect: ") + video_fx_name(settings_.milkdrop_fx);
            break;
        case kMdColors:
            settings_.milkdrop_fixed_colors = !settings_.milkdrop_fixed_colors;
            milkdrop_pic_key_.clear();
            status_line_ = settings_.milkdrop_fixed_colors ? "milkdrop: the fixed 256 colours (Sixel)" : "milkdrop: the colours taken from each picture (Sixel)";
            break;
        case kMdName: settings_.milkdrop_show_name = !settings_.milkdrop_show_name; break;
        case kMdView: milkdrop_auto_set(!settings_.milkdrop_auto); break;
        default: break;
    }
    milkdrop_apply_settings();
}

void App::handle_milkdrop_menu_key(int key) {
    if (mdb_open_) { handle_mdb_key(key); return; }
    const bool arrow = last_key_was_arrow();
    if (arrow && key == 'A') { md_menu_row_ = (md_menu_row_ + kMdRows - 1) % kMdRows; return; }
    if (arrow && key == 'B') { md_menu_row_ = (md_menu_row_ + 1) % kMdRows; return; }
    if (arrow && (key == 'C' || key == 'D')) { milkdrop_menu_adjust(md_menu_row_, key == 'C' ? 1 : -1); return; }
    if (arrow) return;
    if (key == '\r' || key == '\n') {
        if (md_menu_row_ == kMdBrowse) { mdb_open(); return; }
        if (md_menu_row_ == kMdRandom || md_menu_row_ == kMdPreset) Milkdrop::instance().random_preset();
        else milkdrop_menu_adjust(md_menu_row_, 1);
        return;
    }
    if (key == ' ') { milkdrop_menu_adjust(md_menu_row_, 1); return; }
    if (key == 'n' || key == 'N') { Milkdrop::instance().next_preset(); return; }
    if (key == 'b' || key == 'B') { Milkdrop::instance().prev_preset(); return; }
    if (key == 'l' || key == 'L') { milkdrop_menu_adjust(kMdLock, 1); return; }
    if (key == 27 || key_is("HKeyVisualsMenu", key)) {
        mode_ = Mode::Browse;
        save_settings(settings_);
    }
}

// ---- the preset overlay ----

namespace {
bool is_preset_file(const fs::path& p) {
    const fs::path e = p.extension();
    return e == ".milk" || e == ".prjm" || e == ".MILK";
}
std::string lower_ascii(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
fs::path md_favs_file() { return config_path().parent_path() / "milkdrop_favourites.txt"; }
} // namespace

void App::md_favs_load() const {
    if (md_favs_loaded_) return;
    md_favs_loaded_ = true;
    md_favs_.clear();
    std::ifstream in(md_favs_file(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (std::find(md_favs_.begin(), md_favs_.end(), line) == md_favs_.end()) md_favs_.push_back(line);
    }
}

void App::md_favs_save() const {
    std::ostringstream o;
    o << "# MilkDrop favourites (Mousiki): one preset file per line, full paths. [a] / [d] in the preset overlay.\n";
    for (const auto& f : md_favs_) o << f << "\n";
    write_text_atomic(md_favs_file(), o.str());
}

void App::mdb_list(const std::string& dir, bool favs, const std::string& select_path) {
    mdb_items_.clear();
    mdb_in_favs_ = favs;
    mdb_dir_ = dir;
    const fs::path root = path_from_utf8(mdb_root_);
    if (favs) {
        mdb_items_.push_back({0, "..", mdb_root_, ""});
        md_favs_load();
        std::error_code ec;
        for (const auto& f : md_favs_) {
            const fs::path p = path_from_utf8(f);
            const bool there = fs::is_regular_file(p, ec);
            mdb_items_.push_back({there ? 3 : 4, path_utf8(p.stem()), f, ""});
        }
    } else {
        const fs::path d = path_from_utf8(dir);
        if (d != root && !dir.empty()) mdb_items_.push_back({0, "..", path_utf8(d.parent_path()), ""});
        else {
            md_favs_load();
            mdb_items_.push_back({1, "Favourites", "", ""});
        }
        std::vector<MdbItem> folders, presets;
        std::error_code ec;
        for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
            const fs::path& p = it->path();
            if (it->is_directory(ec)) {
                if (p.filename() == "textures") continue;   // (the texture pack, when it sits inside the preset folder)
                folders.push_back({2, path_utf8(p.filename()), path_utf8(p), ""});
            } else if (is_preset_file(p)) presets.push_back({3, path_utf8(p.stem()), path_utf8(p), ""});
        }
        auto by_name = [](const MdbItem& a, const MdbItem& b) { return lower_ascii(a.name) < lower_ascii(b.name); };
        std::sort(folders.begin(), folders.end(), by_name);
        std::sort(presets.begin(), presets.end(), by_name);
        for (auto& f : folders) mdb_items_.push_back(std::move(f));
        for (auto& p : presets) mdb_items_.push_back(std::move(p));
    }
    mdb_sel_ = 0;
    if (!select_path.empty())
        for (size_t k = 0; k < mdb_items_.size(); ++k)
            if (mdb_items_[k].path == select_path) { mdb_sel_ = static_cast<int>(k); break; }
    if (mdb_sel_ == 0 && mdb_items_.size() > 1 && mdb_items_[0].kind == 0) mdb_sel_ = 1;   // (not on "..")
    mdb_top_ = std::max(0, mdb_sel_ - 3);
}

void App::mdb_open() {
    mdb_track_.clear();
    mdb_opt_ = -1;
    Milkdrop& m = Milkdrop::instance();
    std::string why;
    if (!settings_.use_milkdrop || !m.available(&why)) {
        status_line_ = settings_.use_milkdrop ? "milkdrop: cannot run here -- " + why : "milkdrop: switched off (Settings -> ANIMA/VIZ -> Use MilkDrop)";
        return;
    }
    mdb_root_ = m.find_presets_dir();
    if (mdb_root_.empty()) { status_line_ = "milkdrop: no preset folder"; return; }
    mdb_open_ = true;
    mdb_moved_ = false;
    mdb_typing_ = mdb_searching_ = false;
    mdb_query_.clear();
    mdb_prev_path_ = m.preset_path();
    mdb_prev_locked_ = m.locked();
    m.set_locked(true);   // (the preview stays while it is looked at: no timer, no hard cut)
    // open where the preset shown lies, on it (More Options of a track: on the track's own preset, when it has one)
    std::string at = mdb_prev_path_;
    if (!mdb_track_.empty()) {
        const MilkdropTrackOpts mo = cover_track_milkdrop(mdb_track_);
        if (mo.preset.find('/') != std::string::npos || mo.preset.find('\\') != std::string::npos) at = mo.preset;
    }
    std::string dir = mdb_root_;
    if (!at.empty()) {
        const fs::path p = path_from_utf8(at), r = path_from_utf8(mdb_root_);
        const std::string pr = path_utf8(p.parent_path().lexically_normal()), rr = path_utf8(r.lexically_normal());
        if (pr.size() >= rr.size() && pr.compare(0, rr.size(), rr) == 0) dir = path_utf8(p.parent_path());
    }
    mdb_list(dir, false, at);
    milkdrop_pic_key_.clear();
    force_redraw_ = true;
}

// Style Option -> More Options: the same overlay for one track -- ENTER makes the preset the track's own; on top its options (the preset,
// whether it stays while the track plays, an effect of its own).
void App::mdb_open_track(const std::string& track, const std::string& title) {
    mdb_open();
    if (!mdb_open_) return;
    // (mdb_open() opened on the preset shown; with the track known it opens again on the track's own)
    mdb_open_ = false;
    Milkdrop::instance().set_locked(mdb_prev_locked_);
    mdb_track_ = track;
    mdb_track_title_ = title.empty() ? path_utf8(path_from_utf8(track).stem()) : title;
    mdb_opt_ = -1;
    Milkdrop& m = Milkdrop::instance();
    mdb_open_ = true;
    mdb_moved_ = false;
    mdb_prev_path_ = m.preset_path();
    mdb_prev_locked_ = m.locked();
    m.set_locked(true);
    std::string at = mdb_prev_path_;
    const MilkdropTrackOpts mo = cover_track_milkdrop(track);
    if (mo.preset.find('/') != std::string::npos || mo.preset.find('\\') != std::string::npos) at = mo.preset;
    std::string dir = mdb_root_;
    if (!at.empty()) {
        const fs::path p = path_from_utf8(at), r = path_from_utf8(mdb_root_);
        const std::string pr = path_utf8(p.parent_path().lexically_normal()), rr = path_utf8(r.lexically_normal());
        if (pr.size() >= rr.size() && pr.compare(0, rr.size(), rr) == 0) dir = path_utf8(p.parent_path());
    }
    mdb_list(dir, false, at);
}

void App::mdb_track_adjust(int row, int dir, bool reset) {
    MilkdropTrackOpts mo = cover_track_milkdrop(mdb_track_);
    const bool playing = has_track_ && path_utf8(current_.path) == mdb_track_;
    std::string msg;
    if (row == 0) {
        if (!reset) { status_line_ = "milkdrop: choose the track's preset in the list below ([ENTER]); [d] here: as it comes"; return; }
        mo.preset.clear();
        msg = "this track: the presets come as they come";
    } else if (row == 1) {
        mo.keep = reset ? -1 : mo.keep == 0 ? 1 : 0;
        msg = mo.keep == 0 ? "this track: its preset first, then the presets go on" : "this track: its preset stays while it plays";
    } else {
        if (reset) mo.fx = -1;
        else if (mo.fx < 0) mo.fx = dir > 0 ? 0 : -1;
        else if (mo.fx == 0 && dir < 0) mo.fx = -1;
        else mo.fx = video_fx_step(mo.fx, dir);
        msg = mo.fx < 0 ? "this track: the visuals' effect" : std::string("this track: the effect ") + video_fx_name(video_fx_canon(mo.fx));
    }
    set_cover_track_milkdrop(mdb_track_, mo);
    status_line_ = "milkdrop: " + msg;
    if (meta_) meta_->sty_msg = "saved: " + msg;
    if (playing) {
        md_track_fx_ = mo.fx >= 0 && mo.fx < kVideoFxCount ? mo.fx : -1;
        if (row == 1) {   // (the lock of the track: given now, not only at the next start; the overlay gives it back when it closes)
            if (mo.keep == 0 && md_track_locked_) { mdb_prev_locked_ = md_user_locked_; md_track_locked_ = false; }
            else if (mo.keep != 0 && !mo.preset.empty() && !md_track_locked_) { md_user_locked_ = mdb_prev_locked_; mdb_prev_locked_ = true; md_track_locked_ = true; }
        }
        if (row == 0 && md_track_locked_) { mdb_prev_locked_ = md_user_locked_; md_track_locked_ = false; }
        milkdrop_pic_key_.clear();
    }
}

void App::mdb_close(bool chosen) {
    Milkdrop& m = Milkdrop::instance();
    if (!chosen && mdb_moved_ && !mdb_prev_path_.empty()) m.play_path(mdb_prev_path_);   // back to the preset from before
    m.set_locked(mdb_prev_locked_);
    mdb_open_ = false;
    mdb_track_.clear();
    mdb_opt_ = -1;
    milkdrop_pic_key_.clear();
    force_redraw_ = true;
}

void App::mdb_preview() {
    if (mdb_sel_ < 0 || mdb_sel_ >= static_cast<int>(mdb_items_.size())) return;
    const MdbItem& it = mdb_items_[static_cast<size_t>(mdb_sel_)];
    if (it.kind != 3) return;
    // (the render thread takes only the newest wish at each picture: scrolling fast does not load every preset on the way)
    Milkdrop::instance().play_path(it.path);
    mdb_moved_ = true;
}

void App::mdb_search_run() {
    if (mdb_index_root_ != mdb_root_) {   // all presets under the preset folder, once
        mdb_index_.clear();
        mdb_index_root_ = mdb_root_;
        const fs::path root = path_from_utf8(mdb_root_);
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->is_directory(ec)) { if (it->path().filename() == "textures") it.disable_recursion_pending(); continue; }
            if (!is_preset_file(it->path())) continue;
            MdbIdx e;
            e.name = path_utf8(it->path().stem());
            e.path = path_utf8(it->path());
            const fs::path rel = it->path().parent_path().lexically_relative(root);
            for (const auto& part : rel) if (part != ".") e.where += (e.where.empty() ? "" : " / ") + path_utf8(part);
            e.name_n = normalize_for_search(e.name);
            e.where_n = normalize_for_search(e.where);
            e.all_n = e.name_n + " " + e.where_n;
            mdb_index_.push_back(std::move(e));
        }
    }
    mdb_items_.clear();
    mdb_sel_ = 0;
    mdb_top_ = 0;
    if (mdb_query_.empty()) return;
    const PreparedQuery pq = prepare_query(mdb_query_);
    std::vector<std::pair<double, size_t>> hits;
    for (size_t k = 0; k < mdb_index_.size(); ++k) {
        const MdbIdx& e = mdb_index_[k];
        double s = substring_score(pq, e.name_n);   // the name first (100 .. 1000), then the folder, then fuzzy over both (< 100)
        if (s < 0.0) { s = substring_score(pq, e.where_n); if (s >= 0.0) s *= 0.5; }
        if (s < 0.0) s = word_fuzzy_score(pq, e.all_n);
        if (s > 0.0) hits.push_back({s, k});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (hits.size() > 2000) hits.resize(2000);
    for (const auto& h : hits) {
        const MdbIdx& e = mdb_index_[h.second];
        mdb_items_.push_back({3, e.name, e.path, e.where});
    }
}

void App::mdb_search_clear() {
    mdb_typing_ = mdb_searching_ = false;
    mdb_query_.clear();
    mdb_list(mdb_before_favs_ ? mdb_root_ : mdb_before_dir_, mdb_before_favs_, Milkdrop::instance().preset_path());
}

void App::handle_mdb_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(mdb_items_.size());
    const int page = std::max(1, term_rows_ - 8);
    auto move_to = [&](int k) {
        if (n == 0) return;
        const int was = mdb_sel_;
        mdb_sel_ = std::clamp(k, 0, n - 1);
        if (mdb_sel_ != was) mdb_preview();
    };
    auto enter = [&](const MdbItem it) {   // (a copy: mdb_list() replaces the list the item came from)
        if (it.kind == 0) {   // up
            const bool from_favs = mdb_in_favs_;
            const std::string from = from_favs ? std::string() : mdb_dir_;
            mdb_list(it.path, false, from);   // (on the folder it came from)
            if (from_favs) for (size_t k = 0; k < mdb_items_.size(); ++k) if (mdb_items_[k].kind == 1) mdb_sel_ = static_cast<int>(k);
        } else if (it.kind == 1) mdb_list(mdb_root_, true, "");
        else if (it.kind == 2) mdb_list(it.path, false, "");
        mdb_preview();
    };
    // --- More Options of a track: the option rows on top (UP from the first row of the list reaches them)
    if (!mdb_track_.empty() && !mdb_typing_) {
        if (mdb_opt_ >= 0) {
            if (arrow && key == 'A') { mdb_opt_ = std::max(0, mdb_opt_ - 1); return; }
            if (arrow && key == 'B') { if (++mdb_opt_ > 2) mdb_opt_ = -1; return; }
            if (arrow && (key == 'C' || key == 'D')) { mdb_track_adjust(mdb_opt_, key == 'C' ? 1 : -1, false); return; }
            if (!arrow && (key == '\r' || key == '\n' || key == ' ')) { mdb_track_adjust(mdb_opt_, 1, false); return; }
            if (!arrow && (key == 'd' || key == 'D' || key == kKeyDelete || key == 127 || key == 8)) { mdb_track_adjust(mdb_opt_, 0, true); return; }
            if (!arrow && key == 27) { mdb_close(false); return; }
            if (!arrow && (key == '/' || key_is("HKeySearch", key))) mdb_opt_ = -1;   // (on to the search below)
            else return;
        } else if (arrow && key == 'A' && mdb_sel_ == 0 && !last_key_was_shifted()) { mdb_opt_ = 2; return; }
    }
    if (arrow && (key == 'A' || key == 'B')) { move_to(mdb_sel_ + (key == 'B' ? 1 : -1) * (last_key_was_shifted() ? page : 1)); return; }
    // --- the search: "/" types a query; the results are every preset that matches, best first
    if (mdb_typing_ && !arrow) {
        if (key == 27) { mdb_search_clear(); return; }
        if (key == '\r' || key == '\n' || key == '\t') { mdb_typing_ = false; if (mdb_query_.empty()) mdb_search_clear(); return; }
        if (key == 127 || key == 8) {
            if (mdb_query_.empty()) { mdb_search_clear(); return; }
            do mdb_query_.pop_back(); while (!mdb_query_.empty() && (static_cast<unsigned char>(mdb_query_.back()) & 0xC0) == 0x80);
            mdb_search_run(); mdb_preview(); return;
        }
        if ((key >= 32 && key < 127) || (key >= 128 && key < 256)) {
            mdb_query_ += static_cast<char>(key);
            if (key < 128 || (key & 0xC0) == 0x80) { mdb_search_run(); mdb_preview(); }   // (a whole letter: after the last byte of it)
            return;
        }
        return;
    }
    if (!arrow && (key == '/' || key_is("HKeySearch", key))) {
        if (!mdb_searching_) { mdb_before_dir_ = mdb_dir_; mdb_before_favs_ = mdb_in_favs_; }
        mdb_typing_ = mdb_searching_ = true;
        if (mdb_query_.empty()) { mdb_items_.clear(); mdb_sel_ = 0; mdb_top_ = 0; }
        return;
    }
    if (mdb_searching_ && ((!arrow && key == 27) || (arrow && key == 'D'))) { mdb_search_clear(); return; }
    if (!arrow && key == kKeyHome) { move_to(0); return; }
    if (!arrow && key == kKeyEnd) { move_to(n - 1); return; }
    if (n > 0 && arrow && key == 'C') {
        const MdbItem it = mdb_items_[static_cast<size_t>(mdb_sel_)];
        if (it.kind == 1 || it.kind == 2) enter(it);
        return;
    }
    if (arrow && key == 'D') {   // up a folder
        if (n > 0 && mdb_items_[0].kind == 0) enter(mdb_items_[0]);
        return;
    }
    if (arrow) return;
    if (key == 127 || key == 8) { if (n > 0 && mdb_items_[0].kind == 0) enter(mdb_items_[0]); return; }
    if (key == 27 || key_is("HKeyVisualsMenu", key)) { mdb_close(false); return; }
    if (n == 0) return;
    const MdbItem it = mdb_items_[static_cast<size_t>(mdb_sel_)];
    if (key == '\r' || key == '\n') {
        if (it.kind == 3 && !mdb_track_.empty()) {   // More Options: the track's own preset
            const std::string track = mdb_track_;
            MilkdropTrackOpts mo = cover_track_milkdrop(track);
            mo.preset = it.path;
            set_cover_track_milkdrop(track, mo);
            const bool playing = has_track_ && path_utf8(current_.path) == track;
            if (meta_) meta_->sty_msg = "saved: this track shows \"" + it.name + "\"" + (mo.keep == 0 ? " first" : "");
            mdb_close(playing);   // (another track's choice: the preset of this one comes back)
            if (playing) milkdrop_track_apply();
        } else if (it.kind == 3) {
            Milkdrop::instance().play_path(it.path);
            status_line_ = "milkdrop: " + it.name;
            mdb_close(true);
        } else if (it.kind == 4) status_line_ = "milkdrop: this favourite's file is gone ([d] takes it out)";
        else enter(it);
        return;
    }
    if (key == 'a' || key == 'A') {
        if (it.kind != 3) { status_line_ = "milkdrop: [a] adds a preset (not a folder) to the favourites"; return; }
        md_favs_load();
        if (std::find(md_favs_.begin(), md_favs_.end(), it.path) != md_favs_.end()) { status_line_ = "milkdrop: " + it.name + " is a favourite already"; return; }
        md_favs_.push_back(it.path);
        md_favs_save();
        status_line_ = "milkdrop: " + it.name + " added to the favourites (" + std::to_string(md_favs_.size()) + ")";
        if (mdb_in_favs_) mdb_list(mdb_root_, true, it.path);
        milkdrop_apply_settings();
        return;
    }
    if ((key == 'd' || key == 'D' || key == kKeyDelete) && mdb_in_favs_ && (it.kind == 3 || it.kind == 4)) {
        md_favs_.erase(std::remove(md_favs_.begin(), md_favs_.end(), it.path), md_favs_.end());
        md_favs_save();
        status_line_ = "milkdrop: " + it.name + " taken out of the favourites (" + std::to_string(md_favs_.size()) + " left)";
        const int keep = mdb_sel_;
        mdb_list(mdb_root_, true, "");
        mdb_sel_ = std::clamp(keep, 0, static_cast<int>(mdb_items_.size()) - 1);
        milkdrop_apply_settings();
        mdb_preview();
        return;
    }
    if (key == 'd' || key == 'D') status_line_ = "milkdrop: [d] takes a preset out of the favourites (in the Favourites folder)";
}

// The preset overlay over the whole screen: the folder on the left, the visuals (the preview) on the right.
std::string App::build_mdb_screen(int W, const char* clear_prefix) const {
    const int H = std::max(6, term_rows_ - 1);
    const int ph = H - 2;
    int left_w = std::clamp(W * 2 / 5, 30, 64);
    const int mg = 1;
    int pw = W - 2 - left_w - 1 - 2 * mg;
    if (pw < 20) { left_w = W - 2; pw = 0; }   // (too narrow for the preview: the list alone)
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    std::vector<std::string> pic;
    if (pw > 0) {
        const std::vector<std::string> nb = milkdrop_name_box(pw - 2);
        if (!milkdrop_draw(pic, pw, ph, 1 + left_w + 1 + mg, 1, false, 0, nb, nb.empty() ? 0 : pw)) {
            pic.assign(static_cast<size_t>(ph), std::string(static_cast<size_t>(pw), ' '));
            const std::string msg = truncate_str(Milkdrop::instance().status().empty() ? "starting ..." : Milkdrop::instance().status(), pw);
            const int pad = std::max(0, (pw - display_width(msg)) / 2);
            pic[static_cast<size_t>(ph / 2)] = std::string(static_cast<size_t>(pad), ' ') + msg + std::string(static_cast<size_t>(std::max(0, pw - pad - display_width(msg))), ' ');
        }
    }
    // the left column: (More Options of a track: its options first) where we are, then the list
    const int inner = left_w - 2;
    std::vector<std::string> left;
    if (!mdb_track_.empty()) {
        const MilkdropTrackOpts mo = cover_track_milkdrop(mdb_track_);
        const std::string fxn = video_fx_name(video_fx_canon(settings_.milkdrop_fx));
        const std::string vals[3] = {
            mo.preset.empty() ? std::string("as it comes") : path_utf8(path_from_utf8(mo.preset).stem()),
            mo.preset.empty() ? std::string("(with a preset)") : mo.keep == 0 ? std::string("off (it starts, then the presets go on)") : std::string("on (while it plays)"),
            mo.fx < 0 ? "as the visuals (" + fxn + ")" : std::string(video_fx_name(video_fx_canon(mo.fx)))};
        const char* labels[3] = {"Preset", "Keep Preset", "Effect"};
        const int lw = 13;
        for (int r = 0; r < 3; ++r) {
            const std::string body = pad_right(truncate_str(pad_right(labels[r], lw) + vals[r], inner), inner);
            if (mdb_opt_ == r) left.push_back(" " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + body + "\x1b[0m ");
            else left.push_back(" " + ansi_for(settings_.list_color, false) + body + "\x1b[0m ");
        }
        left.push_back(" " + std::string(static_cast<size_t>(inner), '-') + " ");
    }
    {
        std::string where;
        if (mdb_searching_) {
            where = "Search: " + mdb_query_ + (mdb_typing_ ? "\u2588" : "");
            const std::string cnt = mdb_query_.empty() ? std::string() : "  (" + std::to_string(mdb_items_.size()) + (mdb_items_.size() >= 2000 ? "+" : "") + ")";
            left.push_back(" " + search_idle_sgr(settings_) + pad_right(truncate_str(where + cnt, inner), inner) + "\x1b[0m ");
            left.push_back(std::string(static_cast<size_t>(left_w), ' '));
        } else if (mdb_in_favs_) where = "Favourites";
        else {
            const fs::path rel = path_from_utf8(mdb_dir_).lexically_relative(path_from_utf8(mdb_root_));
            where = "Presets";
            if (!rel.empty() && rel != ".") for (const auto& part : rel) where += " / " + path_utf8(part);
        }
        if (!mdb_searching_) {
            left.push_back(" " + pad_right(truncate_str(where, inner), inner) + " ");
            left.push_back(std::string(static_cast<size_t>(left_w), ' '));
        }
    }
    const int list_rows = std::max(1, ph - static_cast<int>(left.size()));
    const int n = static_cast<int>(mdb_items_.size());
    if (mdb_sel_ < mdb_top_) mdb_top_ = mdb_sel_;
    if (mdb_sel_ >= mdb_top_ + list_rows) mdb_top_ = mdb_sel_ - list_rows + 1;
    mdb_top_ = std::clamp(mdb_top_, 0, std::max(0, n - list_rows));
    const std::string shown = Milkdrop::instance().preset_path();
    for (int r = 0; r < list_rows; ++r) {
        const int k = mdb_top_ + r;
        if (k >= n) { left.push_back(std::string(static_cast<size_t>(left_w), ' ')); continue; }
        const MdbItem& it = mdb_items_[static_cast<size_t>(k)];
        std::string text, sgr = ansi_for(settings_.list_color, false);
        switch (it.kind) {
            case 0: text = "◂ .."; break;
            case 1: text = "★ Favourites (" + std::to_string(md_favs_.size()) + ")"; sgr = "\x1b[1m" + ansi_for(settings_.header_color, false); break;
            case 2: text = "▸ " + it.name; break;
            case 3: text = "  " + it.name; break;   // (a search result: its folder follows, grey)
            default: text = "  " + it.name + "  (file gone)"; sgr = "\x1b[90m"; break;
        }
        const bool fav = (it.kind == 3 && !mdb_in_favs_ && std::find(md_favs_.begin(), md_favs_.end(), it.path) != md_favs_.end());
        if (fav) text = "★" + text.substr(1);   // (a favourite, also in its own folder)
        std::string body = pad_right(truncate_str(text, inner), inner);
        if (!it.where.empty() && display_width(text) + 4 < inner) {   // the folder of a search result, in the room the name leaves
            const std::string w = truncate_str("  " + it.where, inner - display_width(text));
            body = text + (k == mdb_sel_ ? w : "\x1b[90m" + w + "\x1b[0m" + sgr) + std::string(static_cast<size_t>(std::max(0, inner - display_width(text) - display_width(w))), ' ');
        }
        if (k == mdb_sel_ && mdb_opt_ < 0) left.push_back(" " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + body + "\x1b[0m ");
        else if (k == mdb_sel_) left.push_back(" \x1b[1m" + body + "\x1b[0m ");
        else if (it.kind == 3 && it.path == shown) left.push_back(" " + ansi_for(settings_.list_playing_color) + body + "\x1b[0m ");
        else left.push_back(" " + sgr + body + "\x1b[0m ");
    }
    std::ostringstream f;
    f << clear_prefix << "\x1b[?25l";
    const std::string count = mdb_in_favs_ ? std::to_string(md_favs_.size()) + " favourites" : std::to_string(Milkdrop::instance().preset_count()) + " presets";
    f << box_top(mdb_track_.empty() ? "MILKDROP PRESETS  " + count : "MILKDROP FOR THIS TRACK: " + truncate_str(mdb_track_title_, std::max(10, W - 40)), W, border) << "\n";
    for (int r = 0; r < ph; ++r) {
        const std::string l = r < static_cast<int>(left.size()) ? left[static_cast<size_t>(r)] : std::string(static_cast<size_t>(left_w), ' ');
        f << bar << l;
        if (pw > 0) {
            const std::string p = r < static_cast<int>(pic.size()) ? pic[static_cast<size_t>(r)] : std::string(static_cast<size_t>(pw), ' ');
            f << bar << std::string(static_cast<size_t>(mg), ' ') << p << "\x1b[0m" << std::string(static_cast<size_t>(mg), ' ');
        }
        f << bar << "\n";
    }
    const std::string legend = !mdb_track_.empty() && mdb_opt_ >= 0 ? "[\u2191\u2193] select  [\u2190\u2192] change  [d] as it comes  [\u2193] the presets  [ESC] back"
                             : !mdb_track_.empty() && !mdb_typing_ && !mdb_searching_ ? "[ENTER] the track's preset  [\u2191] its options  [/] search  [\u2190] up  [a] favourite  [ESC] back"
                             : mdb_typing_ ? "type to search (fuzzy)  [\u2191\u2193] select  [ENTER] to the list  [ESC] clear"
                             : mdb_searching_ ? "[\u2191\u2193] select  [ENTER] choose  [a] add to favourites  [/] search  [ESC/\u2190] clear"
                             : mdb_in_favs_ ? "[↑↓] select  [ENTER] choose  [d] remove from favourites  [/] search  [←] back  [ESC] close"
                                            : "[↑↓] select  [ENTER] choose / open  [a] add to favourites  [/] search  [←] up  [ESC] close";
    f << box_bottom(W, legend, border_bottom.empty() ? border : border_bottom);
    return clamp_output_rows(f.str(), term_rows_);
}

} // namespace muisc
