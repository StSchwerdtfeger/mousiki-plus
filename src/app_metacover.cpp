// The cover in the meta data editor: the picture under the fields and the "Cover Options" overlay (choose a picture for the track,
// "no cover", back to automatic, delete a fetched / folder picture, search pictures by file name). Member functions of App.
#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "path_utf8.h"
#include "picture_formats.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <system_error>
#include "app_internal.h"

namespace muisc {

namespace {

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_picture_name(const fs::path& p) {
    const std::string e = lower_ascii(path_utf8(p.extension()));
    for (const char* x : kCoverPictureExtensions) if (e == x) return true;
    return false;
}

std::string choice_of(const CoverCandidate& c) { return c.kind == CoverCandidate::Embedded ? std::string(kCoverEmbedded) : path_utf8(c.file); }

}  // namespace

// The cover the track has now, as braille text (the same cover the player would show, choices included).
std::vector<std::string> App::meta_cover_pane(int inner, int rows) const {
    const std::string path = meta_hovering_path();
    if (path.empty() || rows < 2) return std::vector<std::string>(static_cast<size_t>(std::max(rows, 0)), std::string(static_cast<size_t>(inner), ' '));
    CoverRequest rq;
    rq.path = path;
    rq.title = path_utf8(path_from_utf8(path).stem());
    rq.album = meta_hover_values_[static_cast<size_t>(MetaField::Album)];
    rq.search = settings_.cover_search;
    rq.store_rev = cover_store_revision();
    const CoverSource src = resolve_cover_source(rq);
    const bool overlay_up = meta_choice_open_ || meta_cov_open_ || meta_save_.active;   // (a picture would be drawn over a floating panel)
    if (cf_full_res_ && gfx_proto_ != GfxProto::None && !overlay_up && !src.none && !src.file.empty()) {
        if (cover_full_picture(src.file, src.embedded, inner, rows, meta_pane_col_ + 2, 4 + kMetaFieldCount + 3)) {
            std::vector<std::string> pic;
            for (int r = 0; r < rows; ++r) pic.push_back(gfx_hole(inner));
            return pic;
        }
    }
    const int cols = std::min(inner, rows * 2);
    const auto lines = cover_preview_lines(src.file, src.embedded, cols, rows, (src.none || src.file.empty()) ? std::string("no cover") : std::string());
    std::vector<std::string> out;
    const int lpad = std::max(0, (inner - cols) / 2);
    for (int r = 0; r < rows; ++r) {
        const std::string row = r < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(r)] : std::string(static_cast<size_t>(cols), ' ');
        out.push_back(std::string(static_cast<size_t>(lpad), ' ') + row + "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, inner - lpad - cols)), ' '));
    }
    return out;
}

void App::meta_toggle_cover_res() {
    if (gfx_proto_ == GfxProto::None) { meta_status_ = "this terminal shows no pictures (no Kitty / Sixel graphics): the cover stays ASCII"; return; }
    cf_full_res_ = !cf_full_res_;
    cf_pic_key_.clear();
    meta_status_ = cf_full_res_ ? "cover: full resolution" : "cover: ASCII";
}

void App::meta_cov_open() {
    const std::string path = meta_hovering_path();
    if (path.empty()) { meta_status_ = "no title selected"; return; }
    meta_cov_track_ = path;
    meta_cov_open_ = true;
    meta_cov_search_ = false;
    meta_cov_ask_delete_ = false;
    meta_cov_query_.clear();
    meta_cov_msg_.clear();
    meta_cov_sel_ = 0;
    text_disengage();
    meta_cov_rebuild();
    // start on the picture the track uses now
    const std::string cur = cover_track_choice(path);
    for (size_t i = 0; i < meta_cov_items_.size(); ++i) {
        const auto& it = meta_cov_items_[i];
        if ((cur.empty() && it.special == 1) || (cur == kCoverNone && it.special == 2) || (it.special == 0 && choice_of(it.c) == cur)) { meta_cov_sel_ = static_cast<int>(i); break; }
    }
}

void App::meta_cov_rebuild() {
    const fs::path track = path_from_utf8(meta_cov_track_);
    const std::string q = lower_ascii(meta_cov_query_);
    meta_cov_items_.clear();
    if (q.empty()) {
        meta_cov_items_.push_back({1, {}});
        meta_cov_items_.push_back({2, {}});
    }
    std::vector<fs::path> seen;
    auto add = [&](const CoverCandidate& c) {
        if (!q.empty() && lower_ascii(c.label).find(q) == std::string::npos) return;
        for (const auto& s : seen) if (s == c.file) return;
        seen.push_back(c.file);
        meta_cov_items_.push_back({0, c});
    };
    for (const auto& c : cover_candidates(track, false)) add(c);
    if (q.empty() ? has_embedded_picture(track) : (std::string("inside the file").find(q) != std::string::npos && has_embedded_picture(track))) {
        CoverCandidate e;
        e.kind = CoverCandidate::Embedded;
        e.file = track;
        e.label = "inside the file";
        meta_cov_items_.insert(meta_cov_items_.begin() + (q.empty() ? 2 : 0), MetaCovItem{0, e});
    }
    // the covers of the track's album (its fetched covers in other folders, the pictures chosen for it)
    const std::string album = meta_hover_values_[static_cast<size_t>(MetaField::Album)];
    if (!album.empty()) {
        const AlbumCovers ac = album_covers(track.parent_path(), album, track, false, false);
        for (const auto& f : ac.files) {
            CoverCandidate c;
            c.file = f;
            c.kind = path_utf8(f.parent_path().filename()) == kFetchedCoversDir ? CoverCandidate::Fetched : CoverCandidate::Folder;
            c.label = path_utf8(f.parent_path().filename()) + "/" + path_utf8(f.filename());
            add(c);
        }
    }
    // a search by file name also looks into the folders around the track (the folder above it, two levels down)
    if (!q.empty()) {
        std::error_code ec;
        const fs::path root = track.parent_path().has_parent_path() ? track.parent_path().parent_path() : track.parent_path();
        int count = 0;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && count < 300; it.increment(ec)) {
            if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
            std::error_code e2;
            if (!it->is_regular_file(e2) || !is_picture_name(it->path())) continue;
            const std::string name = path_utf8(it->path().filename());
            if (lower_ascii(name).find(q) == std::string::npos) continue;
            CoverCandidate c;
            c.file = it->path();
            c.kind = path_utf8(it->path().parent_path().filename()) == kFetchedCoversDir ? CoverCandidate::Fetched : CoverCandidate::Folder;
            c.label = path_utf8(it->path().parent_path().filename()) + "/" + name;
            bool dup = false;
            for (const auto& s : seen) if (s == c.file) { dup = true; break; }
            if (dup) continue;
            seen.push_back(c.file);
            meta_cov_items_.push_back({0, c});
            ++count;
        }
    }
    meta_cov_sel_ = std::clamp(meta_cov_sel_, 0, std::max(0, static_cast<int>(meta_cov_items_.size()) - 1));
}

bool App::handle_meta_cov_key(int key) {
    if (!meta_cov_open_) return false;
    const bool arrow = last_key_was_arrow();
    const int n = static_cast<int>(meta_cov_items_.size());
    if (meta_cov_ask_delete_) {
        if (key == 'y' || key == 'Y') {
            meta_cov_ask_delete_ = false;
            if (meta_cov_sel_ >= 0 && meta_cov_sel_ < n) {
                std::string why;
                if (delete_cover_candidate(meta_cov_items_[static_cast<size_t>(meta_cov_sel_)].c, &why)) meta_cov_msg_ = "picture deleted";
                else meta_cov_msg_ = why.empty() ? "cannot be deleted" : why;
                meta_cov_rebuild();
            }
        } else if (key == 'n' || key == 'N' || key == 27) {
            meta_cov_ask_delete_ = false;
        }
        return true;
    }
    if (key == 27) {
        if (meta_cov_search_) { meta_cov_search_ = false; text_disengage(); return true; }
        if (!meta_cov_query_.empty()) { meta_cov_query_.clear(); edit_caret_ = edit_anchor_ = 0; meta_cov_rebuild(); return true; }
        meta_cov_open_ = false;
        return true;
    }
    if (key == 9) { meta_cov_search_ = !meta_cov_search_; if (!meta_cov_search_) text_disengage(); return true; }
    if (arrow && (key == 'A' || key == 'B')) {
        if (n > 0) meta_cov_sel_ = std::clamp(meta_cov_sel_ + (key == 'A' ? -1 : 1) * (last_key_was_shifted() ? 8 : 1), 0, n - 1);
        return true;
    }
    if (meta_cov_search_) {
        if (key == '\r' || key == '\n') { meta_cov_search_ = false; text_disengage(); return true; }
        edit_focus("meta-cov-search", meta_cov_query_);
        text_engage("meta-cov-search");
        if (edit_text_key(meta_cov_query_, edit_caret_, edit_anchor_, key, 80, &meta_cov_msg_)) { meta_cov_sel_ = 0; meta_cov_rebuild(); }
        return true;
    }
    if (key == '/') { meta_cov_search_ = true; return true; }
    if ((key == '\r' || key == '\n') && n > 0) {
        const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(meta_cov_sel_)];
        std::string choice;
        std::string what;
        if (it.special == 1) { choice = ""; what = "automatic: Settings -> Cover Search decides again"; }
        else if (it.special == 2) { choice = kCoverNone; what = "no cover (the disk is shown)"; }
        else { choice = choice_of(it.c); what = it.c.label; }
        set_cover_track_choice(meta_cov_track_, choice);
        meta_cov_open_ = false;
        meta_status_ = "cover of this track: " + what;
        return true;
    }
    if ((key == kKeyDelete || key == 127 || key == 'd') && n > 0) {
        const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(meta_cov_sel_)];
        if (it.special != 0) { meta_cov_msg_ = "nothing to delete here"; return true; }
        if (!cover_deletable(it.c)) { meta_cov_msg_ = "the picture inside a file cannot be deleted"; return true; }
        meta_cov_ask_delete_ = true;
        return true;
    }
    return true;
}

std::vector<std::string> App::build_meta_cov_panel() const {
    const int W = kMetaCovPanelWidth;
    const int inner = W - 2;
    const int list_w = 40;
    const int prev_w = inner - list_w - 1;
    const int rows = 14;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    const int n = static_cast<int>(meta_cov_items_.size());
    std::vector<std::string> lines;
    lines.push_back(box_top("COVER OPTIONS  " + truncate_str(path_utf8(path_from_utf8(meta_cov_track_).filename()), 40), W, border));
    {   // search by file name
        std::string field;
        int cols = 0;
        if (meta_cov_search_) {
            EditPaint p = paint_edit_field(meta_cov_query_, edit_caret_, edit_anchor_, 40, search_sgr(settings_), true);
            field = search_run(settings_, p.s, true);
            cols = p.cols;
        } else {
            const std::string plain = truncate_str(meta_cov_query_.empty() ? std::string("(TAB or / : search pictures by file name)") : meta_cov_query_, 60);
            field = search_run(settings_, plain, !meta_cov_query_.empty());
            cols = display_width(plain);
        }
        const std::string pre = " File name: ";
        lines.push_back(obar + search_idle_sgr(settings_) + pre + field + "\x1b[0m" + std::string(static_cast<size_t>(std::max(0, inner - cols - static_cast<int>(pre.size()))), ' ') + obar);
    }
    const std::string cur = cover_track_choice(meta_cov_track_);
    // the preview of the highlighted picture
    std::vector<std::string> prev;
    {
        std::string file, msg;
        bool emb = false;
        if (n > 0) {
            const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(meta_cov_sel_)];
            if (it.special == 0) { file = path_utf8(it.c.file); emb = it.c.kind == CoverCandidate::Embedded; }
            else if (it.special == 2) msg = "no cover";
            else msg = "automatic";
        } else msg = "no picture found";
        prev = cover_preview_lines(file, emb, prev_w, rows, msg);
    }
    const int first = std::clamp(meta_cov_sel_ - rows / 2, 0, std::max(0, n - rows));
    for (int r = 0; r < rows; ++r) {
        const int k = first + r;
        std::string left;
        if (k < n) {
            const MetaCovItem& it = meta_cov_items_[static_cast<size_t>(k)];
            std::string label, mark = "   ";
            if (it.special == 1) { label = "Automatic (Settings -> Cover Search)"; if (cur.empty()) mark = " * "; }
            else if (it.special == 2) { label = "No cover (show the disk)"; if (cur == kCoverNone) mark = " * "; }
            else { label = it.c.label; if (choice_of(it.c) == cur) mark = " * "; }
            left = pad_right(truncate_str(mark + label, list_w), list_w);
            if (k == meta_cov_sel_ && !meta_cov_search_) left = "\x1b[7m" + left + "\x1b[0m";
        } else {
            left = std::string(static_cast<size_t>(list_w), ' ');
        }
        const std::string right = r < static_cast<int>(prev.size()) ? prev[static_cast<size_t>(r)] : std::string(static_cast<size_t>(prev_w), ' ');
        lines.push_back(obar + left + obar + right + "\x1b[0m" + obar);
    }
    std::string msg = meta_cov_ask_delete_ ? "Delete this picture from the disk?   [Y]es   [N]o" : meta_cov_msg_;
    lines.push_back(obar + pad_right(truncate_str(" " + msg, inner), inner) + obar);
    lines.push_back(box_bottom(W, "[↑↓] select  [ENTER] use  [DEL] delete  [TAB] search  [ESC] close", border));
    return lines;
}

}  // namespace muisc
