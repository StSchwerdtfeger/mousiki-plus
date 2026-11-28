// The local library (see local_source.cpp): LocalTrack describes one file, LocalSource scans the music
// folders and finds tracks.
#pragma once
#include "path_utf8.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// A folder with a file of this name is the creator's folder (the ^ overlay): the library scan leaves its files out, they are
// only reachable through the overlay's "Created files..." list.
inline constexpr const char* kCreatorMarker = ".mousiki-creator";
// A folder with this file in it holds duplicates that were set aside (MESSY LIBRARY?): the library does not list its files.
inline constexpr const char* kSetAsideMarker = ".mousiki-set-aside";

// A file's path kept as one plain string. An fs::path of the standard library (libstdc++, and libc++ too) also keeps every part of the
// path split off ("/home", "me", "Music", ...) -- about 600 bytes for a typical library file against 150 for the string -- and a
// library of 100,000 files holds each track's path twice (the library and the list on screen). Whoever needs the real fs::path
// (every call that opens, names or splits the file) gets one made on the spot by the conversion below.
class LightPath {
public:
    LightPath() = default;
    LightPath(const fs::path& p) : native_(p.native()) {}   // NOLINT: a path goes in as it is
    operator fs::path() const { return fs::path(native_); }   // NOLINT: and comes out as a real one where one is needed
    LightPath& operator=(const fs::path& p) { native_ = p.native(); return *this; }
    const fs::path::string_type& native() const { return native_; }
    bool empty() const { return native_.empty(); }
    fs::path filename() const { return fs::path(native_).filename(); }
    fs::path extension() const { return fs::path(native_).extension(); }
    fs::path stem() const { return fs::path(native_).stem(); }
    fs::path parent_path() const { return fs::path(native_).parent_path(); }
    std::string string() const { return fs::path(native_).string(); }
    std::string utf8() const {   // path_utf8(path), without making the real path first
#if defined(_WIN32)
        return path_utf8(fs::path(native_));
#else
        return native_;
#endif
    }
    bool operator==(const LightPath& o) const { return native_ == o.native_ || fs::path(native_) == fs::path(o.native_); }
    bool operator!=(const LightPath& o) const { return !(*this == o); }
    bool operator<(const LightPath& o) const { return fs::path(native_) < fs::path(o.native_); }
    // against a real path: the same text (as fs::path's own == would say: it compares the parts, which for the paths of this program
    // -- always built from a full string -- gives the same answer)
    friend bool operator==(const LightPath& a, const fs::path& b) { return a.equals(b); }
    friend bool operator==(const fs::path& a, const LightPath& b) { return b.equals(a); }
    friend bool operator!=(const LightPath& a, const fs::path& b) { return !(a == b); }
    friend bool operator!=(const fs::path& a, const LightPath& b) { return !(a == b); }
private:
    // Two paths made of the same text are equal; two of different text can still be equal where a separator is doubled (and on
    // Windows where "/" and "\\" meet), so only a text without those is decided by the text alone (most rows of a long list are not
    // the track looked for: no path is made for them).
    bool equals(const fs::path& b) const {
        if (native_ == b.native()) return true;
#if !defined(_WIN32)
        if (native_.find("//") == std::string::npos && b.native().find("//") == std::string::npos) return false;
#endif
        return fs::path(native_) == b;
    }
    fs::path::string_type native_;
};

struct LocalTrack {
    std::string title;   // filename stem
    LightPath path;
    std::string folder_artist; // parent directory name, used as a cheap
                                // "Artist" column guess for the list view
                                // (real tag lookup is reserved for the
                                // currently-loaded track's metadata panel)
};

// The rows of the track list on screen: positions in the list of all local tracks (4 bytes a row instead of a second copy of every
// track). Reads like a vector of tracks (size(), empty(), view[i]). Whoever changes the list of all tracks sets (or clear()s) the
// view right after: a position is only good for the list it was made from.
class LocalView {
public:
    explicit LocalView(const std::vector<LocalTrack>& all) : all_(&all) {}
    size_t size() const { return ids_.size(); }
    bool empty() const { return ids_.empty(); }
    const LocalTrack& operator[](size_t i) const { return (*all_)[ids_[i]]; }
    size_t id(size_t i) const { return ids_[i]; }   // the position of row i in the list of all tracks
    void clear() { std::vector<uint32_t>().swap(ids_); }
    LocalView& operator=(std::vector<uint32_t> ids) { ids_ = std::move(ids); return *this; }
private:
    const std::vector<LocalTrack>* all_;
    std::vector<uint32_t> ids_;
};

class LocalSource {
public:
    // Scans the provided paths, falling back to defaults if empty.
    // `diagnostics`, if non-null, gets one line appended per root -- whether
    // it existed as a directory and how many audio files were found under
    // it (or, if it didn't exist/wasn't a directory, says so). Left null by
    // find()'s call site below, since that's an internal lookup rather than
    // a user-visible scan the log should report on.
    std::vector<LocalTrack> scan(const std::vector<std::string>& custom_paths = {},
                                  std::vector<std::string>* diagnostics = nullptr) const;

    // Case-insensitive substring match against title.
    std::optional<LocalTrack> find(const std::string& query, const std::vector<std::string>& custom_paths = {}) const;

    // Video files (mp4, mkv, mov, avi, webm ...): listed only while the Video mode is on (set_include_video(true)); they play as audio
    // (ffmpeg decodes the sound) and the Video view shows their picture. ".webm" can also be an audio-only download.
    static bool is_video_file(const fs::path& p);
    void set_include_video(bool on) { include_video_ = on; }
    bool include_video() const { return include_video_; }

private:
    bool include_video_ = false;
    static bool is_audio_file(const fs::path& p);
};

} // namespace muisc
