#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// One LRC line, optionally with per-word timestamps (enhanced/A2 LRC),
// which is what makes word-level highlighting possible during playback.
struct LyricLine {
    double start_time = 0.0;
    std::string full_text;
    std::vector<std::pair<double, std::string>> words; // empty if not word-synced
};

enum class LyricsStatus {
    Ok,
    ModuleMissing,   // Python 'requests' package not installed -> show pip-install hint
    PythonMissing,   // python3 not found on PATH
    NotFound,        // ran fine, no lyrics available for this track
    Error,
};

struct LyricsResult {
    LyricsStatus status = LyricsStatus::Error;
    std::vector<LyricLine> lines;
    std::string message;   // human-readable status/error, shown in the lyrics panel
    std::string source;    // "local" | "better-lyrics" | "lrclib" | ""
    std::string raw_lrc;   // the raw LRC text, kept so it can be cached to a sidecar file
    // Timing correction in seconds, as the lyrics editor (ALT+L) shows it:
    // POSITIVE = the lyrics appear LATER than their timestamps say, negative =
    // earlier. Read from the sidecar's "[offset:...]" tag (which uses the LRC
    // convention, in milliseconds, positive = earlier -- the sign is flipped on
    // the way in and out), 0 when there is none.
    double delay = 0.0;
    // The text carried no timestamps (lyrics embedded in the file as plain text): the lines are spread evenly over the
    // track, which is only a rough guide.
    bool unsynced = false;
};

// Priority chain: a local sidecar .lrc file in a "lyrics" subfolder of the
// track's own folder (checked first, no subprocess spawned at all; a sidecar
// left next to the track by an older build is still read as a fallback) -> the Python helper script
// (scripts/fetch_lyrics.py), which tries Better Lyrics first (word-level
// TTML, converted to enhanced LRC) and falls back to LRCLIB (line-synced
// only) if Better Lyrics has nothing -- see scripts/lrc.py. Whatever
// comes back from a network fetch is written back to the sidecar file,
// so the next time this track plays (even offline) it's a local-file
// hit.
LyricsResult fetch_synced_lyrics(const std::string& title, const std::string& artist,
                                  const std::string& helper_script_path,
                                  const fs::path& track_path = fs::path(),
                                  bool force_network = false,
                                  double track_seconds = 0.0);

// --- the versions of a track's lyrics ------------------------------------------------------------------------
// Lyrics can come from three places: written INTO the audio file (tags), a FETCHED copy in the "lyrics" folder next to
// the track, and older fetched copies that a refetch (or a switch) put aside so nothing is ever overwritten -- a timing
// correction saved with the lyrics editor survives a refetch that way. Chain when a track loads: the fetched copy in use
// (if any), else the lyrics in the file, else the network.
struct LyricsVersion {
    enum class Kind { Embedded, Fetched, Archived };
    Kind kind = Kind::Fetched;
    fs::path file;          // Fetched: the copy in use; Archived: the saved older copy; empty for Embedded
    std::string label;      // "Embedded in the file" / "Fetched 2026-10-11 06:15"
    std::string detail;     // "lrclib, word-synced, timing +0.5 s"
    bool active = false;    // the one that loads when the track plays
};
std::vector<LyricsVersion> list_lyrics_versions(const fs::path& track_path);
// Makes `v` the one in use: the copy in use is put aside first (never lost), then `v` takes its place. False on failure.
bool activate_lyrics_version(const fs::path& track_path, const LyricsVersion& v);
// Deletes the saved copy behind `v` (the fetched copy in use or an older one). Lyrics embedded in the audio file are
// part of the file and are never touched: false for them and on failure.
bool delete_lyrics_version(const LyricsVersion& v);

// --- reading the saved lyrics (READ LYRICS overlay, CTRL+L) ---------------------------------------------------
// The raw text behind one version: the .lrc of a fetched / older copy, or the lyrics written into the audio file.
// Empty when it cannot be read.
std::string read_lyrics_version_text(const fs::path& track_path, const LyricsVersion& v);
// The "lyrics" folder of a track (fetched copies, older copies and the "translations" subfolder live in it).
fs::path lyrics_folder_of(const fs::path& track_path);
// Readable lines of an LRC / plain lyrics text: timestamps, word tags and [ar:]-style header tags are removed, a
// run of blank lines becomes one, leading / trailing blank lines are dropped.
std::vector<std::string> lyrics_plain_lines(const std::string& raw);

// Writes `delay_sec` (same sign as LyricsResult::delay) into the track's sidecar
// .lrc as an "[offset:...]" tag, replacing an existing one (0 removes the tag).
// The timestamps themselves are not touched. False when there is no sidecar to
// write into (nothing fetched/cached for this track) or the write failed.
bool save_lyrics_delay(const fs::path& track_path, double delay_sec);

} // namespace muisc
