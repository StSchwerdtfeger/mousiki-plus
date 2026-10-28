// Track metadata (see metadata_probe.cpp): TrackMetadata for the panel, RowMeta for the lists, and the
// probe functions that fill them.
#pragma once
#include <filesystem>
#include <string>

namespace muisc {

namespace fs = std::filesystem;

struct TrackMetadata {
    std::string name;
    std::string artist = "-";
    std::string album = "-";
    std::string year = "-";
    std::string sampling = "-";
    std::string type = "audio only";
    std::string format = "-";
    std::string file_size = "-";
    std::string location = "-";
    std::string extra_label;  // e.g. "Video ID" — empty if unused
    std::string extra_value;
};

// Runs `ffprobe` once against `file` and fills in what it can. Missing
// tags are left as "-" rather than failing the whole probe.
// Fast, narrow probe for just the duration (the mm:ss column of the file lists,
// without a full metadata probe per row).
// Returns -1 on failure.
double probe_duration_seconds(const fs::path& file);

// One ffprobe call for duration plus the real artist/title/album tags. It fills the Artist column of
// the file lists (instead of a guess from the parent folder name) and lets search match the embedded
// tags, not only the file name.
struct RowMeta {
    std::string artist; // empty if untagged
    std::string title;  // embedded title tag, empty if untagged/absent
    std::string album;  // embedded album tag, empty if untagged/absent
    std::string year;   // embedded date/year tag, trimmed to "YYYY" (or "" if untagged/absent)
    double duration_sec = -1.0;
    // True once a real probe_row_meta() (ffprobe) call has actually run for
    // this file, as opposed to a cache entry that only holds a duration
    // filled in by the cheap native header parser. Search needs this to
    // tell "genuinely untagged" apart from "tags not fetched yet" -- see
    // the callers in app.cpp for why that distinction matters.
    bool tags_resolved = false;
};
RowMeta probe_row_meta(const fs::path& file);

// EVERY tag of the file (title, artist, album artist, genre, composer, comment, label, ... whatever the container holds, also
// the ones no screen shows) as one text, the values separated by " | ". One ffprobe call; "" when there is none. For the queue search.
std::string probe_all_tag_text(const fs::path& file);

TrackMetadata probe_metadata(const fs::path& file, const std::string& fallback_name,
                              const std::string& fallback_artist, const std::string& location_label);

} // namespace muisc
