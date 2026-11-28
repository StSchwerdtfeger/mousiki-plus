#include "video_links.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "moved_files.h"
#include "path_utf8.h"
#include "settings.h"

namespace muisc {
namespace fs = std::filesystem;

struct VideoLinks::Impl {
    mutable std::mutex m;
    std::map<std::string, std::string> links;   // track -> video
    fs::path file;
    bool loaded = false;
};

VideoLinks::VideoLinks() : d_(new Impl) {}

VideoLinks& VideoLinks::get() {
    static VideoLinks v;
    return v;
}

fs::path VideoLinks::file() const {
    std::lock_guard<std::mutex> lk(d_->m);
    if (d_->file.empty()) return config_path().parent_path() / "video_links.txt";
    return d_->file;
}

void VideoLinks::load_locked() {
    d_->links.clear();
    d_->loaded = true;
    fs::path f = d_->file.empty() ? config_path().parent_path() / "video_links.txt" : d_->file;
    std::ifstream in(f, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) continue;
        d_->links[line.substr(0, tab)] = line.substr(tab + 1);
    }
}

void VideoLinks::save_locked() const {
    fs::path f = d_->file.empty() ? config_path().parent_path() / "video_links.txt" : d_->file;
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    if (d_->links.empty()) { fs::remove(f, ec); return; }
    fs::path tmp = f;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (const auto& kv : d_->links) out << kv.first << '\t' << kv.second << '\n';
        if (!out.good()) { out.close(); fs::remove(tmp, ec); return; }
    }
    fs::rename(tmp, f, ec);
    if (ec) fs::remove(tmp, ec);
}

void VideoLinks::use_file(const fs::path& p) {
    std::lock_guard<std::mutex> lk(d_->m);
    d_->file = p;
    load_locked();
}

void VideoLinks::reload() {
    std::lock_guard<std::mutex> lk(d_->m);
    load_locked();
}

std::string VideoLinks::video_for(const std::string& track) const {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) const_cast<VideoLinks*>(this)->load_locked();
    auto it = d_->links.find(track);
    return it == d_->links.end() || it->second == "-" ? std::string() : it->second;
}

bool VideoLinks::is_none(const std::string& track) const {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) const_cast<VideoLinks*>(this)->load_locked();
    auto it = d_->links.find(track);
    return it != d_->links.end() && it->second == "-";
}

void VideoLinks::mark_none(const std::string& track) {
    if (track.empty()) return;
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) load_locked();
    d_->links[track] = "-";
    save_locked();
}

std::vector<std::string> VideoLinks::tracks_for(const std::string& video) const {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) const_cast<VideoLinks*>(this)->load_locked();
    std::vector<std::string> out;
    for (const auto& kv : d_->links) if (kv.second == video && kv.second != "-") out.push_back(kv.first);
    return out;
}

std::vector<std::pair<std::string, std::string>> VideoLinks::all() const {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) const_cast<VideoLinks*>(this)->load_locked();
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& kv : d_->links) if (kv.second != "-") out.push_back(kv);
    return out;
}

bool VideoLinks::has(const std::string& track) const { return !video_for(track).empty(); }

size_t VideoLinks::size() const {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) const_cast<VideoLinks*>(this)->load_locked();
    return d_->links.size();
}

void VideoLinks::set(const std::string& track, const std::string& video) {
    if (track.empty()) return;
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) load_locked();
    if (video.empty()) d_->links.erase(track);
    else d_->links[track] = video;
    save_locked();
}

bool VideoLinks::remove(const std::string& track) {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) load_locked();
    const bool had = d_->links.erase(track) > 0;
    if (had) save_locked();
    return had;
}

int VideoLinks::rename_paths(const std::vector<std::pair<std::string, std::string>>& renames) {
    if (renames.empty()) return 0;
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) load_locked();
    std::map<std::string, std::string> out;
    int changed = 0;
    auto map_one = [&](const std::string& p) {
        for (const auto& r : renames) if (r.first == p) return r.second;
        return p;
    };
    for (const auto& kv : d_->links) {
        const std::string t = map_one(kv.first), v = map_one(kv.second);
        if (t != kv.first || v != kv.second) ++changed;
        out[t] = v;
    }
    if (changed) { d_->links = std::move(out); save_locked(); }
    return changed;
}

int VideoLinks::rename_folder(const std::string& from, const std::string& to) {
    if (from.empty() || from == to) return 0;
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) load_locked();
    auto below = [&](const std::string& p) { return p.size() > from.size() && p.compare(0, from.size(), from) == 0 && (p[from.size()] == '/' || p[from.size()] == '\\'); };
    std::map<std::string, std::string> out;
    int changed = 0;
    for (const auto& kv : d_->links) {
        std::string t = kv.first, v = kv.second;
        if (below(t)) { t = to + t.substr(from.size()); }
        if (below(v)) { v = to + v.substr(from.size()); }
        if (t != kv.first || v != kv.second) ++changed;
        out[t] = v;
    }
    if (changed) { d_->links = std::move(out); save_locked(); }
    return changed;
}

int VideoLinks::mend(const std::vector<std::string>& existing_videos) {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) load_locked();
    std::set<std::string> vids;
    for (const auto& kv : d_->links) vids.insert(kv.second);
    const std::map<std::string, std::string> moves = find_moved(vids, existing_videos);   // (by name; a tie or a rename by size and length: moved_files.h)
    int changed = 0;
    for (auto& kv : d_->links) {
        const auto it = moves.find(kv.second);
        if (it == moves.end()) continue;
        kv.second = it->second;
        ++changed;
    }
    if (changed) save_locked();
    return changed;
}

int VideoLinks::mend_tracks(const std::vector<std::string>& existing_tracks) {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) load_locked();
    std::set<std::string> keys;
    for (const auto& kv : d_->links) keys.insert(kv.first);
    const std::map<std::string, std::string> found = find_moved(keys, existing_tracks);
    int changed = 0;
    std::vector<std::pair<std::string, std::string>> moves;   // old key, new key
    for (const auto& m : found) {
        if (d_->links.count(m.second)) continue;   // (that song has its own link already)
        moves.emplace_back(m.first, m.second);
    }
    for (const auto& m : moves) {
        const auto it = d_->links.find(m.first);
        if (it == d_->links.end()) continue;
        const std::string v = it->second;
        d_->links.erase(it);
        d_->links[m.second] = v;
        ++changed;
    }
    if (changed) save_locked();
    return changed;
}

}  // namespace muisc
