#pragma once
// The Latin-letter name of an artist from MusicBrainz (scripts/lookup_artist_name.py), for the "Latin letters" display of names
// written in Chinese characters. Runs one name at a time in the background; see App::poll_name_lookup().
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

struct NameLookupResult {
    bool ok = false;
    bool notfound = false;        // MusicBrainz was reachable and has no such artist (or no Latin name for it)
    bool script_missing = false;
    bool python_missing = false;
    std::string latin;
    std::string error;            // a temporary problem (not reachable, HTTP 5xx ...): ask again later
};

// One JSON line of the script.
NameLookupResult parse_name_lookup_line(const std::string& line);

NameLookupResult lookup_artist_name(const std::filesystem::path& script, const std::string& name);

struct KanjiReadResult {
    bool ok = false;
    bool missing = false;         // pykakasi is not installed (or no Python / script): nothing more is tried this session
    std::vector<std::pair<std::string, std::string>> readings;
    std::string error;
};

// Reads Japanese texts with pykakasi (scripts/kanji_reading.py, optional package installed by the user, runs on this computer).
KanjiReadResult read_kanji(const std::filesystem::path& script, const std::vector<std::string>& texts);

}  // namespace muisc
