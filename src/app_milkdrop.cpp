// app_milkdrop.cpp
// Part of class App (src/app.h): the MilkDrop visuals (milkdrop.h) in the visualization field, and the visuals overlay (":" by default,
// HKeyVisualsMenu): the preset (previous / next / random / lock) and the MilkDrop options, which are the same values as Settings ->
// ANIMA/VIZ -> MILKDROP.
#include "app.h"
#include "app_internal.h"
#include "cover_store.h"
#include "meta_state.h"
#include "milkdrop.h"
#include "milkdrop_folders.h"
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
const int kMdHcdSteps[] = {3, 5, 10, 15, 20, 30, 45, 60, 90, 120};
const int kMdVarSteps[] = {0, 2, 5, 10, 15, 20, 30};
const int kMdDurSteps[] = {0, 10, 15, 20, 25, 30, 45, 60, 90, 120, 180, 300};
template <size_t N> int md_step(const int (&a)[N], int cur, int dir) {
    // the nearest step, then one on (a value from config.txt between two steps snaps onto them)
    size_t k = 0;
    for (size_t i = 0; i < N; ++i) if (std::abs(a[i] - cur) < std::abs(a[k] - cur)) k = i;
    const long n = std::clamp<long>(static_cast<long>(k) + dir, 0, static_cast<long>(N) - 1);
    return a[static_cast<size_t>(n)];
}
// (in the order of the overlay: general / shuffle and transition / graphics and performance)
enum MdRow { kMdPreset, kMdBrowse, kMdRandom, kMdLock, kMdName, kMdUseFx, kMdFx, kMdView, kMdIndSet, kMdIndRevert, kMdIndReset, kMdIndCopy, kMdUndo, kMdIndOn,
             kMdBright, kMdSat, kMdContrast,
             kMdDuration, kMdShuffle, kMdFolder, kMdFavShuffle, kMdHardCuts, kMdCutSens, kMdTransition, kMdHcd, kMdVar,
             kMdFps, kMdRes, kMdGpu, kMdColors, kMdMesh, kMdAspect, kMdRatio, kMdClean, kMdRows };
const char* md_label(int row, bool ts) {
    switch (row) {
        case kMdPreset: return "Preset"; case kMdBrowse: return "Preset Overlay"; case kMdRandom: return "Random Preset"; case kMdLock: return "Lock Preset";
        case kMdName: return "Show Preset Name"; case kMdUseFx: return "Use Effect"; case kMdFx: return "Effect";
        case kMdView: return ts ? "MilkDrop (this track)" : "Auto MilkDrop";
        case kMdIndSet: return "Set current values as individual settings"; case kMdIndRevert: return "Reset to last track settings";
        case kMdIndReset: return "Reset individual values to default settings"; case kMdIndCopy: return "Set individual settings as default settings";
        case kMdUndo: return "Undo change"; case kMdIndOn: return "Use individual settings for this track";
        case kMdBright: return "Brightness"; case kMdSat: return "Saturation"; case kMdContrast: return "Contrast";
        case kMdDuration: return "Preset Duration"; case kMdShuffle: return "Shuffle All Presets"; case kMdFolder: return "Shuffle Folder"; case kMdFavShuffle: return "Shuffle Favourites";
        case kMdHardCuts: return "Hard Cuts"; case kMdCutSens: return "Hard Cut Sensitivity"; case kMdTransition: return "Preset Transition";
        case kMdHcd: return "Hard Cut Interval"; case kMdVar: return "Duration Variance";
        case kMdFps: return "Frame Rate"; case kMdRes: return "Resolution"; case kMdGpu: return "GPU"; case kMdColors: return "Sixel Colours";
        case kMdMesh: return "Mesh Detail"; case kMdAspect: return "Aspect Correction"; case kMdRatio: return "Aspect Ratio"; case kMdClean: return "Clean Start";
        default: return "";
    }
}
const char* md_category(int row) {   // the heading of the group this row belongs to
    return row < kMdIndSet ? "GENERAL" : row < kMdBright ? "INDIVIDUAL TRACK SETTINGS MANAGEMENT" : row < kMdDuration ? "PICTURE" : row < kMdFps ? "SHUFFLE / TRANSITION" : "GRAPHICS / PERFORMANCE";
}
bool md_action_row(int row) { return row >= kMdIndSet && row <= kMdUndo; }   // (a row that does something: ENTER runs it)
bool md_rec_row(int row) {   // a row a track can have its own value for (the others are the same for every track)
    return row == kMdPreset || row == kMdLock || row == kMdName || row == kMdUseFx || row == kMdFx || row == kMdDuration || row == kMdShuffle || row == kMdFavShuffle ||
           row == kMdHardCuts || row == kMdCutSens || row == kMdTransition;
}
const int kMdCutSensSteps[] = {5, 10, 15, 20, 25, 30, 40, 50, 60};
} // namespace

App::MdEff App::milkdrop_eff(const MilkdropTrackOpts& t) const {
    MdEff e;
    e.dur = t.dur >= 0 ? t.dur : std::max(0, settings_.milkdrop_preset_secs);
    e.shuffle = t.shuf >= 0 ? t.shuf == 1 : settings_.milkdrop_shuffle;
    e.favs = t.favs >= 0 ? t.favs == 1 : settings_.milkdrop_fav_shuffle;
    e.cuts = t.cuts >= 0 ? t.cuts == 1 : settings_.milkdrop_hard_cuts;
    e.fx_on = t.fxon >= 0 ? t.fxon == 1 : settings_.milkdrop_fx_on;
    e.name = t.name >= 0 ? t.name == 1 : settings_.milkdrop_show_name;
    e.cut_sens = std::clamp(t.hcs >= 0 ? t.hcs : settings_.milkdrop_cut_sens, 5, 60);
    e.trans = std::clamp(t.trans >= 0 ? t.trans : settings_.milkdrop_transition, 0, 10);
    return e;
}

void App::milkdrop_apply_settings() const {
    static bool lock_given = false;   // (the saved lock is put on once, when the visuals are first configured)
    if (!lock_given) { lock_given = true; Milkdrop::instance().set_locked(settings_.milkdrop_lock); }
    const MdEff e = milkdrop_eff();
    MilkdropOptions o;
    o.presets_dir = settings_.milkdrop_presets;
    for (const std::string& e : settings_.milkdrop_extra_paths) if (!e.empty()) o.extra_dirs.push_back(e);
    o.fps = std::clamp(settings_.milkdrop_fps, 10, 60);
    o.max_width = std::clamp(settings_.milkdrop_res, 160, 1920);
    o.preset_seconds = e.dur;
    o.shuffle = e.shuffle;
    o.cut_sens = static_cast<float>(e.cut_sens) / 10.0f;
    o.hard_cuts = e.cuts;
    o.transition = e.trans;
    o.start_preset = settings_.milkdrop_default_preset;
    o.gpu = settings_.milkdrop_gpu;
    o.hard_cut_secs = std::clamp(settings_.milkdrop_hcd, 3, 120);
    o.aspect = settings_.milkdrop_aspect;
    { static const int mw[] = {16, 32, 64, 96}, mh[] = {12, 24, 48, 72}; const int mi = std::clamp(settings_.milkdrop_mesh, 0, 3); o.mesh_w = mw[mi]; o.mesh_h = mh[mi]; }
    o.variance = static_cast<float>(std::clamp(settings_.milkdrop_var, 0, 30));
    o.clean_start = settings_.milkdrop_clean;
    o.favourites_only = e.favs;
    if (o.favourites_only) o.favourites = md_shuffle_paths();
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

std::vector<std::string> App::milkdrop_name_box(int width, bool always) const {
    Milkdrop& m = Milkdrop::instance();
    if (!milkdrop_eff().name || (!always && m.since_change() > 3.0)) return {};
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

namespace {
// The track's preset stays while it plays: when it has one (unless it says no), or when it says yes.
bool md_track_wants_lock(const MilkdropTrackOpts& mo) { return (!mo.preset.empty() && mo.keep != 0) || mo.keep == 1; }
}

void App::milkdrop_track_live(const MilkdropTrackOpts& mo_in) {
    const MilkdropTrackOpts mo = mo_in.ioff == 1 ? MilkdropTrackOpts{} : mo_in;   // (switched off: the general settings are in force)
    md_trk_ = mo;
    md_track_fx_ = mo.fx >= 0 && mo.fx < kVideoFxCount ? mo.fx : -1;
    Milkdrop& m = Milkdrop::instance();
    const bool want = md_track_wants_lock(mo);
    if (want && !md_track_locked_) { md_user_locked_ = m.locked(); m.set_locked(true); md_track_locked_ = true; }
    else if (!want && md_track_locked_) { m.set_locked(md_user_locked_); md_track_locked_ = false; }
    milkdrop_pic_key_.clear();
    milkdrop_apply_settings();
}

bool App::milkdrop_shown_in_field() const {
    return settings_.milkdrop_view && !milkdrop_win_on_ && milkdrop_usable();
}

void App::milkdrop_effect_step(int dir, bool global) {
    const bool own = !global && !md_individual_off() && !audiobook_ && has_track_ && current_.is_local && !current_.path.empty();
    int nx = 0;
    if (own) {
        const std::string trk = path_utf8(current_.path);
        MilkdropTrackOpts mo = cover_track_milkdrop(trk);
        nx = video_fx_step(mo.fx >= 0 ? mo.fx : video_fx_canon(settings_.milkdrop_fx), dir);
        mo.fx = nx;
        if (!milkdrop_eff().fx_on) mo.fxon = 1;
        set_cover_track_milkdrop(trk, mo);
        milkdrop_track_live(mo);
    } else {
        nx = video_fx_step(settings_.milkdrop_fx, dir);
        settings_.milkdrop_fx = nx;
        settings_.milkdrop_fx_on = true;
        settings_dirty_ = true;
    }
    milkdrop_pic_key_.clear();
    status_line_ = std::string(own ? "milkdrop effect of this track: " : "milkdrop effect: ") + video_fx_name(nx);
}

void App::milkdrop_effect_toggle(bool global) {
    const bool own = !global && !md_individual_off() && !audiobook_ && has_track_ && current_.is_local && !current_.path.empty();
    bool now_on = false;
    if (own) {
        const std::string trk = path_utf8(current_.path);
        MilkdropTrackOpts mo = cover_track_milkdrop(trk);
        now_on = !milkdrop_eff().fx_on;
        mo.fxon = now_on ? 1 : 0;
        set_cover_track_milkdrop(trk, mo);
        milkdrop_track_live(mo);
    } else {
        settings_.milkdrop_fx_on = !settings_.milkdrop_fx_on;
        now_on = settings_.milkdrop_fx_on;
        settings_dirty_ = true;
    }
    milkdrop_pic_key_.clear();
    status_line_ = now_on ? "milkdrop: the effect is on" : "milkdrop: no effect on the visuals";
}

void App::milkdrop_track_apply() {
    MilkdropTrackOpts mo;
    if (!audiobook_ && has_track_ && current_.is_local && !current_.path.empty()) mo = cover_track_milkdrop(path_utf8(current_.path));
    if (mo.ioff == 1) mo = MilkdropTrackOpts();   // (the individual settings are switched off: kept in the record, the general ones are in force)
    md_trk_ = mo;   // (the temporary layer of the visuals menu starts as the stored settings: what was changed there is gone when the track is loaded again)
    md_preset_changed_ = false;
    md_undo_.clear();
    const bool want_lock = md_track_wants_lock(mo);
    // first give back what was done for the track before (unless the view was changed by hand meanwhile)
    // (the lock as it was before the track before locked its preset: what was set by hand comes back, not "unlocked")
    if (md_track_locked_ && !want_lock) { Milkdrop::instance().set_locked(md_user_locked_); md_track_locked_ = false; }
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
    if (mo.on < 0 && mo.preset.empty() && !want_lock) { milkdrop_apply_settings(); return; }   // (nothing of its own to load)
    if (settings_.use_milkdrop && milkdrop_usable()) {
        Milkdrop& m = Milkdrop::instance();
        if (!mo.preset.empty()) {
            // a path (chosen in More Options) is played by its file; an older record has only the name (looked up in the list)
            if (mo.preset.find('/') != std::string::npos || mo.preset.find('\\') != std::string::npos) m.play_path(mo.preset);
            else m.play_named(mo.preset);
        }
        if (want_lock && !md_track_locked_) {   // (kept while the track plays)
            md_user_locked_ = m.locked();
            m.set_locked(true);
            md_track_locked_ = true;
        }
    }
    milkdrop_apply_settings();
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

bool App::md_row_shown(int row) const {
    if (row == kMdIndSet || row == kMdIndRevert) return md_scope_track_.empty() && !md_target_track().empty();   // (the temporary layer: only in the menu of the playing track)
    if (row == kMdUndo) return md_scope_track_.empty();
    if (row == kMdIndOn || row == kMdIndCopy || row == kMdIndReset) return !md_target_track().empty();   // (these act on one track: the one the menu is for, or the one that plays)
    return true;
}

// The track the individual settings rows act on: the track of the track menu, else the local track that plays ("" = none).
std::string App::md_target_track() const {
    if (!md_scope_track_.empty()) return md_scope_track_;
    if (!audiobook_ && has_track_ && current_.is_local && !current_.path.empty()) return path_utf8(current_.path);
    return std::string();
}

// The individual settings of the playing track are switched off (kept, but the general ones are in force).
bool App::md_individual_off() const {
    if (audiobook_ || !has_track_ || !current_.is_local || current_.path.empty()) return false;
    return cover_track_milkdrop(path_utf8(current_.path)).ioff == 1;
}

static bool md_track_has(int i, const MilkdropTrackOpts& t) {   // the playing track has a value of its own for this row
    return (i == kMdDuration && t.dur >= 0) || (i == kMdShuffle && t.shuf >= 0) || (i == kMdFavShuffle && t.favs >= 0) || (i == kMdCutSens && t.hcs >= 0) ||
           (i == kMdHardCuts && t.cuts >= 0) || (i == kMdTransition && t.trans >= 0) || (i == kMdUseFx && t.fxon >= 0) || (i == kMdFx && t.fx >= 0) ||
           (i == kMdName && t.name >= 0) || (i == kMdPreset && !t.preset.empty()) || (i == kMdLock && t.keep >= 0);
}

static bool md_track_any(const MilkdropTrackOpts& t) {   // the track has individual settings of any kind
    for (int i = 0; i < kMdRows; ++i) if (md_track_has(i, t)) return true;
    return t.on >= 0;
}

static bool md_global_row(int row) {   // (how the pictures are drawn: not a matter of one track)
    return row == kMdFolder || row == kMdBright || row == kMdSat || row == kMdContrast || row == kMdFps || row == kMdRes || row == kMdGpu || row == kMdColors || row == kMdHcd || row == kMdVar || row == kMdMesh || row == kMdAspect || row == kMdRatio || row == kMdClean;
}

// The playing track has individual MilkDrop settings in force: the visuals menu then shows them (and "(this track)") and what is changed there only
// changes a temporary layer (md_trk_), which is read from the track's record again when the track is loaded.
bool App::md_ind_active() const {
    if (audiobook_ || !has_track_ || !current_.is_local || current_.path.empty()) return false;
    const MilkdropTrackOpts r = cover_track_milkdrop(path_utf8(current_.path));
    return r.ioff != 1 && md_track_any(r);
}

bool App::md_changed() const {   // the temporary layer differs from what the track has stored (Set current values / Reset to individual settings are for that case)
    if (!md_ind_active()) return false;
    return md_trk_ != cover_track_milkdrop(path_utf8(current_.path)) || md_preset_changed_;
}

bool App::md_values_default(const MilkdropTrackOpts& r) const {
    const MdEff e = milkdrop_eff(r), g = milkdrop_eff(MilkdropTrackOpts{});
    return e.dur == g.dur && e.shuffle == g.shuffle && e.favs == g.favs && e.cuts == g.cuts && e.fx_on == g.fx_on && e.name == g.name && e.cut_sens == g.cut_sens &&
           e.trans == g.trans && (r.fx < 0 || video_fx_canon(r.fx) == video_fx_canon(settings_.milkdrop_fx)) && (r.keep < 0 || (r.keep == 1) == settings_.milkdrop_lock) && r.preset.empty();
}

App::MdSnap App::md_snap() const {
    MdSnap s;
    s.trk = md_trk_; s.preset_changed = md_preset_changed_;
    s.preset = Milkdrop::instance().preset_path(); s.locked = Milkdrop::instance().locked();
    s.secs = settings_.milkdrop_preset_secs; s.cut_sens = settings_.milkdrop_cut_sens; s.trans = settings_.milkdrop_transition; s.fps = settings_.milkdrop_fps;
    s.res = settings_.milkdrop_res; s.bright = settings_.milkdrop_bright; s.sat = settings_.milkdrop_sat; s.contrast = settings_.milkdrop_contrast;
    s.hcd = settings_.milkdrop_hcd; s.var = settings_.milkdrop_var; s.mesh = settings_.milkdrop_mesh; s.ratio = settings_.milkdrop_ratio; s.fx = settings_.milkdrop_fx;
    s.shuffle = settings_.milkdrop_shuffle; s.fav = settings_.milkdrop_fav_shuffle; s.cuts = settings_.milkdrop_hard_cuts; s.aspect = settings_.milkdrop_aspect;
    s.clean = settings_.milkdrop_clean; s.fx_on = settings_.milkdrop_fx_on; s.fixed = settings_.milkdrop_fixed_colors; s.name = settings_.milkdrop_show_name;
    s.autom = settings_.milkdrop_auto; s.lock_setting = settings_.milkdrop_lock;
    s.folder = settings_.milkdrop_shuffle_folder; s.gpu = settings_.milkdrop_gpu; s.defpreset = settings_.milkdrop_default_preset;
    return s;
}

bool App::md_snap_same(const MdSnap& a, const MdSnap& b) {
    return a.trk == b.trk && a.preset_changed == b.preset_changed && a.preset == b.preset && a.locked == b.locked && a.secs == b.secs && a.cut_sens == b.cut_sens &&
           a.trans == b.trans && a.fps == b.fps && a.res == b.res && a.bright == b.bright && a.sat == b.sat && a.contrast == b.contrast && a.hcd == b.hcd && a.var == b.var &&
           a.mesh == b.mesh && a.ratio == b.ratio && a.fx == b.fx && a.shuffle == b.shuffle && a.fav == b.fav && a.cuts == b.cuts && a.aspect == b.aspect && a.clean == b.clean &&
           a.fx_on == b.fx_on && a.fixed == b.fixed && a.name == b.name && a.autom == b.autom && a.lock_setting == b.lock_setting && a.folder == b.folder && a.gpu == b.gpu && a.defpreset == b.defpreset;
}

void App::md_restore(const MdSnap& s) {
    settings_.milkdrop_preset_secs = s.secs; settings_.milkdrop_cut_sens = s.cut_sens; settings_.milkdrop_transition = s.trans; settings_.milkdrop_fps = s.fps;
    settings_.milkdrop_res = s.res; settings_.milkdrop_bright = s.bright; settings_.milkdrop_sat = s.sat; settings_.milkdrop_contrast = s.contrast;
    settings_.milkdrop_hcd = s.hcd; settings_.milkdrop_var = s.var; settings_.milkdrop_mesh = s.mesh; settings_.milkdrop_ratio = s.ratio; settings_.milkdrop_fx = s.fx;
    settings_.milkdrop_shuffle = s.shuffle; settings_.milkdrop_fav_shuffle = s.fav; settings_.milkdrop_hard_cuts = s.cuts; settings_.milkdrop_aspect = s.aspect;
    settings_.milkdrop_clean = s.clean; settings_.milkdrop_fx_on = s.fx_on; settings_.milkdrop_fixed_colors = s.fixed; settings_.milkdrop_show_name = s.name;
    settings_.milkdrop_lock = s.lock_setting; settings_.milkdrop_shuffle_folder = s.folder; settings_.milkdrop_gpu = s.gpu; settings_.milkdrop_default_preset = s.defpreset;
    if (settings_.milkdrop_auto != s.autom) milkdrop_auto_set(s.autom);
    Milkdrop& m = Milkdrop::instance();
    if (m.locked() != s.locked) m.set_locked(s.locked);
    if (!s.preset.empty() && s.preset != m.preset_path()) m.play_path(s.preset);
    md_trk_ = s.trk;
    md_preset_changed_ = s.preset_changed;
    md_track_fx_ = md_trk_.fx >= 0 && md_trk_.fx < kVideoFxCount ? md_trk_.fx : -1;
    milkdrop_apply_settings();
    milkdrop_pic_key_.clear();
    save_settings(settings_);
}

void App::md_undo_push() {   // before a change in the visuals menu (Undo change)
    md_undo_.push_back(md_snap());
    if (md_undo_.size() > 60) md_undo_.erase(md_undo_.begin());
}

// One row of a track's record (the stored one, or the temporary layer): the value steps, [d] puts it back to "as in Settings". false = not a row of the record.
bool App::md_edit_rec(MilkdropTrackOpts& mo, int row, int dir, bool reset) {
    const MdEff e = milkdrop_eff(mo);
    auto flag = [&](int& f, bool eff_now) { f = reset ? -1 : eff_now ? 0 : 1; };
    switch (row) {
        case kMdLock: mo.keep = reset ? -1 : md_track_wants_lock(mo) ? 0 : 1; return true;
        case kMdDuration: mo.dur = reset ? -1 : md_step(kMdDurSteps, e.dur, dir); return true;
        case kMdShuffle: flag(mo.shuf, e.shuffle); return true;
        case kMdFavShuffle: flag(mo.favs, e.favs); return true;
        case kMdCutSens: mo.hcs = reset ? -1 : md_step(kMdCutSensSteps, e.cut_sens, dir); return true;
        case kMdHardCuts: flag(mo.cuts, e.cuts); return true;
        case kMdTransition: mo.trans = reset ? -1 : std::clamp(e.trans + dir, 0, 10); return true;
        case kMdUseFx: flag(mo.fxon, e.fx_on); return true;
        case kMdFx:
            if (reset) mo.fx = -1;
            else mo.fx = video_fx_step(mo.fx >= 0 ? mo.fx : video_fx_canon(settings_.milkdrop_fx), dir);
            return true;
        case kMdName: flag(mo.name, e.name); return true;
        default: return false;
    }
}

std::string App::milkdrop_row_value(int row, const MilkdropTrackOpts* rec) const {
    Milkdrop& m = Milkdrop::instance();
    const bool ts = rec != nullptr;
    auto secs = [](int v) { return v <= 0 ? std::string("off") : std::to_string(v) + " s"; };
    auto onoff = [](bool b) { return std::string(b ? "on" : "off"); };
    auto tenths = [](int v) { char b[8]; std::snprintf(b, sizeof b, "%.1f", v / 10.0); return std::string(b); };
    auto cut = [](int v) { return v <= 0 ? std::string("cut") : std::to_string(v) + " s"; };
    auto own = [&](int v, const std::string& mine, const std::string& dflt) { return !ts || v >= 0 ? mine : "as Settings (" + dflt + ")"; };
    const int sd = std::max(0, settings_.milkdrop_preset_secs);
    switch (row) {
        case kMdPreset: {
            if (ts && !rec->preset.empty()) return truncate_str(path_utf8(path_from_utf8(rec->preset).stem()), 40);
            std::string preset = m.preset_name();
            if (preset.empty()) preset = m.running() ? "(starting ...)" : "(shows when the visuals run)";
            return preset;
        }
        case kMdBrowse: return "[ENTER]";
        case kMdRandom: return "[ENTER]";
        case kMdLock:
            if (ts) return md_track_wants_lock(*rec) ? (rec->keep < 0 ? "on (it has a preset)" : "on") : "off";
            return onoff(m.locked());
        case kMdDuration: return own(ts ? rec->dur : 0, secs(ts ? rec->dur : sd), secs(sd));
        case kMdShuffle: return own(ts ? rec->shuf : 0, onoff(ts ? rec->shuf == 1 : settings_.milkdrop_shuffle), onoff(settings_.milkdrop_shuffle));
        case kMdFolder: {
            const std::string f = md_shuffle_label();
            return f + " (" + std::to_string(md_shuffle_paths().size()) + ")";
        }
        case kMdFavShuffle: {
            const size_t nf = md_shuffle_paths().size();
            auto fav = [&](bool b) { return !b ? std::string("off") : nf == 0 ? std::string("on (none yet: all)") : "on (" + std::to_string(nf) + ")"; };
            return own(ts ? rec->favs : 0, fav(ts ? rec->favs == 1 : settings_.milkdrop_fav_shuffle), fav(settings_.milkdrop_fav_shuffle));
        }
        case kMdCutSens: return own(ts ? rec->hcs : 0, tenths(ts ? rec->hcs : std::clamp(settings_.milkdrop_cut_sens, 5, 60)), tenths(std::clamp(settings_.milkdrop_cut_sens, 5, 60)));
        case kMdHardCuts: return own(ts ? rec->cuts : 0, onoff(ts ? rec->cuts == 1 : settings_.milkdrop_hard_cuts), onoff(settings_.milkdrop_hard_cuts));
        case kMdTransition: return own(ts ? rec->trans : 0, cut(ts ? rec->trans : settings_.milkdrop_transition), cut(settings_.milkdrop_transition));
        case kMdFps: return std::to_string(settings_.milkdrop_fps) + " fps";
        case kMdRes: return std::to_string(settings_.milkdrop_res) + " px";
        case kMdGpu: {
            std::string g = milkdrop_gpu_label(settings_.milkdrop_gpu);
            const std::string r = m.renderer();
            if (settings_.milkdrop_gpu.empty() && !r.empty()) g += " (" + r + ")";   // (what draws now, next to "automatic")
            return g;
        }
        case kMdUseFx: return own(ts ? rec->fxon : 0, onoff(ts ? rec->fxon == 1 : settings_.milkdrop_fx_on), onoff(settings_.milkdrop_fx_on));
        case kMdFx: {
            const std::string dflt = video_fx_name(video_fx_canon(settings_.milkdrop_fx));
            return ts && rec->fx < 0 ? "as Settings (" + dflt + ")" : ts ? std::string(video_fx_name(video_fx_canon(rec->fx))) : dflt;
        }
        case kMdBright: return std::to_string(std::clamp(settings_.milkdrop_bright, 0, 100)) + "%";
        case kMdSat: return std::to_string(std::clamp(settings_.milkdrop_sat, 0, 100)) + "%";
        case kMdContrast: return std::to_string(std::clamp(settings_.milkdrop_contrast, 0, 100)) + "%";
        case kMdColors: return settings_.milkdrop_fixed_colors ? "fixed" : "adaptive";
        case kMdName: return own(ts ? rec->name : 0, onoff(ts ? rec->name == 1 : settings_.milkdrop_show_name), onoff(settings_.milkdrop_show_name));
        case kMdHcd: return std::to_string(std::clamp(settings_.milkdrop_hcd, 3, 120)) + " s";
        case kMdVar: return secs(settings_.milkdrop_var);
        case kMdMesh: return mesh_name(settings_.milkdrop_mesh);
        case kMdAspect: return onoff(settings_.milkdrop_aspect);
        case kMdRatio: return milkdrop_ratio_name(settings_.milkdrop_ratio);
        case kMdClean: return onoff(settings_.milkdrop_clean);
        case kMdView:
            if (ts) return rec->on < 0 ? "as Settings" : rec->on == 1 ? "on" : "off";
            return onoff(settings_.milkdrop_auto);
        default: return {};
    }
}

std::vector<std::string> App::build_milkdrop_menu_panel(bool fill) const {
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
    const bool ts = !md_scope_track_.empty();
    const MilkdropTrackOpts rec = ts ? cover_track_milkdrop(md_scope_track_) : MilkdropTrackOpts();
    md_favs_load();
    std::vector<std::string> lines;
    lines.push_back(box_top(ts ? "Visuals: MilkDrop (this track)" : "Visuals: MilkDrop", W, border));
    if (ts) {   // (the note in the highlight colours, as in the video options)
        const std::string t = truncate_str("Only for this track: " + md_scope_title_, inner);
        lines.push_back(bar + " " + ansi_for(settings_.header_color, false) + pad_right(t, inner) + R + " " + bar);
    }
    std::vector<std::string> body;
    int sel_line = 0;
    if (md_browse_pending_ && !mdb_open_) {   // (back from the Preset Overlay: a preset chosen there is a change of the temporary layer)
        md_browse_pending_ = false;
        if (m.preset_path() != md_browse_from_) { if (!ts && md_ind_active()) md_preset_changed_ = true; }
        else if (!md_undo_.empty()) md_undo_.pop_back();   // (nothing was chosen in the Preset Overlay: the snapshot taken when it was opened is not a change)
    }
    const bool act = !ts && md_ind_active();   // (the playing track has individual settings: the menu shows them, and what is changed is only temporary)
    const std::string target = md_target_track();
    const MilkdropTrackOpts target_rec = target.empty() ? MilkdropTrackOpts() : cover_track_milkdrop(target);
    const bool changed = act && (md_trk_ != target_rec || md_preset_changed_);
    auto enabled = [&](int i) {   // an action row that has nothing to do is drawn in the legend colours
        switch (i) {
            case kMdIndSet: case kMdIndRevert: return changed;
            case kMdIndReset: return target_rec.ioff == 1 || (md_track_any(target_rec) && !(act ? md_values_default(md_trk_) && !md_preset_changed_ : md_values_default(target_rec)));
            case kMdIndCopy: return target_rec.ioff != 1 && md_track_any(target_rec) && (act ? !md_values_default(md_trk_) || md_preset_changed_ : !md_values_default(target_rec));
            case kMdUndo: return !md_undo_.empty();
            default: return true;
        }
    };
    const std::string legend_col = legend_sgr(settings_).empty() ? DIM : legend_sgr(settings_);
    std::string last_cat;
    for (int i = 0; i < kMdRows; ++i) {
        if (!md_row_shown(i)) continue;
        if (last_cat != md_category(i)) {   // (a heading above each group, in the header colour)
            if (!last_cat.empty()) body.push_back(row("", false));
            const std::string hc = ansi_for(settings_.header_color, false);
            body.push_back(bar + " " + hc + pad_right(std::string(md_category(i)), inner) + R + " " + bar);
            last_cat = md_category(i);
        }
        const bool sel = i == md_menu_row_;
        if (sel) sel_line = static_cast<int>(body.size());
        if (md_action_row(i)) {   // (a row that does something: the label, and [ENTER] on the right -- in the legend colours while there is nothing to do)
            const std::string lab = truncate_str(std::string(sel ? "> " : "  ") + md_label(i, ts), inner - 9);
            const std::string left = pad_right(lab, inner - 8);
            const bool on = enabled(i);
            if (sel) body.push_back(bar + " " + HI + left + "[ENTER] " + R + " " + bar);
            else body.push_back(bar + " " + left + (on ? std::string() : legend_col) + "[ENTER]" + (on ? std::string() : R) + " " + " " + bar);
            continue;
        }
        std::string val;
        if (i == kMdIndOn) {   // (the individual settings of the track: kept when off, the general ones are in force then)
            val = target_rec.ioff == 1 ? "off" : md_track_any(target_rec) ? "on" : "off (none set)";
        } else if (act && md_rec_row(i)) {   // (the value in force: the track's own when it has one, else Settings)
            const bool mine = md_track_has(i, md_trk_);
            const std::string dv = milkdrop_row_value(i, nullptr);
            val = mine ? milkdrop_row_value(i, &md_trk_) : dv;
            if (i == kMdLock) {   // (the lock is what the visuals do right now; the default is the setting)
                const bool locked = Milkdrop::instance().locked();
                val = locked ? "on" : "off";
                if (locked == settings_.milkdrop_lock) val += "  (default)";
            } else if (i != kMdPreset && (!mine || val == dv)) val += "  (default)";   // (a default value is marked, whether the track has it set or not)
        } else {
            val = milkdrop_row_value(i, ts ? &rec : nullptr);
            if ((ts || act) && md_global_row(i)) val += "  (all tracks)";
        }
        const std::string lab_text = i == kMdFavShuffle ? "Shuffle " + md_shuffle_label() : std::string(md_label(i, ts));
        std::string plain;
        if (i == kMdPreset) {   // the preset's name gets the room the label leaves
            const std::string lab = std::string(sel ? "> " : "  ") + pad_right(lab_text, 10);
            plain = lab + "< " + truncate_str(val, std::max(4, inner - display_width(lab) - 4)) + " >";
        } else {
            const int lw = std::clamp(display_width(lab_text) + 1, 30, std::max(30, inner - 12));   // (a long label gets the room it needs)
            const int vw = std::max(1, inner - 2 - lw);
            const std::string tv = display_width(val) > vw ? truncate_str(val, vw) : val;
            plain = std::string(sel ? "> " : "  ") + pad_right(lab_text, lw) + pad_left(tv, vw);
        }
        body.push_back(row(plain, sel, !ok));
    }
    body.push_back(row("", false));
    if (!ok) {
        body.push_back(row("Not available: " + why, false, true));
        body.push_back(row("Install projectM 4 and a preset pack (see the manual).", false, true));
    }
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + "\x1b[0m" + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    const std::string close_keys = (hotkey_text("HKeyVisualsMenu", ":") == "-" ? std::string() : hotkey_text("HKeyVisualsMenu", ":") + " / ") + "ESC";
    std::vector<std::string> leg;
    if (ind_confirm_.active) push_legend_block(leg, W, border_bottom, {"Keep changes?", "[Y] keep   [N / ESC] undo"}, leg_in, leg_bot);
    else if (ts) push_legend_block(leg, W, border_bottom, {"[UP/DOWN] select   [LEFT/RIGHT] change   [ENTER] pin / change", "[d] as in Settings   [N] / [B] next / previous   [" + close_keys + "] back"}, leg_in, leg_bot);
    else push_legend_block(leg, W, border_bottom, {"[UP/DOWN] select   [LEFT/RIGHT] change   [ENTER] random / run", act ? "[N] / [B] preset   [L] lock   [d] as in Settings   [" + close_keys + "] close" : "[N] / [B] next / previous   [L] lock   [" + close_keys + "] close"}, leg_in, leg_bot);
    // the rows scroll with the cursor when the window is not tall enough (the headings and rows are about 35 lines)
    const int avail = std::max(6, term_rows_ - 1 - static_cast<int>(lines.size()) - static_cast<int>(leg.size()));
    if (fill) while (static_cast<int>(body.size()) < avail) body.push_back(row("", false));   // (the full screen version: the commands sit at the bottom of the screen)
    const int total = static_cast<int>(body.size());
    if (total <= avail) { for (const std::string& l : body) lines.push_back(l); }
    else {
        const int first = std::clamp(sel_line - avail / 2, 0, total - avail);
        for (int i = first; i < first + avail; ++i) {
            if ((i == first && first > 0) || (i == first + avail - 1 && first + avail < total)) lines.push_back(row(i == first ? "  \u25b2 ..." : "  \u25bc ...", false, true));
            else lines.push_back(body[static_cast<size_t>(i)]);
        }
    }
    for (const std::string& l : leg) lines.push_back(l);
    return lines;
}

// The visuals overlay for one track (Style Option -> More Options; the big video overlay's): the values are the track's own, kept in its
// record (-1 = as in Settings), and in force at once when it plays. [d] / DEL puts the row back to "as in Settings".
void App::milkdrop_menu_open(const std::string& track, const std::string& title, Mode back) {
    md_scope_track_ = track;
    md_scope_title_ = title.empty() && !track.empty() ? path_utf8(path_from_utf8(track).stem()) : title;
    md_return_mode_ = back;
    md_menu_row_ = 0;
    mode_ = Mode::MilkdropMenu;
    force_redraw_ = true;
}

void App::milkdrop_menu_track_adjust(int row, int dir, bool reset) {
    if (md_scope_track_.empty()) return;
    if (md_global_row(row)) {   // (the same for every track: changed in Settings, as in the main menu)
        if (reset) { status_line_ = "milkdrop: this row is the same for all tracks (no own value to reset)"; return; }
        milkdrop_menu_adjust(row, dir);
        return;
    }
    Milkdrop& m = Milkdrop::instance();
    MilkdropTrackOpts mo = cover_track_milkdrop(md_scope_track_);
    const bool playing = has_track_ && path_utf8(current_.path) == md_scope_track_;
    switch (row) {
        case kMdPreset:
            if (reset) mo.preset.clear();
            else if (dir == 2) {   // ENTER: the preset shown now is this track's
                const std::string now = m.preset_path();
                if (now.empty()) { status_line_ = "milkdrop: no preset is shown yet"; return; }
                mo.preset = now;
            } else { if (dir > 0) m.next_preset(); else m.prev_preset(); status_line_ = "milkdrop: [ENTER] makes the preset shown this track's own"; return; }
            break;
        case kMdRandom: m.random_preset(); return;
        case kMdBrowse: return;
        case kMdView: mo.on = reset ? -1 : mo.on < 0 ? (dir > 0 ? 1 : 0) : mo.on == 1 ? (dir > 0 ? 0 : -1) : (dir > 0 ? -1 : 1); break;   // as in Settings -> on -> off
        default: if (!md_edit_rec(mo, row, dir, reset)) return; break;
    }
    set_cover_track_milkdrop(md_scope_track_, mo);
    std::string lab = row == kMdFavShuffle ? "Shuffle " + md_shuffle_label() : std::string(md_label(row, true));
    for (char& ch : lab) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const std::string msg = "this track: " + lab + " " + milkdrop_row_value(row, &mo);
    status_line_ = "milkdrop: " + msg + (mo.ioff == 1 ? "  (the individual settings are off for this track: switch them on to use it)" : "");
    if (meta_) meta_->sty_msg = "saved: " + msg;
    if (playing) {
        if (row == kMdView || (row == kMdPreset && !mo.preset.empty())) milkdrop_track_apply();   // (the view / the preset itself changed)
        else milkdrop_track_live(mo);
    }
    milkdrop_pic_key_.clear();
}

// A row of the visuals menu while the playing track has individual settings: the change is only a temporary layer (md_trk_) -- the track's stored
// settings come back when the track is loaded again, or with "Reset to last track settings"; "Set current values as individual settings" keeps it.
void App::milkdrop_menu_temp(int row, int dir, bool reset) {
    Milkdrop& m = Milkdrop::instance();
    std::string what;
    switch (row) {
        case kMdPreset:
            if (reset) return;
            if (dir > 0) m.next_preset(); else m.prev_preset();
            md_preset_changed_ = true;
            status_line_ = std::string("milkdrop: ") + (dir > 0 ? "the next" : "the previous") + " preset (only until this track is loaded again, unless you set the current values as its individual settings)";
            return;
        case kMdLock:
            if (reset) { md_trk_.keep = -1; milkdrop_track_live(md_trk_); }
            else {
                const bool nl = !m.locked();
                if (!md_track_locked_) { md_user_locked_ = m.locked(); md_track_locked_ = true; }   // (given back when the next track is loaded)
                m.set_locked(nl);
                md_trk_.keep = nl ? 1 : 0;
            }
            break;
        default: {
            MilkdropTrackOpts mo = md_trk_;
            if (!md_edit_rec(mo, row, dir, reset)) return;
            milkdrop_track_live(mo);
            break;
        }
    }
    milkdrop_pic_key_.clear();
    std::string lab = row == kMdFavShuffle ? "Shuffle " + md_shuffle_label() : std::string(md_label(row, false));
    for (char& ch : lab) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    status_line_ = "milkdrop: " + lab + " " + milkdrop_row_value(row, &md_trk_) + " (only until this track is loaded again)";
}

// The rows of INDIVIDUAL TRACK SETTINGS MANAGEMENT: they act on the track the menu is for, else on the track that plays.
void App::milkdrop_ind_action(int row) {
    const std::string trk = md_target_track();
    if (trk.empty()) return;
    const bool ts = !md_scope_track_.empty();
    const bool playing = has_track_ && !audiobook_ && path_utf8(current_.path) == trk;
    MilkdropTrackOpts mo = cover_track_milkdrop(trk);
    Milkdrop& m = Milkdrop::instance();
    bool had_preset = false;
    switch (row) {
        case kMdIndSet: {   // the temporary values become the track's individual settings
            if (ts || !md_changed()) { status_line_ = "milkdrop: nothing has been changed yet"; return; }
            mo = md_trk_;
            if (md_preset_changed_ && !m.preset_path().empty()) mo.preset = m.preset_path();   // (the preset that is shown now is the track's own)
            mo.ioff = -1;
            md_undo_.clear();
            status_line_ = "milkdrop: the current values are now the individual settings of this track";
            break;
        }
        case kMdIndRevert: {   // the temporary changes are dropped: the track's stored settings are in force again
            if (ts || !md_changed()) { status_line_ = "milkdrop: nothing has been changed yet"; return; }
            ind_confirm_ = IndConfirm();
            ind_confirm_.active = true; ind_confirm_.md = true; ind_confirm_.kind = 1; ind_confirm_.track = trk; ind_confirm_.tsnap = md_snap();
            md_undo_.clear();
            md_trk_ = mo;
            md_preset_changed_ = false;
            if (!mo.preset.empty()) { if (mo.preset.find('/') != std::string::npos || mo.preset.find('\\') != std::string::npos) m.play_path(mo.preset); else m.play_named(mo.preset); }
            milkdrop_track_apply();
            milkdrop_pic_key_.clear();
            status_line_ = "milkdrop: back to the individual settings of this track -- keep changes? [Y] / [N]";
            force_redraw_ = true;
            return;
        }
        case kMdUndo:
            if (ts || md_undo_.empty()) { status_line_ = "milkdrop: nothing to undo"; return; }
            md_restore(md_undo_.back());
            md_undo_.pop_back();
            status_line_ = "milkdrop: the last change is undone";
            force_redraw_ = true;
            return;
        case kMdIndOn: {
            const bool none = mo.ioff != 1 && !md_track_any(mo);
            mo.ioff = mo.ioff == 1 ? -1 : none ? -1 : 1;
            if (mo.ioff == -1 && !md_track_any(mo)) {   // (a track without individual settings gets them: a copy of the default values, to change from there)
                const MdEff e = milkdrop_eff(MilkdropTrackOpts{});
                mo.dur = e.dur; mo.shuf = e.shuffle ? 1 : 0; mo.favs = e.favs ? 1 : 0; mo.cuts = e.cuts ? 1 : 0; mo.hcs = e.cut_sens; mo.trans = e.trans;
                mo.fxon = e.fx_on ? 1 : 0; mo.name = e.name ? 1 : 0; mo.fx = video_fx_canon(settings_.milkdrop_fx);
                md_undo_.clear();
                status_line_ = "milkdrop: this track has individual settings now (a copy of the default values: change them from here)";
            } else status_line_ = mo.ioff == 1 ? "milkdrop: the individual settings of this track are off (kept; the default settings apply)"
                                               : "milkdrop: the individual settings of this track are on again";
            break;
        }
        case kMdIndReset:   // the track's individual values are removed: the general settings (as in Settings) are in force
            if (!md_track_any(mo) && mo.ioff != 1) { status_line_ = "milkdrop: this track has no individual settings"; return; }
            had_preset = !mo.preset.empty();
            ind_confirm_ = IndConfirm();
            ind_confirm_.active = true; ind_confirm_.md = true; ind_confirm_.kind = 0; ind_confirm_.track = trk; ind_confirm_.omd = mo;
            mo = MilkdropTrackOpts();
            md_undo_.clear();
            status_line_ = "milkdrop: the individual settings of this track are removed -- keep changes? [Y] / [N]";
            break;
        case kMdIndCopy: {   // the individual values of the track become the default (general) settings
            const bool live = !ts && md_ind_active();
            const MilkdropTrackOpts src = live ? md_trk_ : mo;
            if (md_values_default(src) && !(live && md_preset_changed_)) { status_line_ = "milkdrop: this track has no individual values that differ from the default settings"; return; }
            md_undo_push();
            if (src.dur >= 0) settings_.milkdrop_preset_secs = src.dur;
            if (src.shuf >= 0) settings_.milkdrop_shuffle = src.shuf == 1;
            if (src.favs >= 0) settings_.milkdrop_fav_shuffle = src.favs == 1;
            if (src.cuts >= 0) settings_.milkdrop_hard_cuts = src.cuts == 1;
            if (src.hcs >= 0) settings_.milkdrop_cut_sens = src.hcs;
            if (src.trans >= 0) settings_.milkdrop_transition = src.trans;
            if (src.fxon >= 0) settings_.milkdrop_fx_on = src.fxon == 1;
            if (src.name >= 0) settings_.milkdrop_show_name = src.name == 1;
            if (src.fx >= 0) settings_.milkdrop_fx = video_fx_canon(src.fx);
            if (src.keep >= 0) settings_.milkdrop_lock = src.keep == 1;
            if (live && md_preset_changed_ && !m.preset_path().empty()) settings_.milkdrop_default_preset = m.preset_path();
            else if (!src.preset.empty()) settings_.milkdrop_default_preset = src.preset;
            save_settings(settings_);
            milkdrop_apply_settings();
            milkdrop_pic_key_.clear();
            status_line_ = "milkdrop: the individual values of this track are now the default settings (Undo change takes them back)";
            force_redraw_ = true;
            return;
        }
        default: return;
    }
    set_cover_track_milkdrop(trk, mo);
    if (playing) milkdrop_track_apply();
    if (playing && had_preset && row == kMdIndReset) {   // the track's own preset goes with its individual values: the general default preset (or one from the list) is shown
        const std::string& d = settings_.milkdrop_default_preset;
        if (d.find('/') != std::string::npos || d.find('\\') != std::string::npos) m.play_path(d);
        else if (!d.empty()) m.play_named(d);
        else m.random_preset();
    }
    milkdrop_pic_key_.clear();
    force_redraw_ = true;
}

void App::milkdrop_menu_adjust(int row, int dir) {
    Milkdrop& m = Milkdrop::instance();
    if (md_ind_active() && md_rec_row(row)) { milkdrop_menu_temp(row, dir, false); return; }   // (the playing track has individual settings: only a temporary change)
    switch (row) {
        case kMdPreset: if (dir > 0) m.next_preset(); else m.prev_preset(); break;
        case kMdRandom: m.random_preset(); break;
        case kMdLock: m.set_locked(!m.locked()); settings_.milkdrop_lock = m.locked(); save_settings(settings_); status_line_ = m.locked() ? "milkdrop: the preset stays" : "milkdrop: the presets change again"; break;
        case kMdDuration: settings_.milkdrop_preset_secs = md_step(kMdDurSteps, settings_.milkdrop_preset_secs, dir); break;
        case kMdBrowse: if (dir > 0) { md_browse_pending_ = true; md_browse_from_ = m.preset_path(); mdb_open(); } return;
        case kMdShuffle: settings_.milkdrop_shuffle = !settings_.milkdrop_shuffle; break;
        case kMdFolder: {   // the folder that is shuffled: Favourites, Recommended, the folders made in the preset overlay
            const std::vector<std::string> ch = md_folder_choices();
            const std::string now = md_shuffle_label();
            size_t at = 0;
            for (size_t i = 0; i < ch.size(); ++i) if (ch[i] == now) at = i;
            at = (at + (dir < 0 ? ch.size() - 1 : 1)) % ch.size();
            settings_.milkdrop_shuffle_folder = at == 0 ? std::string() : ch[at];
            status_line_ = "milkdrop: the shuffle plays from " + ch[at] + " (" + std::to_string(md_shuffle_paths().size()) + " presets)";
            break;
        }
        case kMdFavShuffle:
            settings_.milkdrop_fav_shuffle = !settings_.milkdrop_fav_shuffle;
            status_line_ = !settings_.milkdrop_fav_shuffle ? "milkdrop: all presets again"
                         : md_shuffle_paths().empty() ? "milkdrop: only " + md_shuffle_label() + " -- there are no presets in it yet ([a] / SHIFT+a in the Preset Overlay): all presets until then"
                         : "milkdrop: only the " + std::to_string(md_shuffle_paths().size()) + " presets of " + md_shuffle_label();
            break;
        case kMdCutSens: settings_.milkdrop_cut_sens = md_step(kMdCutSensSteps, settings_.milkdrop_cut_sens, dir); break;
        case kMdHardCuts: settings_.milkdrop_hard_cuts = !settings_.milkdrop_hard_cuts; break;
        case kMdTransition: settings_.milkdrop_transition = std::clamp(settings_.milkdrop_transition + dir, 0, 10); break;
        case kMdFps: settings_.milkdrop_fps = md_step(kMdFpsSteps, settings_.milkdrop_fps, dir); break;
        case kMdRes: settings_.milkdrop_res = md_step(kMdResSteps, settings_.milkdrop_res, dir); milkdrop_pic_key_.clear(); break;
        case kMdGpu: {   // automatic, then every card (Linux); elsewhere the system decides
            const std::vector<MilkdropGpu> g = Milkdrop::gpus();
            if (g.empty()) { status_line_ = "milkdrop GPU: the system chooses the graphics card here (Windows: Settings > System > Display > Graphics)"; break; }
            int at = 0;
            for (size_t i = 0; i < g.size(); ++i) if (g[i].id == settings_.milkdrop_gpu) at = static_cast<int>(i) + 1;
            at = (at + dir + static_cast<int>(g.size()) + 1) % (static_cast<int>(g.size()) + 1);
            settings_.milkdrop_gpu = at == 0 ? std::string() : g[static_cast<size_t>(at) - 1].id;
            status_line_ = "milkdrop GPU: " + milkdrop_gpu_label(settings_.milkdrop_gpu) + " -- the visuals restart on it";
            break;
        }
        case kMdBright: settings_.milkdrop_bright = std::clamp(settings_.milkdrop_bright + 5 * dir, 0, 100); milkdrop_pic_key_.clear(); break;
        case kMdSat: settings_.milkdrop_sat = std::clamp(settings_.milkdrop_sat + 5 * dir, 0, 100); milkdrop_pic_key_.clear(); break;
        case kMdContrast: settings_.milkdrop_contrast = std::clamp(settings_.milkdrop_contrast + 5 * dir, 0, 100); milkdrop_pic_key_.clear(); break;
        case kMdHcd: settings_.milkdrop_hcd = md_step(kMdHcdSteps, settings_.milkdrop_hcd, dir); break;
        case kMdVar: settings_.milkdrop_var = md_step(kMdVarSteps, settings_.milkdrop_var, dir); break;
        case kMdMesh: settings_.milkdrop_mesh = std::clamp(settings_.milkdrop_mesh + dir, 0, 3); status_line_ = "milkdrop mesh: " + mesh_name(settings_.milkdrop_mesh) + " -- the visuals restart"; break;
        case kMdAspect: settings_.milkdrop_aspect = !settings_.milkdrop_aspect; break;
        case kMdRatio: settings_.milkdrop_ratio = (settings_.milkdrop_ratio + (dir < 0 ? kMilkRatioCount - 1 : 1)) % kMilkRatioCount; status_line_ = std::string("milkdrop aspect ratio: ") + milkdrop_ratio_name(settings_.milkdrop_ratio) + (settings_.milkdrop_aspect ? "" : " (Aspect Correction is off: the picture fills the place)"); break;
        case kMdClean: settings_.milkdrop_clean = !settings_.milkdrop_clean; break;
        case kMdUseFx:
            settings_.milkdrop_fx_on = !settings_.milkdrop_fx_on;
            milkdrop_pic_key_.clear();
            status_line_ = settings_.milkdrop_fx_on ? "milkdrop: the effect is on again" : "milkdrop: no effect on the visuals";
            break;
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
    if (row != kMdPreset && row != kMdRandom && row != kMdLock && row != kMdBrowse) save_settings(settings_);   // (saved with every change: not only when the menu is closed by ESC)
}

void App::handle_milkdrop_menu_key(int key) {
    if (ind_confirm_key(key)) return;
    if (mdb_open_) { handle_mdb_key(key); return; }
    const bool ts = !md_scope_track_.empty();
    const bool arrow = last_key_was_arrow();
    auto move = [&](int d) {
        for (int i = 0; i < kMdRows; ++i) { md_menu_row_ = (md_menu_row_ + d + kMdRows) % kMdRows; if (md_row_shown(md_menu_row_)) break; }
    };
    auto preset_op = [&](int d) {   // 1 next, -1 previous, 0 random: a change of the visuals (the temporary layer when the track has individual settings)
        md_undo_push();
        Milkdrop& m = Milkdrop::instance();
        if (d > 0) m.next_preset(); else if (d < 0) m.prev_preset(); else m.random_preset();
        if (md_ind_active()) md_preset_changed_ = true;
        force_redraw_ = true;
    };
    auto change = [&](int row, int dir) {
        if (row >= kMdIndSet && row <= kMdIndOn) { if (dir > 0 || row == kMdIndOn) milkdrop_ind_action(row); return; }   // (the individual settings rows: an action row only by ENTER / SPACE / RIGHT)
        if (ts) { milkdrop_menu_track_adjust(row, dir, false); return; }
        if (row == kMdPreset) { preset_op(dir > 0 ? 1 : -1); return; }
        if (row == kMdRandom) { preset_op(0); return; }
        if (row == kMdBrowse) { if (dir > 0) { md_undo_push(); milkdrop_menu_adjust(row, dir); } return; }
        const MdSnap before = md_snap();
        milkdrop_menu_adjust(row, dir);
        if (!md_snap_same(before, md_snap())) { md_undo_.push_back(before); if (md_undo_.size() > 60) md_undo_.erase(md_undo_.begin()); }
        force_redraw_ = true;
    };
    if (!md_row_shown(md_menu_row_)) move(1);
    if (arrow && (key == 'A' || key == 'B') && (last_key_was_shifted() || last_key_was_page())) {   // SHIFT+UP / DOWN, PgUp / PgDn: a page of rows (not around the ends)
        const int d = key == 'A' ? -1 : 1;
        for (int step = 0, cur = md_menu_row_; step < 8; ++step) {
            int nxt = cur;
            do { nxt += d; } while (nxt >= 0 && nxt < kMdRows && !md_row_shown(nxt));
            if (nxt < 0 || nxt >= kMdRows) break;
            cur = nxt;
            md_menu_row_ = cur;
        }
        return;
    }
    if (arrow && key == 'A') { move(-1); return; }
    if (arrow && key == 'B') { move(1); return; }
    if (arrow && (key == 'C' || key == 'D')) { change(md_menu_row_, key == 'C' ? 1 : -1); return; }
    if (arrow) return;
    if (key == '\r' || key == '\n') {
        if (md_menu_row_ == kMdBrowse) { if (ts) mdb_open_track(md_scope_track_, md_scope_title_); else change(kMdBrowse, 1); return; }
        if (md_menu_row_ == kMdRandom) { if (ts) Milkdrop::instance().random_preset(); else preset_op(0); return; }
        if (md_menu_row_ == kMdPreset) { if (ts) milkdrop_menu_track_adjust(kMdPreset, 2, false); else preset_op(0); return; }
        change(md_menu_row_, 1);
        return;
    }
    if (key == ' ') { change(md_menu_row_, 1); return; }
    if (key == 'd' || key == 'D' || key == kKeyDelete || key == 127 || key == 8) {   // [d]: the row goes back to "as in Settings"
        if (md_menu_row_ >= kMdIndSet && md_menu_row_ <= kMdIndOn) return;
        if (ts) { milkdrop_menu_track_adjust(md_menu_row_, 0, true); return; }
        if (md_ind_active() && md_rec_row(md_menu_row_) && md_menu_row_ != kMdPreset) {
            const MdSnap before = md_snap();
            milkdrop_menu_temp(md_menu_row_, 0, true);
            if (!md_snap_same(before, md_snap())) md_undo_.push_back(before);
            force_redraw_ = true;
        }
        return;
    }
    if (key == 'n' || key == 'N') { if (ts) Milkdrop::instance().next_preset(); else preset_op(1); return; }
    if (key == 'b' || key == 'B') { if (ts) Milkdrop::instance().prev_preset(); else preset_op(-1); return; }
    if (key == 'l' || key == 'L') { change(kMdLock, 1); return; }
    if (key == 27 || key_is("HKeyVisualsMenu", key)) {
        mode_ = md_return_mode_;
        md_return_mode_ = Mode::Browse;
        const bool was_track = ts;
        md_undo_.clear();
        md_scope_track_.clear();
        md_scope_title_.clear();
        if (!was_track) save_settings(settings_);
        force_redraw_ = true;
    }
}

// The visuals overlay over the whole screen: its options on the left, the visuals (the picture the settings change) on the right.
// "" = too narrow for the picture (the options then come as the panel over the screen, as before).
std::string App::build_milkdrop_menu_screen(int W, const char* clear_prefix) const {
    const int Wp = kMdMenuPanelWidth, mg = 1;
    const int H = std::max(6, term_rows_ - 1);
    const int pw = W - Wp - 2 - 2 * mg, ph = H - 2;
    if (pw < 20) return std::string();
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    const std::vector<std::string> left = build_milkdrop_menu_panel(true);
    std::vector<std::string> pic;
    const std::vector<std::string> nb = milkdrop_name_box(pw - 2);
    std::string why;
    const bool avail = Milkdrop::instance().available(&why);
    if (!avail || !milkdrop_draw(pic, pw, ph, Wp + 1 + mg, 1, false, 0, nb, nb.empty() ? 0 : pw)) {
        pic.assign(static_cast<size_t>(ph), std::string(static_cast<size_t>(pw), ' '));
        const std::string full = !avail ? "Not available: " + why : Milkdrop::instance().status().empty() ? "starting ..." : Milkdrop::instance().status();
        const std::vector<std::string> ml = wrap_lines(full, std::max(10, pw - 2), 8);
        int r0 = std::max(0, (ph - static_cast<int>(ml.size())) / 2);
        for (size_t k = 0; k < ml.size() && r0 + static_cast<int>(k) < ph; ++k) {
            const int pad = std::max(0, (pw - display_width(ml[k])) / 2);
            pic[static_cast<size_t>(r0) + k] = std::string(static_cast<size_t>(pad), ' ') + ml[k] + std::string(static_cast<size_t>(std::max(0, pw - pad - display_width(ml[k]))), ' ');
        }
    }
    std::ostringstream f;
    f << clear_prefix << "\x1b[?25l";
    for (int r = 0; r < H; ++r) {
        f << (r < static_cast<int>(left.size()) ? left[static_cast<size_t>(r)] : std::string(static_cast<size_t>(Wp), ' '));
        if (r == 0) f << box_top("VISUALS", W - Wp, border);
        else if (r == H - 1) f << box_bottom(W - Wp, "", border_bottom);
        else {
            const std::string p = r - 1 < static_cast<int>(pic.size()) ? pic[static_cast<size_t>(r - 1)] : std::string(static_cast<size_t>(pw), ' ');
            f << bar << std::string(static_cast<size_t>(mg), ' ') << p << "\x1b[0m" << std::string(static_cast<size_t>(mg), ' ') << bar;
        }
        if (r + 1 < H) f << "\n";
    }
    return clamp_output_rows(f.str(), term_rows_);
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

fs::path App::md_config_dir() const { return config_path().parent_path(); }

std::vector<std::string> App::md_folder_choices() const {
    std::vector<std::string> v = {"Favourites", "Recommended"};
    for (const std::string& n : mdf::names(md_config_dir())) v.push_back(n);
    return v;
}

std::string App::md_shuffle_label() const {
    const std::string& f = settings_.milkdrop_shuffle_folder;
    if (f.empty() || f == "Favourites") return "Favourites";
    if (f == "Recommended") return f;
    return mdf::exists(md_config_dir(), f) ? f : std::string("Favourites");   // (a folder that is gone: the Favourites)
}

std::vector<std::string> App::md_shuffle_paths() const {
    const std::string l = md_shuffle_label();
    if (l == "Favourites") { md_favs_load(); return md_favs_; }
    if (l == "Recommended") return const_cast<App*>(this)->md_recommended_paths();
    std::error_code ec;
    const auto t = fs::last_write_time(mdf::dir_of(md_config_dir()) / path_from_utf8(l + ".txt"), ec);
    const std::string key = l + "|" + (ec ? std::string("-") : std::to_string(static_cast<long long>(t.time_since_epoch().count())));
    if (md_shuf_key_ != key) { md_shuf_cache_ = mdf::load(md_config_dir(), l); md_shuf_key_ = key; }
    return md_shuf_cache_;
}

bool App::mdb_is_extra_root(const fs::path& d) const {
    for (const std::string& e : Milkdrop::instance().extra_preset_dirs()) if (path_from_utf8(e).lexically_normal() == d.lexically_normal()) return true;
    return false;
}

bool App::mdb_inside(const std::string& file) const {
    const fs::path p = path_from_utf8(file);
    const std::string pr = path_utf8(p.parent_path().lexically_normal());
    auto under = [&](const std::string& root) {
        const std::string rr = path_utf8(path_from_utf8(root).lexically_normal());
        return !rr.empty() && pr.size() >= rr.size() && pr.compare(0, rr.size(), rr) == 0;
    };
    if (under(mdb_root_)) return true;
    for (const std::string& e : Milkdrop::instance().extra_preset_dirs()) if (under(e)) return true;
    return false;
}

static const char* const kMdbRecs = "<recommended>";   // (mdb_dir_ while the Recommended folder is shown)
static const std::string kMdbUserPre = "<folder>:";    // (mdb_dir_ while a folder of the user is shown: this, then its name)

void App::mdb_list(const std::string& dir, bool favs, const std::string& select_path) {
    mdb_items_.clear();
    mdb_in_favs_ = favs;
    mdb_in_recs_ = !favs && dir == kMdbRecs;
    mdb_in_user_ = (!favs && dir.compare(0, kMdbUserPre.size(), kMdbUserPre) == 0) ? dir.substr(kMdbUserPre.size()) : std::string();
    mdb_dir_ = dir;
    const fs::path root = path_from_utf8(mdb_root_);
    if (mdb_in_recs_) {
        mdb_items_.push_back({0, "..", mdb_root_, ""});
        for (const std::string& f : md_recommended_paths()) {
            const fs::path p = path_from_utf8(f);
            fs::path rel = p.parent_path().lexically_relative(root);
            std::string where;
            if (!rel.empty() && *rel.begin() != "..") { for (const auto& part : rel) if (part != ".") where += (where.empty() ? "" : " / ") + path_utf8(part); }
            else where = path_utf8(p.parent_path().filename());
            mdb_items_.push_back({3, path_utf8(p.stem()), f, where});
        }
    } else if (!mdb_in_user_.empty()) {
        mdb_items_.push_back({0, "..", mdb_root_, ""});
        std::error_code ec;
        for (const std::string& f : mdf::load(md_config_dir(), mdb_in_user_)) {
            const fs::path p = path_from_utf8(f);
            mdb_items_.push_back({fs::is_regular_file(p, ec) ? 3 : 4, path_utf8(p.stem()), f, ""});
        }
    } else if (favs) {
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
        if (d != root && !dir.empty()) mdb_items_.push_back({0, "..", mdb_is_extra_root(d) ? mdb_root_ : path_utf8(d.parent_path()), ""});
        else {
            md_favs_load();
            mdb_items_.push_back({1, "Favourites", "", ""});
            { const size_t nrec = md_recommended_paths().size(); if (nrec > 0) mdb_items_.push_back({5, "Recommended", std::to_string(nrec), ""}); }   // (path: how many)
            for (const std::string& n : mdf::names(md_config_dir())) mdb_items_.push_back({6, n, std::to_string(mdf::load(md_config_dir(), n).size()), ""});   // (the folders of the user; path: how many)
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
        if (d == root || dir.empty())   // the further preset folders (Settings -> PATHS) sit at the top level, after the folders of the regular one
            for (const std::string& e : Milkdrop::instance().extra_preset_dirs()) folders.push_back({2, path_utf8(path_from_utf8(e).filename()) + "  (extra path)", e, ""});
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
    mdb_default_ = false;
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
        if (mdb_inside(at)) dir = path_utf8(path_from_utf8(at).parent_path());
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
        if (mdb_inside(at)) dir = path_utf8(path_from_utf8(at).parent_path());
    }
    mdb_list(dir, false, at);
}

// Settings -> MILKDROP -> Default Preset / Preset Overlay: the same overlay; ENTER on a preset makes it the one the visuals start with.
void App::mdb_open_default() {
    mdb_open();
    if (!mdb_open_) return;
    mdb_default_ = true;
    const std::string& d = settings_.milkdrop_default_preset;
    if (d.find('/') != std::string::npos || d.find('\\') != std::string::npos) {   // (on the default preset, when there is one)
        if (mdb_inside(d)) mdb_list(path_utf8(path_from_utf8(d).parent_path()), false, d);
    }
}

void App::mdb_close(bool chosen) {
    Milkdrop& m = Milkdrop::instance();
    if (!chosen && mdb_moved_ && !mdb_prev_path_.empty()) m.play_path(mdb_prev_path_);   // back to the preset from before
    m.set_locked(mdb_prev_locked_);
    mdb_open_ = false; mdb_del_ask_.clear(); mdb_pick_open_ = false; mdb_new_ = false;
    mdb_track_.clear();
    mdb_default_ = false;
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

void App::mdb_build_index() {
    std::string key = mdb_root_;
    for (const std::string& x : Milkdrop::instance().extra_preset_dirs()) key += "\n" + x;
    if (mdb_index_root_ != key) {   // all presets under the preset folder(s), once
        mdb_index_.clear();
        mdb_index_root_ = key;
        std::vector<std::string> roots{mdb_root_};
        for (const std::string& x : Milkdrop::instance().extra_preset_dirs()) roots.push_back(x);
        for (size_t ri = 0; ri < roots.size(); ++ri) {
            const fs::path root = path_from_utf8(roots[ri]);
            std::error_code ec;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
                if (it->is_directory(ec)) { if (it->path().filename() == "textures") it.disable_recursion_pending(); continue; }
                if (!is_preset_file(it->path())) continue;
                MdbIdx e;
                e.name = path_utf8(it->path().stem());
                e.path = path_utf8(it->path());
                if (ri > 0) e.where = path_utf8(root.filename());   // (a further folder: named first)
                const fs::path rel = it->path().parent_path().lexically_relative(root);
                for (const auto& part : rel) if (part != ".") e.where += (e.where.empty() ? "" : " / ") + path_utf8(part);
                e.name_n = normalize_for_search(e.name);
                e.where_n = normalize_for_search(e.where);
                e.all_n = e.name_n + " " + e.where_n;
                mdb_index_.push_back(std::move(e));
            }
        }
    }
}

// The Recommended list: milkdrop_recommended.txt -- the one that ships with Mousiki (recommended.txt beside the preset folder) and the
// user's own in the config folder. A line is a preset's path inside the collection ("Fractal/some preset.milk") or only its file name
// (the extension may be left out); it is looked up in the presets that are installed, so the list fits any copy of the collection.
std::vector<std::string> App::md_recommended_paths() {
    const std::string root = Milkdrop::instance().find_presets_dir();
    if (root.empty()) return {};
    mdb_root_ = root;
    mdb_build_index();
    std::vector<fs::path> files;
    files.push_back(path_from_utf8(root).parent_path() / "recommended.txt");
    files.push_back(config_path().parent_path() / "milkdrop_recommended.txt");
    // The list is asked for on every redraw of the MilkDrop options (their values show how many presets a folder has): resolving it walks
    // the whole preset index for every line of the lists, so it is made once and kept until the preset folders or the two lists change.
    std::string ckey = mdb_index_root_ + "|" + std::to_string(mdb_index_.size());
    for (const fs::path& f : files) {
        std::error_code ec;
        const auto t = fs::last_write_time(f, ec);
        ckey += "|" + (ec ? std::string("-") : std::to_string(static_cast<long long>(t.time_since_epoch().count())));
    }
    if (!md_rec_key_.empty() && md_rec_key_ == ckey) return md_rec_cache_;
    std::vector<std::string> lowered;
    lowered.reserve(mdb_index_.size());
    for (const MdbIdx& e : mdb_index_) { std::string l = lower_ascii(e.path); std::replace(l.begin(), l.end(), '\\', '/'); lowered.push_back(std::move(l)); }
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
            std::string want = lower_ascii(line);
            const bool has_ext = want.size() > 5 && want.compare(want.size() - 5, 5, ".milk") == 0;
            for (size_t k = 0; k < lowered.size(); ++k) {
                const std::string& cand = lowered[k];
                // the path ends with the line ("/" before it, unless the line is the whole path); without an extension: the line plus ".milk"
                bool hit = false;
                for (const std::string& w : has_ext ? std::vector<std::string>{want} : std::vector<std::string>{want + ".milk", want + ".milk2"}) {
                    if (cand.size() < w.size() || cand.compare(cand.size() - w.size(), w.size(), w) != 0) continue;
                    if (cand.size() == w.size() || cand[cand.size() - w.size() - 1] == '/') { hit = true; break; }
                }
                if (!hit) continue;
                if (std::find(out.begin(), out.end(), mdb_index_[k].path) == out.end()) out.push_back(mdb_index_[k].path);
                break;   // (the first preset that fits)
            }
        }
    }
    md_rec_key_ = ckey;
    md_rec_cache_ = out;
    return out;
}

void App::mdb_search_run() {
    mdb_build_index();
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
    md_shuf_key_.clear();   // (a key may change a folder file: the shuffle list is read again)
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
            const bool from_favs = mdb_in_favs_, from_recs = mdb_in_recs_;
            const std::string from_user = mdb_in_user_;
            const std::string from = from_favs || from_recs || !from_user.empty() ? std::string() : mdb_dir_;
            mdb_list(it.path, false, from);   // (on the folder it came from)
            if (from_favs || from_recs) for (size_t k = 0; k < mdb_items_.size(); ++k) if (mdb_items_[k].kind == (from_favs ? 1 : 5)) mdb_sel_ = static_cast<int>(k);
            if (!from_user.empty()) for (size_t k = 0; k < mdb_items_.size(); ++k) if (mdb_items_[k].kind == 6 && mdb_items_[k].name == from_user) mdb_sel_ = static_cast<int>(k);
        } else if (it.kind == 5) mdb_list(kMdbRecs, false, "");
        else if (it.kind == 6) mdb_list(kMdbUserPre + it.name, false, "");
        else if (it.kind == 1) mdb_list(mdb_root_, true, "");
        else if (it.kind == 2) mdb_list(it.path, false, "");
        mdb_preview();
    };
    if (mdb_new_) {   // the name of a new folder
        if (arrow) return;
        if (key == 27) { mdb_new_ = false; mdb_new_add_ = false; status_line_ = "milkdrop: no folder made"; return; }
        if (key == '\r' || key == '\n') {
            std::string made;
            if (!mdf::create(md_config_dir(), mdb_new_name_, &made)) status_line_ = mdb_new_name_.empty() ? "milkdrop: a folder needs a name" : "milkdrop: no folder made -- \"" + mdb_new_name_ + "\" is there already or is a name of its own (Favourites, Recommended)";
            else {
                status_line_ = "milkdrop: the folder \"" + made + "\" is made";
                if (mdb_new_add_ && !mdb_pick_preset_.empty()) { mdf::add(md_config_dir(), made, mdb_pick_preset_); status_line_ += ", the preset is in it"; }
                if (mdb_in_favs_ || mdb_in_recs_ || !mdb_in_user_.empty() || mdb_searching_) { /* (the list stays) */ }
                else mdb_list(mdb_dir_, false, "");
            }
            mdb_new_ = false; mdb_new_add_ = false;
            return;
        }
        if (key == 127 || key == 8) { while (!mdb_new_name_.empty()) { const unsigned char c = static_cast<unsigned char>(mdb_new_name_.back()); mdb_new_name_.pop_back(); if ((c & 0xC0) != 0x80) break; } return; }
        if (((key >= 32 && key < 127) || (key >= 128 && key < 256)) && mdb_new_name_.size() < 60) mdb_new_name_ += static_cast<char>(key);
        return;
    }
    if (!mdb_del_ask_.empty()) {   // the warning before a folder is deleted
        if (arrow) return;
        if (key == 'y' || key == 'Y' || key == '\r' || key == '\n') {
            const std::string name = mdb_del_ask_;
            mdb_del_ask_.clear();
            MdbUndo u; u.kind = 1; u.folder = name; u.paths = mdf::load(md_config_dir(), name); u.was_shuffle = settings_.milkdrop_shuffle_folder == name;
            mdf::remove_folder(md_config_dir(), name);
            mdb_undo_push(u);
            if (settings_.milkdrop_shuffle_folder == name) settings_.milkdrop_shuffle_folder.clear();
            status_line_ = "milkdrop: the folder \"" + name + "\" is deleted (SHIFT+u brings it back)";
            mdb_list(mdb_dir_, false, "");
            milkdrop_apply_settings();
        } else if (key == 27 || key == 'n' || key == 'N') {
            mdb_del_ask_.clear();
            status_line_ = "milkdrop: the folder is kept";
        }
        return;
    }
    if (mdb_pick_open_) { mdb_pick_key(key); return; }
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
        if (it.kind == 1 || it.kind == 2 || it.kind == 5 || it.kind == 6) enter(it);
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
            status_line_ = "milkdrop: this track shows \"" + it.name + "\"";
            mdb_close(playing);   // (another track's choice: the preset of this one comes back)
            if (playing) milkdrop_track_apply();
        } else if (it.kind == 3 && mdb_default_) {   // Settings: the preset the visuals start with
            settings_.milkdrop_default_preset = it.path;
            settings_dirty_ = true;
            status_line_ = "milkdrop: the visuals start with \"" + it.name + "\" (s saves)";
            mdb_close(false);   // (the preset shown before comes back)
            milkdrop_apply_settings();
            // ... but a preset that was just chosen as the default is the one to see: the visuals show it now (and it stays when the Settings
            // preview ends), not only after the next start of MilkDrop
            Milkdrop::instance().play_path(it.path);
            settings_preview_set_prev(it.path);
        } else if (it.kind == 3) {
            Milkdrop::instance().play_path(it.path);
            status_line_ = "milkdrop: " + it.name;
            mdb_close(true);
        } else if (it.kind == 4) status_line_ = "milkdrop: this favourite's file is gone ([d] takes it out)";
        else enter(it);
        return;
    }
    if (key == 'e' || key == 'E') { milkdrop_effect_toggle(mdb_default_); return; }   // [e]: the effect on the visuals on / off (the preview shows it)
    if (key_is("HKeyVideoFx", key) || key_is("HKeyVideoFxBack", key)) { milkdrop_effect_step(key_is("HKeyVideoFxBack", key) ? -1 : 1, mdb_default_); return; }   // [g] / [h]: the next / previous effect
    if (key == 'f' || key == 'F') {   // [f]: straight to the Favourites folder
        if (mdb_searching_) { mdb_typing_ = mdb_searching_ = false; mdb_query_.clear(); }
        md_favs_load();
        mdb_list(mdb_root_, true, "");
        mdb_preview();
        return;
    }
    if (key == 'n' || key == 'N') {   // a new folder of presets
        if (mdb_searching_ || mdb_in_favs_ || mdb_in_recs_ || !mdb_in_user_.empty()) { status_line_ = "milkdrop: [n] makes a new folder (in the list of the preset folders: [\u2190] goes up)"; return; }
        mdb_new_ = true; mdb_new_name_.clear(); mdb_new_add_ = false; mdb_pick_preset_.clear();
        return;
    }
    if (key == 'U') {   // SHIFT+u: takes back the last delete
        if (!mdb_undo_pop()) status_line_ = "milkdrop: nothing to undo";
        return;
    }
    if (key == 'A') {   // SHIFT+a: the folder this preset goes into
        if (it.kind != 3) { status_line_ = "milkdrop: SHIFT+a adds a preset (not a folder) to a folder of your choice"; return; }
        mdb_pick_open(it.path, it.name);
        return;
    }
    if ((key == 'd' || key == 'D' || key == kKeyDelete) && !mdb_in_user_.empty() && (it.kind == 3 || it.kind == 4)) {
        {
            MdbUndo u; u.kind = 2; u.folder = mdb_in_user_; u.preset = it.path;
            const std::vector<std::string> before = mdf::load(md_config_dir(), mdb_in_user_);
            u.index = static_cast<size_t>(std::find(before.begin(), before.end(), it.path) - before.begin());
            mdb_undo_push(u);
        }
        mdf::remove_preset(md_config_dir(), mdb_in_user_, it.path);
        status_line_ = "milkdrop: " + it.name + " taken out of \"" + mdb_in_user_ + "\" (SHIFT+u brings it back)";
        const int keep = mdb_sel_;
        mdb_list(mdb_dir_, false, "");
        mdb_sel_ = std::clamp(keep, 0, static_cast<int>(mdb_items_.size()) - 1);
        milkdrop_apply_settings();
        mdb_preview();
        return;
    }
    if ((key == 'd' || key == 'D' || key == kKeyDelete) && it.kind == 6) {   // a folder of the user: deleted (the presets stay where they are) with a second press
        mdb_del_ask_ = it.name;   // (a warning first: [ENTER] / [y] deletes it, SHIFT+u brings it back)
        return;
    }
    if (key == 'a') {
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
        {
            MdbUndo u; u.kind = 3; u.preset = it.path;
            u.index = static_cast<size_t>(std::find(md_favs_.begin(), md_favs_.end(), it.path) - md_favs_.begin());
            mdb_undo_push(u);
        }
        md_favs_.erase(std::remove(md_favs_.begin(), md_favs_.end(), it.path), md_favs_.end());
        md_favs_save();
        status_line_ = "milkdrop: " + it.name + " taken out of the favourites (" + std::to_string(md_favs_.size()) + " left, SHIFT+u brings it back)";
        const int keep = mdb_sel_;
        mdb_list(mdb_root_, true, "");
        mdb_sel_ = std::clamp(keep, 0, static_cast<int>(mdb_items_.size()) - 1);
        milkdrop_apply_settings();
        mdb_preview();
        return;
    }
    if (key == 'd' || key == 'D') status_line_ = "milkdrop: [d] takes a preset out of the favourites or of a folder (in that folder)";
}

void App::mdb_undo_push(MdbUndo u) {
    mdb_undo_.push_back(std::move(u));
    if (mdb_undo_.size() > 30) mdb_undo_.erase(mdb_undo_.begin());
}

// SHIFT+u: the delete that was made last is taken back (a folder with its presets, a preset in its place in a folder / in the favourites).
bool App::mdb_undo_pop() {
    if (mdb_undo_.empty()) return false;
    const MdbUndo u = mdb_undo_.back();
    mdb_undo_.pop_back();
    if (u.kind == 1) {
        if (mdf::exists(md_config_dir(), u.folder)) { status_line_ = "milkdrop: the folder \"" + u.folder + "\" is there again already"; return true; }
        mdf::save(md_config_dir(), u.folder, u.paths);
        if (u.was_shuffle) settings_.milkdrop_shuffle_folder = u.folder;
        status_line_ = "milkdrop: the folder \"" + u.folder + "\" is back (" + std::to_string(u.paths.size()) + " presets)";
        if (mdb_in_favs_ || mdb_in_recs_ || !mdb_in_user_.empty() || mdb_searching_) { /* (the list stays) */ }
        else mdb_list(mdb_dir_, false, "");
    } else if (u.kind == 2) {
        std::vector<std::string> v = mdf::load(md_config_dir(), u.folder);
        if (std::find(v.begin(), v.end(), u.preset) == v.end()) v.insert(v.begin() + static_cast<std::ptrdiff_t>(std::min(u.index, v.size())), u.preset);
        mdf::save(md_config_dir(), u.folder, v);
        status_line_ = "milkdrop: the preset is back in \"" + u.folder + "\"";
        if (mdb_in_user_ == u.folder) mdb_list(mdb_dir_, false, u.preset);
    } else {
        md_favs_load();
        if (std::find(md_favs_.begin(), md_favs_.end(), u.preset) == md_favs_.end()) md_favs_.insert(md_favs_.begin() + static_cast<std::ptrdiff_t>(std::min(u.index, md_favs_.size())), u.preset);
        md_favs_save();
        status_line_ = "milkdrop: the preset is back in the favourites (" + std::to_string(md_favs_.size()) + ")";
        if (mdb_in_favs_) mdb_list(mdb_root_, true, u.preset);
    }
    milkdrop_apply_settings();
    mdb_preview();
    return true;
}

// SHIFT+a in the preset overlay: the small overlay that chooses the folder a preset is added to (Favourites, the folders of the user, a new one).
void App::mdb_pick_open(const std::string& preset, const std::string& title) {
    mdb_pick_names_.clear();
    mdb_pick_names_.push_back("Favourites");
    for (const std::string& n : mdf::names(md_config_dir())) mdb_pick_names_.push_back(n);
    mdb_pick_names_.push_back("+ New folder ...");
    mdb_pick_sel_ = 0;
    mdb_pick_preset_ = preset;
    mdb_pick_title_ = title;
    mdb_pick_open_ = true;
}

bool App::mdb_pick_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(mdb_pick_names_.size());
    if (arrow) {
        if (key == 'A') mdb_pick_sel_ = (mdb_pick_sel_ + n - 1) % n;
        else if (key == 'B') mdb_pick_sel_ = (mdb_pick_sel_ + 1) % n;
        else if (key == 'D') mdb_pick_open_ = false;
        return true;
    }
    if (key == 27) { mdb_pick_open_ = false; return true; }
    if (key == '\r' || key == '\n' || key == ' ') {
        const std::string name = mdb_pick_names_[static_cast<size_t>(mdb_pick_sel_)];
        mdb_pick_open_ = false;
        if (mdb_pick_sel_ == n - 1) { mdb_new_ = true; mdb_new_name_.clear(); mdb_new_add_ = true; return true; }   // (the preset goes into it once it is made)
        if (name == "Favourites") {
            md_favs_load();
            if (std::find(md_favs_.begin(), md_favs_.end(), mdb_pick_preset_) != md_favs_.end()) status_line_ = "milkdrop: " + mdb_pick_title_ + " is a favourite already";
            else { md_favs_.push_back(mdb_pick_preset_); md_favs_save(); status_line_ = "milkdrop: " + mdb_pick_title_ + " added to the favourites (" + std::to_string(md_favs_.size()) + ")"; }
        } else {
            const int r = mdf::add(md_config_dir(), name, mdb_pick_preset_);
            status_line_ = r == 0 ? "milkdrop: " + mdb_pick_title_ + " added to \"" + name + "\"" : r == 1 ? "milkdrop: " + mdb_pick_title_ + " is in \"" + name + "\" already" : "milkdrop: the folder \"" + name + "\" could not be written";
            if (mdb_in_user_ == name) mdb_list(mdb_dir_, false, mdb_pick_preset_);
        }
        milkdrop_apply_settings();
        return true;
    }
    return true;
}

std::vector<std::string> App::mdb_pick_panel() const {
    const int W = 44, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    std::vector<std::string> lines;
    lines.push_back(box_top("ADD TO A FOLDER", W, border));
    const std::string t = truncate_str(mdb_pick_title_, inner);
    lines.push_back(bar + " " + ansi_for(settings_.header_color, false) + pad_right(t, inner) + R + " " + bar);
    const int n = static_cast<int>(mdb_pick_names_.size());
    const int vis = std::clamp(term_rows_ - 12, 3, 12);
    const int top = std::clamp(mdb_pick_sel_ - vis / 2, 0, std::max(0, n - vis));
    for (int i = top; i < std::min(n, top + vis); ++i) {
        const bool sel = i == mdb_pick_sel_;
        std::string name = mdb_pick_names_[static_cast<size_t>(i)];
        std::string txt = std::string(sel ? "> " : "  ") + truncate_str(name, inner - 2);
        lines.push_back(bar + " " + (sel ? HI : "") + pad_right(txt, inner) + R + " " + bar);
    }
    lines.push_back(box_bottom(W, "[ENTER] add  [ESC] cancel", border_bottom));
    return lines;
}

std::string App::mdb_floating(int W) const {
    std::ostringstream f;
    if (mdb_new_) {
        const int PW = 44, inner = PW - 4;
        const std::string border = ansi_for(settings_.border_color, false);
        const std::string R = "\x1b[0m";
        const std::string bar = border + settings_.box_vertical + R;
        std::vector<std::string> lines;
        lines.push_back(box_top("NEW FOLDER", PW, border));
        lines.push_back(bar + " " + ansi_for(settings_.header_color, false) + pad_right("The name of the folder:", inner) + R + " " + bar);
        lines.push_back(bar + " " + pad_right(truncate_str(mdb_new_name_ + "\u2588", inner), inner) + " " + bar);
        lines.push_back(box_bottom(PW, mdb_new_add_ ? "[ENTER] make it, add the preset  [ESC] cancel" : "[ENTER] make it  [ESC] cancel", ansi_for(settings_.border_color_bottom, false)));
        draw_floating_panel(f, lines, PW, W);
    } else if (!mdb_del_ask_.empty()) {
        const int PW = 56, inner = PW - 4;
        const std::string border = ansi_for(settings_.border_color, false);
        const std::string R = "\x1b[0m";
        const std::string bar = border + settings_.box_vertical + R;
        const std::string head = ansi_for(settings_.header_color, false);
        std::vector<std::string> lines;
        lines.push_back(box_top("DELETE FOLDER?", PW, border));
        lines.push_back(bar + " " + head + pad_right(truncate_str("\"" + mdb_del_ask_ + "\"", inner), inner) + R + " " + bar);
        const size_t cnt = mdf::load(md_config_dir(), mdb_del_ask_).size();
        std::vector<std::string> txt = wrap_lines("The folder and its list of " + std::to_string(cnt) + " preset" + (cnt == 1 ? "" : "s") + " are deleted. The presets themselves stay where they are. SHIFT+u brings the folder back.", inner, 6);
        for (const std::string& t : txt) lines.push_back(bar + " " + pad_right(t, inner) + " " + bar);
        lines.push_back(box_bottom(PW, "[ENTER/y] delete  [ESC/n] keep", ansi_for(settings_.border_color_bottom, false)));
        draw_floating_panel(f, lines, PW, W);
    } else if (mdb_pick_open_) {
        draw_floating_panel(f, mdb_pick_panel(), 44, W);
    }
    return f.str();
}

// The preset overlay over the whole screen: the folder on the left, the visuals (the preview) on the right.
std::string App::build_mdb_screen(int W, const char* clear_prefix) const {
    const int H = std::max(6, term_rows_ - 1);
    const int ph = H - 2;   // (the box; its bottom border carries the first row of commands, the second row stands below it)
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
    // the left column: where we are, then the list
    const int inner = left_w - 2;
    std::vector<std::string> left;
    {
        std::string where;
        if (mdb_searching_) {
            where = "Search: " + mdb_query_ + (mdb_typing_ ? "\u2588" : "");
            const std::string cnt = mdb_query_.empty() ? std::string() : "  (" + std::to_string(mdb_items_.size()) + (mdb_items_.size() >= 2000 ? "+" : "") + ")";
            left.push_back(" " + search_idle_sgr(settings_) + pad_right(truncate_str(where + cnt, inner), inner) + "\x1b[0m ");
            left.push_back(std::string(static_cast<size_t>(left_w), ' '));
        } else if (mdb_in_favs_) where = "Favourites";
        else if (!mdb_in_user_.empty()) where = "Folder: " + mdb_in_user_;
        else if (mdb_in_recs_) where = "Recommended";
        else {
            fs::path rel = path_from_utf8(mdb_dir_).lexically_relative(path_from_utf8(mdb_root_));
            where = "Presets";
            for (const std::string& x : Milkdrop::instance().extra_preset_dirs()) {   // inside a further folder: its name, then the way in it
                const fs::path xr = path_from_utf8(x), r2 = path_from_utf8(mdb_dir_).lexically_relative(xr);
                if (!r2.empty() && *r2.begin() != "..") { where += " / " + path_utf8(xr.filename()); rel = r2; break; }
            }
            if (!rel.empty() && rel != "." && *rel.begin() != "..") for (const auto& part : rel) where += " / " + path_utf8(part);
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
            case 5: text = "\u2714 Recommended (" + it.path + ")"; sgr = "\x1b[1m" + ansi_for(settings_.header_color, false); break;
            case 6: text = "\u25a4 " + it.name + " (" + it.path + ")"; sgr = "\x1b[1m" + ansi_for(settings_.header_color, false); break;
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
        if (k == mdb_sel_) left.push_back(" " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + body + "\x1b[0m ");
        else if (it.kind == 3 && it.path == shown) left.push_back(" " + ansi_for(settings_.list_playing_color) + body + "\x1b[0m ");
        else left.push_back(" " + sgr + body + "\x1b[0m ");
    }
    std::ostringstream f;
    f << clear_prefix << "\x1b[?25l";
    const std::string count = !mdb_in_user_.empty() ? std::to_string(mdb_items_.size() > 0 ? mdb_items_.size() - 1 : 0) + " in " + mdb_in_user_ : mdb_in_recs_ ? std::to_string(mdb_items_.size() > 0 ? mdb_items_.size() - 1 : 0) + " recommended" : mdb_in_favs_ ? std::to_string(md_favs_.size()) + " favourites" : std::to_string(Milkdrop::instance().preset_count()) + " presets";
    const std::string fx_now = milkdrop_look().fx == 0 ? std::string("off") : std::string(video_fx_name(milkdrop_look().fx));
    f << box_top(!mdb_track_.empty() ? "MILKDROP FOR THIS TRACK: " + truncate_str(mdb_track_title_, std::max(10, W - 40))
                 : mdb_default_ ? "MILKDROP DEFAULT PRESET  " + count : "MILKDROP PRESETS  " + count + "  |  effect: " + fx_now, W, border) << "\n";
    for (int r = 0; r < ph; ++r) {
        const std::string l = r < static_cast<int>(left.size()) ? left[static_cast<size_t>(r)] : std::string(static_cast<size_t>(left_w), ' ');
        f << bar << l;
        if (pw > 0) {
            const std::string p = r < static_cast<int>(pic.size()) ? pic[static_cast<size_t>(r)] : std::string(static_cast<size_t>(pw), ' ');
            f << bar << std::string(static_cast<size_t>(mg), ' ') << p << "\x1b[0m" << std::string(static_cast<size_t>(mg), ' ');
        }
        f << bar << "\n";
    }
    // The commands: two centred rows under the box
    std::string leg1, leg2;
    if (mdb_typing_) { leg1 = "type to search (fuzzy)  [\u2191\u2193] select"; leg2 = "[ENTER] to the list  [ESC] clear"; }
    else if (mdb_searching_) { leg1 = "[\u2191\u2193] select  [ENTER] choose  [a] add to favourites  [SHIFT+a] add to a folder"; leg2 = "[f] favourites  [/] search  [ESC/\u2190] clear"; }
    else if (mdb_in_favs_) { leg1 = "[\u2191\u2193] select  [ENTER] choose  [d] remove from favourites  [SHIFT+u] undo remove"; leg2 = "[SHIFT+a] add to a folder  [/] search  [e] effect  [\u2190] back  [ESC] close"; }
    else if (!mdb_in_user_.empty()) { leg1 = "[\u2191\u2193] select  [ENTER] choose  [d] remove from this folder  [SHIFT+u] undo remove"; leg2 = "[f] favourites  [SHIFT+a] add to another folder  [/] search  [e] effect  [\u2190] back  [ESC] close"; }
    else if (!mdb_track_.empty()) { leg1 = "[ENTER] this track's preset  [f] favourites  [a] add to favourites  [SHIFT+a] add to a folder"; leg2 = "[/] search  [e] effect  [g/h] change  [\u2190] up  [ESC] back"; }
    else if (mdb_default_) { leg1 = "[ENTER] the default preset  [f] favourites  [a] add to favourites  [SHIFT+a] add to a folder"; leg2 = "[/] search  [e] effect  [g/h] change  [\u2190] up  [ESC] back"; }
    else { leg1 = "[ENTER] choose / open  [f] favourites  [a] add to favourites  [SHIFT+a] add to a folder  [n] new folder"; leg2 = "[d] delete folder  [SHIFT+u] undo delete  [/] search  [e] effect  [\u2190] up  [ESC] close"; }
    // like the big video overlay: row 1 sits ON the bottom border line (centred), row 2 below the box in the otherwise empty last terminal row
    f << centred_bottom(settings_, W, truncate_str(leg1, W - 6), border_bottom.empty() ? border : border_bottom);
    std::string out = clamp_output_rows(f.str(), term_rows_);   // (the clamp would cut a row after the box: row 2 is added behind it)
    if (term_rows_ > H) {
        const std::string l2 = truncate_str(leg2, W - 2);
        out += "\n" + std::string(static_cast<size_t>(std::max(0, (W - display_width(l2)) / 2)), ' ') + (border_bottom.empty() ? border : border_bottom) + l2 + "\x1b[0m";   // (no line break after it: the terminal must not scroll)
    }
    return out;
}

} // namespace muisc
