// The MilkDrop engine (milkdrop.h): with projectM and presets installed it draws pictures offscreen that follow the sound; without
// them it says why and draws nothing. (On a machine without projectM the drawing test only checks that.)
#include "test_framework.h"
#include "milkdrop.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>

using namespace muisc;

TEST(milkdrop_draws_pictures_when_installed) {
    Milkdrop& m = Milkdrop::instance();
    std::string why;
    if (!m.available(&why)) {
        CHECK(!why.empty());   // (says why: shown in the overlay)
        bool fresh = true;
        CHECK(m.frame(320, 180, fresh) == nullptr);
        std::printf("    (projectM not here: %s)\n", why.c_str());
        return;
    }
    MilkdropOptions o;
    o.fps = 30; o.max_width = 160; o.preset_seconds = 0;
    m.configure(o);
    std::vector<float> tone(2 * 1470);
    double ph = 0;
    const VideoFrame* f = nullptr;
    int pictures = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < end && pictures < 20) {
        for (size_t i = 0; i < tone.size() / 2; ++i) { tone[2 * i] = tone[2 * i + 1] = 0.5f * static_cast<float>(std::sin(ph)); ph += 2 * M_PI * 110 / 44100; }
        milkdrop_feed(tone.data(), tone.size() / 2, 44100);
        bool fresh = false;
        f = m.frame(320, 180, fresh);
        if (fresh) ++pictures;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    CHECK(pictures >= 10);
    CHECK(f != nullptr);
    if (f) {
        CHECK_EQ(f->w, 160);          // at most max_width across ...
        CHECK_EQ(f->h, 90);           // ... with the aspect of the place asked for
        CHECK_EQ(f->rgb.size(), static_cast<size_t>(160 * 90 * 3));
    }
    CHECK(m.preset_count() > 0);
    CHECK(!m.preset_name().empty());
    CHECK(!m.renderer().empty());
    const int before = m.preset_index();
    m.next_preset();
    const auto t2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool fresh = false;
    while (std::chrono::steady_clock::now() < t2 && m.preset_index() == before) { m.frame(320, 180, fresh); std::this_thread::sleep_for(std::chrono::milliseconds(30)); }
    CHECK(m.preset_index() != before);   // the next preset
    m.shutdown();
    CHECK(!m.running());
}

// The favourites list (Shuffle Favourites) and play_path() (the preset overlay): with favourites asked for, the list is exactly them;
// play_path() goes to a preset by its file; without favourites the whole folder again.
TEST(milkdrop_favourites_and_play_path) {
    Milkdrop& m = Milkdrop::instance();
    if (!m.available()) return;   // (projectM not here: see the test above)
    const std::string dir = m.find_presets_dir();
    CHECK(!dir.empty());
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
        if (e.path().extension() == ".milk") files.push_back(e.path().string());
        if (files.size() == 3) break;
    }
    CHECK_EQ(files.size(), static_cast<size_t>(3));
    if (files.size() < 3) return;
    auto pump_until = [&](auto cond) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        bool fresh = false;
        while (std::chrono::steady_clock::now() < end && !cond()) { m.frame(160, 90, fresh); std::this_thread::sleep_for(std::chrono::milliseconds(30)); }
        return cond();
    };
    MilkdropOptions o;
    o.fps = 30; o.max_width = 160; o.preset_seconds = 0; o.shuffle = false;
    o.favourites_only = true;
    o.favourites = {files[0], files[1]};
    m.configure(o);
    CHECK(pump_until([&] { return m.favourites_in_use() && m.preset_count() == 2 && !m.preset_name().empty(); }));
    CHECK(m.preset_path() == files[0] || m.preset_path() == files[1]);
    m.play_path(files[1]);
    CHECK(pump_until([&] { return m.preset_path() == files[1]; }));
    m.play_path(files[2]);   // not in the list: loaded on its own
    CHECK(pump_until([&] { return m.preset_path() == files[2]; }));
    o.favourites_only = false;
    m.configure(o);
    CHECK(pump_until([&] { return !m.favourites_in_use() && m.preset_count() > 2; }));
    m.shutdown();
}

#include "milkdrop_folders.h"
#include "corners.h"
#include <fstream>

TEST(milkdrop_preset_folders_make_add_remove) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "mousiki_mdf_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    CHECK_EQ(mdf::clean_name("  My/Mix: *1*  "), std::string("MyMix 1"));
    CHECK(mdf::clean_name("favourites").empty());
    CHECK(mdf::clean_name("Recommended").empty());
    CHECK(mdf::create(dir, "Chill"));
    CHECK(!mdf::create(dir, "chill") || mdf::names(dir).size() == 2);   // (a second file name differing in case: the file system decides)
    CHECK(mdf::create(dir, "Alpha"));
    CHECK(mdf::exists(dir, "Chill"));
    CHECK_EQ(mdf::add(dir, "Chill", "/p/a.milk"), 0);
    CHECK_EQ(mdf::add(dir, "Chill", "/p/a.milk"), 1);   // (there already)
    CHECK_EQ(mdf::add(dir, "Chill", "/p/b.milk"), 0);
    CHECK_EQ(mdf::load(dir, "Chill").size(), static_cast<size_t>(2));
    CHECK(mdf::remove_preset(dir, "Chill", "/p/a.milk"));
    CHECK_EQ(mdf::load(dir, "Chill").size(), static_cast<size_t>(1));
    const auto names = mdf::names(dir);
    CHECK(names.size() >= 2);
    CHECK_EQ(names[0], std::string("Alpha"));   // (sorted by name)
    CHECK(mdf::remove_folder(dir, "Chill"));
    CHECK(!mdf::exists(dir, "Chill"));
    fs::remove_all(dir);
}

TEST(thick_borders_use_the_heavy_glyphs_and_keep_the_length) {
    std::string s = "\xE2\x95\xAD\xE2\x94\x80\xE2\x95\xAE\xE2\x94\x82 \xE2\x94\x8C\xE2\x94\x90\xE2\x94\x94\xE2\x94\x98\xE2\x95\xB0\xE2\x95\xAF\xE2\x94\x9C\xE2\x94\xA4\xE2\x94\xAC\xE2\x94\xB4\xE2\x94\xBC\xE2\x95\xB7\xE2\x95\xB5 a|b";
    const size_t n = s.size();
    muisc::thicken_borders(s);
    CHECK_EQ(s.size(), n);
    // ╭━╮┃ ┏┓┗┛╰╯┣┫┳┻╋╻╹ a|b (the round corners stay round; plain text is untouched)
    CHECK_EQ(s, std::string("\xE2\x95\xAD\xE2\x94\x81\xE2\x95\xAE\xE2\x94\x83 \xE2\x94\x8F\xE2\x94\x93\xE2\x94\x97\xE2\x94\x9B\xE2\x95\xB0\xE2\x95\xAF\xE2\x94\xA3\xE2\x94\xAB\xE2\x94\xB3\xE2\x94\xBB\xE2\x95\x8B\xE2\x95\xBB\xE2\x95\xB9 a|b"));
    std::string r = "\xE2\x95\xAD\xE2\x95\xAE\xE2\x95\xB0\xE2\x95\xAF\xE2\x94\x80\xE2\x94\x82\xE2\x94\x8C\xE2\x94\xBC";   // mode 2: ╭ ╮ ╰ ╯ ─ │ ┌ ┼ become U+E1C0 ... of the font Mousiki Corners
    muisc::thicken_borders(r, 2);
    CHECK_EQ(r, std::string("\xEE\x87\x80\xEE\x87\x81\xEE\x87\x82\xEE\x87\x83\xEE\x87\x88\xEE\x87\x89\xEE\x87\x84\xEE\x87\x8E"));
    std::string braille = "\xE2\xA0\x80\xE2\xA3\xBF\xE2\x96\x88";   // braille and a block stay as they are
    const std::string keep = braille;
    muisc::thicken_borders(braille);
    CHECK_EQ(braille, keep);
}

TEST(thick_borders_set_only_the_title_of_a_pane_in_bold) {
    const std::string b = "\x1b[1m", off = "\x1b[22m";
    CHECK_EQ(muisc::bold_title("LIBRARY"), b + "LIBRARY" + off);
    CHECK_EQ(muisc::bold_title("LOCAL AUDIO FILES (500 tracks)"), b + "LOCAL AUDIO FILES" + off + " (500 tracks)");
    CHECK_EQ(muisc::bold_title("LIBRARY [Audio] (sorted)"), b + "LIBRARY" + off + " [Audio] (sorted)");
    CHECK_EQ(muisc::bold_title("STYLE OPTION  song.mp3"), b + "STYLE OPTION" + off + "  song.mp3");
    CHECK_EQ(muisc::bold_title("OSCILLOSCOPE - Artist - Song"), b + "OSCILLOSCOPE" + off + " - Artist - Song");
    CHECK_EQ(muisc::bold_title("MILKDROP FOR THIS TRACK: Some Song"), b + "MILKDROP FOR THIS TRACK:" + off + " Some Song");
    CHECK_EQ(muisc::bold_title("LOCKED IN: TRACK"), b + "LOCKED IN: TRACK" + off);   // (a second title word, not information)
    CHECK_EQ(muisc::bold_title("SEARCH LIBRARY \x1b[1;33m\xE2\x97\x80"), b + "SEARCH LIBRARY" + off + " \x1b[1;33m\xE2\x97\x80");   // (the focus triangle keeps its own colour)
    CHECK_EQ(muisc::bold_title("\x1b[1mCOLOURED\x1b[0m"), std::string("\x1b[1mCOLOURED\x1b[0m"));   // (already styled: untouched)
}

TEST(a_marked_corner_is_left_alone_by_the_sharp_and_round_passes) {
    const std::string mark = muisc::kKeepCornerMark;
    std::string s = "\xE2\x95\xAD " + mark + "\xE2\x95\xAD";   // ╭ (changes), marked ╭ (stays)
    muisc::sharpen_corners(s);
    CHECK_EQ(s, std::string("\xE2\x94\x8C ") + mark + "\xE2\x95\xAD");
    std::string r = "\xE2\x94\x8C " + mark + "\xE2\x94\x8C";
    muisc::roundify_corners(r);
    CHECK_EQ(r, std::string("\xE2\x95\xAD ") + mark + "\xE2\x94\x8C");
}

TEST(sharp_corners_swap_the_round_glyphs_and_keep_the_length) {
    std::string s = "\xE2\x95\xAD\xE2\x94\x80\xE2\x95\xAE x \xE2\x95\xB0\xE2\x94\x80\xE2\x95\xAF";   // ╭─╮ x ╰─╯
    const size_t n = s.size();
    muisc::sharpen_corners(s);
    CHECK_EQ(s.size(), n);
    CHECK_EQ(s, std::string("\xE2\x94\x8C\xE2\x94\x80\xE2\x94\x90 x \xE2\x94\x94\xE2\x94\x80\xE2\x94\x98"));   // ┌─┐ x └─┘
    muisc::roundify_corners(s);   // (the Settings screen: the other way round)
    CHECK_EQ(s, std::string("\xE2\x95\xAD\xE2\x94\x80\xE2\x95\xAE x \xE2\x95\xB0\xE2\x94\x80\xE2\x95\xAF"));
}
