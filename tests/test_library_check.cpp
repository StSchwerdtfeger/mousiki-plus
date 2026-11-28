// MESSY LIBRARY?: duplicate groups (never by name alone), the print-based finding of moved files, the id tag of downloads.
#include "test_framework.h"
#include "library_check.h"
#include "file_prints.h"
#include "moved_files.h"
#include "native_duration.h"
#include <filesystem>
#include <fstream>

using namespace muisc;
namespace fs = std::filesystem;

static LcFile mk(const std::string& path, long long size, long long dur, const std::string& artist = "", const std::string& title = "", const std::string& vid = "") {
    LcFile f; f.path = path; f.size = size; f.dur_ms = dur; f.artist = artist; f.title = title; f.vid = vid;
    return f;
}

TEST(library_check_groups_copies_downloads_and_songs_but_not_names_alone) {
    std::vector<LcFile> files = {
        mk("/m/a/song.mp3", 100, 200000, "Ann", "Song"),
        mk("/m/a/song (1).mp3", 100, 200000, "Ann", "Song"),       // an identical copy, in the same folder
        mk("/m/b/other.opus", 50, 120000, "", "", "aaaaaaaaaaa"),
        mk("/m/c/renamed.opus", 70, 121000, "", "", "aaaaaaaaaaa"),   // the same download
        mk("/m/d/x.flac", 900, 300000, "Bob", "Tune"),
        mk("/m/e/x.mp3", 300, 300800, "Bob", "Tune"),              // the same recording, another file
        mk("/m/f/intro.mp3", 10, 30000, "Cy", "Intro"),
        mk("/m/g/intro.mp3", 11, 90000, "Cy", "Intro"),            // one name, other length: not a duplicate
        mk("/m/h/01.mp3", 5, 100000),
        mk("/m/i/01.mp3", 6, 100000),                              // numbers only: never compared by name
    };
    auto hash = [](const std::string& p) -> uint64_t { return p.find("song") != std::string::npos ? 7u : 9u; };
    const LcResult r = lc_find_duplicates(files, {}, hash);
    CHECK_EQ(r.groups.size(), static_cast<size_t>(3));
    CHECK_EQ(r.n_identical, 1);
    CHECK_EQ(r.n_same_id, 1);
    CHECK_EQ(r.n_same_song, 1);
    CHECK_EQ(r.n_same_name_other_length, 1);
    bool found_identical = false;
    for (const LcGroup& g : r.groups) {
        if (g.kind != LcKind::Identical) continue;
        found_identical = true;
        CHECK(g.any_same_folder());
        CHECK_EQ(r.files[static_cast<size_t>(g.members[0].file)].path, std::string("/m/a/song.mp3"));   // the one without the copy mark stays first
        CHECK(g.members[1].same_folder);
        CHECK_EQ(g.extra_bytes, 100LL);
    }
    CHECK(found_identical);
}

TEST(library_check_let_it_rest_hides_a_group_and_a_new_copy_brings_it_back) {
    std::vector<LcFile> files = {mk("/a/s.mp3", 100, 1000), mk("/b/s.mp3", 100, 1000)};
    auto hash = [](const std::string&) -> uint64_t { return 5; };
    const std::vector<std::set<std::string>> rested = {{"/a/s.mp3", "/b/s.mp3"}};
    LcResult r = lc_find_duplicates(files, rested, hash);
    CHECK_EQ(r.groups.size(), static_cast<size_t>(1));
    CHECK(r.groups[0].rested);
    CHECK_EQ(r.n_rested, 1);
    CHECK_EQ(r.n_identical, 0);
    files.push_back(mk("/c/s.mp3", 100, 1000));   // a third copy: not covered by the decision
    r = lc_find_duplicates(files, rested, hash);
    CHECK(!r.groups[0].rested);
}

TEST(library_check_name_keys_ignore_copy_marks_and_numbers) {
    CHECK_EQ(lc_name_key(mk("/x/Track 05.mp3", 1, 1)), std::string());
    CHECK_EQ(lc_name_key(mk("/x/01.mp3", 1, 1)), std::string());
    CHECK_EQ(lc_name_key(mk("/x/Song (2).mp3", 1, 1)), lc_name_key(mk("/y/song.flac", 1, 1)));
    CHECK_EQ(lc_name_key(mk("/x/Song - Copy.mp3", 1, 1)), lc_name_key(mk("/y/Song.mp3", 1, 1)));
    CHECK_EQ(lc_name_key(mk("/x/Song [dQw4w9WgXcQ].opus", 1, 1)), lc_name_key(mk("/y/Song.mp3", 1, 1)));
    CHECK(lc_name_key(mk("/x/a.mp3", 1, 1, "Ann", "Song")) != lc_name_key(mk("/x/a.mp3", 1, 1, "Ann", "Other")));
}

TEST(moved_files_use_size_and_length_when_names_are_not_enough) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_prints_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "a"); fs::create_directories(dir / "b"); fs::create_directories(dir / "c");
    auto write = [&](const fs::path& p, size_t n) { std::ofstream(p, std::ios::binary) << std::string(n, 'x'); };
    write(dir / "a" / "Intro.mp3", 100);
    write(dir / "b" / "Intro.mp3", 200);        // two files of one name: the print decides
    write(dir / "c" / "New Name.mp3", 300);     // a renamed file
    FilePrints::get().use_file(dir / "prints.tsv");
    FilePrints::get().set_duration_lookup([](const std::string&) -> long long { return 180000; });
    const std::string gone_intro = path_utf8(dir / "old" / "Intro.mp3"), gone_named = path_utf8(dir / "old" / "Old Name.mp3");
    FilePrints::get().set(gone_intro, 200, 180000);
    FilePrints::get().set(gone_named, 300, 180000);
    const std::vector<std::string> existing = {path_utf8(dir / "a" / "Intro.mp3"), path_utf8(dir / "b" / "Intro.mp3"), path_utf8(dir / "c" / "New Name.mp3")};
    auto moves = find_moved({gone_intro, gone_named}, existing);
    CHECK_EQ(moves[gone_intro], path_utf8(dir / "b" / "Intro.mp3"));        // the one of 200 bytes
    CHECK_EQ(moves[gone_named], path_utf8(dir / "c" / "New Name.mp3"));     // the only file of 300 bytes and that length
    // without a print nothing is guessed
    FilePrints::get().use_file(dir / "prints2.tsv");
    moves = find_moved({gone_intro, gone_named}, existing);
    CHECK(moves.empty());
    // a second file of the same size and length: a rename is not certain any more
    FilePrints::get().set(gone_named, 300, 180000);
    write(dir / "a" / "Other.mp3", 300);
    auto existing2 = existing; existing2.push_back(path_utf8(dir / "a" / "Other.mp3"));
    moves = find_moved({gone_named}, existing2);
    CHECK(moves.empty());
    FilePrints::get().use_file(fs::path());
    FilePrints::get().set_duration_lookup(nullptr);
    fs::remove_all(dir);
}

TEST(id3_tag_of_a_download_carries_the_video_id) {
    const fs::path f = fs::temp_directory_path() / "mousiki_id3_vid_test.mp3";
    auto ss = [](size_t n) { std::string s(4, '\0'); s[0] = static_cast<char>((n >> 21) & 0x7F); s[1] = static_cast<char>((n >> 14) & 0x7F); s[2] = static_cast<char>((n >> 7) & 0x7F); s[3] = static_cast<char>(n & 0x7F); return s; };
    auto frame = [&](const char* id, const std::string& payload) { return std::string(id) + ss(payload.size()) + std::string(2, '\0') + payload; };
    std::string body = frame("TIT2", std::string("\x03", 1) + "Song") + frame("TPE1", std::string("\x03", 1) + "Ann") +
                       frame("TXXX", std::string("\x03", 1) + std::string("MOUSIKI_ID\0abcdefghijk", 22));
    std::string tag = "ID3" + std::string("\x04\x00\x00", 3) + ss(body.size()) + body;
    { std::ofstream o(f, std::ios::binary); o << tag << std::string(400, '\0'); }
    const NativeId3Tags t = probe_id3v2_native(f);
    CHECK(t.resolved);
    CHECK_EQ(t.title, std::string("Song"));
    CHECK_EQ(t.vid, std::string("abcdefghijk"));
    fs::remove(f);
}
