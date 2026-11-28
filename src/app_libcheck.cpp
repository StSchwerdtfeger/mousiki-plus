// app_libcheck.cpp
// Part of class App (src/app.h): MESSY LIBRARY? (Settings -> GENERAL -> MESSY LIBRARY?): the library report and the clean-up of duplicates.
//
// The rules of this screen, because it touches the user's files:
//  * Nothing is ever deleted. "Clean up" only SETS ASIDE: the files you marked are moved into a folder inside the library folder they came from
//    ("Mousiki - duplicates set aside", one sub-folder per time), which the library does not list. You delete that folder yourself, when you are sure.
//  * Setting aside takes: a mark on each file (SPACE), [m], and the word MOVE typed out. At least one file of every group always stays unmarked.
//  * What a file had of its own (plays, its style) goes to the copy that stays; a list of where each file came from is written next to them, and
//    [u] puts the newest batch back.
//  * "Let it rest" keeps a group out of the way for good: it is a deliberate copy (an album and a single, a remaster ...).
#include "app.h"
#include "app_state_lcheck.h"
#include "app_dirs.h"
#include "bg_tasks.h"
#include "console_log.h"
#include "cover_store.h"
#include "file_prints.h"
#include "local_source.h"
#include "moved_files.h"
#include "string_util.h"
#include "text_edit.h"
#include "utf8_util.h"
#include "video_links.h"
#include "app_internal.h"
#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace muisc {

namespace {

constexpr const char* kSetAsideFolder = "Mousiki - duplicates set aside";
constexpr const char* kManifestName = "where_they_came_from.txt";

fs::path rested_file() { return config_path().parent_path() / "lib_rested.txt"; }

std::vector<std::set<std::string>> load_rested() {
    std::vector<std::set<std::string>> out;
    std::ifstream in(rested_file(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        std::set<std::string> s;
        size_t a = 0;
        while (a <= line.size()) {
            const size_t b = line.find('\t', a);
            const std::string p = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!p.empty()) s.insert(p);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (s.size() >= 2) out.push_back(std::move(s));
    }
    return out;
}

void save_rested(const std::vector<std::set<std::string>>& v) {
    std::string text;
    for (const auto& s : v) { bool first = true; for (const std::string& p : s) { text += (first ? "" : "\t") + p; first = false; } text += "\n"; }
    std::error_code ec;
    fs::create_directories(rested_file().parent_path(), ec);
    if (v.empty()) { fs::remove(rested_file(), ec); return; }
    write_text_atomic(rested_file(), text, /*binary=*/true);
}

// `s` cut to `w` columns, with "..." at the end; padded with spaces to exactly `w` columns
std::string fit(const std::string& s, int w) {
    if (w <= 0) return {};
    std::string t = s;
    if (display_width(t) > w) t = utf8_take(t, std::max(0, w - 3)) + "...";
    const int dw = display_width(t);
    return t + std::string(static_cast<size_t>(std::max(0, w - dw)), ' ');
}

// the END of a path kept (the file name matters most): "...ist/Album/song.mp3"
std::string tail_fit(const std::string& s, int w) {
    if (display_width(s) <= w) return s;
    const std::vector<std::string> ch = utf8_chars(s);
    size_t from = 0;
    int dw = display_width(s);
    while (from < ch.size() && dw > w - 3) { dw -= display_width(ch[from]); ++from; }
    std::string t = "...";
    for (size_t i = from; i < ch.size(); ++i) t += ch[i];
    return t;
}

std::string stamp_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[40];
    std::strftime(b, sizeof b, "%Y-%m-%d %H.%M.%S", &tmv);
    return b;
}

bool path_inside(const std::string& file, const std::string& dir) {
    if (dir.empty() || file.size() <= dir.size()) return false;
    if (file.compare(0, dir.size(), dir) != 0) return false;
    const char c = file[dir.size()];
    return c == '/' || c == '\\' || dir.back() == '/' || dir.back() == '\\';
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- opening, closing, the check

void App::libcheck_open(int page) {
    lc_->page = page;
    lc_->rep_scroll = 0;
    lc_->cg = lc_->cm = lc_->view_top = 0;
    lc_->marked.clear();
    lc_->confirm = 0;
    lc_->typed.clear();
    lc_->note.clear();
    lc_->show_rested = false;
    lc_->rested = load_rested();
    libcheck_orphans();
    libcheck_start();
    mode_ = Mode::LibraryCheck;
    force_redraw_ = true;
}

void App::libcheck_close() {
    if (lc_->job) lc_->job->progress.cancel.store(true);   // a check that is still running stops
    lc_->job.reset();
    mode_ = Mode::Settings;
    force_redraw_ = true;
}

void App::libcheck_start() {
    if (lc_->job) lc_->job->progress.cancel.store(true);
    auto job = std::make_shared<LcState::Job>();
    job->rested = lc_->rested;
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        job->input.reserve(all_local_tracks_.size());
        for (const auto& t : all_local_tracks_) {
            if (LocalSource::is_video_file(t.path)) continue;
            LcFile f;
            f.path = path_utf8(t.path);
            const auto it = row_meta_cache_.find(f.path);
            if (it != row_meta_cache_.end()) {
                f.title = it->second.title; f.artist = it->second.artist; f.vid = it->second.vid;
                if (it->second.duration_sec > 0) f.dur_ms = static_cast<long long>(it->second.duration_sec * 1000.0);
                if (!it->second.tags_resolved) ++job->tags_missing;
            } else {
                ++job->tags_missing;
            }
            job->input.push_back(std::move(f));
        }
    }
    lc_->job = job;
    lc_->adopted = false;
    lc_->last_progress = -1;
    std::shared_ptr<BgTasks> bg = bg_;
    spawn_bg("library check", [job, bg]() {
        job->progress.stage.store(1);
        job->progress.total.store(static_cast<int>(job->input.size()));
        job->progress.done.store(0);
        for (LcFile& f : job->input) {   // sizes (the lengths the tags did not give: from the file's own header)
            if (job->progress.cancel.load() || bg->quit.load()) return;
            std::error_code ec;
            const auto sz = fs::file_size(path_from_utf8(f.path), ec);
            f.size = ec ? -1 : static_cast<long long>(sz);
            if (f.dur_ms <= 0) f.dur_ms = FilePrints::get().duration_ms_of(f.path);
            job->progress.done.fetch_add(1);
        }
        job->result = lc_find_duplicates(std::move(job->input), job->rested,
                                         [job](const std::string& p) { return lc_hash_file(p, &job->progress.cancel); }, &job->progress);
        job->finished.store(true);
    });
}

void App::libcheck_poll() {
    if (mode_ != Mode::LibraryCheck || !lc_->job) return;
    auto& job = *lc_->job;
    if (job.finished.load() && !lc_->adopted) {
        lc_->result = std::move(job.result);
        lc_->adopted = true;
        lc_->cg = lc_->cm = lc_->view_top = 0;
        force_redraw_ = true;
        return;
    }
    if (!lc_->adopted) {   // progress changed: draw it
        const int p = job.progress.stage.load() * 1000000 + job.progress.done.load() / 50;
        if (p != lc_->last_progress) { lc_->last_progress = p; force_redraw_ = true; }
    }
}

// The per-track data (plays, a track's own style, video links) of files that are not there any more: what the next scan can find again, and what it cannot.
void App::libcheck_orphans() {
    lc_->orphan_short.clear();
    lc_->orphan_full.clear();
    std::set<std::string> keys = history_.local_paths();
    for (const std::string& k : cover_store_track_keys()) keys.insert(k);
    for (const auto& kv : VideoLinks::get().all()) { keys.insert(kv.first); keys.insert(kv.second); }
    std::vector<std::string> missing;
    for (const std::string& k : keys) {
        if (k.empty() || k.compare(0, 3, "yt:") == 0) continue;
        std::error_code ec;
        if (!fs::exists(path_from_utf8(k), ec)) missing.push_back(k);
    }
    std::vector<std::string> existing;
    existing.reserve(all_local_tracks_.size());
    for (const auto& t : all_local_tracks_) existing.push_back(path_utf8(t.path));
    const std::set<std::string> miss_set(missing.begin(), missing.end());
    const std::map<std::string, std::string> found = find_moved(miss_set, existing);
    std::multimap<std::string, std::string> by_name;
    for (const std::string& e : existing) by_name.emplace(moved_name_key(e), e);
    int n_found = 0, n_ambiguous = 0, n_lost = 0;
    std::vector<std::string> lines;
    for (const std::string& m : missing) {
        std::string what;
        if (found.count(m)) { ++n_found; what = "will be found again at " + found.at(m); }
        else if (by_name.count(moved_name_key(m)) >= 2) { ++n_ambiguous; what = "the name is not unique: " + std::to_string(by_name.count(moved_name_key(m))) + " files are called " + moved_name_key(m); }
        else { ++n_lost; what = "no file of that name (or of that size and length) in the library"; }
        lines.push_back("    " + m + "  --  " + what);
    }
    lc_->orphan_short.push_back("PER-TRACK DATA OF FILES THAT ARE NOT THERE (plays, a track's own style, video links)");
    if (missing.empty()) {
        lc_->orphan_short.push_back("  none: every file that has data of its own is where Mousiki expects it");
    } else {
        lc_->orphan_short.push_back("  " + std::to_string(missing.size()) + " files are missing:");
        lc_->orphan_short.push_back("    found again by the next scan (name, or size and length) ..... " + std::to_string(n_found));
        lc_->orphan_short.push_back("    several files have that name, none can be told apart ........ " + std::to_string(n_ambiguous));
        lc_->orphan_short.push_back("    not in the library (moved out of its folders, or deleted) ... " + std::to_string(n_lost));
        lc_->orphan_short.push_back("  Their data waits: add the folder they went to (Settings -> LOCAL MUSIC PATH) and it is linked again at the next scan.");
        lc_->orphan_short.push_back("  [w] writes the list of these files.");
    }
    lc_->orphan_full = lc_->orphan_short;
    for (const std::string& l : lines) lc_->orphan_full.push_back(l);
}

void App::libcheck_write_report() {
    std::vector<std::string> L;
    L.push_back("MOUSIKI LIBRARY REPORT  " + stamp_now());
    L.push_back("");
    const std::vector<std::string> body = lc_report_lines(lc_->result, /*full=*/true);
    for (const std::string& s : body) L.push_back(s);
    L.push_back("");
    for (const std::string& s : lc_->orphan_full) L.push_back(s);
    std::string text;
    for (const std::string& s : L) text += s + "\n";
    const fs::path dir = cache_.download_dir() / "history";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string name = stamp_now();
    for (char& c : name) if (c == ' ') c = '_';
    const fs::path file = dir / ("library_report_" + name + ".txt");
    if (write_text_atomic(file, text, /*binary=*/true)) lc_->note = "written: " + path_utf8(file);
    else lc_->note = "could not write " + path_utf8(file);
    status_line_ = lc_->note;
    force_redraw_ = true;
}

// ---------------------------------------------------------------------------------------------------------------- let it rest / set aside / undo

void App::libcheck_rest(int gi) {
    if (gi < 0 || gi >= static_cast<int>(lc_->result.groups.size())) return;
    LcGroup& g = lc_->result.groups[static_cast<size_t>(gi)];
    std::set<std::string> paths;
    for (const LcMember& m : g.members) paths.insert(lc_->result.files[static_cast<size_t>(m.file)].path);
    if (!g.rested) {
        lc_->rested.push_back(paths);
        g.rested = true;
        for (const std::string& p : paths) lc_->marked.erase(p);
        lc_->note = "let rest: these files stay as they are, they are not listed again (SHIFT+... [t] shows them)";
    } else {
        lc_->rested.erase(std::remove_if(lc_->rested.begin(), lc_->rested.end(), [&](const std::set<std::string>& rs) {
            return std::includes(rs.begin(), rs.end(), paths.begin(), paths.end());
        }), lc_->rested.end());
        g.rested = false;
        lc_->note = "this group is listed again";
    }
    save_rested(lc_->rested);
    // the counts of the report
    lc_->result.n_identical = lc_->result.n_same_id = lc_->result.n_same_song = lc_->result.n_rested = 0;
    lc_->result.extra_bytes_all = 0;
    for (const LcGroup& x : lc_->result.groups) {
        if (x.rested) { ++lc_->result.n_rested; continue; }
        (x.kind == LcKind::Identical ? lc_->result.n_identical : x.kind == LcKind::SameId ? lc_->result.n_same_id : lc_->result.n_same_song) += 1;
        lc_->result.extra_bytes_all += x.extra_bytes;
    }
}

void App::libcheck_set_aside() {
    struct Item { std::string path, keep; };
    std::vector<Item> items;
    for (const LcGroup& g : lc_->result.groups) {
        if (g.rested) continue;
        std::string keep;
        for (const LcMember& m : g.members) {
            const std::string& p = lc_->result.files[static_cast<size_t>(m.file)].path;
            if (!lc_->marked.count(p)) { keep = p; break; }
        }
        if (keep.empty()) continue;   // (never: at least one file of a group stays unmarked)
        for (const LcMember& m : g.members) {
            const std::string& p = lc_->result.files[static_cast<size_t>(m.file)].path;
            if (lc_->marked.count(p)) items.push_back({p, keep});
        }
    }
    if (items.empty()) { lc_->note = "nothing is marked"; return; }
    std::vector<std::string> roots;
    for (const std::string& r : library_roots()) roots.push_back(r);
    const std::string stamp = stamp_now();
    std::vector<std::pair<std::string, std::string>> renames;   // the file that goes -> the copy that stays
    int moved = 0, failed = 0;
    std::string first_batch;
    for (const Item& it : items) {
        std::error_code ec;
        const fs::path src = path_from_utf8(it.path);
        if (!fs::exists(src, ec)) { ++failed; continue; }
        std::string root;
        for (const std::string& r : roots) if (path_inside(it.path, r) && r.size() > root.size()) root = r;
        const fs::path base = root.empty() ? src.parent_path() : path_from_utf8(root);
        const fs::path batch = base / kSetAsideFolder / path_from_utf8(stamp);
        fs::create_directories(batch, ec);
        if (ec) { ++failed; continue; }
        { std::ofstream mk(batch / kSetAsideMarker, std::ios::app); }   // (the library does not list this folder)
        fs::path dest = batch / src.filename();
        for (int n = 2; fs::exists(dest, ec) && n < 1000; ++n) dest = batch / path_from_utf8(path_utf8(src.stem()) + " (" + std::to_string(n) + ")" + path_utf8(src.extension()));
        fs::rename(src, dest, ec);
        if (ec) {   // another disk: copy, and remove the original only when the copy is complete
            ec.clear();
            fs::copy_file(src, dest, fs::copy_options::none, ec);
            if (!ec && fs::file_size(dest, ec) == fs::file_size(src, ec) && !ec) fs::remove(src, ec);
            else { std::error_code e2; fs::remove(dest, e2); ++failed; continue; }
        }
        { std::ofstream man(batch / kManifestName, std::ios::app | std::ios::binary); man << path_utf8(dest.filename()) << "\t" << it.path << "\t" << it.keep << "\n"; }
        renames.emplace_back(it.path, it.keep);
        if (first_batch.empty()) first_batch = path_utf8(batch);
        ++moved;
    }
    if (!renames.empty()) {   // what the files had of their own goes to the copy that stays
        history_.rename_ids(renames);
        cover_store_rename_paths(renames);
        std::vector<std::pair<std::string, std::string>> vl;
        for (const auto& r : renames) if (!VideoLinks::get().has(r.second)) vl.push_back(r);
        if (!vl.empty()) VideoLinks::get().rename_paths(vl);
    }
    lc_->marked.clear();
    lc_->confirm = 0;
    lc_->typed.clear();
    lc_->note = "set aside: " + std::to_string(moved) + " file" + (moved == 1 ? "" : "s") + (failed ? ", " + std::to_string(failed) + " could not be moved" : std::string())
              + (first_batch.empty() ? std::string() : " -- in " + first_batch + "  (nothing was deleted; [u] puts them back)");
    status_line_ = lc_->note;
    ConsoleLog::instance().log_basic("library check: " + lc_->note);
    rescan_library();
    libcheck_start();
    force_redraw_ = true;
}

void App::libcheck_undo() {
    // the newest batch folder of all library folders (their names are the time, so they sort)
    fs::path newest;
    std::string newest_name;
    std::error_code ec;
    for (const std::string& r : library_roots()) {
        const fs::path dir = path_from_utf8(r) / kSetAsideFolder;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_directory(ec) || !fs::exists(e.path() / kManifestName, ec)) continue;
            const std::string nm = path_utf8(e.path().filename());
            if (nm > newest_name) { newest_name = nm; newest = e.path(); }
        }
    }
    if (newest.empty()) { lc_->note = "nothing to put back: no set-aside batch found"; return; }
    std::ifstream in(newest / kManifestName, std::ios::binary);
    std::string line, left;
    int back = 0, skipped = 0;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t a = line.find('\t');
        const size_t b = a == std::string::npos ? std::string::npos : line.find('\t', a + 1);
        if (a == std::string::npos) continue;
        const fs::path now = newest / path_from_utf8(line.substr(0, a));
        const fs::path orig = path_from_utf8(line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1));
        if (!fs::exists(now, ec)) continue;                       // (already taken out by hand)
        if (fs::exists(orig, ec)) { ++skipped; left += line + "\n"; continue; }   // (something is there now: it is not overwritten)
        fs::create_directories(orig.parent_path(), ec);
        fs::rename(now, orig, ec);
        if (ec) { ++skipped; left += line + "\n"; ec.clear(); continue; }
        ++back;
    }
    in.close();
    if (skipped == 0) {   // everything is back: the batch folder goes (it only holds the manifest and the marker now)
        fs::remove(newest / kManifestName, ec);
        fs::remove(newest / kSetAsideMarker, ec);
        fs::remove(newest, ec);
    }
    lc_->note = "put back: " + std::to_string(back) + " file" + (back == 1 ? "" : "s") + (skipped ? ", " + std::to_string(skipped) + " left where they are (their old place is taken)" : std::string())
              + "  (the plays they had stay with the other copy)";
    status_line_ = lc_->note;
    rescan_library();
    libcheck_start();
    force_redraw_ = true;
}

// ---------------------------------------------------------------------------------------------------------------- keys

void App::libcheck_key(int key) {
    const bool arrow = last_key_was_arrow();
    LcState& L = *lc_;
    force_redraw_ = true;
    if (L.confirm == 1) {   // the question before anything is moved
        if (!arrow && key == 27) { L.confirm = 0; L.typed.clear(); L.note = "cancelled: nothing was moved"; return; }
        if (!arrow && (key == '\r' || key == '\n')) {
            if (L.typed == "MOVE") libcheck_set_aside();
            else L.note = "not moved: type the word MOVE (capital letters), then ENTER";
            if (L.confirm == 1 && L.typed != "MOVE") return;
            return;
        }
        if (!arrow && (key == '\b' || key == 127)) { if (!L.typed.empty()) L.typed.pop_back(); return; }
        if (!arrow && key >= 32 && key < 127 && L.typed.size() < 12) L.typed += static_cast<char>(key);
        return;
    }
    if (!arrow && key == 27) { if (L.page == 1) { L.page = 0; return; } libcheck_close(); return; }
    if (!arrow && (key == 'w' || key == 'W')) { if (L.adopted) libcheck_write_report(); else L.note = "wait until the check is done"; return; }
    if (!arrow && (key == 'c' || key == 'C')) { L.marked.clear(); L.rested = load_rested(); libcheck_orphans(); libcheck_start(); L.note = "checking again ..."; return; }
    if (!arrow && (key == 'u' || key == 'U')) { libcheck_undo(); return; }
    if (L.page == 0) {
        if (!arrow && (key == 'd' || key == 'D' || key == '\r' || key == '\n')) { L.page = 1; L.cg = L.cm = L.view_top = 0; return; }
        if (arrow && key == 'A') L.rep_scroll = std::max(0, L.rep_scroll - 1);
        else if (arrow && key == 'B') ++L.rep_scroll;   // (clamped when drawn)
        else if (key == kKeyHome) L.rep_scroll = 0;
        return;
    }
    // ---- the duplicates page
    if (!L.adopted) return;
    auto& groups = L.result.groups;
    auto shown = [&](int g) { return L.show_rested || !groups[static_cast<size_t>(g)].rested; };
    // make the cursor sit on a shown group
    if (L.cg < 0 || L.cg >= static_cast<int>(groups.size()) || !shown(L.cg)) {
        L.cg = -1;
        for (int g = 0; g < static_cast<int>(groups.size()); ++g) if (shown(g)) { L.cg = g; break; }
        L.cm = 0;
        if (L.cg < 0) return;
    }
    auto step_group = [&](int dir) {
        int g = L.cg;
        for (;;) { g += dir; if (g < 0 || g >= static_cast<int>(groups.size())) return false; if (shown(g)) { L.cg = g; L.cm = dir > 0 ? 0 : static_cast<int>(groups[static_cast<size_t>(g)].members.size()) - 1; return true; } }
    };
    if (arrow && key == 'A') { if (L.cm > 0) --L.cm; else step_group(-1); return; }
    if (arrow && key == 'B') { if (L.cm + 1 < static_cast<int>(groups[static_cast<size_t>(L.cg)].members.size())) ++L.cm; else step_group(+1); return; }
    if (!arrow && (key == 'n' || key == 'N')) { step_group(+1); L.cm = 0; return; }
    if (!arrow && (key == 'p' || key == 'P')) { step_group(-1); L.cm = 0; return; }
    if (!arrow && (key == 't' || key == 'T')) { L.show_rested = !L.show_rested; L.note = L.show_rested ? "the groups that rest are listed too ([r] on one lets it be listed again)" : "the groups that rest are hidden"; return; }
    LcGroup& g = groups[static_cast<size_t>(L.cg)];
    const std::string& cur = L.result.files[static_cast<size_t>(g.members[static_cast<size_t>(L.cm)].file)].path;
    if (!arrow && (key == 'r' || key == 'R')) { libcheck_rest(L.cg); if (!shown(L.cg)) { if (!step_group(+1)) step_group(-1); L.cm = 0; } return; }
    if (!arrow && key == ' ') {
        if (g.rested) { L.note = "this group rests: [r] lists it again first"; return; }
        if (L.marked.count(cur)) { L.marked.erase(cur); return; }
        int unmarked = 0;
        for (const LcMember& m : g.members) if (!L.marked.count(L.result.files[static_cast<size_t>(m.file)].path)) ++unmarked;
        if (unmarked <= 1) { L.note = "at least one file of a group has to stay: unmark another first"; return; }
        L.marked.insert(cur);
        return;
    }
    if (!arrow && (key == 's' || key == 'S')) {   // a suggestion: the identical copies that lie in the SAME folder as the file that stays (the accidental "song (1).mp3")
        int n = 0;
        for (const LcGroup& x : groups) {
            if (x.rested || x.kind != LcKind::Identical) continue;
            const std::string keep_folder = path_utf8(path_from_utf8(L.result.files[static_cast<size_t>(x.members[0].file)].path).parent_path());
            for (size_t i = 1; i < x.members.size(); ++i) {
                const std::string& p = L.result.files[static_cast<size_t>(x.members[i].file)].path;
                if (x.members[i].link == static_cast<int>(LcKind::Identical) + 1 && path_utf8(path_from_utf8(p).parent_path()) == keep_folder) { L.marked.insert(p); ++n; }
            }
        }
        L.note = n ? std::to_string(n) + " marked: byte-identical copies in the same folder as the file that stays -- look them over; nothing moves until [m] and MOVE" : "nothing to suggest: no identical copy lies in the same folder as its original";
        return;
    }
    if (!arrow && (key == 'x' || key == 'X')) { L.marked.clear(); L.note = "all marks removed"; return; }
    if (!arrow && (key == 'm' || key == 'M')) {
        if (L.marked.empty()) { L.note = "nothing is marked: SPACE marks a file to be set aside"; return; }
        L.confirm = 1;
        L.typed.clear();
        return;
    }
}

// ---------------------------------------------------------------------------------------------------------------- the panel

std::vector<std::string> App::build_libcheck_panel(int W) const {
    const LcState& L = *lc_;
    const int Wp = std::clamp(W - 4, 72, 132);
    const int inner = Wp - 4;
    const int max_rows = std::max(12, term_rows_ - 6);   // (the panel's rows without its top and bottom lines)
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m", BD = "\x1b[1m", YE = "\x1b[33m", RE = "\x1b[31m", DM = "\x1b[2m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, const std::string& style = std::string()) {
        const std::string body = fit(plain, inner);
        return bar + " " + (style.empty() ? body : style + body + R) + " " + bar;
    };
    std::vector<std::string> lines;
    const bool dups = L.page == 1;
    lines.push_back(box_top(dups ? "CLEAN UP DUPLICATES  (nothing is deleted)" : "MESSY LIBRARY?  --  REPORT", Wp, border));

    // progress of the background check
    std::string progress;
    if (L.job && !L.adopted) {
        const int st = L.job->progress.stage.load(), d = L.job->progress.done.load(), t = L.job->progress.total.load();
        progress = st <= 1 ? "Looking at the files ... " + std::to_string(d) + " / " + std::to_string(t)
                 : st == 2 ? "Comparing the contents of files of the same size ... " + std::to_string(d) + " / " + std::to_string(t)
                 : "Comparing tags and lengths ...";
    }

    // -- confirm question
    if (L.confirm == 1) {
        int n = static_cast<int>(L.marked.size());
        lines.push_back(row(""));
        lines.push_back(row("SET ASIDE " + std::to_string(n) + " FILE" + (n == 1 ? "" : "S") + "?", BD));
        lines.push_back(row(""));
        for (const std::string& l : wrap_lines("Nothing is deleted. The marked files are moved into a folder named \"" + std::string(kSetAsideFolder) + "\", inside the library folder each one came from; the library does not list them any more. "
                                                  "What they had of their own (plays, their style) goes to the copy that stays. A list of where each file came from is written next to them, and [u] puts the newest batch back. "
                                                  "You delete that folder yourself, when you are sure.", inner - 2, 8)) lines.push_back(row("  " + l));
        lines.push_back(row(""));
        lines.push_back(row("  Type the word MOVE and press ENTER (ESC cancels):   " + L.typed + "_", BD));
        if (!L.note.empty()) { lines.push_back(row("")); lines.push_back(row("  " + L.note, YE)); }
        lines.push_back(row(""));
        lines.push_back(centred_bottom(settings_, Wp, "[ENTER] move after typing MOVE  [ESC] cancel", border_bottom));
        return lines;
    }

    if (!dups) {
        // ---- the report page
        std::vector<std::string> body;
        if (!progress.empty()) body.push_back(progress);
        if (L.adopted) {
            if (L.job && L.job->tags_missing > 0)
                body.push_back("Note: the tags of " + std::to_string(L.job->tags_missing) + " files were not read yet when this started (the library reads them in the background). [c] checks again with more.");
            const std::vector<std::string> rep = lc_report_lines(L.result, false);
            body.insert(body.end(), rep.begin(), rep.end());
        }
        body.push_back("");
        for (const std::string& s : L.orphan_short) body.push_back(s);
        body.push_back("");
        for (const std::string& s : wrap_lines("A duplicate is never found by its name alone: the same bytes, the same download (video id), or the same artist + title AND the same length. "
                                                 "One name with clearly different lengths (an intro, a live version, a remix) is left alone. Nothing here deletes a file.", inner - 2, 5)) body.push_back(s);
        const int vis = std::max(4, max_rows - 4);
        const int scroll = std::clamp(L.rep_scroll, 0, std::max(0, static_cast<int>(body.size()) - vis));
        lines.push_back(row(""));
        for (int i = 0; i < vis; ++i) {
            const int idx = scroll + i;
            if (idx >= static_cast<int>(body.size())) { lines.push_back(row("")); continue; }
            const std::string& b = body[static_cast<size_t>(idx)];
            const bool head = !b.empty() && b[0] != ' ' && b.find("Files looked") != 0 && b.find("Note:") != 0 && b.find("Looking") != 0 && b.find("Comparing") != 0 && b.find("A duplicate") != 0 && b.find("One name") != 0;
            lines.push_back(row(b, head ? BD : std::string()));
        }
        lines.push_back(row(L.note.empty() ? (static_cast<int>(body.size()) > vis ? "[UP/DOWN] scrolls" : "") : L.note, YE));
        lines.push_back(centred_bottom(settings_, Wp, "[d] clean up duplicates  [w] write all to a .txt file  [c] check again  [u] put the last batch back  [ESC] back", border_bottom));
        return lines;
    }

    // ---- the duplicates page
    if (!L.adopted) {
        lines.push_back(row(""));
        lines.push_back(row(progress, BD));
        lines.push_back(row(""));
        for (int i = 0; i < 6; ++i) lines.push_back(row(""));
        lines.push_back(centred_bottom(settings_, Wp, "[ESC] back to the report", border_bottom));
        return lines;
    }
    const auto& groups = L.result.groups;
    struct Rw { int g = -1, m = -1; };   // m < 0: the header of group g
    std::vector<Rw> rows;
    int cursor_row = 0, n_shown = 0, shown_before = 0;
    for (int g = 0; g < static_cast<int>(groups.size()); ++g) {
        if (!L.show_rested && groups[static_cast<size_t>(g)].rested) continue;
        ++n_shown;
        if (g < L.cg) ++shown_before;
        rows.push_back({g, -1});
        for (int m = 0; m < static_cast<int>(groups[static_cast<size_t>(g)].members.size()); ++m) {
            if (g == L.cg && m == L.cm) cursor_row = static_cast<int>(rows.size());
            rows.push_back({g, m});
        }
    }
    if (rows.empty()) {
        lines.push_back(row(""));
        lines.push_back(row(L.result.n_rested > 0 ? "No duplicates are listed: " + std::to_string(L.result.n_rested) + " group(s) rest ([t] shows them)." : "No duplicates found.", BD));
        lines.push_back(row(""));
        for (const std::string& l : wrap_lines("Checked: the same bytes, the same download (video id), and the same artist + title with the same length. A file name alone is never taken as proof.", inner - 2, 3)) lines.push_back(row("  " + l));
        for (int i = 0; i < 4; ++i) lines.push_back(row(""));
        lines.push_back(centred_bottom(settings_, Wp, "[c] check again  [u] put the last batch back  [ESC] back to the report", border_bottom));
        return lines;
    }
    // the lines under the list, wrapped (nothing is cut off): the file under the cursor, the note, the legend
    std::vector<std::pair<std::string, std::string>> foot_lines;
    {
        const LcGroup& g = groups[static_cast<size_t>(L.cg)];
        const LcMember& mm = g.members[static_cast<size_t>(L.cm)];
        const LcFile& f = L.result.files[static_cast<size_t>(mm.file)];
        const std::string why = mm.link == 0 ? "This is the one that stays by default (a name without a copy mark, then the shorter path)."
                        : mm.link == static_cast<int>(LcKind::Identical) + 1 ? "It has exactly the same bytes as the first file."
                        : mm.link == static_cast<int>(LcKind::SameId) + 1 ? "It is the same YouTube download as the first file (same video id)."
                        : "It has the same artist + title and the same length as the first file.";
        const std::string tags = f.artist.empty() && f.title.empty() ? "no tags" : f.artist + " - " + f.title;
        for (const std::string& l : wrap_lines(f.path, inner, 2)) foot_lines.push_back({l, DM});
        {   // artist - title scrolls when it is too long; size and length stay at the end
            const std::string sz = "   " + lc_size_text(f.size) + "   " + lc_dur_text(f.dur_ms);
            const int tw = std::max(10, inner - display_width(sz));
            foot_lines.push_back({fit(marquee_or_truncate(tags, tw, L.cg * 4096 + L.cm, L.marq_idx[1], L.marq_since[1]), tw) + sz, DM});
        }
        for (const std::string& l : wrap_lines(why, inner, 2)) foot_lines.push_back({l, DM});
        if (!L.note.empty()) for (const std::string& l : wrap_lines(L.note, inner, 2)) foot_lines.push_back({l, YE});
        const size_t legend_at = foot_lines.size();
        const std::string legend = "[m] SET ASIDE moves the marked files out of the library into a folder: nothing is deleted, [u] puts them back. "
                                   "[r] LET IT REST keeps every file where it is and only stops listing this group: for duplicates you want on purpose "
                                   "(an album version and a single that share tracks); a new copy brings the group back. "
                                   "[SPACE] mark  [s] mark the same-folder copies  [x] unmark all  [t] show / hide resting groups  [ESC] back. "
                                   "Yellow rows: another file of the group lies in the same folder (often an accidental copy).";
        for (const std::string& l : wrap_lines(legend, inner, 7)) foot_lines.push_back({l, DM});
        while (foot_lines.size() < 12) foot_lines.insert(foot_lines.begin() + static_cast<std::ptrdiff_t>(legend_at), {std::string(), std::string()});   // a steady height: the list does not jump while the cursor moves
    }
    const int foot = static_cast<int>(foot_lines.size());
    const int vis = std::max(5, max_rows - foot - 2);
    int top = std::clamp(L.view_top, 0, std::max(0, static_cast<int>(rows.size()) - vis));
    // keep the cursor in view (with its group header when it is the first member)
    const int want_from = (L.cm == 0 && cursor_row > 0) ? cursor_row - 1 : cursor_row;
    if (want_from < top) top = want_from;
    if (cursor_row >= top + vis) top = cursor_row - vis + 1;
    top = std::clamp(top, 0, std::max(0, static_cast<int>(rows.size()) - vis));
    const_cast<LcState&>(L).view_top = top;
    lines.push_back(row("group " + std::to_string(shown_before + 1) + " of " + std::to_string(n_shown) + "     marked to be set aside: " + std::to_string(L.marked.size()) +
                        (L.show_rested ? "     (the groups that rest are shown)" : ""), DM));
    for (int i = 0; i < vis; ++i) {
        const int idx = top + i;
        if (idx >= static_cast<int>(rows.size())) { lines.push_back(row("")); continue; }
        const Rw& rw = rows[static_cast<size_t>(idx)];
        const LcGroup& g = groups[static_cast<size_t>(rw.g)];
        if (rw.m < 0) {
            const char* kn = g.kind == LcKind::Identical ? "IDENTICAL COPIES" : g.kind == LcKind::SameId ? "SAME DOWNLOAD (video id)" : "SAME SONG, OTHER FILE (artist + title + length)";
            lines.push_back(row(std::string(kn) + "   " + std::to_string(g.members.size()) + " files   extra: " + lc_size_text(g.extra_bytes) +
                                (g.rested ? "   [LET REST]" : "") + (g.any_same_folder() ? "   some in the SAME FOLDER" : ""), BD));
            continue;
        }
        const LcMember& mm = g.members[static_cast<size_t>(rw.m)];
        const LcFile& f = L.result.files[static_cast<size_t>(mm.file)];
        const bool marked = L.marked.count(f.path) > 0;
        const bool cur = rw.g == L.cg && rw.m == L.cm;
        const std::string tail = lc_size_text(f.size);
        const int path_w = std::max(10, inner - 4 - 1 - 9 - 1 - 5 - 1 - 12);
        const std::string flag = mm.same_folder ? "SAME FOLDER" : (mm.link == 0 ? "first" : "");
        // the row under the cursor scrolls its whole path when it does not fit (Marquee Scrolling), the others are cut at the front
        const std::string shown_path = cur ? fit(marquee_or_truncate(f.path, path_w, L.cg * 4096 + L.cm, L.marq_idx[0], L.marq_since[0]), path_w) : fit(tail_fit(f.path, path_w), path_w);
        const std::string plain = std::string(marked ? "[x] " : "[ ] ") + shown_path + " " + fit(tail, 9) + " " + fit(lc_dur_text(f.dur_ms), 5) + " " + fit(flag, 12);
        std::string style = cur ? HI : std::string();
        if (!cur) style = marked ? RE : mm.same_folder ? YE : std::string();
        else if (marked) style = HI + RE;
        lines.push_back(row(plain, style));
    }
    for (const auto& fl : foot_lines) lines.push_back(row(fl.first, fl.second));
    lines.push_back(centred_bottom(settings_, Wp, "[UP/DOWN] [SPACE] mark  [m] set aside  [r] let it rest  [ESC] back", border_bottom));
    return lines;
}

} // namespace muisc
