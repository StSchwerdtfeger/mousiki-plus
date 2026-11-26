// The library's tag cache between runs (src/tag_cache.h).
#include "test_framework.h"
#include "tag_cache.h"
#include "local_source.h"
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

// The sweep writes the cache straight from its own tags (no second map of every path and tag): the file comes out the same.
TEST(tag_cache_stream_save_writes_what_the_map_save_writes) {
    mt::TempHome home;
    const fs::path f = tag_cache_path(false);
    TagCacheMap m;
    TagCacheEntry e;
    e.stamp = {99, 7};
    e.meta.title = "Song\twith tab";
    e.meta.artist = "Artist";
    e.meta.album = "Album";
    e.meta.year = "2001";
    e.meta.duration_sec = 61.25;
    e.meta.tags_resolved = true;
    m["/music/one.mp3"] = e;
    const bool ok = tag_cache_save_stream(f, [&](const TagCacheEmit& emit) {
        for (const auto& kv : m) emit(kv.first, kv.second.stamp, kv.second.meta);
    });
    CHECK(ok);
    const TagCacheMap back = tag_cache_load(f);
    CHECK_EQ(back.size(), static_cast<size_t>(1));
    const auto it = back.find("/music/one.mp3");
    CHECK(it != back.end());
    if (it != back.end()) {
        CHECK(it->second.stamp == e.stamp);
        CHECK_EQ(it->second.meta.title, e.meta.title);
        CHECK_EQ(it->second.meta.year, e.meta.year);
        CHECK(it->second.meta.tags_resolved);
    }
}

// The library list keeps a file's path as plain text (LightPath); it must hand back exactly the path it was given.
TEST(light_path_round_trips_and_compares_like_a_path) {
    const fs::path p = fs::path("/music") / "Artist" / "01 - Song.mp3";
    LocalTrack t{"01 - Song", p, "Artist"};
    const fs::path back = t.path;
    CHECK(back == p);
    CHECK(t.path == p);
    CHECK(p == t.path);
    CHECK(!(t.path != p));
    CHECK_EQ(t.path.filename().string(), std::string("01 - Song.mp3"));
    CHECK_EQ(t.path.extension().string(), std::string(".mp3"));
    CHECK_EQ(t.path.stem().string(), std::string("01 - Song"));
    CHECK_EQ(t.path.parent_path().string(), (fs::path("/music") / "Artist").string());
    CHECK_EQ(t.path.utf8(), p.string());
    LocalTrack copy = t;   // a copy is a real copy
    copy.path = fs::path("/other/x.flac");
    CHECK(t.path == p);
    CHECK(copy.path != t.path);
    CHECK(LightPath().empty());
}
