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
