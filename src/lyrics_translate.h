#pragma once
// ---------------------------------------------------------------------------
// Translating lyrics for the READ LYRICS overlay (CTRL+L).
//
// The translation runs through scripts/translate_lyrics.py (Google Translate / MyMemory, chosen in the translate overlay) and is kept next to the lyrics it belongs
// to: <track folder>/lyrics/translations/<name>.<lang>.txt. The file starts with a header line
//   #mousiki-translation to=en from=de lines=42 hash=<fingerprint of the original lines>
// followed by exactly one translated line per original line, so the overlay can show each one right below its
// original. When the original lyrics change (a refetch), the fingerprint no longer matches and the file is ignored.
// ---------------------------------------------------------------------------
#include "lyrics_fetcher.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

// What to translate with: the service and the languages ("auto" = the service detects the language of the lyrics).
struct TranslateRequest {
    std::string service = "google";    // google | mymemory
    std::string from = "auto";
    std::string to = "en";
};

// A translation that is saved for one version of the lyrics (the entries of the TRANSLATION pane).
struct TranslationInfo {
    std::string to;                    // language it is in ("en")
    std::string from;                  // language it came from ("de")
    std::string service;               // who made it ("google")
};

struct LyricsTranslation {
    bool ok = false;
    std::string service;               // who made it (saved with the translation)
    bool same = false;                 // the lyrics already are in the target language: there is nothing to translate
    std::string from;                  // detected source language code ("de", "ja", ...)
    std::string error;                 // when !ok
    std::vector<std::string> lines;    // one per original line ("" stays ""); empty when `same`
};

// Where the translation of one version of the lyrics is kept (the file need not exist yet).
fs::path lyrics_translation_file(const fs::path& track_path, const LyricsVersion& v, const std::string& to);
// Reads it back; false when there is none, or when it belongs to other lyrics than `plain`.
bool load_lyrics_translation(const fs::path& track_path, const LyricsVersion& v, const std::string& to,
                             const std::vector<std::string>& plain, LyricsTranslation& out);
// Every real translation saved for this version of the lyrics (not the "already in that language" notes, and not
// those of other lyrics), sorted by language name.
std::vector<TranslationInfo> list_lyrics_translations(const fs::path& track_path, const LyricsVersion& v,
                                                      const std::vector<std::string>& plain);
// Deletes one saved translation (also its "already in that language" note); false when there was none.
bool delete_lyrics_translation(const fs::path& track_path, const LyricsVersion& v, const std::string& to);
bool save_lyrics_translation(const fs::path& track_path, const LyricsVersion& v, const std::string& to,
                             const std::vector<std::string>& plain, const LyricsTranslation& t);
// Runs the helper script (blocks until it is done -- call it from a worker thread).
LyricsTranslation translate_lyrics_lines(const std::vector<std::string>& plain, const TranslateRequest& req,
                                         const fs::path& script);
// "de" -> "German"; an unknown code comes back as it is.
std::string language_name(const std::string& code);
// The languages the overlay offers (code, name), sorted by name, and the services (id, name).
const std::vector<std::pair<std::string, std::string>>& translate_languages();
const std::vector<std::pair<std::string, std::string>>& translate_services();
std::string translate_service_name(const std::string& id);

} // namespace muisc
