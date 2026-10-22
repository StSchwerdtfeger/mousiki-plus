#pragma once
// Tags of tracker modules and chiptunes (MOD / XM / S3M / IT ..., NSF / SPC / VGM ..., SID).
//
// These formats hold few or no tags, and ffmpeg cannot write any of them. The meta editor therefore splits an edit:
//   * what the file itself can store is written into the file's header, for real
//       tracker modules : the title (the module's song name)
//       SID tunes       : the title (name) and the artist (author)
//   * everything else (artist / album / year of a tracker module, album / year of a SID tune, all four tags of game
//     music) goes to a small side file next to the music: <folder>/chip_tracker_meta data/metadata.json
// Reading puts the side file's values over what the file reports, in the lists, search, the meta editor and the
// metadata block of the player (probe_row_meta() / probe_metadata() call overlay()).
#include "meta_editor.h"
#include <array>
#include <filesystem>
#include <string>

namespace muisc {
namespace chipmeta {

namespace fs = std::filesystem;

constexpr const char* kFolderName = "chip_tracker_meta data";
constexpr const char* kFileName = "metadata.json";

// Tracker module, game music or SID tune: the files this applies to.
bool applies(const fs::path& media);
// Can `field` (MetaField, 1..4) be written into the file itself?
bool stored_in_file(const fs::path& media, int field);
// The side file of a media file's folder.
fs::path sidecar_path(const fs::path& media);

// Put the side file's values over the given ones (only for fields it has).
void overlay(const fs::path& media, std::string& artist, std::string& title, std::string& album, std::string& year);

// Save the edited tag fields of `e` (fields 1..4 with e.edited set): into the file where the format allows it, into the
// side file otherwise. An empty value removes the entry from the side file. Returns false with `err` on failure.
bool save_tags(const fs::path& media, const MetaEditEntry& e, std::string* err);
// A file was renamed: its side file entry follows.
void rename_entry(const fs::path& folder, const std::string& old_name, const std::string& new_name);

} // namespace chipmeta
} // namespace muisc
