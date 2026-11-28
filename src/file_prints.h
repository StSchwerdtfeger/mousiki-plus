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
    bool known() const { return size >= 0; }
};

class FilePrints {
public:
    static FilePrints& get();
    FilePrint find(const std::string& path_utf8) const;
    // Remembers the print of an existing file (a duration of -1 keeps the one known before). Written to disk by save_if_dirty().
    void set(const std::string& path_utf8, long long size, long long dur_ms);
    void save_if_dirty();
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
