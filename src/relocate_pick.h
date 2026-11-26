#pragma once
// Which file replaces a missing playlist entry: `candidates` are the paths in the library with the same file name as the missing one.
// The one in a folder of the same name wins; otherwise a single candidate is taken; with several candidates and no clear one nothing is
// taken (a missing "01 Intro.mp3" must not turn into another album's "01 Intro.mp3"). Returns the index, or -1.
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "path_utf8.h"
#include "utf8_util.h"

namespace muisc {

inline int pick_relocated_track(const std::filesystem::path& missing, const std::vector<std::filesystem::path>& candidates) {
    if (candidates.empty()) return -1;
    const std::string old_dir = ascii_lower_str(path_utf8(missing.parent_path().filename()));
    int pick = -1;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (ascii_lower_str(path_utf8(candidates[i].parent_path().filename())) != old_dir) continue;
        if (pick >= 0) return -1;   // two folders of that name: not clear either
        pick = static_cast<int>(i);
    }
    if (pick >= 0) return pick;
    return candidates.size() == 1 ? 0 : -1;
}

}  // namespace muisc
