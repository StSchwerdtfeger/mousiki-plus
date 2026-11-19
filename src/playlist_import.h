#pragma once
// The reader of an imported M3U / M3U8 playlist (src/app_playlist.cpp). Declared here so the fuzz targets and the tests can call it.
#include <filesystem>
#include <string>
#include <vector>

namespace muisc {

struct ExtInf { std::string artist, title; };

// Reads one extended-M3U / plain M3U(8) file: the optional "#PLAYLIST:" name and the track paths. Relative paths are resolved against the
// file's own folder; stream URLs are skipped (a playlist here is local files only), "file://" URLs are unwrapped. Returns false if the file
// cannot be read. `extinf_out` (optional, one per track) gets the "Artist - Title" of the entry's #EXTINF line ({} when it has none).
bool import_parse_m3u(const std::filesystem::path& file, std::string* name_out, std::vector<std::filesystem::path>* tracks_out, int* skipped_urls,
                      std::vector<ExtInf>* extinf_out = nullptr);

} // namespace muisc
