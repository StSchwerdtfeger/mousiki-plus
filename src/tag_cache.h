// The tags of the library between runs: a small text file in ~/.cache/mousiki (tag_cache.txt; tag_cache_books.txt for the audio
// book library). At start-up every file whose size and modification time are still the ones in the file gets its tags from there at
// once; only new and changed files are read again (the background sweep, app_rowmeta.cpp). The sweep writes the file anew when it
// is done. A missing, old or damaged file just means everything is read as before.
#pragma once
#include "metadata_probe.h"
#include <filesystem>
#include <string>
#include <unordered_map>

namespace muisc {

struct TagStamp {
    long long size = -1;    // bytes; -1 = the file could not be read
    long long mtime = 0;    // modification time, in the file system's own clock ticks
    bool ok() const { return size >= 0; }
    bool operator==(const TagStamp& o) const { return size == o.size && mtime == o.mtime; }
};
struct TagCacheEntry {
    TagStamp stamp;
    RowMeta meta;           // title, artist, album, year, duration (tags_resolved is true for every entry)
};
using TagCacheMap = std::unordered_map<std::string, TagCacheEntry>;   // key: the path as UTF-8 (path_utf8)

TagStamp tag_stamp(const std::filesystem::path& file);
std::filesystem::path tag_cache_path(bool books);
TagCacheMap tag_cache_load(const std::filesystem::path& file);
// Writes the whole map (to a temporary file first, then renamed, so a crash never leaves half a file). False on a write error.
bool tag_cache_save(const std::filesystem::path& file, const TagCacheMap& map);

} // namespace muisc
