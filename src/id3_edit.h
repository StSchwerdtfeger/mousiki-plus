// id3_edit: change the text frames (title, artist, album, year) of an MP3's ID3v2 tag and leave everything else of the file as it is.
//
// The meta editor used to rewrite the file with ffmpeg (-map_metadata 0 -c copy). That drops what ffmpeg has no tag name for -- embedded
// lyrics (USLT) are only kept in some versions, ratings and play counts (POPM, PCNT) never -- and turns an ID3v2.3 tag into v2.4. Here every
// frame that is not edited is carried over byte for byte, the version of the tag stays, and the audio is copied unchanged.
#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

enum class Id3EditResult {
    Done,            // the file has the new frames
    NotApplicable,   // a tag this code does not edit (v2.2, unsynchronised, extended header, footer, damaged): use another way, the file is untouched
    Failed,          // writing went wrong: the file is untouched, `err` says why
};

// edits: {frame, value} with frame one of "TIT2" (title), "TPE1" (artist), "TALB" (album), "YEAR" (TYER in a v2.3 tag, TDRC in v2.4).
// An empty value removes the frame. A file without an ID3v2 tag gets a new v2.3 tag.
Id3EditResult id3_edit_text_frames(const std::filesystem::path& file, const std::vector<std::pair<std::string, std::string>>& edits, std::string* err = nullptr);

} // namespace muisc
