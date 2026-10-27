// app_creator.cpp
// Part of class App (src/app.h): The Creator overlay (text or picture to oscilloscope music) and its preview.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "book_art.h"
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
#include "app_internal.h"
#include "update_check.h"
#include "open_url.h"
#include "app_settings_tables.h"
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
#include <unordered_map>
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

namespace muisc {


// ---------------------------------------------------------------------
// Creator (HKeyCreator = ^) -- see app.h. The sound is made by creator.h; this is the panel around it.
// ---------------------------------------------------------------------
namespace {
constexpr int kCreatorLens[5] = {5, 10, 20, 30, 60};
constexpr int kCreatorFps[4] = {30, 60, 90, 120};
constexpr const char* kCreatorMotionNames[4] = {"None", "Spin", "Tumble", "Pulse"};
constexpr const char* kCreatorDetailNames[3] = {"Low", "Medium", "High"};
constexpr const char* kCreatorReverbNames[3] = {"Off", "Light", "Hall"};
// The rows of the creator panel (the order on the screen depends on the creator mode, see creator_rows()).
enum CreatorRow { RMode, RText, RSound, RLen, RInvert, ROpt1, ROpt2, ROpt3, RGrid, RCreate, RFiles, RView, RStart };
constexpr const char* kCreatorViewNames[3] = {"Spectrogram", "Oscilloscope (XY)", "Oscilloscope (YT)"};
constexpr int kCreatorViewKind[3] = {1, 0, 2};   // preview kind of each view: 0 XY scope, 1 spectrogram, 2 YT scope
constexpr size_t kCreatorMaxRecordSeconds = 300;

}  // namespace

bool App::creator_load() {
    using namespace creator;
    const std::string key = "T:" + creator_text_;
    auto gray = std::static_pointer_cast<Gray>(creator_gray_);
    if (!gray || creator_loaded_key_.compare(0, key.size() + 1, key + "|") != 0) {
        auto g = std::make_shared<Gray>();
        std::string t = creator_text_;
        std::replace(t.begin(), t.end(), '|', '\n');
        *g = gray_from_text(t, 6);
        if (g->empty()) { creator_status_ = "Type some text first"; creator_gray_.reset(); creator_preview_.clear(); creator_loaded_key_.clear(); return false; }
        creator_gray_ = g;
        gray = g;
    }
    creator_loaded_key_ = key + "|n";
    creator_preview_ = braille_preview(*gray, 40, 6, 128, false);
    return true;
}

fs::path App::creator_dir() const {
    fs::path dir;
    const std::string cache_dir = path_utf8(cache_.cache_dir());
    for (const auto& p : settings_.local_music_paths)
        if (!p.empty() && p != cache_dir) { dir = path_from_utf8(p); break; }
    if (dir.empty()) {
        const char* home = std::getenv("HOME");
        if (!home) home = std::getenv("USERPROFILE");
        dir = fs::path(home ? home : ".") / "Music";
    }
    return dir / "creator";
}

bool App::creator_dir_prepare() {
    const fs::path dir = creator_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path marker = dir / kCreatorMarker;
    if (fs::exists(marker, ec)) return false;
    std::ofstream(marker) << "Mousiki Plus creator folder: the files in here are not part of the library; open them from the creator overlay (^), Created files.\n";
    return true;
}

void App::creator_list_refresh() {
    creator_files_.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(creator_dir(), ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".wav") creator_files_.push_back(e.path());
    }
    std::sort(creator_files_.begin(), creator_files_.end(), [](const fs::path& x, const fs::path& y) {
        std::error_code e1, e2;
        return fs::last_write_time(x, e1) > fs::last_write_time(y, e2);   // newest first
    });
    creator_list_sel_ = std::clamp(creator_list_sel_, 0, std::max(0, static_cast<int>(creator_files_.size()) - 1));
}

void App::creator_open_file(const fs::path& wav) {
    auto audio = std::make_shared<creator::Stereo>();
    if (!creator::read_wav16(path_utf8(wav), *audio)) { creator_status_ = "Could not read " + path_utf8(wav.filename()); return; }
    LocalTrack t;
    t.title = path_utf8(wav.stem());
    t.path = wav;
    t.folder_artist = "creator";
    mode_ = Mode::Browse;
    creator_remember_previous();
    start_local_track(t);
    creator_track_ = t;
    creator_audio_ = audio;
    creator_preview_kind_ = t.title.find("_spectrogram") != std::string::npos ? 1 : 0;   // as the creator names its files
    creator_preview_open();
}

void App::creator_render_and_play() {
    using namespace creator;
    if (!creator_load()) return;
    const auto gray = std::static_pointer_cast<Gray>(creator_gray_);
    const bool inv = creator_invert_ == 1 || (creator_invert_ == 0 && gray_is_mostly_bright(*gray));
    const double seconds = kCreatorLens[std::clamp(creator_len_, 0, 4)];
    Stereo audio;
    std::string name = creator_text_;
    if (creator_target_ == 0) {
        TraceOptions to;
        to.invert = inv;
        to.simplify = creator_detail_ == 0 ? 1.6f : creator_detail_ == 1 ? 0.8f : 0.4f;
        auto paths = trace_outlines(*gray, to);
        if (paths.empty()) { creator_status_ = "Nothing to draw: no clear shapes (try Invert)"; return; }
        XyOptions xo;
        xo.rate = 48000;
        xo.seconds = seconds;
        xo.fps = kCreatorFps[std::clamp(creator_fps_, 0, 3)];
        xo.motion = static_cast<Motion>(std::clamp(creator_motion_, 0, 3));
        fit_to_unit(paths);
        limit_length(paths, (xo.rate / xo.fps) * 0.03);
        paths = order_paths(std::move(paths));
        if (!render_xy(paths, xo, audio)) { creator_status_ = "Nothing to draw"; return; }
    } else {
        SpecOptions so;
        so.seconds = seconds;
        so.scale = static_cast<Scale>(std::clamp(creator_scale_, 0, 6));
        so.octaves = std::clamp(creator_octaves_, 3, 6);
        so.reverb = creator_reverb_ == 0 ? 0.0 : creator_reverb_ == 1 ? 0.25 : 0.5;
        so.invert = inv;
        if (!render_spectrogram_sound(*gray, so, audio)) { creator_status_ = "Silent: the picture has no bright part (try Invert)"; return; }
    }
    // <first music folder>/creator, or ~/Music/creator
    const fs::path dir = creator_dir();
    std::error_code ec;
    creator_dir_prepare();
    std::string base;
    for (size_t i = 0; i < name.size() && base.size() < 24;) {
        const size_t len = static_cast<size_t>(utf8_seq_len(static_cast<unsigned char>(name[i])));
        const std::string ch = name.substr(i, len);
        i += len;
        if (len == 1) base += (std::isalnum(static_cast<unsigned char>(ch[0])) ? ch[0] : '_');
        else base += ch;
    }
    if (base.empty()) base = "creation";
    base += creator_target_ == 0 ? "_scope" : "_spectrogram";
    fs::path out = dir / (base + ".wav");
    for (int n = 2; fs::exists(out, ec) && n < 1000; ++n) out = dir / (base + "_" + std::to_string(n) + ".wav");
    std::string err;
    if (!write_wav16(path_utf8(out), audio, &err)) { creator_status_ = "Could not save: " + err; return; }
    log_event("Creator: saved " + path_utf8(out));
    rescan_library();
    LocalTrack t;
    t.title = path_utf8(out.stem());
    t.path = out;
    t.folder_artist = "creator";
    mode_ = Mode::Browse;
    creator_remember_previous();
    start_local_track(t);
    creator_track_ = t;
    creator_audio_ = std::make_shared<Stereo>(std::move(audio));
    creator_preview_kind_ = creator_target_;
    creator_preview_open();
    status_line_ = std::string("Created ") + (creator_target_ == 0 ? "oscilloscope music" : "spectrogram sound") +
                   " (" + std::to_string(static_cast<int>(seconds)) + " s): " + path_utf8(out);
}

void App::creator_key(int key) {
    using namespace creator;
    const bool arrow = last_key_was_arrow();
    // The list of the created files
    if (creator_list_open_) {
        const int n = static_cast<int>(creator_files_.size());
        if (arrow && key == 'A') { if (n) creator_list_sel_ = (creator_list_sel_ + n - 1) % n; return; }
        if (arrow && key == 'B') { if (n) creator_list_sel_ = (creator_list_sel_ + 1) % n; return; }
        if (!arrow && (key == 27 || key == 127 || key == 8)) { creator_list_open_ = false; return; }
        if (!arrow && (key == '\r' || key == '\n') && n > 0) { creator_list_open_ = false; creator_open_file(creator_files_[static_cast<size_t>(creator_list_sel_)]); return; }
        return;   // SPACE and everything else do nothing
    }
    const std::vector<int> ids = creator_rows();
    const int rows = static_cast<int>(ids.size());
    creator_row_ = std::clamp(creator_row_, 0, rows - 1);
    const int id = ids[static_cast<size_t>(creator_row_)];
    const bool on_field = id == RText;
    auto cycle = [](int& v, int n, int dir) { v = ((v + dir) % n + n) % n; };
    // The three options of the sound depend on it: scope (frame rate, motion, detail) or spectrogram (scale, octaves, reverb).
    auto change = [&](int dir) {
        switch (id) {
            case RMode: cycle(creator_mode_, 2, dir); break;
            case RSound: cycle(creator_target_, 2, dir); break;
            case RLen: cycle(creator_len_, 5, dir); break;
            case RInvert: cycle(creator_invert_, 3, dir); break;
            case ROpt1: if (creator_target_ == 0) cycle(creator_fps_, 4, dir); else cycle(creator_scale_, 7, dir); break;
            case ROpt2: if (creator_target_ == 0) cycle(creator_motion_, 4, dir); else creator_octaves_ = std::clamp(creator_octaves_ + dir, 3, 6); break;
            case ROpt3: if (creator_target_ == 0) cycle(creator_detail_, 3, dir); else cycle(creator_reverb_, 3, dir); break;
            case RGrid: creator_grid_ = !creator_grid_; break;
            case RView: cycle(creator_live_view_, 3, dir); break;
            default: break;
        }
    };
    if (arrow && key == 'A') { creator_row_ = (creator_row_ + rows - 1) % rows; return; }
    if (arrow && key == 'B') { creator_row_ = (creator_row_ + 1) % rows; return; }
    if (!arrow && key == 9) { creator_row_ = (creator_row_ + 1) % rows; return; }   // TAB
    if (!arrow && key == 27) { mode_ = Mode::Browse; return; }
    if (on_field) {
        if (!arrow && (key == '\r' || key == '\n')) { creator_status_.clear(); if (creator_load()) creator_row_ = std::min(rows - 1, creator_row_ + 1); return; }
        // SPACE types a space in the text (needed for words); on every other row it does nothing
        edit_focus("creator-text", creator_text_);
        if (edit_text_key(creator_text_, edit_caret_, edit_anchor_, key, 80, &creator_status_)) {
            creator_status_.clear();
            creator_load();
        }
        return;
    }
    if (arrow && key == 'D') { change(-1); return; }
    if (arrow && key == 'C') { change(+1); return; }
    if (!arrow && (key == '\r' || key == '\n')) {
        if (id == RCreate) { creator_render_and_play(); return; }
        if (id == RStart) { creator_status_.clear(); creator_live_start(); return; }
        if (id == RFiles) { creator_list_refresh(); creator_list_open_ = true; creator_status_.clear(); return; }
        change(+1);
        return;
    }
    if (!arrow && key == ' ') return;   // SPACE is off here (a stray space on a German keyboard must not create anything)
    std::string act = resolve_hotkey_action(key);
    if (act == "HKeyCreator") mode_ = Mode::Browse;
}

std::vector<int> App::creator_rows() const {
    if (creator_mode_ == 1) {
        std::vector<int> v{RMode, RView};
        if (kCreatorViewKind[std::clamp(creator_live_view_, 0, 2)] != 1) v.push_back(RGrid);
        v.push_back(RStart);
        v.push_back(RFiles);
        return v;
    }
    std::vector<int> v{RMode, RText, RSound, RLen, RInvert, ROpt1, ROpt2, ROpt3};
    if (creator_target_ == 0) v.push_back(RGrid);
    v.push_back(RCreate);
    v.push_back(RFiles);
    return v;
}

bool App::creator_on_text_field() const {
    const std::vector<int> ids = creator_rows();
    return creator_mode_ == 0 && creator_row_ >= 0 && creator_row_ < static_cast<int>(ids.size()) && ids[static_cast<size_t>(creator_row_)] == RText;
}

std::vector<std::string> App::build_creator_panel() const {
    const int W = kCreatorPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    auto two = [&](bool cursor, const std::string& left, const std::string& right) {
        const std::string l = std::string(cursor ? "> " : "  ") + left;
        const int gap = std::max(1, inner - display_width(l) - display_width(right));
        return l + std::string(static_cast<size_t>(gap), ' ') + right;
    };
    constexpr int kTotalLines = 21;   // the panel keeps its height in both views
    std::vector<std::string> lines;
    if (creator_list_open_) {
        lines.push_back(box_top("Creator: created files", W, border));
        const int n = static_cast<int>(creator_files_.size());
        const int visible = kTotalLines - 4;
        const int first = std::clamp(creator_list_sel_ - visible / 2, 0, std::max(0, n - visible));
        for (int i = 0; i < visible; ++i) {
            const int k = first + i;
            if (k < n) lines.push_back(row(std::string(k == creator_list_sel_ ? "> " : "  ") + path_utf8(creator_files_[static_cast<size_t>(k)].filename()), k == creator_list_sel_));
            else lines.push_back(row(n == 0 && i == 0 ? "  (nothing created yet)" : "", false));
        }
        lines.push_back(row(n ? "Newest first. ENTER plays the file and opens its preview." : "Create something first.", false));
        lines.push_back(box_bottom(W, "[UP/DOWN] [ENTER] preview  [ESC] back", border_bottom));
        return lines;
    }
    const std::vector<int> ids = creator_rows();
    const int cur = std::clamp(creator_row_, 0, static_cast<int>(ids.size()) - 1);
    const bool mic = creator_mode_ == 1;
    const bool scope = creator_target_ == 0;
    lines.push_back(box_top("Creator", W, border));
    for (int idx = 0; idx < static_cast<int>(ids.size()); ++idx) {
        const int id = ids[static_cast<size_t>(idx)];
        const bool at = idx == cur;
        switch (id) {
        case RMode: lines.push_back(row(two(at, "Creator mode", mic ? "< Microphone >" : "< Text to sound >"), at)); break;
        case RText: {   // the text field, with the caret while it has the keyboard
            const std::string prefix = std::string(at ? "> " : "  ") + "Text: ";
            if (at) {
                const EditPaint p = paint_edit_field(creator_text_, edit_caret_, edit_anchor_, std::max(1, inner - display_width(prefix) - 1), "", true);
                const int fill = std::max(0, inner - display_width(prefix) - p.cols);
                lines.push_back(bar + " " + prefix + p.s + std::string(static_cast<size_t>(fill), ' ') + " " + bar);
            } else {
                lines.push_back(row(prefix + (creator_text_.empty() ? std::string("(type a text, | = new line)") : creator_text_), false));
            }
            break;
        }
        case RSound: lines.push_back(row(two(at, "Sound", scope ? "< Oscilloscope (XY) >" : "< Spectrogram >"), at)); break;
        case RLen: lines.push_back(row(two(at, "Length", "< " + std::to_string(kCreatorLens[std::clamp(creator_len_, 0, 4)]) + " s >"), at)); break;
        case RInvert: lines.push_back(row(two(at, "Invert", creator_invert_ == 0 ? "< Auto >" : creator_invert_ == 1 ? "< On >" : "< Off >"), at)); break;
        case ROpt1: lines.push_back(row(scope ? two(at, "Frame rate", "< " + std::to_string(kCreatorFps[std::clamp(creator_fps_, 0, 3)]) + " per second >")
                                              : two(at, "Scale", std::string("< ") + creator::scale_name(static_cast<creator::Scale>(std::clamp(creator_scale_, 0, 6))) + " >"), at)); break;
        case ROpt2: lines.push_back(row(scope ? two(at, "Motion", std::string("< ") + kCreatorMotionNames[std::clamp(creator_motion_, 0, 3)] + " >")
                                              : two(at, "Octaves", "< " + std::to_string(std::clamp(creator_octaves_, 3, 6)) + " >"), at)); break;
        case ROpt3: lines.push_back(row(scope ? two(at, "Detail", std::string("< ") + kCreatorDetailNames[std::clamp(creator_detail_, 0, 2)] + " >")
                                              : two(at, "Reverb", std::string("< ") + kCreatorReverbNames[std::clamp(creator_reverb_, 0, 2)] + " >"), at)); break;
        case RGrid: lines.push_back(row(two(at, "Scope grid", creator_grid_ ? "< On >" : "< Off >"), at)); break;
        case RView: lines.push_back(row(two(at, "Live view", std::string("< ") + kCreatorViewNames[std::clamp(creator_live_view_, 0, 2)] + " >"), at)); break;
        case RCreate: lines.push_back(row(two(at, "Create, save and preview", ""), at)); break;
        case RStart: lines.push_back(row(two(at, "Start the microphone preview", ""), at)); break;
        case RFiles: lines.push_back(row(two(at, "Created files...", ""), at)); break;
        default: break;
        }
    }
    if (!mic) {
        for (int i = 0; i < 6; ++i) {   // a braille preview of the text (6 rows, centred)
            if (i < static_cast<int>(creator_preview_.size())) {
                const int pad = std::max(0, (inner - 40) / 2);
                lines.push_back(bar + " " + std::string(static_cast<size_t>(pad), ' ') + creator_preview_[static_cast<size_t>(i)] +
                                std::string(static_cast<size_t>(std::max(0, inner - pad - 40)), ' ') + " " + bar);
            } else {
                lines.push_back(row(i == 2 && creator_preview_.empty() ? "      (the text appears here)" : "", false));
            }
        }
    }
    // the rest of the height: the hints of the microphone mode, or empty rows
    {
        std::vector<std::string> hint;
        if (mic) {
            hint = {"  The big preview shows your microphone live.", "  [ENTER] there starts / stops a recording (a WAV file);",
                    "  [V] changes the view, [G] the scope grid; [ESC] closes it.", "  Recordings appear under \"Created files...\"."};
        }
        while (static_cast<int>(lines.size()) < kTotalLines - 3) {
            const size_t k = lines.size() - (ids.size() + 1);
            lines.push_back(row(mic && k < hint.size() ? hint[k] : "", false));
        }
    }
    {   // the note / the last message, on two rows (wrapped at a space)
        const std::string note = creator_status_.empty()
            ? (mic ? "Watch your microphone as a spectrogram or an oscilloscope, and record it. Allow microphone access if asked."
               : scope ? "Left channel = x, right channel = y: the XY scope draws the text. A preview opens after creation."
                     : "The text is the spectrogram: time runs left to right, pitch bottom to top. A preview opens after creation.")
            : creator_status_;
        std::string l1 = note, l2;
        if (display_width(note) > inner) {
            size_t cut = std::string::npos;
            for (size_t k = 0; k < note.size(); ++k)
                if (note[k] == ' ' && display_width(note.substr(0, k)) <= inner) cut = k;
            if (cut != std::string::npos) { l1 = note.substr(0, cut); l2 = note.substr(cut + 1); }
        }
        lines.push_back(row(l1, false));
        lines.push_back(row(l2, false));
    }
    lines.push_back(box_bottom(W, "[UP/DOWN] [LEFT/RIGHT] change  [ENTER] go  [ESC] close", border_bottom));
    return lines;
}

// ---------------------------------------------------------------------
// The creator's preview (opens after "Create", loops until ESC) -- see app.h.
// ---------------------------------------------------------------------
void App::creator_preview_open() {
    if (!creator_audio_ || creator_audio_->l.empty()) return;
    creator_fed_ = 0;
    creator_last_pos_ = 0.0;
    creator_scope_.reset();
    creator_spectro_.reset();
    creator_spectro_gfx_.invalidate();
    if (creator_preview_kind_ == 1) {
        // the spectrogram's own settings: the picture as it was drawn (log pitch, the rows of the scale), one channel, one page
        // as long as the clip
        const auto freqs = creator::scale_frequencies(static_cast<creator::Scale>(std::clamp(creator_scale_, 0, 6)), 36, std::clamp(creator_octaves_, 3, 6));
        SpectroSettings sp;
        sp.style = 1;
        sp.motion = 0;
        sp.scale = 1;
        sp.min_freq = std::max(20, static_cast<int>(freqs.front() * 0.85));
        sp.max_freq = static_cast<int>(freqs.back() * 1.15);
        sp.channels = 1;
        sp.scheme = 0;
        const double secs = creator_audio_->seconds();
        sp.span = std::clamp(static_cast<int>(std::ceil(secs / 5.0)) * 5, 5, 60);
        creator_spectro_set_ = sp;
        creator_spectro_.set_visible(true);
    } else {
        creator_spectro_.set_visible(false);
    }
    creator_show_ = true;
    scope_big_ = false;
    spectro_full_ = false;
    force_redraw_ = true;
}

void App::creator_remember_previous() {
    if (creator_show_) return;   // already in a preview: what was playing before it stays the song to come back to
    creator_prev_valid_ = false;
    if (has_track_ && current_is_local_ && !current_path_.empty()) {
        creator_prev_.path = current_path_;
        creator_prev_.title = path_utf8(current_path_.stem());
        creator_prev_.folder_artist = path_utf8(current_path_.parent_path().filename());
        creator_prev_valid_ = true;
    }
}

void App::creator_resume_tick() {
    if (!creator_resume_ || load_in_progress_.load()) return;
    creator_resume_ = false;
    if (!creator_prev_valid_) return;
    creator_prev_valid_ = false;
    start_local_track(creator_prev_);
    status_line_ = "Preview closed: back to " + creator_prev_.title;
}

void App::creator_preview_close() {
    if (creator_live_) {
        if (creator_rec_on_) creator_record_save();
        if (creator_mic_) creator_mic_->stop();
        creator_live_ = false;
    }
    creator_show_ = false;
    creator_spectro_.set_visible(false);
    creator_audio_.reset();
    if (has_track_) {
        player_.stop();
        has_track_ = false;
        history_end_current_play();
    }
    creator_resume_ = creator_prev_valid_;   // the song that played before comes back (creator_resume_tick)
    mode_ = Mode::Creator;                   // back to the creator overlay; ESC there closes it
    creator_list_open_ = false;
    status_line_ = "Preview closed";
    force_redraw_ = true;
}

// ---------------------------------------------------------------------
// The microphone preview (creator mode "Microphone"): the big preview made from the live microphone; ENTER records.
// ---------------------------------------------------------------------
void App::creator_live_start() {
    if (!creator_mic_) creator_mic_ = std::make_unique<MicCapture>();
    creator_remember_previous();                 // a playing song would be heard by the microphone: it stops and comes back afterwards
    std::string err;
    if (!creator_mic_->start(44100, err)) { creator_status_ = "Microphone: " + err; return; }
    if (has_track_) {
        player_.stop();
        has_track_ = false;
        history_end_current_play();
    }
    creator_live_ = true;
    creator_rec_on_ = false;
    creator_rec_.clear();
    creator_rec_rate_ = creator_mic_->rate();
    creator_audio_.reset();
    creator_track_ = LocalTrack{};
    creator_preview_kind_ = kCreatorViewKind[std::clamp(creator_live_view_, 0, 2)];
    creator_scope_.reset();
    creator_spectro_.reset();
    creator_spectro_gfx_.invalidate();
    SpectroSettings sp;
    sp.style = 1;
    sp.motion = 1;       // scrolling, as a live picture
    sp.scale = 1;
    sp.min_freq = 50;
    sp.max_freq = 12000;
    sp.channels = 1;
    sp.scheme = 0;
    sp.span = 10;
    creator_spectro_set_ = sp;
    creator_spectro_.set_visible(creator_preview_kind_ == 1);
    creator_show_ = true;
    mode_ = Mode::Browse;
    scope_big_ = false;
    spectro_full_ = false;
    force_redraw_ = true;
    status_line_ = "Microphone: " + creator_mic_->device_name();
}

void App::creator_live_tick() {
    if (!creator_mic_ || !creator_mic_->running()) return;
    std::vector<float> lr;
    creator_mic_->read(lr);
    if (lr.empty()) return;
    const size_t n = lr.size() / 2;
    if (creator_preview_kind_ == 1) creator_spectro_.push(lr.data(), n, creator_rec_rate_);
    else creator_scope_.push_frames(lr.data(), n);
    if (creator_rec_on_) {
        creator_rec_.insert(creator_rec_.end(), lr.begin(), lr.end());
        if (creator_rec_.size() / 2 >= kCreatorMaxRecordSeconds * static_cast<size_t>(creator_rec_rate_)) {
            creator_record_save();
            status_line_ += " (the 5 minute limit was reached)";
        }
    }
}

void App::creator_record_toggle() {
    if (creator_rec_on_) { creator_record_save(); return; }
    creator_rec_.clear();
    creator_rec_on_ = true;
    status_line_ = "Recording ... [ENTER] stops and saves";
    force_redraw_ = true;
}

void App::creator_record_save() {
    creator_rec_on_ = false;
    force_redraw_ = true;
    if (creator_rec_.size() < 2 * static_cast<size_t>(creator_rec_rate_) / 20) { status_line_ = "Recording too short: not saved"; creator_rec_.clear(); return; }
    creator::Stereo st;
    st.rate = creator_rec_rate_;
    const size_t n = creator_rec_.size() / 2;
    st.l.resize(n);
    st.r.resize(n);
    for (size_t i = 0; i < n; ++i) { st.l[i] = creator_rec_[2 * i]; st.r[i] = creator_rec_[2 * i + 1]; }
    creator_rec_.clear();
    std::error_code ec;
    fs::create_directories(creator_dir(), ec);
    const bool newly = creator_dir_prepare();
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tmv);
    fs::path out = creator_dir() / (std::string("mic_") + stamp + ".wav");
    for (int k = 2; fs::exists(out, ec) && k < 1000; ++k) out = creator_dir() / (std::string("mic_") + stamp + "_" + std::to_string(k) + ".wav");
    std::string err;
    if (!creator::write_wav16(path_utf8(out), st, &err)) { status_line_ = "Could not save the recording: " + err; return; }
    log_event("Creator: saved " + path_utf8(out));
    if (newly) rescan_library();
    status_line_ = "Recording saved: " + path_utf8(out) + " (" + std::to_string(static_cast<int>(st.seconds())) + " s)";
}

void App::creator_preview_loop() {
    // the same as the play mode "loop": the track is already decoded, so it starts again at once
    launch_device_play_async();
    has_track_ = true;
    history_end_current_play();
    player_.clear_finished();
    history_begin_current_play();
    creator_fed_ = 0;
    creator_last_pos_ = 0.0;
    creator_scope_.reset();
    creator_spectro_.reset();
    creator_spectro_gfx_.invalidate();
}

void App::creator_preview_tick() {
    if (creator_live_) { creator_live_tick(); return; }
    if (!creator_show_ || !creator_audio_ || !has_track_) return;
    const creator::Stereo& a = *creator_audio_;
    const double pos = player_.poll_elapsed();
    if (pos + 0.3 < creator_last_pos_) {   // the file started again (or was sought back)
        creator_fed_ = 0;
        creator_scope_.reset();
        creator_spectro_.reset();
        creator_spectro_gfx_.invalidate();
    }
    creator_last_pos_ = pos;
    const size_t target = std::min(a.l.size(), static_cast<size_t>(std::max(0.0, pos) * a.rate));
    std::vector<float> lr;
    while (creator_fed_ < target) {
        const size_t n = std::min<size_t>(4096, target - creator_fed_);
        lr.resize(n * 2);
        for (size_t i = 0; i < n; ++i) { lr[2 * i] = a.l[creator_fed_ + i]; lr[2 * i + 1] = a.r[creator_fed_ + i]; }
        if (creator_preview_kind_ == 0) creator_scope_.push_frames(lr.data(), n);
        else creator_spectro_.push(lr.data(), n, a.rate);
        creator_fed_ += n;
    }
}

void App::creator_preview_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (!arrow && key == 27) { creator_preview_close(); return; }
    if (!arrow && (key == 'g' || key == 'G') && creator_preview_kind_ != 1) { creator_grid_ = !creator_grid_; force_redraw_ = true; return; }
    if (creator_live_) {
        if (!arrow && (key == '\r' || key == '\n')) { creator_record_toggle(); return; }
        if (!arrow && (key == 'v' || key == 'V')) {
            creator_live_view_ = (creator_live_view_ + 1) % 3;
            creator_preview_kind_ = kCreatorViewKind[creator_live_view_];
            creator_scope_.reset();
            creator_spectro_.reset();
            creator_spectro_.set_visible(creator_preview_kind_ == 1);
            creator_spectro_gfx_.invalidate();
            force_redraw_ = true;
        }
        return;
    }
    if (!arrow && key == ' ') {
        if (player_.is_paused()) player_.resume(); else player_.pause();
        return;
    }
}

std::string App::build_creator_preview(int W, const char* clear_prefix) const {
    const int H = std::max(6, term_rows_ - 1);
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string legend = ansi_for(settings_.legend_color, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const bool scope = creator_preview_kind_ != 1;
    const bool live = creator_live_;
    std::string title = live ? std::string("CREATOR - MICROPHONE - ") + kCreatorViewNames[std::clamp(creator_live_view_, 0, 2)]
                             : std::string("CREATOR PREVIEW - ") + (scope ? "oscilloscope music" : "spectrogram");
    if (!creator_track_.title.empty()) title += " - " + creator_track_.title;
    if (live && creator_rec_on_) {
        const int secs = static_cast<int>(creator_rec_.size() / 2 / static_cast<size_t>(std::max(1, creator_rec_rate_)));
        char t[40];
        std::snprintf(t, sizeof t, " - REC %d:%02d", secs / 60, secs % 60);
        title += t;
    }
    std::ostringstream f;
    f << clear_prefix << "\x1b[?25l";
    f << box_top(truncate_str(title, W - 6), W, border) << "\n";
    const int rows = std::max(2, H - 2);
    const bool image = gfx_proto_ != GfxProto::None;
    if (scope) {
        // a square picture in the middle; its own look: the CRT beam in music mode (the exact figure), classic green phosphor
        int cw0 = 10, ch0 = 20;
        if (image) gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
        cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
        int cols = image ? static_cast<int>(std::lround(static_cast<double>(rows) * ch0 / cw0)) : rows * 2;
        if (creator_preview_kind_ == 2) cols = W - 4;   // the time trace uses the full width
        cols = std::clamp(cols, 4, W - 4);
        const int margin = std::max(0, (W - 2 - cols) / 2);
        OscilloscopeVisualizer::Params p;
        p.decay = std::pow(0.80f, static_cast<float>(std::clamp(viz_dt_, 0.004, 0.15) * 30.0));
        p.fixed_gain = !live;          // a file keeps its exact figure; the microphone is levelled automatically
        p.mono_phase = false;
        p.crt = true;
        p.dwell = !live;
        p.low_rate_blank = !live;
        p.trace = 1024;
        p.glow = 0.60f;
        p.mode = creator_preview_kind_ == 2 ? 1 : 0;
        const int gkind = creator_preview_kind_ == 2 ? 1 : 0;
        std::vector<std::string> pane;
        if (image) {
            const int scale = 2;
            const int cws = std::max(2, std::min(std::clamp(640 / std::max(1, cols), 4, cw0), cw0 / scale));
            const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
            GfxFrame& F = gfx_;
            F.id = 0; F.scale = scale; F.out_cw = cw0; F.out_ch = ch0; F.crop = 0; F.cut_w = 0;
            F.cols = cols; F.rows = rows; F.w = cols * cws; F.h = rows * chs;
            F.row = 1; F.col = 1 + margin;
            creator_scope_.render_image(F.w, F.h, p, F.level, F.hue);
            for (auto& c : F.pal) c = {90, 255, 130};
            std::fill(F.hue.begin(), F.hue.end(), static_cast<uint8_t>(0));
            if (creator_grid_) F.grid = scope_grid_pixels(F.w, F.h, cols, rows, gkind, 4, 4); else F.grid.clear();
            int gr = 128, gg = 128, gb = 128;
            try_parse_rgb(settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color, gr, gg, gb);
            F.grid_rgb = {static_cast<uint8_t>(gr), static_cast<uint8_t>(gg), static_cast<uint8_t>(gb)};
            F.active = true;
            F.fresh = gfx_due_;
            gfx2_.active = false;
            pane.assign(static_cast<size_t>(rows), gfx_hole(cols));
        } else {
            const std::string grid_ansi = ansi_for(settings_.scope_grid_color.empty() ? settings_.list_color : settings_.scope_grid_color, false);
            auto color_of = [&](int, int, uint8_t, float k) { return osci_rgb_seq(90, 255, 130, k); };
            pane = scope_big_pane(creator_scope_, p, cols, rows, creator_grid_, gkind, grid_ansi, color_of, 4, 4);
        }
        for (int r = 0; r < rows; ++r)
            f << bar << std::string(static_cast<size_t>(margin), ' ') << (r < static_cast<int>(pane.size()) ? pane[static_cast<size_t>(r)] : std::string(static_cast<size_t>(cols), ' '))
              << std::string(static_cast<size_t>(std::max(0, W - 2 - margin - cols)), ' ') << bar << "\n";
    } else {
        SpectroAnalyzer& a = creator_spectro_;
        a.update(creator_spectro_set_);
        const SpectroSettings& sp = creator_spectro_set_;
        const int label_w = 6;
        const int cols = std::max(4, W - 2 - label_w - 1);
        const int srows = std::max(2, rows - 1);
        int cw = 10, chh = 20;
        if (image) gfx_cell_pixels(settings_.cell_pixels, cw, chh);
        const int unit = image ? std::max(1, chh) : 4;
        const auto labels = spectro_axis(sp, a.rate(), srows * unit, unit);
        std::vector<std::string> lab(static_cast<size_t>(srows));
        for (const auto& l : labels) if (l.first >= 0 && l.first < srows) lab[static_cast<size_t>(l.first)] = l.second;
        std::string ruler(static_cast<size_t>(cols), ' ');
        const int step = sp.span <= 5 ? 1 : sp.span <= 10 ? 2 : sp.span <= 30 ? 5 : 10;
        for (int t = 0; t <= sp.span; t += step) {
            const int x = static_cast<int>(static_cast<long long>(t) * (cols - 1) / std::max(1, sp.span));
            const std::string txt = std::to_string(t) + "s";
            const int x0 = std::clamp(x - (t == 0 ? 0 : t == sp.span ? static_cast<int>(txt.size()) - 1 : static_cast<int>(txt.size()) / 2), 0, std::max(0, cols - static_cast<int>(txt.size())));
            for (size_t k = 0; k < txt.size() && x0 + static_cast<int>(k) < cols; ++k) ruler[static_cast<size_t>(x0) + k] = txt[k];
        }
        f << bar << legend << std::string(static_cast<size_t>(label_w + 1), ' ') << ruler << R << bar << "\n";
        std::vector<std::string> body;
        if (image) {
            spectro_area_[0] = 2;
            spectro_area_[1] = 1 + label_w + 1;
            spectro_area_[2] = cols;
            spectro_area_[3] = srows;
        } else body = a.braille(sp, cols, srows);
        for (int r = 0; r < srows; ++r) {
            f << bar << legend << pad_left(lab[static_cast<size_t>(r)], label_w) << R << " ";
            if (image) f << gfx_hole(cols);
            else f << (r < static_cast<int>(body.size()) ? body[static_cast<size_t>(r)] : std::string(static_cast<size_t>(cols), ' '));
            f << bar << "\n";
        }
    }
    const std::string grid_hint = scope ? "  [G] grid " + std::string(creator_grid_ ? "off" : "on") : std::string();
    if (live) f << box_bottom(W, std::string("[ENTER] ") + (creator_rec_on_ ? "stop and save" : "record") + "  [V] view" + grid_hint + "  [ESC] close", border_bottom) << "\n";
    else f << box_bottom(W, "[ESC] stop and close  [SPACE] pause" + grid_hint + "  -- loops; your scope / spectrogram settings are not used", border_bottom) << "\n";
    return clamp_output_rows(f.str(), term_rows_);
}

} // namespace muisc
