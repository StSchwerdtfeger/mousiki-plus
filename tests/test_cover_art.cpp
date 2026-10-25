// The album cover of the music player: where it is looked for (cover_art.cpp) and the settings that choose it.
#include "test_framework.h"
#include "cover_art.h"
#include "settings.h"

#include <filesystem>
#include <fstream>

using namespace muisc;
namespace fs = std::filesystem;

namespace {
void touch(const fs::path& p) { std::ofstream(p, std::ios::binary) << "x"; }
}

TEST(cover_name_closeness_ignores_case_punctuation_and_extra_words) {
    CHECK_EQ(name_closeness("Thriller", "thriller"), 1.0);
    CHECK_EQ(name_closeness("Abbey_Road", "abbey road"), 1.0);
    CHECK(name_closeness("Abbey Road - front", "Abbey Road") >= kCoverCloseness);   // the album's name inside a longer one
    CHECK(name_closeness("Thriler", "Thriller") >= kCoverCloseness);                // a typo
    CHECK(name_closeness("Dark Side of the Moon", "dark side moon") >= kCoverCloseness);
    CHECK(name_closeness("Bad", "Thriller") < kCoverCloseness);
    CHECK(name_closeness("cover", "Thriller") < kCoverCloseness);
    CHECK_EQ(name_closeness("", "Thriller"), 0.0);
    CHECK_EQ(name_closeness("!!!", "???"), 0.0);
}

TEST(cover_search_steps_follow_the_setting) {
    using S = CoverStep;
    CHECK(cover_steps(0) == (std::vector<S>{S::Embedded, S::FolderClose, S::FolderGeneric, S::FolderAny}));      // Default
    CHECK(cover_steps(1) == (std::vector<S>{S::FolderClose, S::FolderGeneric, S::Embedded, S::FolderAny}));      // Folder First
    CHECK(cover_steps(2) == (std::vector<S>{S::Embedded, S::FolderClose, S::FolderAny}));                         // Within File First
    CHECK(cover_steps(3) == (std::vector<S>{S::FolderClose, S::FolderGeneric, S::FolderAny}));                    // Image in Folder Only
    CHECK(cover_steps(4) == (std::vector<S>{S::Embedded}));                                         // Within File Only
    CHECK(cover_steps(99) == cover_steps(0));
}

TEST(cover_folder_picture_is_the_closest_named_one_then_a_generic_one) {
    mt::TempHome home;
    const fs::path d = home.path() / "mixed";
    fs::create_directories(d);
    touch(d / "Alpha.jpg");
    touch(d / "Beta Album.png");
    touch(d / "cover.jpg");
    touch(d / "notes.txt");
    CHECK(pick_folder_picture(d, {"Alpha", "x"}, false).filename() == "Alpha.jpg");
    CHECK(pick_folder_picture(d, {"", "beta album (deluxe)"}, false).filename() == "Beta Album.png");   // title or album: either may match
    CHECK(pick_folder_picture(d, {"Gamma", "Gamma Song"}, false).empty());                              // nothing is close
    CHECK(pick_folder_picture(d, {"Gamma"}, true).filename() == "cover.jpg");                           // the generic one
    fs::remove(d / "cover.jpg");
    CHECK(pick_folder_picture(d, {"Gamma"}, true).empty());                                             // no generic one, not just any picture
    CHECK(pick_folder_picture(home.path() / "missing", {"Alpha"}, false).empty());
}

TEST(cover_settings_round_trip_and_defaults) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK(!s.album_cover);       // off until switched on
    CHECK_EQ(s.cover_mode, 2);   // cover behind the disk
    CHECK_EQ(s.cover_search, 0);
    s.album_cover = true;
    s.cover_mode = 1;
    s.cover_search = 3;
    save_settings(s);
    Settings back = load_settings();
    CHECK(back.album_cover);
    CHECK_EQ(back.cover_mode, 1);
    CHECK_EQ(back.cover_search, 3);
    for (int search = 0; search < 5; ++search)
        for (int mode = 0; mode < 5; ++mode) {
            back.cover_search = search;
            back.cover_mode = mode;
            save_settings(back);
            const Settings again = load_settings();
            CHECK_EQ(again.cover_search, search);
            CHECK_EQ(again.cover_mode, mode);
        }
}

TEST(cover_names_from_the_user_report_match) {
    CHECK(name_closeness("test-song-cover", "test_song-cover3") >= kCoverCloseness);
    CHECK(name_closeness("test_cov", "test_song-cover3") >= kCoverCloseness);   // an abbreviated word
}

TEST(cover_folder_picture_takes_every_regular_format) {
    mt::TempHome home;
    const fs::path d = home.path() / "formats";
    fs::create_directories(d);
    for (const char* ext : {".jpg", ".JPEG", ".png", ".webp", ".bmp", ".gif", ".tiff", ".avif", ".heic", ".jxl", ".jp2", ".tga", ".ico"}) {
        const fs::path f = d / (std::string("Thriller") + ext);
        touch(f);
        CHECK(pick_folder_picture(d, {"Thriller"}, false) == f);
        fs::remove(f);
    }
    touch(d / "Thriller.cr2");   // camera RAW: not a cover
    touch(d / "Thriller.txt");
    CHECK(pick_folder_picture(d, {"Thriller"}, false).empty());
}

TEST(cover_any_picture_is_the_alphabetical_last_resort) {
    mt::TempHome home;
    const fs::path d = home.path() / "any";
    fs::create_directories(d);
    touch(d / "zebra.png");
    touch(d / "Moon.jpg");
    CHECK(pick_folder_picture(d, {"Gamma"}, false, true).filename() == "Moon.jpg");
}

TEST(cover_contrast_is_saved_in_steps_of_five) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK_EQ(s.cover_contrast, 0);
    s.cover_contrast = 35;
    save_settings(s);
    CHECK_EQ(load_settings().cover_contrast, 35);
}
