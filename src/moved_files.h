// Finding a file again that was moved or renamed outside Mousiki: by its file name, and only when that is unambiguous.
// (The same rule as the pairs of tracks and videos: VideoLinks::mend.) Used for the data that is kept per file path:
// a track's own style, the listening history.
#pragma once
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

// For each path of `old_paths` that is not there any more (a path that exists is never touched), the path of the one file in
// `existing` that has the same file name. Nothing is guessed: no match, or two files of that name, leave the path out of the result.
// Two old paths never get the same target. (A target exists, so it is never itself a missing old path: no chains.)
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
    for (const std::string& o : missing) {
        const auto range = by_name.equal_range(moved_name_key(o));
        if (range.first == range.second || std::next(range.first) != range.second) continue;
        found[o] = range.first->second;
        ++wanted[range.first->second];
    }
    for (const auto& kv : found) if (wanted[kv.second] == 1) out[kv.first] = kv.second;
    return out;
}

}  // namespace muisc
