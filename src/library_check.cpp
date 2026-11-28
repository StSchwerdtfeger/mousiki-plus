// Library check: duplicate groups and the report text. See library_check.h.
#include "library_check.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <numeric>
#include <unordered_map>

#include "path_utf8.h"
#include "text_edit.h"
#include "utf8_util.h"

namespace muisc {

uint64_t lc_hash_file(const std::string& path_utf8_s, const std::atomic<bool>* cancel) {
    std::ifstream in(path_from_utf8(path_utf8_s), std::ios::binary);
    if (!in) return 0;
    uint64_t h = 1469598103934665603ull;
    std::vector<char> buf(1 << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) { h ^= static_cast<unsigned char>(buf[static_cast<size_t>(i)]); h *= 1099511628211ull; }
        if (cancel && cancel->load()) return 0;
    }
    return h ? h : 1;
}

namespace {

std::string stem_of(const std::string& path) { return path_utf8(path_from_utf8(path).stem()); }
std::string folder_of(const std::string& path) { return path_utf8(path_from_utf8(path).parent_path()); }

// "Song (1)", "Song - Copy", "Song copy 2", "Song [dQw4w9WgXcQ]" -> "Song"
std::string strip_copy_marks(std::string s) {
    for (int guard = 0; guard < 4; ++guard) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '_')) s.pop_back();
        const size_t n = s.size();
        if (n > 2 && s.back() == ')') {   // " (1)" / " (12)"
            const size_t open = s.rfind('(');
            if (open != std::string::npos && open + 2 < n && s.find_first_not_of("0123456789", open + 1) == n - 1) { s.resize(open); continue; }
        }
        if (n > 2 && s.back() == ']') {   // " [id]"
            const size_t open = s.rfind('[');
            if (open != std::string::npos && n - open >= 7 && n - open <= 40) { s.resize(open); continue; }
        }
        const std::string low = ascii_lower_str(s);
        bool cut = false;
        for (const char* m : {" - copy", "-copy", " copy", " - kopie", " kopie", "_copy"}) {
            const std::string mk = m;
            const size_t at = low.rfind(mk);
            if (at != std::string::npos && at > 0) {
                const std::string tail = low.substr(at + mk.size());
                if (tail.find_first_not_of(" 0123456789()") == std::string::npos) { s.resize(at); cut = true; break; }
            }
        }
        if (!cut) break;
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '_')) s.pop_back();
    return s;
}

bool has_copy_mark(const std::string& stem) { return strip_copy_marks(stem) != stem; }

std::string collapse(const std::string& s) {
    std::string o;
    bool sp = true;
    for (char c : s) {
        if (c == ' ') { if (!sp) o += ' '; sp = true; }
        else { o += c; sp = false; }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

int letters_in(const std::string& s) {   // bytes of letters (a UTF-8 letter counts every byte: enough for "is there anything to compare by")
    int n = 0;
    for (unsigned char c : s) if (std::isalpha(c) || c >= 0x80) ++n;
    return n;
}

struct Dsu {
    std::vector<int> p;
    std::vector<int> weakest;   // per root: the weakest evidence used (LcKind as int)
    explicit Dsu(size_t n) : p(n), weakest(n, 0) { std::iota(p.begin(), p.end(), 0); }
    int find(int x) { while (p[static_cast<size_t>(x)] != x) { p[static_cast<size_t>(x)] = p[static_cast<size_t>(p[static_cast<size_t>(x)])]; x = p[static_cast<size_t>(x)]; } return x; }
    void join(int a, int b, int kind) {
        a = find(a); b = find(b);
        if (a == b) return;   // already held together by evidence at least as strong as this one
        const int w = std::max(kind, std::max(weakest[static_cast<size_t>(a)], weakest[static_cast<size_t>(b)]));
        if (a != b) p[static_cast<size_t>(b)] = a;
        weakest[static_cast<size_t>(a)] = w;
    }
};

}  // namespace

std::string lc_name_key(const LcFile& f) {
    if (!f.artist.empty() && !f.title.empty()) return "t:" + collapse(normalize_for_search(f.artist)) + "|" + collapse(normalize_for_search(strip_copy_marks(f.title)));
    std::string s = collapse(normalize_for_search(strip_copy_marks(stem_of(f.path))));
    if (letters_in(s) < 3) return {};
    // a name alone is only used when it is a real name: "intro", "outro", "interlude" are in many albums and are not the same song
    if (s.find(' ') == std::string::npos || letters_in(s) < 8) return {};
    // "track 05", "titel 3", "piste 12", "audio 1": numbered, not named
    for (const char* p : {"track ", "titel ", "piste ", "pista ", "audio ", "spur ", "title "}) {
        const std::string pre = p;
        if (s.compare(0, pre.size(), pre) == 0 && s.find_first_not_of("0123456789 ", pre.size()) == std::string::npos) return {};
    }
    return "n:" + s;
}

LcResult lc_find_duplicates(std::vector<LcFile> files, const std::vector<std::set<std::string>>& rested,
                            const std::function<uint64_t(const std::string&)>& hash, LcProgress* progress) {
    LcResult R;
    R.files = std::move(files);
    const int n = static_cast<int>(R.files.size());
    auto stage = [&](int s) { if (progress) progress->stage.store(s); };
    auto cancelled = [&] { return progress && progress->cancel.load(); };
    Dsu dsu(static_cast<size_t>(n));
    std::vector<uint64_t> hashes(static_cast<size_t>(n), 0);
    for (const LcFile& f : R.files) { if (f.size > 0) R.total_bytes += f.size; if (f.dur_ms <= 0) ++R.files_without_length; if (f.title.empty() && f.artist.empty()) ++R.files_without_tags; }

    // 1. the same size, then the same bytes
    stage(1);
    std::map<long long, std::vector<int>> by_size;
    for (int i = 0; i < n; ++i) if (R.files[static_cast<size_t>(i)].size > 0) by_size[R.files[static_cast<size_t>(i)].size].push_back(i);
    int to_hash = 0;
    for (const auto& kv : by_size) if (kv.second.size() >= 2) to_hash += static_cast<int>(kv.second.size());
    if (progress) { progress->total.store(to_hash); progress->done.store(0); }
    stage(2);
    for (const auto& kv : by_size) {
        if (kv.second.size() < 2) continue;
        std::map<uint64_t, std::vector<int>> by_hash;
        for (int i : kv.second) {
            if (cancelled()) { stage(4); return R; }
            const uint64_t h = hash(R.files[static_cast<size_t>(i)].path);
            hashes[static_cast<size_t>(i)] = h;
            if (progress) progress->done.fetch_add(1);
            if (h) by_hash[h].push_back(i);
        }
        for (const auto& hv : by_hash) for (size_t k = 1; k < hv.second.size(); ++k) dsu.join(hv.second[0], hv.second[k], static_cast<int>(LcKind::Identical));
    }

    // 2. the same download: one YouTube id in the tags
    stage(3);
    {
        std::unordered_map<std::string, int> first_of;
        for (int i = 0; i < n; ++i) {
            const std::string& v = R.files[static_cast<size_t>(i)].vid;
            if (v.empty()) continue;
            const auto it = first_of.find(v);
            if (it == first_of.end()) first_of.emplace(v, i);
            else dsu.join(it->second, i, static_cast<int>(LcKind::SameId));
        }
    }

    // 3. the same song: the same artist and title (or the same file name) and the same length. And the names with other lengths, counted.
    std::map<std::string, std::vector<int>> by_name;
    for (int i = 0; i < n; ++i) {
        const std::string k = lc_name_key(R.files[static_cast<size_t>(i)]);
        if (!k.empty()) by_name[k].push_back(i);
    }
    constexpr long long kSameLen = 1500;
    for (auto& kv : by_name) {
        if (kv.second.size() < 2) continue;
        std::vector<int> v = kv.second;
        std::vector<int> timed;
        for (int i : v) if (R.files[static_cast<size_t>(i)].dur_ms > 0) timed.push_back(i);
        std::sort(timed.begin(), timed.end(), [&](int a, int b) { return R.files[static_cast<size_t>(a)].dur_ms < R.files[static_cast<size_t>(b)].dur_ms; });
        // clusters of lengths: every member within kSameLen of the first of its cluster
        size_t a = 0;
        int clusters = 0;
        while (a < timed.size()) {
            size_t b = a + 1;
            while (b < timed.size() && R.files[static_cast<size_t>(timed[b])].dur_ms - R.files[static_cast<size_t>(timed[a])].dur_ms <= kSameLen) { dsu.join(timed[a], timed[b], static_cast<int>(LcKind::SameSong)); ++b; }
            ++clusters;
            a = b;
        }
        if (clusters >= 2) {   // one name, clearly different lengths: not duplicates
            ++R.n_same_name_other_length;
            if (R.same_name_examples.size() < 12) {
                std::string ex = kv.first.size() > 2 && kv.first[1] == ':' ? kv.first.substr(2) : kv.first;
                ex += "  (" + std::to_string(timed.size()) + " files:";
                for (size_t q = 0; q < timed.size() && q < 5; ++q) ex += (q ? ", " : " ") + lc_dur_text(R.files[static_cast<size_t>(timed[q])].dur_ms);
                ex += ")";
                R.same_name_examples.push_back(ex);
            }
        }
    }

    // the groups
    std::map<int, std::vector<int>> comps;
    for (int i = 0; i < n; ++i) comps[dsu.find(i)].push_back(i);
    for (auto& kv : comps) {
        if (kv.second.size() < 2) continue;
        LcGroup g;
        g.kind = static_cast<LcKind>(dsu.weakest[static_cast<size_t>(kv.first)]);
        std::vector<int> m = kv.second;
        // the one to keep by default first: a name without a copy mark, then the shorter path
        std::sort(m.begin(), m.end(), [&](int a, int b) {
            const std::string& pa = R.files[static_cast<size_t>(a)].path; const std::string& pb = R.files[static_cast<size_t>(b)].path;
            const bool ca = has_copy_mark(stem_of(pa)), cb = has_copy_mark(stem_of(pb));
            if (ca != cb) return !ca;
            if (pa.size() != pb.size()) return pa.size() < pb.size();
            return pa < pb;
        });
        const int first = m[0];
        std::sort(m.begin() + 1, m.end(), [&](int a, int b) {   // the others: by folder, so those of one folder stand together
            const std::string fa = folder_of(R.files[static_cast<size_t>(a)].path), fb = folder_of(R.files[static_cast<size_t>(b)].path);
            if (fa != fb) return fa < fb;
            return R.files[static_cast<size_t>(a)].path < R.files[static_cast<size_t>(b)].path;
        });
        std::map<std::string, int> per_folder;
        for (int i : m) ++per_folder[folder_of(R.files[static_cast<size_t>(i)].path)];
        const LcFile& F = R.files[static_cast<size_t>(first)];
        for (int i : m) {
            LcMember mm;
            mm.file = i;
            mm.same_folder = per_folder[folder_of(R.files[static_cast<size_t>(i)].path)] >= 2;
            if (i != first) {
                const LcFile& X = R.files[static_cast<size_t>(i)];
                if (X.size == F.size && hashes[static_cast<size_t>(i)] != 0 && hashes[static_cast<size_t>(i)] == hashes[static_cast<size_t>(first)]) mm.link = static_cast<int>(LcKind::Identical) + 1;
                else if (!X.vid.empty() && X.vid == F.vid) mm.link = static_cast<int>(LcKind::SameId) + 1;
                else mm.link = static_cast<int>(LcKind::SameSong) + 1;
                if (X.size > 0) g.extra_bytes += X.size;
            }
            g.members.push_back(mm);
        }
        std::set<std::string> paths;
        for (const LcMember& mm : g.members) paths.insert(R.files[static_cast<size_t>(mm.file)].path);
        for (const auto& rs : rested) {
            if (std::includes(rs.begin(), rs.end(), paths.begin(), paths.end())) { g.rested = true; break; }
        }
        R.groups.push_back(std::move(g));
    }
    std::sort(R.groups.begin(), R.groups.end(), [&](const LcGroup& a, const LcGroup& b) {
        if (a.rested != b.rested) return !a.rested;
        if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
        if (a.any_same_folder() != b.any_same_folder()) return a.any_same_folder();
        if (a.extra_bytes != b.extra_bytes) return a.extra_bytes > b.extra_bytes;
        return R.files[static_cast<size_t>(a.members[0].file)].path < R.files[static_cast<size_t>(b.members[0].file)].path;
    });
    for (const LcGroup& g : R.groups) {
        if (g.rested) { ++R.n_rested; continue; }
        (g.kind == LcKind::Identical ? R.n_identical : g.kind == LcKind::SameId ? R.n_same_id : R.n_same_song) += 1;
        R.extra_bytes_all += g.extra_bytes;
    }
    stage(4);
    return R;
}

std::string lc_size_text(long long b) {
    char buf[32];
    if (b < 0) return "?";
    if (b >= (1LL << 30)) std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(b) / (1 << 30));
    else if (b >= (1LL << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / (1 << 20));
    else std::snprintf(buf, sizeof buf, "%lld KB", b / 1024);
    return buf;
}

std::string lc_dur_text(long long ms) {
    if (ms <= 0) return "-";
    const long long s = (ms + 500) / 1000;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld", s / 60, s % 60);
    return buf;
}

static const char* kind_name(LcKind k) {
    return k == LcKind::Identical ? "identical copy" : k == LcKind::SameId ? "same download (video id)" : "same song, other file";
}

std::vector<std::string> lc_report_lines(const LcResult& r, bool full) {
    std::vector<std::string> L;
    int same_folder_groups = 0;
    for (const LcGroup& g : r.groups) if (!g.rested && g.any_same_folder()) ++same_folder_groups;
    L.push_back("Files looked at: " + std::to_string(r.files.size()) + "  (" + lc_size_text(r.total_bytes) + ")");
    L.push_back("");
    L.push_back("DUPLICATES -- the same song in more than one file");
    L.push_back("  identical copies (the same bytes) ........ " + std::to_string(r.n_identical) + " groups");
    L.push_back("  same download (the same video id) ........ " + std::to_string(r.n_same_id) + " groups");
    L.push_back("  same song, other file (name + length) .... " + std::to_string(r.n_same_song) + " groups");
    L.push_back("  of these, with files in the same folder .. " + std::to_string(same_folder_groups) + " groups   (the likely accidental copies)");
    L.push_back("  let rest (kept on purpose) ............... " + std::to_string(r.n_rested) + " groups");
    L.push_back("  the extra copies take up ................. " + lc_size_text(r.extra_bytes_all) + "   (nothing is removed by this check)");
    L.push_back("");
    L.push_back("NOT DUPLICATES (listed for your information)");
    L.push_back("  one name, clearly different lengths ...... " + std::to_string(r.n_same_name_other_length) + " (an intro, a live version, a remix: left alone)");
    if (r.files_without_length > 0) L.push_back("  files whose length is unknown ............ " + std::to_string(r.files_without_length) + " (they can only be found as copies of each other)");
    if (full) {
        for (const std::string& e : r.same_name_examples) L.push_back("    " + e);
        L.push_back("");
        int gi = 0;
        for (const LcGroup& g : r.groups) {
            ++gi;
            L.push_back("GROUP " + std::to_string(gi) + ": " + kind_name(g.kind) + (g.rested ? "  [let rest]" : "") + (g.any_same_folder() ? "  [files in the same folder]" : "") +
                        "  (" + std::to_string(g.members.size()) + " files, extra copies " + lc_size_text(g.extra_bytes) + ")");
            for (const LcMember& m : g.members) {
                const LcFile& f = r.files[static_cast<size_t>(m.file)];
                L.push_back(std::string(m.link == 0 ? "  keep?  " : "         ") + f.path + "   " + lc_size_text(f.size) + "  " + lc_dur_text(f.dur_ms) + (m.same_folder ? "   [same folder]" : ""));
            }
            L.push_back("");
        }
    }
    return L;
}

}  // namespace muisc
