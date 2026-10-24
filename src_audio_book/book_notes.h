// The notes of an audio book: <book folder>/<folder name>_notes.txt. A plain text file a person can read and edit; every note is
//
//   === Chapter 03.mp3 | 0:12:34 | The title of the note
//   the text of the note,   (the title may end in " {set=<seconds> changed=<seconds>}": when it was written and last changed)
//   as many lines as it takes
//
// (a text line that itself starts with "===" is written with a space in front of it). Export and import use the same format.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace muisc {

struct BookNote {
    std::string file;                 // the chapter's file name (with its ending)
    double sec = 0.0;                 // where in that chapter
    std::string title;
    std::vector<std::string> text;    // lines
    long long set = 0;                // when it was written (seconds since 1970; 0 = unknown: an older note)
    long long changed = 0;            // when it was last changed
    bool operator==(const BookNote& o) const { return file == o.file && static_cast<long long>(sec) == static_cast<long long>(o.sec) && title == o.title && text == o.text; }
};

namespace book_notes {
std::filesystem::path path_for(const std::filesystem::path& book_dir);   // <dir>/<dir name>_notes.txt
std::vector<BookNote> parse(const std::string& text);
std::string format(const std::vector<BookNote>& notes);
bool load(const std::filesystem::path& file, std::vector<BookNote>& out);   // false: no such file (out is empty)
bool save(const std::filesystem::path& file, const std::vector<BookNote>& notes);
// Adds the notes of `from` that `into` does not have yet (same chapter, second, title and text count as the same); returns how many.
int merge(std::vector<BookNote>& into, const std::vector<BookNote>& from);
std::string time_text(double sec);                   // h:mm:ss
bool parse_time(const std::string& s, double& sec);  // h:mm:ss, m:ss or plain seconds (a decimal part is allowed)
} // namespace book_notes

} // namespace muisc
