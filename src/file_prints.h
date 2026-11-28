// A small memory of what the files with data of their own looked like: size and duration, per file path (file_prints.tsv in the config folder).
// Mousiki keeps per-track data (a track's own style, the listening history, the video links) under the file's path. A file that was moved or
// renamed outside Mousiki is found again by its name (moved_files.h); the print is what breaks a tie when the name is not unique, and what finds a
// file that was renamed (the same size and the same duration, and no other file like it). Thread safe.
#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace muisc {

struct FilePrint {
    long long size = -1;     // bytes, -1 = unknown
    long long dur_ms = -1;   // milliseconds, -1 = unknown
    long long seen_day = 0;  // the last day (days since 1970) the file was there; 0 = not known
    bool known() const { return size >= 0; }
};

class FilePrints {
public:
    static FilePrints& get();
    FilePrint find(const std::string& path_utf8) const;
    // Remembers the print of an existing file (a duration of -1 keeps the one known before). Written to disk by save_if_dirty().
    void set(const std::string& path_utf8, long long size, long long dur_ms);
    void save_if_dirty();
    // Forgets the prints of files that have not been seen for `grace_days` and are not there: deleted, or moved out of where Mousiki looks (it cannot
    // tell which; the days are the time to find a moved file). A file whose drive or share is not connected is never forgotten: when the first
    // folder above it that exists is not its own folder, and that folder is empty (an unmounted mount point) or there is none (a missing drive), it is
    // left alone. Returns how many were forgotten.
    int forget_missing(int grace_days);
    // For the tests: another file (and nothing loaded yet).
    void use_file(const std::filesystem::path& f);
    // The duration (ms) of an existing file from what the library already read from its tags; -1 when unknown. Set by the App.
    void set_duration_lookup(std::function<long long(const std::string&)> f);
    long long duration_ms_of(const std::string& path_utf8) const;
private:
    FilePrints();
    struct Impl;
    Impl* d_;
};

}  // namespace muisc
