// READ LYRICS overlay (stored lyrics versions, translations, search). Split out of app.cpp.
// Member functions of App (declared in app.h); the helpers shared with app.cpp are in app_internal.h.
#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "keyboard_layout.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "console_log.h"
#include "karaoke_font.h"
#include "glyph_raster.h"
#include "chiptune.h"
#include "scope_big.h"
#include "scope_window.h"
#include "wizard.h"
#include "creator.h"
#include "mic_capture.h"
#include "process_util.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <sys/types.h>
#include <sys/stat.h>   // file_added_time(): stat() / _wstat64()
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "app_internal.h"

namespace muisc {

void App::readlyrics_open() {
    fs::path track;
    std::string title;
    if (has_track_ && !current_path_.empty()) {
        track = current_path_;
        title = (metadata_.artist == "-" || metadata_.artist.empty()) ? metadata_.name : metadata_.artist + " - " + metadata_.name;
    } else if (list_source_ == ListSource::Local && selected_ >= 0 && selected_ < static_cast<int>(local_view_.size())) {
        const LocalTrack& t = local_view_[static_cast<size_t>(selected_)];
        track = t.path;
        title = t.folder_artist.empty() ? t.title : t.folder_artist + " - " + t.title;
    }
    if (track.empty()) { status_line_ = "no track to read lyrics of -- play or hover a track first"; return; }
    rd_track_ = track;
    rd_title_ = title.empty() ? path_utf8(track.stem()) : title;
    rd_versions_ = list_lyrics_versions(track);
    rd_sel_ = 0;
    for (size_t i = 0; i < rd_versions_.size(); ++i) if (rd_versions_[i].active) { rd_sel_ = static_cast<int>(i); break; }
    rd_first_ = 0;
    rd_pane_ = 0;
    rd_confirm_del_ = false;
    rd_opts_open_ = false;
    rd_search_open_ = false;
    rd_opts_warn_.clear();
    rd_status_.clear();
    rd_status_warn_ = false;
    if (rd_busy_) { rd_status_ = "a translation is still running (" + language_name(rd_job_to_) + " with " + translate_service_name(rd_job_svc_) + ")"; }
    readlyrics_load_version();
    mode_ = Mode::ReadLyrics;
    force_redraw_ = true;   // full-screen takeover
}

void App::readlyrics_close() {
    if (rd_opts_dirty_) { save_settings(settings_); rd_opts_dirty_ = false; }
    rd_opts_open_ = false;
    rd_search_open_ = false;
    rd_confirm_del_ = false;
    // A translation that is still running is NOT stopped: it is saved when done (readlyrics_poll() tells).
    mode_ = Mode::Browse;
    force_redraw_ = true;
}

void App::readlyrics_load_version() {
    rd_lines_.clear();
    rd_trans_.clear();
    rd_tlist_.clear();
    rd_from_.clear();
    rd_show_ = rd_same_ = false;
    rd_scroll_ = 0;
    rd_tsel_ = rd_tfirst_ = 0;
    rd_confirm_del_ = false;
    if (rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) return;
    const LyricsVersion& v = rd_versions_[static_cast<size_t>(rd_sel_)];
    rd_lines_ = lyrics_plain_lines(read_lyrics_version_text(rd_track_, v));
    readlyrics_refresh_translations(settings_.translate_to);
}

// Reads the saved translations of the chosen file again; `prefer_to` is the language to select when it is there.
void App::readlyrics_refresh_translations(const std::string& prefer_to) {
    rd_tlist_.clear();
    rd_same_ = false;
    if (rd_lines_.empty() || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) { readlyrics_select_translation(-1); return; }
    const LyricsVersion& v = rd_versions_[static_cast<size_t>(rd_sel_)];
    rd_tlist_ = list_lyrics_translations(rd_track_, v, rd_lines_);
    int pick = rd_tlist_.empty() ? -1 : 0;
    for (size_t i = 0; i < rd_tlist_.size(); ++i) if (rd_tlist_[i].to == prefer_to) { pick = static_cast<int>(i); break; }
    readlyrics_select_translation(pick);
    LyricsTranslation note;   // "these lyrics are in <language> already", saved by an earlier try
    if (load_lyrics_translation(rd_track_, v, settings_.translate_to, rd_lines_, note) && note.same) rd_same_ = true;
}

void App::readlyrics_select_translation(int index) {
    rd_trans_.clear();
    rd_from_.clear();
    rd_to_.clear();
    rd_svc_.clear();
    if (index < 0 || index >= static_cast<int>(rd_tlist_.size()) || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) {
        rd_tsel_ = 0;
        return;
    }
    rd_tsel_ = index;
    const TranslationInfo& info = rd_tlist_[static_cast<size_t>(index)];
    LyricsTranslation t;
    if (load_lyrics_translation(rd_track_, rd_versions_[static_cast<size_t>(rd_sel_)], info.to, rd_lines_, t) && !t.same) {
        rd_trans_ = std::move(t.lines);
        rd_from_ = t.from;
        rd_svc_ = t.service;
        rd_to_ = info.to;
    }
}

// SHIFT+t: hidden -> the chosen translation -> the next one ... -> hidden. Never fetches anything.
void App::readlyrics_toggle_translations() {
    rd_status_warn_ = false;
    if (rd_tlist_.empty()) {
        rd_status_ = rd_busy_ ? "no translation yet -- the running one is not finished" : "no translation saved for this file -- [t] fetches one";
        rd_status_warn_ = true;
        return;
    }
    const int n = static_cast<int>(rd_tlist_.size());
    if (!rd_show_) {
        rd_show_ = true;
    } else if (rd_tsel_ < n - 1) {
        readlyrics_select_translation(rd_tsel_ + 1);
    } else {
        rd_show_ = false;
        readlyrics_select_translation(0);
    }
    if (rd_show_) {
        const TranslationInfo& t = rd_tlist_[static_cast<size_t>(rd_tsel_)];
        rd_status_ = "showing the " + language_name(t.to) + " translation" + (n > 1 ? " (" + std::to_string(rd_tsel_ + 1) + "/" + std::to_string(n) + ", SHIFT+t for the next)" : std::string());
    } else {
        rd_status_ = "translations hidden";
    }
}

void App::readlyrics_delete_translation() {
    rd_confirm_del_ = false;
    if (rd_tsel_ < 0 || rd_tsel_ >= static_cast<int>(rd_tlist_.size()) || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) return;
    const std::string to = rd_tlist_[static_cast<size_t>(rd_tsel_)].to;
    const bool ok = delete_lyrics_translation(rd_track_, rd_versions_[static_cast<size_t>(rd_sel_)], to);
    const int keep = rd_tsel_;
    readlyrics_refresh_translations("");
    readlyrics_select_translation(rd_tlist_.empty() ? -1 : std::min(keep, static_cast<int>(rd_tlist_.size()) - 1));
    if (rd_tlist_.empty()) rd_show_ = false;
    rd_status_warn_ = !ok;
    rd_status_ = ok ? "deleted the " + language_name(to) + " translation" : "could not delete the " + language_name(to) + " translation";
}

// ENTER in the translate overlay: fetch a translation with the service and languages set there.
void App::readlyrics_translate() {
    rd_opts_warn_.clear();
    if (rd_busy_) {
        rd_opts_warn_ = "WARNING: the last translation is not finished yet -- please wait";
        return;
    }
    if (rd_lines_.empty() || rd_sel_ < 0 || rd_sel_ >= static_cast<int>(rd_versions_.size())) {
        rd_opts_warn_ = "there are no lyrics to translate";
        return;
    }
    TranslateRequest req;
    req.service = settings_.translate_service;
    req.from = settings_.translate_from;
    req.to = settings_.translate_to;
    rd_opts_open_ = false;
    if (rd_opts_dirty_) { save_settings(settings_); rd_opts_dirty_ = false; }
    rd_busy_ = true;
    rd_job_to_ = req.to;
    rd_job_svc_ = req.service;
    rd_job_track_ = rd_track_;
    rd_job_version_ = rd_versions_[static_cast<size_t>(rd_sel_)];
    rd_status_warn_ = false;
    rd_status_ = "translating into " + language_name(req.to) + " with " + translate_service_name(req.service) +
                 " ... this takes a while (needs an internet connection); you can go on reading";
    const std::vector<std::string> plain = rd_lines_;
    const LyricsVersion v = rd_job_version_;
    const fs::path track = rd_track_, script = translate_script_;
    spawn_bg("lyrics translation", [this, plain, v, track, script, req]() {
        LyricsTranslation t = translate_lyrics_lines(plain, req, script);
        if (t.ok) save_lyrics_translation(track, v, req.to, plain, t);
        std::lock_guard<std::mutex> lk(rd_mutex_);
        rd_result_ = std::move(t);
        rd_result_ready_ = true;
    });
}

void App::readlyrics_poll() {
    LyricsTranslation t;
    {
        std::lock_guard<std::mutex> lk(rd_mutex_);   // (the flag is written by the worker thread under this lock: never read it without)
        if (!rd_result_ready_) return;
        t = std::move(rd_result_);
        rd_result_ = LyricsTranslation{};
        rd_result_ready_ = false;
    }
    rd_busy_ = false;
    const std::string what = language_name(rd_job_to_);
    // Is the file the translation was made for the one on the screen?
    const bool on_screen = mode_ == Mode::ReadLyrics && rd_sel_ >= 0 && rd_sel_ < static_cast<int>(rd_versions_.size()) &&
        lyrics_translation_file(rd_track_, rd_versions_[static_cast<size_t>(rd_sel_)], "xx") == lyrics_translation_file(rd_job_track_, rd_job_version_, "xx");
    std::string msg;
    bool warn = false;
    if (!t.ok) { msg = "translation into " + what + " failed: " + t.error; warn = true; }
    else if (t.same) msg = "these lyrics are in " + what + " already -- nothing to translate ([t] changes the languages)";
    else msg = "translated from " + language_name(t.from) + " to " + what + " with " + translate_service_name(rd_job_svc_) + " -- saved in the lyrics folder (translations)";
    if (on_screen) {
        if (t.ok) {
            readlyrics_refresh_translations(rd_job_to_);
            if (!t.same && !rd_tlist_.empty()) rd_show_ = true;
            if (t.same) rd_same_ = true;
        }
        rd_status_ = msg;
        rd_status_warn_ = warn;
    } else {
        status_line_ = "lyrics: " + msg;      // READ LYRICS is closed (or shows another file): tell on the main screen
        if (mode_ == Mode::ReadLyrics) { rd_status_ = msg; rd_status_warn_ = warn; }
    }
}

// The translate overlay ([t]): UP / DOWN pick the row, LEFT / RIGHT change it (a letter jumps to the next language
// starting with it), ENTER fetches the translation, ESC closes it. A running translation is not touched by any of it.
void App::readlyrics_options_key(int key) {
    const bool arrow = last_key_was_arrow();
    auto close_panel = [&]() {
        rd_opts_open_ = false;
        rd_opts_warn_.clear();
        if (rd_opts_dirty_) { save_settings(settings_); rd_opts_dirty_ = false; }
    };
    if (key == 27 || key_is("HKeyReadLyrics", key)) { close_panel(); return; }
    if (key == '\r' || key == '\n') { readlyrics_translate(); return; }
    if (arrow && key == 'A') { rd_opts_row_ = std::max(0, rd_opts_row_ - 1); return; }
    if (arrow && key == 'B') { rd_opts_row_ = std::min(2, rd_opts_row_ + 1); return; }
    // the value lists: 0 services, 1 target languages, 2 source languages ("auto" first)
    std::vector<std::string> ids;
    std::vector<std::string> names;
    std::string* cur = nullptr;
    if (rd_opts_row_ == 0) {
        for (const auto& s : translate_services()) { ids.push_back(s.first); names.push_back(s.second); }
        cur = &settings_.translate_service;
    } else {
        if (rd_opts_row_ == 2) { ids.push_back("auto"); names.push_back("Auto-detect"); }
        for (const auto& l : translate_languages()) { ids.push_back(l.first); names.push_back(l.second); }
        cur = rd_opts_row_ == 1 ? &settings_.translate_to : &settings_.translate_from;
    }
    int idx = 0;
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == *cur) { idx = static_cast<int>(i); break; }
    const int n = static_cast<int>(ids.size());
    int next = idx;
    if (arrow && (key == 'C' || key == 'D')) next = (idx + (key == 'C' ? 1 : n - 1)) % n;
    else if (rd_opts_row_ > 0 && std::isalpha(key) && key < 128) {   // jump to the next language that starts with the letter
        const char want = static_cast<char>(std::tolower(key));
        for (int k = 1; k <= n; ++k) {
            const int i = (idx + k) % n;
            if (!names[static_cast<size_t>(i)].empty() && std::tolower(static_cast<unsigned char>(names[static_cast<size_t>(i)][0])) == want) { next = i; break; }
        }
    } else return;
    if (next == idx) return;
    *cur = ids[static_cast<size_t>(next)];
    rd_opts_dirty_ = true;
    rd_opts_warn_.clear();
    rd_same_ = false;
}

void App::handle_readlyrics_key(int key) {
    if (key == 0) return;
    if (rd_opts_open_) { readlyrics_options_key(key); return; }
    if (rd_search_open_) { readlyrics_search_key(key); return; }
    const bool arrow = last_key_was_arrow();
    if (!arrow && key_is("HKeyTranslit", key)) { toggle_latin_letters(); return; }   // Latin letters on / off, also here
    if (key == kKeyCtrlS && !rd_confirm_del_) { readlyrics_search_open(); return; }
    if (rd_confirm_del_) {   // "delete this translation?" -- only y confirms
        if (!arrow && (key == 'y' || key == 'Y')) readlyrics_delete_translation();
        else { rd_confirm_del_ = false; rd_status_ = "kept"; rd_status_warn_ = false; }
        return;
    }
    if (key == 27 || key_is("HKeyReadLyrics", key) || key == 'q' || key == 'Q') { readlyrics_close(); return; }
    if (!arrow && key == '\t') { rd_pane_ = rd_pane_ == 0 ? 1 : 0; rd_status_.clear(); return; }
    if (!arrow && key == 't') { rd_opts_open_ = true; rd_opts_row_ = 0; rd_opts_warn_.clear(); return; }
    if (!arrow && key == 'T') { readlyrics_toggle_translations(); return; }
    const int n = static_cast<int>(rd_versions_.size());
    // LEFT / RIGHT choose in the pane that has the keys ('D' is Left, 'C' is Right)
    if (arrow && (key == 'D' || key == 'C')) {
        const int dir = key == 'D' ? -1 : 1;
        if (rd_pane_ == 0) {
            const int ns = std::clamp(rd_sel_ + dir, 0, std::max(0, n - 1));
            if (ns != rd_sel_) { rd_sel_ = ns; rd_status_.clear(); readlyrics_load_version(); }
        } else if (!rd_tlist_.empty()) {
            const int nt = static_cast<int>(rd_tlist_.size());
            const int ns = std::clamp(rd_tsel_ + dir, 0, nt - 1);
            if (ns != rd_tsel_ || !rd_show_) { readlyrics_select_translation(ns); rd_show_ = true; rd_status_.clear(); }
        }
        return;
    }
    if (rd_pane_ == 1 && (key == kKeyDelete || (!arrow && (key == 'd' || key == 'D')))) {
        if (rd_tlist_.empty()) { rd_status_ = "there is no translation to delete"; rd_status_warn_ = true; }
        else rd_confirm_del_ = true;
        return;
    }
    if (arrow && last_key_was_shifted() && (key == 'A' || key == 'B')) {   // SHIFT+up / down: a page
        const int page = std::max(1, term_rows_ - 14);
        rd_scroll_ = key == 'A' ? std::max(0, rd_scroll_ - page) : rd_scroll_ + page;
        return;
    }
    if (arrow && key == 'A') { if (rd_scroll_ > 0) --rd_scroll_; return; }
    if (arrow && key == 'B') { ++rd_scroll_; return; }          // clamped against the real content while drawing
    if (key == ' ') { rd_scroll_ += std::max(1, term_rows_ - 14); return; }
    if (key == 'b' || key == 'B') { rd_scroll_ = std::max(0, rd_scroll_ - std::max(1, term_rows_ - 14)); return; }
    if (key == kKeyHome) { rd_scroll_ = 0; return; }
    if (key == kKeyEnd) { rd_scroll_ = 1 << 20; return; }       // clamped to the last page while drawing
}

// --- CTRL+s: search the lyrics files ------------------------------------------------------------------------------
// Lyrics live in a "lyrics" folder inside the folder of each track (and of the library folders / the download cache),
// so those are the folders that are searched: one listing per distinct folder, done when the search is opened.
void App::readlyrics_search_open() {
    std::set<std::string> dirs;
    auto add_dir = [&](const fs::path& folder) { if (!folder.empty()) dirs.insert(path_utf8(folder / "lyrics")); };
    for (const auto& t : all_local_tracks_) add_dir(t.path.parent_path());
    for (const auto& p : settings_.local_music_paths) add_dir(path_from_utf8(p));
    add_dir(rd_track_.parent_path());
    rd_search_all_.clear();
    rd_search_dirs_ = 0;
    for (const auto& d : dirs) {
        std::error_code ec;
        const fs::path dir = path_from_utf8(d);
        if (!fs::is_directory(dir, ec)) continue;
        ++rd_search_dirs_;
        const std::string folder = path_utf8(dir.parent_path().filename());
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            const std::string ext = ascii_lower_str(path_utf8(it->path().extension()));
            if (ext != ".lrc" && ext != ".txt") continue;
            LyricsFileHit h;
            h.path = it->path();
            h.name = path_utf8(it->path().stem());
            h.lname = ascii_lower_str(h.name);
            h.folder = folder;
            rd_search_all_.push_back(std::move(h));
            if (rd_search_all_.size() >= 200000) break;
        }
    }
    std::sort(rd_search_all_.begin(), rd_search_all_.end(), [](const LyricsFileHit& a, const LyricsFileHit& b) {
        return a.lname != b.lname ? a.lname < b.lname : a.folder < b.folder;
    });
    rd_search_q_.clear();
    rd_search_sel_ = rd_search_first_ = 0;
    readlyrics_search_rebuild();
    rd_search_open_ = true;
    rd_status_.clear();
}

// Every word of the query has to be somewhere in the file name (any order, case-insensitive for ASCII letters).
void App::readlyrics_search_rebuild() {
    std::vector<std::string> words;
    {
        std::istringstream iss(ascii_lower_str(rd_search_q_));
        std::string w;
        while (iss >> w) words.push_back(w);
    }
    rd_search_view_.clear();
    for (size_t i = 0; i < rd_search_all_.size(); ++i) {
        bool ok = true;
        for (const auto& w : words) if (rd_search_all_[i].lname.find(w) == std::string::npos) { ok = false; break; }
        if (ok) rd_search_view_.push_back(static_cast<int>(i));
    }
    const int n = static_cast<int>(rd_search_view_.size());
    rd_search_sel_ = std::clamp(rd_search_sel_, 0, std::max(0, n - 1));
    rd_search_first_ = std::clamp(rd_search_first_, 0, std::max(0, n - 1));
}

// Makes the reader show one lyrics file. It is handed over as the only "version" of a track that sits in the folder
// above the lyrics folder: that is what makes the translations of this file land in (and come from) the folder the file
// is in, exactly as for a real track (the track file itself is never opened).
void App::readlyrics_show_file(const LyricsFileHit& hit) {
    LyricsVersion v;
    v.kind = LyricsVersion::Kind::Fetched;
    v.file = hit.path;
    v.label = path_utf8(hit.path.filename());
    v.detail = hit.folder;
    v.active = true;
    rd_track_ = hit.path.parent_path().parent_path() / path_from_utf8(hit.name + ".audio");
    rd_title_ = hit.name;
    rd_versions_.assign(1, v);
    rd_sel_ = 0;
    rd_first_ = 0;
    rd_pane_ = 0;
    readlyrics_load_version();
    rd_status_ = "showing a lyrics file from the search -- CTRL+L closes the reader, open it again for the playing track";
    rd_status_warn_ = false;
}

void App::readlyrics_search_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(rd_search_view_.size());
    if (key == 27 || key == kKeyCtrlS) { rd_search_open_ = false; return; }
    if (key == '\r' || key == '\n') {
        if (n == 0) return;
        const LyricsFileHit hit = rd_search_all_[static_cast<size_t>(rd_search_view_[static_cast<size_t>(rd_search_sel_)])];
        rd_search_open_ = false;
        readlyrics_show_file(hit);
        return;
    }
    if (arrow && key == 'A') { rd_search_sel_ = std::max(0, rd_search_sel_ - 1); return; }
    if (arrow && key == 'B') { rd_search_sel_ = std::min(std::max(0, n - 1), rd_search_sel_ + 1); return; }
    if (arrow) return;
    if (key == kKeyHome) { rd_search_sel_ = 0; return; }
    if (key == kKeyEnd) { rd_search_sel_ = std::max(0, n - 1); return; }
    if (key == 127 || key == 8) { pop_utf8_char(rd_search_q_); rd_search_sel_ = rd_search_first_ = 0; readlyrics_search_rebuild(); return; }
    if (is_text_key(key) && rd_search_q_.size() < 100) {
        rd_search_q_ += static_cast<char>(key);
        rd_search_sel_ = rd_search_first_ = 0;
        readlyrics_search_rebuild();
    }
}

std::vector<std::string> App::build_readlyrics_search_panel() const {
    const int PW = 74, inner = PW - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    std::vector<std::string> out;
    out.push_back(box_top("SEARCH LYRICS FILES", PW, border));
    // the query, with a caret
    {
        const std::string pre = "Search: ";
        const int room = std::max(1, inner - display_width(pre) - 1);
        std::string shown = rd_search_q_;   // a long query keeps its END visible: cut from the front
        while (display_width(shown) > room && !shown.empty()) {
            size_t i = 1;
            while (i < shown.size() && (static_cast<unsigned char>(shown[i]) & 0xC0) == 0x80) ++i;
            shown.erase(0, i);
        }
        out.push_back(bar + " " + header_sgr(settings_) + pre + R + shown + "\x1b[7m \x1b[0m" +
                      std::string(static_cast<size_t>(std::max(0, inner - display_width(pre) - display_width(shown) - 1)), ' ') + " " + bar);
    }
    out.push_back(bar + " " + std::string(static_cast<size_t>(inner), ' ') + " " + bar);
    const int list_rows = std::clamp(term_rows_ - 12, 3, 14);
    const int n = static_cast<int>(rd_search_view_.size());
    int first = std::clamp(rd_search_first_, 0, std::max(0, n - 1));
    const int sel = std::clamp(rd_search_sel_, 0, std::max(0, n - 1));
    if (sel < first) first = sel;
    if (sel >= first + list_rows) first = sel - list_rows + 1;
    for (int r = 0; r < list_rows; ++r) {
        const int i = first + r;
        std::string painted;
        if (i < n) {
            const LyricsFileHit& h = rd_search_all_[static_cast<size_t>(rd_search_view_[static_cast<size_t>(i)])];
            const std::string tail = h.folder.empty() ? std::string() : "  (" + h.folder + ")";
            const int tail_w = std::min(display_width(tail), inner / 3);
            std::string left = truncate_str(h.name, std::max(5, inner - tail_w));
            std::string line = pad_right(left, inner - tail_w) + pad_right(truncate_str(tail, tail_w), tail_w);
            painted = i == sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + line + R : line;
        } else if (r == 0 && n == 0) {
            painted = pad_right(rd_search_all_.empty() ? "no lyrics files found in the lyrics folders of the library" : "no lyrics file matches", inner);
        } else {
            painted = std::string(static_cast<size_t>(inner), ' ');
        }
        out.push_back(bar + " " + painted + " " + bar);
    }
    out.push_back(bar + " " + std::string(static_cast<size_t>(inner), ' ') + " " + bar);
    const std::string info = std::to_string(n) + " of " + std::to_string(rd_search_all_.size()) + " files  in " + std::to_string(rd_search_dirs_) + (rd_search_dirs_ == 1 ? " folder" : " folders");
    out.push_back(bar + " " + legend_sgr(settings_) + pad_right(truncate_str(info, inner), inner) + R + " " + bar);
    out.push_back(box_bottom(PW, "[↑↓] Pick  [ENTER] Open  [ESC] Close", border_bottom));
    return out;
}

std::vector<std::string> App::build_readlyrics_options_panel() const {
    const int PW = 66, inner = PW - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    std::vector<std::string> out;
    out.push_back(box_top("TRANSLATE", PW, border));
    auto row = [&](const std::string& label, const std::string& value, bool sel) {
        const std::string lab = pad_right(label, 15);
        const std::string val = pad_right(truncate_str("< " + value + " >", inner - 15), inner - 15);
        out.push_back(bar + " " + header_sgr(settings_) + lab + R + (sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : std::string()) +
                      val + R + " " + bar);
    };
    auto plain = [&](const std::string& s, bool legend, const char* sgr = nullptr) {
        out.push_back(bar + " " + (sgr ? std::string(sgr) : legend ? legend_sgr(settings_) : std::string()) + pad_right(truncate_str(s, inner), inner) + R + " " + bar);
    };
    std::string from_name = settings_.translate_from == "auto" ? "Auto-detect" : language_name(settings_.translate_from);
    row("SERVICE", translate_service_name(settings_.translate_service), rd_opts_row_ == 0);
    row("TRANSLATE INTO", language_name(settings_.translate_to), rd_opts_row_ == 1);
    row("LYRICS ARE IN", from_name, rd_opts_row_ == 2);
    plain("", false);
    const std::string& svc = settings_.translate_service;
    plain(svc == "google" ? "Google Translate's key-less web endpoint (unofficial)."
                          : "MyMemory: free, about 5000 characters a day per address.", true);
    plain(settings_.translate_from == "auto" ? "If detection is wrong, set LYRICS ARE IN by hand." : "The language of the lyrics is set by hand.", true);
    if (!rd_opts_warn_.empty()) plain(rd_opts_warn_, false, "\x1b[31m");
    else if (rd_busy_) plain("Running: " + language_name(rd_job_to_) + " with " + translate_service_name(rd_job_svc_) + " (goes on in the background)", true);
    else plain("", false);
    out.push_back(box_bottom(PW, "[↑↓] Row  [←→] Change  [ENTER] Translate  [ESC] Close", border_bottom));
    return out;
}

void App::build_readlyrics_screen(std::ostringstream& frame, int W, int target_height) {
    if (W < 60) W = 60;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const int inner = W - 4;
    auto boxed = [&](const std::string& painted) { return bar + " " + painted + " " + bar; };

    // One line of choices that scrolls sideways: the lyrics files / the translations. `sel` is highlighted.
    auto chooser = [&](const std::vector<std::string>& items, int& sel, int& first) {
        const int n = static_cast<int>(items.size());
        const int avail = std::max(10, inner - 4);   // the two 2-cell scroll marks
        const std::string sep_txt = " " + (settings_.list_separator.empty() ? std::string("|") : settings_.list_separator) + " ";
        const int sw = display_width(sep_txt);
        auto iw = [&](int i) { return std::min(avail, display_width(items[static_cast<size_t>(i)])); };
        sel = std::clamp(sel, 0, n - 1);
        first = std::clamp(first, 0, n - 1);
        if (sel < first) first = sel;
        auto span_to_sel = [&](int f) { int w = 0; for (int i = f; i <= sel; ++i) w += iw(i) + (i > f ? sw : 0); return w; };
        while (first < sel && span_to_sel(first) > avail) ++first;
        std::vector<int> shown;
        int used = 0;
        for (int i = first; i < n; ++i) {
            const int add = iw(i) + (i > first ? sw : 0);
            if (used + add > avail) break;
            used += add;
            shown.push_back(i);
        }
        std::string mid;
        for (size_t k = 0; k < shown.size(); ++k) {
            const int i = shown[k];
            if (k > 0) mid += border + sep_txt + R;
            const std::string txt = truncate_str(items[static_cast<size_t>(i)], avail);
            mid += (i == sel) ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + txt + R : txt;
        }
        mid += std::string(static_cast<size_t>(std::max(0, avail - used)), ' ');
        const bool more_left = first > 0;
        const bool more_right = !shown.empty() && shown.back() < n - 1;
        return border + (more_left ? "◀ " : "  ") + R + mid + border + (more_right ? " ▶" : "  ") + R;
    };

    // --- top pane 1: ONE line with the lyrics files of the track ---
    frame << box_top("READ LYRICS" + (rd_pane_ == 0 ? focus_indicator(settings_, border) : std::string()), W, border) << "\n";
    {
        std::string line;
        if (rd_versions_.empty()) {
            line = pad_right("no lyrics saved for this track yet -- they are fetched while it plays", inner);
        } else {
            std::vector<std::string> items;
            for (const auto& v : rd_versions_) {
                std::string t = (v.active ? "● " : "") + v.label;
                if (!v.detail.empty()) t += " (" + v.detail + ")";
                items.push_back(t);
            }
            line = chooser(items, rd_sel_, rd_first_);
        }
        frame << boxed(line) << "\n";
    }
    frame << box_bottom(W, n_files_label(rd_versions_.size()), border_bottom) << "\n";

    // --- top pane 2: ONE line with the translations saved for the chosen file ---
    const bool show = rd_show_ && !rd_tlist_.empty() && rd_trans_.size() == rd_lines_.size();
    frame << box_top("TRANSLATION" + (rd_pane_ == 1 ? focus_indicator(settings_, border) : std::string()), W, border) << "\n";
    {
        std::string line;
        if (rd_tlist_.empty()) {
            line = pad_right(rd_lines_.empty() ? "nothing to translate" : "no translation yet -- [t] opens the translate overlay", inner);
        } else {
            std::vector<std::string> items;
            for (size_t i = 0; i < rd_tlist_.size(); ++i) {
                const TranslationInfo& t = rd_tlist_[i];
                items.push_back(std::string(show && static_cast<int>(i) == rd_tsel_ ? "● " : "") + language_name(t.to) +
                                (t.service.empty() ? std::string() : " (" + translate_service_name(t.service) + ")"));
            }
            line = chooser(items, rd_tsel_, rd_tfirst_);
        }
        frame << boxed(line) << "\n";
    }
    {
        std::string label;
        if (rd_busy_) label = "translating into " + language_name(rd_job_to_) + " with " + translate_service_name(rd_job_svc_) + " ...";
        else if (!rd_tlist_.empty()) label = std::to_string(rd_tlist_.size()) + (rd_tlist_.size() == 1 ? " translation" : " translations") + (show ? "" : " (hidden)");
        frame << box_bottom(W, label, border_bottom) << "\n";
    }

    // --- footer first (the lyrics pane takes the rows it leaves) ---
    // The legend is a small tree table (Move / Translate / Session), three rows whatever the width; the same in both panes
    // (TAB only moves the focus indicator): the keys that act on the focused pane say so in one entry.
    std::vector<std::string> hint_lines;
    if (rd_confirm_del_ && rd_tsel_ >= 0 && rd_tsel_ < static_cast<int>(rd_tlist_.size())) {
        hint_lines.push_back(legend_sgr(settings_) + truncate_str(" Delete the " + language_name(rd_tlist_[static_cast<size_t>(rd_tsel_)].to) + " translation?  [y] Yes | [any other key] No", W) + "\x1b[0m");
        hint_lines.push_back(std::string());
        hint_lines.push_back(std::string());
    } else {
        std::vector<LegendGroup> groups = {
            {"Move", {"[\u2190\u2192] Choose file/Translation\001Choose", "[TAB] Focus", "[\u2191\u2193] Scroll", "[SHIFT+\u2191\u2193] Page up/down\001Page", "[HOME/END] Top/Bottom"}},
            {"Translate", {"[t] Translate", "[SHIFT+t] Show/Hide translations\001Show/Hide", "[d/DEL] Delete translation\001Delete"}},
            {"Session", {"[CTRL+s] Search", "[ESC] Close"}},
        };
        const auto lrows = layout_legend_tree(groups, W - 1);
        for (size_t i = 0; i < lrows.size(); ++i) hint_lines.push_back(" " + paint_legend(settings_, lrows, i, border, 0, true));
    }
    const int legend_rows = static_cast<int>(hint_lines.size());
    const int status_rows = rd_status_.empty() ? 0 : 1;
    const int budget = std::min(target_height, term_rows_ - 1);
    const int body = std::max(3, budget - 6 /*two top panes*/ - 2 /*lyrics pane borders*/ - legend_rows - status_rows);
    // two blank rows above and below the lyrics (none on a terminal too short for them)
    const int margin = body >= 10 ? 2 : 0;
    const int text_rows = body - 2 * margin;
    const int wrap_w = std::max(10, inner - 8);   // a little air left and right: the lines are centred

    // --- lyrics pane ---
    struct Row { std::string text; int style; };   // 0 original, 1 translation, 2 blank
    std::vector<Row> rows;
    for (size_t i = 0; i < rd_lines_.size(); ++i) {
        if (rd_lines_[i].empty()) { rows.push_back({std::string(), 2}); continue; }
        for (auto& w : wrap_lines(shown_text(rd_lines_[i]), wrap_w, 8)) rows.push_back({w, 0});
        if (show) {
            const std::string& t = rd_trans_[i];
            if (!t.empty() && ascii_lower_str(t) != ascii_lower_str(rd_lines_[i]))
                for (auto& w : wrap_lines("(" + shown_text(t) + ")", wrap_w, 8)) rows.push_back({w, 1});
        }
    }
    if (rd_lines_.empty()) {
        rows.push_back({rd_versions_.empty() ? "no lyrics saved for this track" : "this file has no readable lyrics", 2});
    }
    const int max_scroll = std::max(0, static_cast<int>(rows.size()) - text_rows);
    rd_scroll_ = std::clamp(rd_scroll_, 0, max_scroll);

    std::string info;
    if (!rows.empty() && !rd_lines_.empty())
        info = "row " + std::to_string(std::min<int>(rd_scroll_ + text_rows, static_cast<int>(rows.size()))) + "/" + std::to_string(rows.size());
    const std::string sep2 = info.empty() ? "" : "  ";
    if (show) info += sep2 + "translation: " + language_name(rd_from_) + " > " + language_name(rd_to_) +
                      (rd_svc_.empty() ? std::string() : " (" + translate_service_name(rd_svc_) + ")");
    else if (rd_same_ && rd_tlist_.empty()) info += sep2 + "already " + language_name(settings_.translate_to);
    else if (!rd_tlist_.empty()) info += sep2 + "translation hidden -- [SHIFT+t] shows it";

    frame << box_top(truncate_str(rd_title_, std::max(10, W - 8)), W, border) << "\n";
    const std::string orig_sgr = ansi_for(settings_.active_line_color, false);
    const std::string trans_sgr = ansi_for(settings_.inactive_line_color, false);
    for (int k = 0; k < body; ++k) {
        const int idx = rd_scroll_ + k - margin;
        std::string painted;
        if (k >= margin && k < body - margin && idx >= 0 && idx < static_cast<int>(rows.size())) {
            const Row& r = rows[static_cast<size_t>(idx)];
            const std::string cut = truncate_str(r.text, inner);
            const int lead = std::max(0, (inner - display_width(cut)) / 2);   // centred
            const std::string t = pad_right(std::string(static_cast<size_t>(lead), ' ') + cut, inner);
            painted = r.style == 0 ? orig_sgr + t + R : r.style == 1 ? trans_sgr + t + R : t;
        } else {
            painted = std::string(static_cast<size_t>(inner), ' ');
        }
        frame << boxed(painted) << "\n";
    }
    frame << box_bottom(W, info, border_bottom) << "\n";

    for (const auto& l : hint_lines) frame << l << "\n";
    if (status_rows) frame << (rd_status_warn_ ? "\x1b[31m" : "\x1b[32m") << truncate_str(rd_status_, W) << "\x1b[0m\n";
}


} // namespace muisc
