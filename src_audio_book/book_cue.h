// The .cue file of an audio book: <book folder>/<folder name>.cue. It holds what the audio containers cannot (WAV has no narrator or
// publisher tag) and, in the later phases, the bookmarks and chapter marks. Plain CUE: the book's header lines come before the first
// FILE line (PERFORMER, TITLE and REM lines such as REM NARRATOR "..."). Lines this class does not know are kept as they are.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace muisc {

// One bookmark: REM BOOKMARK "<file name>" <seconds> <when set, seconds since 1970> (the last number is missing in old files = 0).
struct BookMark {
    std::string file;
    double sec = 0.0;
    long long when = 0;
    bool valid() const { return !file.empty(); }
};
bool parse_bookmark(const std::string& raw, BookMark& out);   // the text after "REM BOOKMARK"
std::string format_bookmark(const BookMark& m);

class BookCue {
public:
    // <dir>/<name of dir>.cue
    static std::filesystem::path path_for(const std::filesystem::path& book_dir);

    // Reads the file (BOM and CRLF aware). False when it does not exist or cannot be read; the object is then empty.
    bool load(const std::filesystem::path& file);
    // Writes the file again with the line endings (and BOM) it was read with. False on a write error.
    bool save(const std::filesystem::path& file) const;

    // A header line: key is the text before the value, e.g. "REM NARRATOR", "REM PUBLISHER", "REM DATE", "PERFORMER", "TITLE".
    // get() returns the value without its quotes ("" when absent). set() replaces the line, adds it to the header when new, and removes
    // it when the value is empty.
    std::string get(const std::string& key) const;
    void set(const std::string& key, const std::string& value);

    // The same for a value that is not one quoted text (a bookmark is a file name and a time: REM BOOKMARK "Chapter 3.mp3" 1234.5):
    // the text after the key, as it stands. An empty value removes the line.
    std::string get_raw(const std::string& key) const;
    void set_raw(const std::string& key, const std::string& raw);

    // Every header line with this key (a book may carry many bookmarks): the text after the key, in file order. set_all_raw()
    // replaces all of them by `values` (written at the place of the first old one, or at the end of the header); empty values are skipped.
    std::vector<std::string> get_all_raw(const std::string& key) const;
    void set_all_raw(const std::string& key, const std::vector<std::string>& values);

    bool empty() const { return lines_.empty(); }
    const std::vector<std::string>& lines() const { return lines_; }

private:
    std::vector<std::string> lines_;
    bool bom_ = false;
    bool crlf_ = false;
    size_t header_end() const;   // index of the first FILE line (lines_.size() when there is none)
    static bool is_key(const std::string& line, const std::string& key);
};

} // namespace muisc
