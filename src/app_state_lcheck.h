// lc_: the data of the MESSY LIBRARY? overlays (the library report and the duplicates clean-up), grouped. Kept in its own header so that a change
// here only rebuilds the files that use `lc_` (see app.h).
#pragma once
#include "app.h"
#include "library_check.h"

namespace muisc {

struct App::LcState {
    // The check runs in the background (sizes, hashes, tags, lengths); the overlay shows the progress and adopts the result when it is done.
    struct Job {
        LcProgress progress;
        std::vector<LcFile> input;
        std::vector<std::set<std::string>> rested;
        LcResult result;
        std::atomic<bool> finished{false};
        int tags_missing = 0;   // files whose tags were not read yet when the check started
    };
    int page = 0;                       // 0 the report, 1 the duplicates
    std::shared_ptr<Job> job;
    bool adopted = false;
    LcResult result;
    std::vector<std::string> orphan_short, orphan_full;   // the per-track data of files that are gone
    std::vector<std::set<std::string>> rested;            // "let it rest": the sets of files that are kept on purpose
    int rep_scroll = 0;                 // the report page: first line shown
    int cg = 0, cm = 0;                 // the cursor: group (index in result.groups), member
    int view_top = 0;                   // the duplicates page: first row shown
    bool show_rested = false;
    std::set<std::string> marked;       // paths marked "to be set aside"
    int confirm = 0;                    // 1: the question before the move is open
    std::string typed;                  // what is typed to confirm
    std::string note;                   // the last result line (shown in the panel)
    int last_progress = -1;
    mutable int marq_idx[2] = {-1, -1};   // the marquee (long names scroll): the row / the tag line it follows
    mutable std::chrono::steady_clock::time_point marq_since[2];
};

} // namespace muisc
