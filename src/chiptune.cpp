// Retro formats (tracker modules, console music via libgme, Commodore 64 SID): recognises these files,
// reads SID headers, renders them to WAV with ffmpeg and checks which decoders ffmpeg was built with.
#include "app_dirs.h"
#include "chiptune.h"
#include "path_utf8.h"
#include "process_util.h"
#include "utf8_util.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace muisc {

namespace {

std::string ext_of(const fs::path& p) { return ascii_lower_str(path_utf8(p.extension())); }

// Latin-1 -> UTF-8 (SID header strings), trimmed at the first NUL.
std::string latin1_to_utf8(const char* s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n && s[i]; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) out += static_cast<char>(c);
        else { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace

bool is_tracker_file(const fs::path& p) {
    static const std::set<std::string> exts = {
        ".mod", ".xm", ".it", ".s3m", ".mptm", ".stm", ".669", ".mtm", ".med", ".okt", ".far", ".ult", ".ams",
        ".dbm", ".digi", ".dmf", ".dsm", ".gdm", ".imf", ".j2b", ".mdl", ".mt2", ".psm", ".ptm", ".umx", ".plm"};
    return exts.count(ext_of(p)) > 0;
}

bool is_gme_file(const fs::path& p) {
    static const std::set<std::string> exts = {".nsf", ".nsfe", ".spc", ".gbs", ".vgm", ".vgz", ".ay", ".hes", ".kss", ".sap", ".gym"};
    return exts.count(ext_of(p)) > 0;
}

bool is_sid_file(const fs::path& p) {
    const std::string e = ext_of(p);
    return e == ".sid" || e == ".psid" || e == ".rsid";
}

bool is_chiptune_file(const fs::path& p) { return is_tracker_file(p) || is_gme_file(p) || is_sid_file(p); }

SidInfo read_sid_header(const fs::path& sid) {
    SidInfo info;
    std::ifstream in(sid, std::ios::binary);
    char h[0x76] = {};
    if (!in.read(h, sizeof h)) return info;
    const std::string magic(h, 4);
    if (magic != "PSID" && magic != "RSID") return info;
    auto be16 = [&](int off) { return (static_cast<unsigned char>(h[off]) << 8) | static_cast<unsigned char>(h[off + 1]); };
    info.ok = true;
    info.rsid = magic == "RSID";
    info.songs = be16(0x0E);
    info.start_song = be16(0x10);
    info.name = latin1_to_utf8(h + 0x16, 32);
    info.author = latin1_to_utf8(h + 0x36, 32);
    info.released = latin1_to_utf8(h + 0x56, 32);
    return info;
}

// FNV-1a: the same value on every compiler and library version (std::hash is not), so cached tunes are found again after an update.
std::string sid_cache_hash(const std::string& s) {
    unsigned long long h = 14695981039346656037ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", h);
    return buf;
}

// A rendered tune is 30-35 MB, and nothing else ever removes them: keep the newest ones up to `limit` bytes in total.
void sid_cache_prune(const fs::path& dir, const fs::path& keep, unsigned long long limit) {
    std::error_code ec;
    struct Entry { fs::path path; fs::file_time_type when; unsigned long long size; };
    std::vector<Entry> files;
    unsigned long long total = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string ext = path_utf8(it->path().extension());
        if (ext != ".wav") continue;
        Entry e{it->path(), it->last_write_time(e2), static_cast<unsigned long long>(it->file_size(e2))};
        if (e2) continue;
        if (e.path == keep) { total += e.size; continue; }
        total += e.size;
        files.push_back(e);
    }
    if (total <= limit) return;
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.when < b.when; });   // oldest first
    for (const auto& e : files) {
        if (total <= limit) break;
        std::error_code e3;
        if (fs::remove(e.path, e3)) total -= std::min(total, e.size);
    }
}

bool render_sid_to_wav(const fs::path& sid, int seconds, fs::path& out_wav, std::string* err) {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = std::getenv("USERPROFILE");
    const fs::path dir = muisc::app_dirs::cache() / "sid";
    std::error_code ec;
    fs::create_directories(dir, ec);
    // Cache key: the file's path, size and modification time plus the length -- a changed file or length renders anew.
    const auto size = fs::file_size(sid, ec);
    // long long on purpose: macOS' libc++ counts file times in __int128, which std::to_string does not take.
    const long long mtime = static_cast<long long>(fs::last_write_time(sid, ec).time_since_epoch().count());
    const std::string key = path_utf8(sid) + "|" + std::to_string(size) + "|" + std::to_string(mtime) + "|" + std::to_string(seconds);
    const std::string name = path_utf8(sid.stem()) + "_" + sid_cache_hash(key) + ".wav";
    out_wav = dir / path_from_utf8(name);
    if (fs::exists(out_wav, ec) && fs::file_size(out_wav, ec) > 44) {
        fs::last_write_time(out_wav, fs::file_time_type::clock::now(), ec);   // used again: the newest, so the oldest go first
        return true;
    }

    const fs::path tmp = fs::path(out_wav).concat(".part");
    // -q: no time display, -t<sec>: play length, -w<file>: write a WAV instead of playing (as fast as the machine can).
    ProcResult r = run_capture({"sidplayfp", "-q", "-t" + std::to_string(std::max(5, seconds)), "-w" + path_utf8(tmp),
                                path_utf8(sid)}, true);
    if (!fs::exists(tmp, ec) || fs::file_size(tmp, ec) <= 44) {
        fs::remove(tmp, ec);
        if (err) {
            const bool missing = r.out.find("not found") != std::string::npos || r.out.find("not recognized") != std::string::npos
                              || r.exit_code == 127 || r.exit_code == 9009;
            *err = missing ? "SID tunes need the sidplayfp program on the PATH (Linux: package sidplayfp, macOS: brew install sidplayfp)"
                           : "sidplayfp could not render this tune";
        }
        return false;
    }
    fs::rename(tmp, out_wav, ec);
    if (ec) { if (err) *err = "cannot write " + path_utf8(out_wav); return false; }
    sid_cache_prune(dir, out_wav, 1000ULL * 1000 * 1000);   // about 1 GB of rendered tunes at most
    return true;
}

bool ffmpeg_has_demuxer(const std::string& name) {
    static std::mutex m;
    static std::map<std::string, bool> cache;
    static std::string demuxers;
    static bool asked = false;
    std::lock_guard<std::mutex> lk(m);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    if (!asked) { demuxers = run_capture({"ffmpeg", "-hide_banner", "-demuxers"}, false).out; asked = true; }
    std::istringstream in(demuxers);
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string flags, n;
        ls >> flags >> n;
        if (n == name) { found = true; break; }
    }
    cache[name] = found;
    return found;
}

std::string chiptune_support_problem(const fs::path& p) {
    if (is_tracker_file(p) && !ffmpeg_has_demuxer("libopenmpt"))
        return "tracker modules need an ffmpeg built with libopenmpt (most Linux packages, the Windows \"full\" builds)";
    if (is_gme_file(p) && !ffmpeg_has_demuxer("libgme"))
        return "game music files need an ffmpeg built with libgme (most Linux packages, the Windows \"full\" builds)";
    return "";
}

} // namespace muisc
