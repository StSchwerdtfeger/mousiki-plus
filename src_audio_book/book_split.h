// Chapter marks and splitting of an audio book file (the pure part of Phase 3; the screen and the ffmpeg calls are in
// app_audiobook_chapters.cpp). A mark is a time inside one audio file and a chapter title; the marks are kept in the book's .cue
// as REM CHAPTER "<file>" <seconds> "<title>" lines until the file is cut. Cutting turns the marks into parts.
#pragma once
#include <string>
#include <utility>
#include <vector>
#include "book_cue.h"

namespace muisc {

struct ChapterMark {
    std::string file;      // the audio file the time belongs to (its name with the ending)
    double sec = 0.0;
    std::string title;
};

bool parse_chapter_mark(const std::string& raw, ChapterMark& out);   // the text after "REM CHAPTER"
std::string format_chapter_mark(const ChapterMark& m);
std::vector<ChapterMark> chapter_marks_of(const BookCue& cue, const std::string& file);          // the marks of one file, by time
void chapter_marks_store(BookCue& cue, const std::string& file, const std::vector<ChapterMark>& marks);   // replaces that file's marks only

// "1:02:03", "12:30", "95" or "1:02:03.5" -> seconds. False for anything else (letters, negative numbers, minutes above 59 with hours).
bool parse_clock(const std::string& text, double& sec);
std::string clock_hms(double sec);   // h:mm:ss, always with the hour ("0:05:09")

bool is_default_title(const std::string& title);   // "Chapter 12"
std::string default_title(int number);              // "Chapter 12"
// Sorts by time and renumbers the standard titles ("Chapter N") so they follow the order; the titles typed by hand stay.
void order_marks(std::vector<ChapterMark>& marks);

// A title as a part of a file name: no characters a file system refuses, no control characters, at most 100 characters.
std::string clean_part_title(const std::string& title);

struct SplitPart {
    double start = 0.0;
    double end = -1.0;     // -1: to the end of the file
    std::string title;
    std::string file;      // the name of the new file: "01 Chapter 1.wav"
    double rstart = 0.0;   // the place the mark gave (start / end can be moved inwards by trim_silence): a place in the old file belongs
    double rend = -1.0;    //   to the part whose [rstart, rend) holds it
};
// The parts for a file of `duration` seconds (0 = unknown). The first part always starts at 0; marks closer than a second to the
// one before, or at the very end, are dropped. Fewer than two parts: empty, and `error` says why.
std::vector<SplitPart> plan_split(std::vector<ChapterMark> marks, double duration, const std::string& ext, std::string* error = nullptr);

// Blank audio: [start, end) pairs in seconds, read from the log of ffmpeg's silencedetect filter ("silence_start: 12.3" / "silence_end: 25.1 |
// silence_duration: 12.8"). A start without an end (the file ends in silence) runs to `duration`.
std::vector<std::pair<double, double>> parse_silences(const std::string& log, double duration);
// Trims the blank at the beginning and at the end of every part so that only `keep` seconds of it stay (start / end move inwards; the
// parts keep their rstart / rend). A part would get shorter than a second: it is left as it is. Returns how many parts were trimmed.
int trim_silence(std::vector<SplitPart>& parts, const std::vector<std::pair<double, double>>& silences, double duration, double keep);

// Where a place in the old file ended up: the new file and the second in it. False when there are no parts.
bool remap_position(const std::vector<SplitPart>& parts, double sec, std::string& file, double& new_sec);

} // namespace muisc
