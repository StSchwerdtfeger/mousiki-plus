// Two different songs with the same title must not share one downloaded file (the cache file is named after the title).
#include "test_framework.h"
#include "cache_manager.h"
#include <filesystem>

using namespace muisc;
namespace fs = std::filesystem;

TEST(cache_key_separates_songs_with_the_same_title) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_cache_key_test";
    fs::remove_all(dir);
    CacheManager c;
    c.set_download_dir(dir);
    CHECK(c.cache_key("Intro", "AAAAAAAAAAA") == "Intro");        // nothing recorded: the plain title
    c.record_audio_download("Intro", "AAAAAAAAAAA");
    CHECK(c.cache_key("Intro", "AAAAAAAAAAA") == "Intro");        // the same video finds its own file again
    CHECK(c.cache_key("Intro", "BBBBBBBBBBB") == "Intro [BBBBBBBBBBB]");   // another video: a file of its own
    c.record_audio_download("Intro [BBBBBBBBBBB]", "BBBBBBBBBBB");
    CHECK(c.cache_key("Intro", "BBBBBBBBBBB") == "Intro [BBBBBBBBBBB]");
    CHECK(c.path_for("Intro [BBBBBBBBBBB]") != c.path_for("Intro"));
    CHECK(c.cache_key("Intro", "https://x/y") == "Intro");        // SoundCloud / Bandcamp pages keep the old naming
    fs::remove_all(dir);
}

TEST(cache_names_never_are_windows_device_names) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_cache_device_test";
    fs::remove_all(dir);
    CacheManager c;
    c.set_download_dir(dir);
    auto stem = [&](const char* title) { return c.path_for(title, "opus").stem().string(); };
    CHECK_EQ(stem("CON"), std::string("con_"));
    CHECK_EQ(stem("nul"), std::string("nul_"));
    CHECK_EQ(stem("Com1"), std::string("com1_"));
    CHECK_EQ(stem("lpt9"), std::string("lpt9_"));
    CHECK_EQ(stem("console"), std::string("console"));   // (only the exact names)
    CHECK_EQ(stem("com10"), std::string("com10"));
    fs::remove_all(dir);
}

#include "youtube_source.h"
#include <fstream>

TEST(download_history_is_read_for_ids_downloaded_before) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_cache_history_test";
    fs::remove_all(dir);
    CacheManager c;
    c.set_download_dir(dir);
    CHECK(!c.in_history("AAAAAAAAAAA"));
    fs::create_directories(dir / "history");
    { std::ofstream f(c.download_archive_path(), std::ios::binary); f << "youtube AAAAAAAAAAA\r\nyoutube BBBBBBBBBBB\n"; }   // (a yt-dlp run outside Mousiki, even with Windows line ends)
    c.record_download("youtube", "CCCCCCCCCCC");   // (a download of Mousiki: the memory is read again at once)
    CHECK(c.in_history("AAAAAAAAAAA"));
    CHECK(c.in_history("BBBBBBBBBBB"));
    CHECK(c.in_history("CCCCCCCCCCC"));
    CHECK(!c.in_history("DDDDDDDDDDD"));
    CHECK(!c.in_history(""));
    fs::remove_all(dir);
}

TEST(a_download_moved_into_the_library_is_played_from_there) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_cache_finder_test";
    fs::remove_all(dir);
    CacheManager c;
    c.set_download_dir(dir);
    YoutubeSource y(c);
    const fs::path lib = dir / "elsewhere";
    fs::create_directories(lib);
    const fs::path with_id = lib / "intro [aaaaaaaaaaa].opus", plain = lib / "home.opus";
    { std::ofstream(with_id) << "x"; std::ofstream(plain) << "x"; }
    y.set_local_finder([&](const std::string& stem, const std::string& id) {
        if (id == "zzzzzzzzzzz") return lib / "renamed.opus";   // (found by the id in its tags)
        if (stem == CacheManager::stem_for("Intro [aaaaaaaaaaa]")) return with_id;
        if (stem == CacheManager::stem_for("Home")) return plain;
        return fs::path();
    });
    auto a = y.resolve_by_id("aaaaaaaaaaa", "Intro", "x");   // the id is in the file name: always this video
    CHECK(a && a->cached_path == with_id && a->from_cache);
    // a plain title is trusted only for an id the history knows: another song of that name in the library is not played for it
    // (without the history entry the finder says no and yt-dlp would be asked -- not reached here: the id is recorded first)
    c.record_download("youtube", "bbbbbbbbbbb");
    auto b = y.resolve_by_id("bbbbbbbbbbb", "Home", "x");
    CHECK(b && b->cached_path == plain && b->from_cache);
    auto z = y.resolve_by_id("zzzzzzzzzzz", "Whatever", "x");   // the file that carries the id wins, whatever it is called now
    CHECK(z && z->cached_path == lib / "renamed.opus" && z->from_cache);
    fs::remove_all(dir);
}
