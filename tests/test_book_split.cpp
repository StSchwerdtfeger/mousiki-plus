// Phase 3 of the audio book player: several bookmarks, chapter marks, the plan for cutting a file, the remapping of places.
#include "test_framework.h"
#include "book_cue.h"
#include "book_split.h"
#include "history.h"

using namespace muisc;

TEST(bookmark_line_round_trip) {
    BookMark m;
    CHECK(parse_bookmark("\"Chapter 3.mp3\" 1234.5 1761283200", m));
    CHECK_EQ(m.file, std::string("Chapter 3.mp3"));
    CHECK(m.sec > 1234.4 && m.sec < 1234.6);
    CHECK_EQ(m.when, 1761283200LL);
    CHECK_EQ(format_bookmark(m), std::string("\"Chapter 3.mp3\" 1234.5 1761283200"));
    BookMark old;   // the single-number lines of the older versions still read
    CHECK(parse_bookmark("\"a.wav\" 12.0", old));
    CHECK_EQ(old.when, 0LL);
    CHECK(!parse_bookmark("garbage", old));
}

TEST(book_cue_many_lines_of_one_key) {
    BookCue c;
    c.set("PERFORMER", "Jane");
    c.set_all_raw("REM BOOKMARK", {"\"a.wav\" 1.0 5", "\"b.wav\" 2.0 6", ""});
    CHECK_EQ(c.get_all_raw("REM BOOKMARK").size(), static_cast<size_t>(2));
    c.set_all_raw("REM BOOKMARK", {"\"c.wav\" 3.0 7"});
    const auto v = c.get_all_raw("REM BOOKMARK");
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    CHECK_EQ(v[0], std::string("\"c.wav\" 3.0 7"));
    CHECK_EQ(c.get("PERFORMER"), std::string("Jane"));   // the other header lines stay
    c.set_all_raw("REM BOOKMARK", {});
    CHECK(c.get_all_raw("REM BOOKMARK").empty());
}

TEST(chapter_mark_lines_and_storing) {
    ChapterMark m;
    CHECK(parse_chapter_mark("\"Dune.wav\" 61.5 \"The Arrival\"", m));
    CHECK_EQ(m.title, std::string("The Arrival"));
    CHECK_EQ(format_chapter_mark(m), std::string("\"Dune.wav\" 61.5 \"The Arrival\""));
    BookCue c;
    chapter_marks_store(c, "a.wav", {{"", 0, "Chapter 1"}, {"", 100, "Chapter 2"}});
    chapter_marks_store(c, "b.wav", {{"", 0, "One"}});
    chapter_marks_store(c, "a.wav", {{"", 0, "Chapter 1"}, {"", 50, "Chapter 2"}, {"", 100, "Chapter 3"}});   // replaces a.wav only
    CHECK_EQ(chapter_marks_of(c, "a.wav").size(), static_cast<size_t>(3));
    CHECK_EQ(chapter_marks_of(c, "b.wav").size(), static_cast<size_t>(1));
    chapter_marks_store(c, "a.wav", {});
    CHECK(chapter_marks_of(c, "a.wav").empty());
    const std::vector<ChapterMark> b_marks = chapter_marks_of(c, "b.wav");   // (kept in a variable: CHECK_EQ holds a reference to the title)
    CHECK(!b_marks.empty());
    if (!b_marks.empty()) CHECK_EQ(b_marks[0].title, std::string("One"));
}

TEST(clock_text_parsing) {
    double s = 0;
    CHECK(parse_clock("1:02:03", s)); CHECK(s == 3723.0);
    CHECK(parse_clock("12:30", s));   CHECK(s == 750.0);
    CHECK(parse_clock("95", s));      CHECK(s == 95.0);
    CHECK(parse_clock(" 0:05:09.5 ", s)); CHECK(s > 309.4 && s < 309.6);
    CHECK(!parse_clock("", s));
    CHECK(!parse_clock("1:75", s));
    CHECK(!parse_clock("1:2:3:4", s));
    CHECK(!parse_clock("-5", s));
    CHECK(!parse_clock("a:b", s));
    CHECK_EQ(clock_hms(3723.0), std::string("1:02:03"));
    CHECK_EQ(clock_hms(309.0), std::string("0:05:09"));
}

TEST(default_titles_follow_the_order) {
    CHECK(is_default_title("Chapter 7"));
    CHECK(!is_default_title("Chapter"));
    CHECK(!is_default_title("Chapter 7: The End"));
    std::vector<ChapterMark> v = {{"", 300, "Chapter 2"}, {"", 0, "Chapter 1"}, {"", 100, "The Storm"}, {"", 400, ""}};
    order_marks(v);
    CHECK_EQ(v[0].title, std::string("Chapter 1"));
    CHECK_EQ(v[1].title, std::string("The Storm"));   // typed by hand: stays
    CHECK_EQ(v[2].title, std::string("Chapter 3"));   // renumbered after the insertion
    CHECK_EQ(v[3].title, std::string("Chapter 4"));   // empty: standard title
}

TEST(part_titles_are_safe_file_names) {
    CHECK_EQ(clean_part_title("What? Why: / No*"), std::string("What Why No"));
    CHECK_EQ(clean_part_title("  .. end ..  "), std::string("end"));
    CHECK_EQ(clean_part_title("a\tb"), std::string("a b"));
    CHECK(clean_part_title(std::string(300, 'x')).size() <= 100);
}

TEST(split_plan_names_and_times) {
    std::string err;
    auto parts = plan_split({{"", 600, "Chapter 2"}, {"", 100, "Chapter 1"}}, 1000, ".wav", &err);   // no mark at 0: the start is added
    CHECK_EQ(parts.size(), static_cast<size_t>(3));
    CHECK(parts[0].start == 0.0 && parts[0].end == 100.0);
    CHECK(parts[2].start == 600.0 && parts[2].end < 0);
    CHECK_EQ(parts[0].file, std::string("01 Chapter 1.wav"));
    CHECK_EQ(parts[1].title, std::string("Chapter 2"));    // renumbered: the start is chapter 1
    CHECK_EQ(parts[2].file, std::string("03 Chapter 3.wav"));

    parts = plan_split({{"", 0, "Intro"}, {"", 0.4, "x"}, {"", 100, "Part: One"}, {"", 999.5, "too late"}}, 1000, ".mp3", &err);
    CHECK_EQ(parts.size(), static_cast<size_t>(2));          // the close mark and the one at the very end are dropped
    CHECK_EQ(parts[0].file, std::string("01 Intro.mp3"));
    CHECK_EQ(parts[1].file, std::string("02 Part One.mp3"));

    parts = plan_split({}, 1000, ".wav", &err);
    CHECK(parts.empty());
    CHECK(!err.empty());
    CHECK(plan_split({{"", 0, "Only"}}, 1000, ".wav").empty());
}

TEST(split_plan_pads_the_numbers) {
    std::vector<ChapterMark> v;
    for (int i = 0; i < 120; ++i) v.push_back({"", i * 10.0, ""});
    const auto parts = plan_split(v, 5000, ".m4a");
    CHECK_EQ(parts.size(), static_cast<size_t>(120));
    CHECK_EQ(parts[0].file, std::string("001 Chapter 1.m4a"));
    CHECK_EQ(parts[119].file, std::string("120 Chapter 120.m4a"));
}

TEST(positions_move_to_the_new_parts) {
    const auto parts = plan_split({{"", 0, ""}, {"", 100, ""}, {"", 250, ""}}, 400, ".wav");
    std::string f;
    double s = 0;
    CHECK(remap_position(parts, 5.0, f, s));    CHECK_EQ(f, parts[0].file); CHECK(s == 5.0);
    CHECK(remap_position(parts, 100.0, f, s));  CHECK_EQ(f, parts[1].file); CHECK(s == 0.0);   // a boundary belongs to the part that starts there
    CHECK(remap_position(parts, 260.5, f, s));  CHECK_EQ(f, parts[2].file); CHECK(s > 10.4 && s < 10.6);
    CHECK(remap_position(parts, 9999.0, f, s)); CHECK_EQ(f, parts[2].file);
    CHECK(!remap_position({}, 1.0, f, s));
}

TEST(reading_history_has_its_own_file) {
    HistoryStore h;
    h.set_file_name("reading_history.json");
    CHECK_EQ(h.file_name(), std::string("reading_history.json"));
}

TEST(silence_log_is_read) {
    const std::string log = "[silencedetect @ 0x1] silence_start: 10.5\n[silencedetect @ 0x1] silence_end: 24 | silence_duration: 13.5\n"
                            "[silencedetect @ 0x1] silence_start: 590\n";
    const auto s = parse_silences(log, 600);
    CHECK_EQ(s.size(), size_t(2));
    CHECK(s[0].first == 10.5 && s[0].second == 24.0);
    CHECK(s[1].first == 590.0 && s[1].second == 600.0);   // the file ends in silence
}

TEST(blank_audio_is_trimmed_to_two_seconds) {
    auto parts = plan_split({{"", 0, ""}, {"", 100, ""}}, 300, ".wav");
    // 10 s of blank at the start of part 2 (100 .. 110) and 30 s of blank at the very end (270 .. 300)
    const std::vector<std::pair<double, double>> sil = {{100.0, 110.0}, {270.0, 300.0}};
    CHECK_EQ(trim_silence(parts, sil, 300, 2.0), 1);               // (one part, trimmed at both ends)
    CHECK(parts[0].start == 0.0 && parts[0].end == 100.0);          // part 1 had no blank at its edges
    CHECK(parts[1].start == 108.0);                                 // 2 s of the blank stay
    CHECK(parts[1].end == 272.0);
    CHECK(parts[1].rstart == 100.0);                                // the mark stays where it was set
    std::string f;
    double s = 0;
    CHECK(remap_position(parts, 105.0, f, s)); CHECK_EQ(f, parts[1].file); CHECK(s == 0.0);        // inside the cut-off blank: at the start
    CHECK(remap_position(parts, 120.0, f, s)); CHECK(s == 12.0);                                   // 120 - 108
    CHECK(remap_position(parts, 299.0, f, s)); CHECK(s == 164.0);                                  // inside the trimmed end: at the end (272 - 108)
}

TEST(a_short_blank_and_an_all_blank_part_stay) {
    auto parts = plan_split({{"", 0, ""}, {"", 100, ""}}, 300, ".wav");
    CHECK_EQ(trim_silence(parts, {{100.0, 101.5}}, 300, 2.0), 0);      // only 1.5 s of blank: nothing to take away
    CHECK_EQ(trim_silence(parts, {{100.0, 300.0}}, 300, 2.0), 0);      // part 2 is blank all through: left whole
    CHECK(parts[1].start == 100.0 && parts[1].end == -1.0);
}
