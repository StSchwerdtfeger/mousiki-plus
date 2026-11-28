// Library check (see library_check.cpp): finds the files of the library that are the same song more than once, and builds the report text of the
// "Messy Library?" overlay. Pure logic (no screen, no App): the App hands it the files it has, with the tags it has read.
//
// What counts as a duplicate -- never the name alone:
//   Identical  the same bytes (same size, and the same content hash). A copy.
//   SameId     the same YouTube id in the tags (MOUSIKI_ID, written when Mousiki downloaded the file): the same download, perhaps converted or renamed.
//   SameSong   the same artist and title in the tags (or the same file name when there are no tags) AND the same length (within 1.5 s): the same
//              recording in another file (another encoding, another folder, an album and a single).
// Files with the same name but another length are not duplicates (an intro, a live version): they are only counted.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace muisc {

struct LcFile {
    std::string path;      // UTF-8
    long long size = -1;   // bytes
    long long dur_ms = -1; // -1 unknown
    std::string title, artist, vid;   // tags (empty = none / not read)
};

enum class LcKind { Identical = 0, SameId = 1, SameSong = 2 };   // strongest evidence first

struct LcMember {
    int file = 0;              // index into LcResult::files
    bool same_folder = false;  // another member of the group lies in the same folder (the likely accidental copy: "song.mp3" and "song (1).mp3")
    int link = 0;              // how it is linked to the first member: 0 = it is the first, else the LcKind + 1 of the evidence
};

struct LcGroup {
    LcKind kind = LcKind::SameSong;   // the weakest evidence that holds the group together
    std::vector<LcMember> members;    // sorted: the shortest path / the same-folder ones together, the first is the one to keep by default
    bool rested = false;              // "let it rest": a decision that this is deliberate
    long long extra_bytes = 0;        // what all members but the first take up
    bool any_same_folder() const { for (const auto& m : members) if (m.same_folder) return true; return false; }
};

struct LcResult {
    std::vector<LcFile> files;
    std::vector<LcGroup> groups;
    int n_identical = 0, n_same_id = 0, n_same_song = 0;   // groups per kind
    int n_rested = 0;
    int n_same_name_other_length = 0;      // sets of files with one name and clearly different lengths (not duplicates)
    std::vector<std::string> same_name_examples;   // some of them, "name (3 files: 2:10, 3:45, 4:01)"
    int files_without_length = 0;          // not comparable by length
    int files_without_tags = 0;
    long long total_bytes = 0;
    long long extra_bytes_all = 0;         // what the extra copies of all groups that are not rested take up
};

struct LcProgress {
    std::atomic<int> stage{0};      // 0 idle, 1 sorting out by size, 2 reading files (hashes), 3 comparing, 4 done
    std::atomic<int> done{0}, total{0};
    std::atomic<bool> cancel{false};
};

// The 64-bit hash of a whole file (FNV-1a over its bytes); 0 when it cannot be read. `cancel` ends it early (then 0).
uint64_t lc_hash_file(const std::string& path_utf8, const std::atomic<bool>* cancel = nullptr);

// The groups. `rested` are the decisions "let it rest": a group whose members are all in one of those sets is marked rested. `hash` is the hash
// function (the tests give their own). Stops early and returns what it has when progress->cancel is set.
LcResult lc_find_duplicates(std::vector<LcFile> files, const std::vector<std::set<std::string>>& rested,
                            const std::function<uint64_t(const std::string&)>& hash, LcProgress* progress = nullptr);

// "Artist - Title" key for the same-song test: the tags when there are both, else the file name without the marks a copy gets ("(1)", "- Copy",
// "[id]"), lower case and without accents. Empty when the name says too little to compare by (digits only, "track 05", under 3 letters).
std::string lc_name_key(const LcFile& f);

// The report as text: `full` = every group and every example (the .txt file), else the screen's summary lines.
std::vector<std::string> lc_report_lines(const LcResult& r, bool full);

// The one line of a member: "folder/ file name  4.2 MB  3:41"
std::string lc_size_text(long long bytes);
std::string lc_dur_text(long long ms);

}  // namespace muisc
