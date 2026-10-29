// The library's tag cache between runs (src/tag_cache.h).
#include "test_framework.h"
#include "tag_cache.h"
#include <filesystem>
#include <fstream>

using namespace muisc;
namespace fs = std::filesystem;

TEST(tag_cache_round_trip_keeps_odd_text) {
    mt::TempHome home;
    const fs::path f = tag_cache_path(false);
    TagCacheMap m;
    TagCacheEntry e;
    e.stamp = {12345, 6789};
    e.meta.title = "Tab\there\\and \"quotes\"";
    e.meta.artist = "Line\nbreak";
    e.meta.album = "東京";
    e.meta.year = "1999";
    e.meta.duration_sec = 215.5;
    e.meta.tags_resolved = true;
    m["/music/a\tb.flac"] = e;
    TagCacheEntry untagged;   // a file with no tags at all is remembered too (no ffprobe for it next time)
    untagged.stamp = {1, 2};
    untagged.meta.tags_resolved = true;
    m["/music/plain.wav"] = untagged;
    CHECK(tag_cache_save(f, m));
    const TagCacheMap back = tag_cache_load(f);
    CHECK_EQ(back.size(), static_cast<size_t>(2));
    const auto it = back.find("/music/a\tb.flac");
    CHECK(it != back.end());
    if (it != back.end()) {
        CHECK(it->second.stamp == e.stamp);
        CHECK_EQ(it->second.meta.title, e.meta.title);
        CHECK_EQ(it->second.meta.artist, e.meta.artist);
        CHECK_EQ(it->second.meta.album, e.meta.album);
        CHECK_EQ(it->second.meta.year, e.meta.year);
        CHECK_NEAR(it->second.meta.duration_sec, 215.5, 1e-6);
        CHECK(it->second.meta.tags_resolved);
    }
    CHECK(back.count("/music/plain.wav") == 1);
}

TEST(tag_cache_ignores_other_formats_and_bad_lines) {
    mt::TempHome home;
    const fs::path f = tag_cache_path(true);
    fs::create_directories(f.parent_path());
    { std::ofstream o(f); o << "# some other format\n/x\t1\t2\t3\ta\tb\tc\td\n"; }
    CHECK(tag_cache_load(f).empty());
    { std::ofstream o(f); o << "# mousiki tag cache 1\nbroken line\n/ok\t1\t2\t3.0\tt\ta\tal\ty\n/bad\tx\t2\t3\tt\ta\tal\ty\n"; }
    const TagCacheMap m = tag_cache_load(f);
    CHECK_EQ(m.size(), static_cast<size_t>(1));
    CHECK(m.count("/ok") == 1);
}

TEST(tag_stamp_sees_a_change) {
    mt::TempHome home;
    const fs::path f = fs::current_path() / "song.mp3";
    { std::ofstream o(f, std::ios::binary); o << "abc"; }
    const TagStamp a = tag_stamp(f);
    CHECK(a.ok());
    CHECK_EQ(a.size, 3LL);
    { std::ofstream o(f, std::ios::binary | std::ios::app); o << "defg"; }
    const TagStamp b = tag_stamp(f);
    CHECK(!(a == b));
    CHECK(!tag_stamp(fs::current_path() / "missing.mp3").ok());
}
