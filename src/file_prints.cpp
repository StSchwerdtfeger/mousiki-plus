#include "file_prints.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <vector>
#include <map>
#include <mutex>

#include "app_dirs.h"
#include "path_utf8.h"
#include "settings.h"

namespace muisc {
namespace fs = std::filesystem;

struct FilePrints::Impl {
    mutable std::mutex m;
    std::map<std::string, FilePrint> prints;
    fs::path file;
    bool loaded = false, dirty = false;
    std::function<long long(const std::string&)> dur_lookup;
    static long long today() { return static_cast<long long>(std::chrono::duration_cast<std::chrono::hours>(std::chrono::system_clock::now().time_since_epoch()).count() / 24); }
    fs::path path() const { return file.empty() ? app_dirs::config() / "file_prints.tsv" : file; }
    void load() {
        loaded = true;
        prints.clear();
        std::ifstream in(path(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            // path <TAB> size <TAB> length [<TAB> day last seen]  (older files have no day: they get today, so the 60 days start now)
            std::vector<size_t> tabs;
            for (size_t q = line.find('\t'); q != std::string::npos; q = line.find('\t', q + 1)) tabs.push_back(q);
            if (tabs.size() < 2) continue;
            const bool with_day = tabs.size() >= 3 && std::all_of(line.begin() + static_cast<long>(tabs.back()) + 1, line.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) && tabs.back() + 1 < line.size();
            const size_t t_len = with_day ? tabs[tabs.size() - 2] : tabs.back();
            const size_t t_size = with_day ? tabs[tabs.size() - 3] : tabs[tabs.size() - 2];
            if (t_size == 0) continue;
            try {
                FilePrint p;
                p.size = std::stoll(line.substr(t_size + 1, t_len - t_size - 1));
                p.dur_ms = std::stoll(line.substr(t_len + 1, with_day ? tabs.back() - t_len - 1 : std::string::npos));
                p.seen_day = with_day ? std::stoll(line.substr(tabs.back() + 1)) : today();
                prints[line.substr(0, t_size)] = p;
            } catch (...) {}
        }
    }
};

FilePrints::FilePrints() : d_(new Impl) {}

FilePrints& FilePrints::get() {
    static FilePrints f;
    return f;
}

FilePrint FilePrints::find(const std::string& p) const {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) d_->load();
    const auto it = d_->prints.find(p);
    return it == d_->prints.end() ? FilePrint{} : it->second;
}

void FilePrints::set(const std::string& p, long long size, long long dur_ms) {
    if (p.empty() || size < 0) return;
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->loaded) d_->load();
    FilePrint& e = d_->prints[p];
    const long long dur = dur_ms >= 0 ? dur_ms : e.dur_ms;
    const long long day = Impl::today();
    if (e.size == size && e.dur_ms == dur && e.seen_day == day) return;
    e.size = size;
    e.dur_ms = dur;
    e.seen_day = day;
    d_->dirty = true;
}

void FilePrints::save_if_dirty() {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->dirty) return;
    d_->dirty = false;
    std::string text;
    for (const auto& kv : d_->prints) if (kv.first.find_first_of("\n\r\t") == std::string::npos) text += kv.first + "\t" + std::to_string(kv.second.size) + "\t" + std::to_string(kv.second.dur_ms) + "\t" + std::to_string(kv.second.seen_day) + "\n";
    std::error_code ec;
    fs::create_directories(d_->path().parent_path(), ec);
    write_text_atomic(d_->path(), text, /*binary=*/true);
}

int FilePrints::forget_missing(int grace_days) {
    std::vector<std::string> old;
    const long long day = Impl::today();
    {
        std::lock_guard<std::mutex> lk(d_->m);
        if (!d_->loaded) d_->load();
        for (const auto& kv : d_->prints) if (day - kv.second.seen_day >= grace_days) old.push_back(kv.first);   // (only these are looked at: no stat of the whole library)
    }
    std::vector<std::string> gone, back;
    for (const std::string& k : old) {
        std::error_code ec;
        const fs::path p = path_from_utf8(k);
        if (fs::exists(p, ec)) { back.push_back(k); continue; }
        if (ec) continue;   // (cannot tell)
        fs::path up = p.parent_path();
        bool own_folder = true;
        while (!up.empty() && !fs::exists(up, ec)) { own_folder = false; const fs::path next = up.parent_path(); if (next == up) { up.clear(); break; } up = next; }
        if (!own_folder) {
            if (up.empty()) continue;                                  // (not even the drive: not connected)
            if (fs::is_empty(up, ec) || ec) continue;                  // (an unmounted mount point is an empty folder)
        }
        gone.push_back(k);
    }
    std::lock_guard<std::mutex> lk(d_->m);
    for (const std::string& k : back) { const auto it = d_->prints.find(k); if (it != d_->prints.end()) it->second.seen_day = day; }
    for (const std::string& k : gone) d_->prints.erase(k);
    if (!back.empty() || !gone.empty()) d_->dirty = true;
    return static_cast<int>(gone.size());
}

void FilePrints::use_file(const fs::path& f) {
    std::lock_guard<std::mutex> lk(d_->m);
    d_->file = f;
    d_->loaded = false;
    d_->dirty = false;
    d_->prints.clear();
}

void FilePrints::set_duration_lookup(std::function<long long(const std::string&)> f) {
    std::lock_guard<std::mutex> lk(d_->m);
    d_->dur_lookup = std::move(f);
}

long long FilePrints::duration_ms_of(const std::string& p) const {
    std::function<long long(const std::string&)> f;
    { std::lock_guard<std::mutex> lk(d_->m); f = d_->dur_lookup; }
    return f ? f(p) : -1;
}

}  // namespace muisc
