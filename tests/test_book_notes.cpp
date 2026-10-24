// The notes of an audio book (<folder>_notes.txt): format, parse, merge, times, and the bookmark line of the .cue.
#include "test_framework.h"
#include "book_notes.h"
#include "book_cue.h"

#include <filesystem>

using namespace muisc;
namespace fs = std::filesystem;

TEST(book_notes_time_text_and_parse) {
    CHECK(book_notes::time_text(0) == "0:00:00");
    CHECK(book_notes::time_text(754) == "0:12:34");
    CHECK(book_notes::time_text(3725) == "1:02:05");
    double s = 0;
    CHECK(book_notes::parse_time("1:02:05", s) && s == 3725.0);
    CHECK(book_notes::parse_time("12:34", s) && s == 754.0);
    CHECK(book_notes::parse_time("90", s) && s == 90.0);
    CHECK(!book_notes::parse_time("x:yy", s));
}

TEST(book_notes_format_parse_round_trip) {
    std::vector<BookNote> in;
    BookNote a; a.file = "Chapter 03.mp3"; a.sec = 754; a.title = "A | pipe in the title"; a.text = {"first line", "", "=== not a header", "last"};
    BookNote b; b.file = "Chapter 04.mp3"; b.sec = 5; b.title = "Only a title";
    in.push_back(a); in.push_back(b);
    const std::vector<BookNote> out = book_notes::parse(book_notes::format(in));
    CHECK(out.size() == 2);
    if (out.size() == 2) { CHECK(out[0] == a); CHECK(out[1] == b); }
}

TEST(book_notes_merge_adds_only_new) {
    BookNote a; a.file = "1.mp3"; a.sec = 10; a.title = "x"; a.text = {"t"};
    BookNote b = a; b.sec = 20;
    std::vector<BookNote> into{a};
    CHECK(book_notes::merge(into, {a, b}) == 1);
    CHECK(into.size() == 2);
    CHECK(book_notes::merge(into, {a, b}) == 0);
}

TEST(book_notes_file_round_trip_and_path) {
    const fs::path d = fs::temp_directory_path() / "mousiki_notes_test" / "My Book";
    std::error_code ec;
    fs::remove_all(d.parent_path(), ec);
    fs::create_directories(d);
    CHECK(book_notes::path_for(d).filename().string() == "My Book_notes.txt");
    std::vector<BookNote> none;
    CHECK(!book_notes::load(book_notes::path_for(d), none));
    BookNote a; a.file = "1.mp3"; a.sec = 3; a.title = "t"; a.text = {"hello"};
    CHECK(book_notes::save(book_notes::path_for(d), {a}));
    std::vector<BookNote> back;
    CHECK(book_notes::load(book_notes::path_for(d), back) && back.size() == 1 && back[0] == a);
    fs::remove_all(d.parent_path(), ec);
}

TEST(book_cue_raw_bookmark_line) {
    const fs::path d = fs::temp_directory_path() / "mousiki_bookmark_test" / "Book";
    std::error_code ec;
    fs::remove_all(d.parent_path(), ec);
    fs::create_directories(d);
    const fs::path f = BookCue::path_for(d);
    BookCue c;
    c.set("REM NARRATOR", "Ann");
    c.set_raw("REM BOOKMARK", "\"Chapter 3.mp3\" 1234.5");
    CHECK(c.save(f));
    BookCue r;
    CHECK(r.load(f));
    CHECK(r.get_raw("REM BOOKMARK") == "\"Chapter 3.mp3\" 1234.5");
    CHECK(r.get("REM NARRATOR") == "Ann");
    r.set_raw("REM BOOKMARK", "\"Chapter 4.mp3\" 7.0");   // replaced, not doubled
    int n = 0;
    for (const std::string& l : r.lines()) if (l.rfind("REM BOOKMARK", 0) == 0) ++n;
    CHECK(n == 1);
    r.set_raw("REM BOOKMARK", "");                         // removed
    CHECK(r.get_raw("REM BOOKMARK").empty());
    fs::remove_all(d.parent_path(), ec);
}

// The three kinds of time stamps share one .cue: several bookmarks, one auto-mark, neither disturbs the other.
TEST(book_cue_automark_and_many_bookmarks) {
    const fs::path d = fs::temp_directory_path() / "mousiki_automark_test" / "Book";
    std::error_code ec;
    fs::remove_all(d.parent_path(), ec);
    fs::create_directories(d);
    const fs::path f = BookCue::path_for(d);
    BookCue c;
    c.set_all_raw("REM BOOKMARK", {"\"a.mp3\" 10.0 100", "\"b.mp3\" 20.0 200", "\"a.mp3\" 30.0 300"});
    c.set_raw("REM AUTOMARK", format_bookmark(BookMark{"b.mp3", 42.5, 400}));
    CHECK(c.save(f));
    BookCue r;
    CHECK(r.load(f));
    CHECK(r.get_all_raw("REM BOOKMARK").size() == 3);
    BookMark am;
    CHECK(parse_bookmark(r.get_raw("REM AUTOMARK"), am));
    CHECK(am.file == "b.mp3" && am.sec > 42.4 && am.sec < 42.6 && am.when == 400);
    r.set_all_raw("REM BOOKMARK", {"\"a.mp3\" 10.0 100"});   // a deletion leaves the auto-mark alone
    CHECK(r.get_all_raw("REM BOOKMARK").size() == 1);
    CHECK(!r.get_raw("REM AUTOMARK").empty());
    r.set_raw("REM AUTOMARK", "");
    CHECK(r.get_raw("REM AUTOMARK").empty());
    CHECK(r.get_all_raw("REM BOOKMARK").size() == 1);
    fs::remove_all(d.parent_path(), ec);
}

// A note remembers when it was written and last changed; an older note (no dates) still reads.
TEST(book_notes_dates_round_trip) {
    BookNote a;
    a.file = "c1.mp3"; a.sec = 65; a.title = "Dated"; a.text = {"line"}; a.set = 1790000000; a.changed = 1790000500;
    BookNote b;
    b.file = "c2.mp3"; b.sec = 5; b.title = "Old"; b.text = {"x"};
    const std::string txt = book_notes::format({a, b});
    CHECK(txt.find("Old\n") != std::string::npos);   // no dates written for a note without
    const std::vector<BookNote> r = book_notes::parse(txt);
    CHECK(r.size() == 2);
    CHECK(r[0].title == "Dated" && r[0].set == 1790000000 && r[0].changed == 1790000500);
    CHECK(r[1].set == 0 && r[1].changed == 0 && r[1].title == "Old");
}
