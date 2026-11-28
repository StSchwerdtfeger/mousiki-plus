#include "file_prints.h"

#include <fstream>
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
    fs::path path() const { return file.empty() ? app_dirs::config() / "file_prints.tsv" : file; }
    void load() {
        loaded = true;
        prints.clear();
        std::ifstream in(path(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            const size_t a = line.rfind('\t');
            if (a == std::string::npos || a == 0) continue;
            const size_t b = line.rfind('\t', a - 1);
            if (b == std::string::npos || b == 0) continue;
            try {
                FilePrint p;
                p.size = std::stoll(line.substr(b + 1, a - b - 1));
                p.dur_ms = std::stoll(line.substr(a + 1));
                prints[line.substr(0, b)] = p;
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
    if (e.size == size && e.dur_ms == dur) return;
    e.size = size;
    e.dur_ms = dur;
    d_->dirty = true;
}

void FilePrints::save_if_dirty() {
    std::lock_guard<std::mutex> lk(d_->m);
    if (!d_->dirty) return;
    d_->dirty = false;
    std::string text;
    for (const auto& kv : d_->prints) text += kv.first + "\t" + std::to_string(kv.second.size) + "\t" + std::to_string(kv.second.dur_ms) + "\n";
    std::error_code ec;
    fs::create_directories(d_->path().parent_path(), ec);
    write_text_atomic(d_->path(), text, /*binary=*/true);
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
