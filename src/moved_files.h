// Finding a file again that was moved or renamed outside Mousiki: by its file name, and only when that is unambiguous.
// (The same rule as the pairs of tracks and videos: VideoLinks::mend.) Used for the data that is kept per file path:
// a track's own style, the listening history.
#pragma once
#include "file_prints.h"
#include "path_utf8.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace muisc {

// The file name of a path, lower case (UTF-8 path in, case does not matter).
inline std::string moved_name_key(const std::string& path) {
    std::string n = path_utf8(path_from_utf8(path).filename());
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return n;
}

// How alike two files are by size and duration: 2 = the same size and the same length (within a second), 1 = the same size and one of the
// lengths is not known, 0 = not the same file (another size, or clearly another length).
inline int print_match(const FilePrint& old_print, const std::string& candidate) {
    if (!old_print.known()) return 0;
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path_from_utf8(candidate), ec);
    if (ec || static_cast<long long>(sz) != old_print.size) return 0;
    const long long d = FilePrints::get().duration_ms_of(candidate);
    if (old_print.dur_ms < 0 || d < 0) return 1;
    const long long diff = old_print.dur_ms > d ? old_print.dur_ms - d : d - old_print.dur_ms;
    return diff <= 1000 ? 2 : 0;
}

// For each path of `old_paths` that is not there any more (a path that exists is never touched), the path of the one file in
// `existing` that it became. Nothing is guessed:
//  1. a file name that is the only one of that name in `existing` -- the file was moved;
//  2. several files of that name: the one whose size and length are those the old file had (FilePrints), when exactly one is;
//  3. no file of that name: a file with the size and the length of the old file, when it is the only such file in `existing` and the old
//     path has a known print -- the file was renamed (the size is needed to tell, so the files of `existing` are looked at only then).
// A target that two old paths want is given to none of them. (A target exists, so it is never itself a missing old path: no chains.)
inline std::map<std::string, std::string> find_moved(const std::set<std::string>& old_paths, const std::vector<std::string>& existing) {
    std::map<std::string, std::string> out;
    if (old_paths.empty() || existing.empty()) return out;
    std::vector<std::string> missing;
    for (const std::string& o : old_paths) {
        if (o.empty() || o.compare(0, 3, "yt:") == 0) continue;
        std::error_code ec;
        if (!std::filesystem::exists(path_from_utf8(o), ec)) missing.push_back(o);
    }
    if (missing.empty()) return out;   // (the usual case: nothing to look up)
    std::multimap<std::string, std::string> by_name;
    for (const std::string& e : existing) by_name.emplace(moved_name_key(e), e);
    std::map<std::string, int> wanted;   // target -> how many old paths want it
    std::map<std::string, std::string> found;
    std::vector<std::string> unnamed;    // missing paths with no file of their name: renamed?
    for (const std::string& o : missing) {
        const auto range = by_name.equal_range(moved_name_key(o));
        if (range.first == range.second) { unnamed.push_back(o); continue; }
        if (std::next(range.first) == range.second) { found[o] = range.first->second; ++wanted[range.first->second]; continue; }
        const FilePrint op = FilePrints::get().find(o);   // several of that name: the print decides
        std::string pick;
        int best = 0, n_best = 0;
        for (auto it = range.first; it != range.second; ++it) {
            const int m = print_match(op, it->second);
            if (m > best) { best = m; pick = it->second; n_best = 1; }
            else if (m == best && m > 0) ++n_best;
        }
        if (best > 0 && n_best == 1) { found[o] = pick; ++wanted[pick]; }
    }
    std::map<std::string, std::vector<std::string>> by_size;   // size -> files (made once, and only when a renamed file is looked for)
    bool by_size_made = false;
    for (const std::string& o : unnamed) {
        const FilePrint op = FilePrints::get().find(o);
        if (!op.known() || op.size <= 0) continue;
        if (!by_size_made) {
            by_size_made = true;
            for (const std::string& e : existing) {
                std::error_code ec;
                const auto sz = std::filesystem::file_size(path_from_utf8(e), ec);
                if (!ec) by_size[std::to_string(sz)].push_back(e);
            }
        }
        const auto it = by_size.find(std::to_string(op.size));
        if (it == by_size.end()) continue;
        std::string pick;
        int n = 0;
        for (const std::string& c : it->second) if (print_match(op, c) == 2) { pick = c; ++n; }   // (a renamed file needs the length too)
        if (n == 1) { found[o] = pick; ++wanted[pick]; }
    }
    for (const auto& kv : found) if (wanted[kv.second] == 1) out[kv.first] = kv.second;
    return out;
}

}  // namespace muisc
