// Input from outside never crashes the program: lyrics, JSON, playlists, cue files and requests to the remote server are fed thousands of
// damaged copies of valid texts (bytes flipped, cut off, doubled, shuffled, filled with random bytes). What comes out does not matter -- the test
// passes when nothing crashes, hangs or trips AddressSanitizer / UndefinedBehaviorSanitizer (the Linux ASan job of the CI runs it that way).
// fuzz/ has the same cases as libFuzzer targets for long runs (fuzz/README.md).
#include "test_framework.h"
#include "playlist_import.h"
#include <filesystem>
#include <vector>
#include "fuzz_cases.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <random>

using namespace muisc;

namespace {

std::string mutate(const std::string& seed, std::mt19937& rng) {
    std::string s = seed;
    auto pick = [&](size_t n) { return n == 0 ? size_t(0) : static_cast<size_t>(rng() % n); };
    const int rounds = 1 + static_cast<int>(rng() % 6);
    for (int r = 0; r < rounds; ++r) {
        switch (rng() % 8) {
            case 0: if (!s.empty()) s[pick(s.size())] = static_cast<char>(rng()); break;                              // one byte
            case 1: s.insert(pick(s.size() + 1), 1, static_cast<char>(rng())); break;                                   // one byte more
            case 2: if (!s.empty()) s.erase(pick(s.size()), 1 + pick(8)); break;                                        // cut a piece out
            case 3: s.resize(pick(s.size() + 1)); break;                                                                // cut off
            case 4: if (!s.empty()) { const size_t a = pick(s.size()); s.insert(pick(s.size() + 1), s.substr(a, 1 + pick(40))); } break;   // copy a piece
            case 5: { static const char* const bits[] = {"[", "]", "<", ">", "%", "%zz", "%0", "\r\n", "\n", "\0", "{", "}", "\"", "\\", "..", "/", "9999999999999999999",
                                                         "-1", "\xEF\xBB\xBF", "\xC3", "\xF0\x9F", "[00:00.00]", "#EXTINF:", "REM BOOKMARK ", "\\u", "e999"};
                      const char* b = bits[rng() % (sizeof bits / sizeof bits[0])];
                      s.insert(pick(s.size() + 1), *b ? std::string(b) : std::string(1, '\0')); break; }
            case 6: { const size_t n = pick(64); std::string junk; for (size_t i = 0; i < n; ++i) junk.push_back(static_cast<char>(rng())); s.insert(pick(s.size() + 1), junk); break; }
            case 7: if (s.size() > 2) { const size_t a = pick(s.size()), b = pick(s.size()); std::swap(s[a], s[b]); } break;
        }
    }
    if (s.size() > 20000) s.resize(20000);
    return s;
}

using Case = void (*)(const uint8_t*, size_t);

void hammer(Case fn, const std::vector<std::string>& seeds, int iterations, unsigned seed) {
    std::mt19937 rng(seed);
    for (const auto& s : seeds) fn(reinterpret_cast<const uint8_t*>(s.data()), s.size());   // the valid texts themselves
    for (int i = 0; i < iterations; ++i) {
        const std::string in = mutate(seeds[rng() % seeds.size()], rng);
        fn(reinterpret_cast<const uint8_t*>(in.data()), in.size());
    }
    // and plain noise, empty, and one long run of the same byte
    for (int i = 0; i < 100; ++i) { std::string n(rng() % 300, '\0'); for (auto& c : n) c = static_cast<char>(rng()); fn(reinterpret_cast<const uint8_t*>(n.data()), n.size()); }
    fn(reinterpret_cast<const uint8_t*>(""), 0);
    const std::string run(20000, '[');
    fn(reinterpret_cast<const uint8_t*>(run.data()), run.size());
    const std::string run2(20000, '%');
    fn(reinterpret_cast<const uint8_t*>(run2.data()), run2.size());
}

const int kIterations = 1500;

} // namespace

TEST(robust_lyrics_text) {
    hammer(fuzz::lyrics_case,
           {"[ar:Artist]\n[ti:Title]\n[offset:+250]\n[00:12.34]First line\n[00:15.00]<00:15.00>En<00:15.40>hanced <00:15.90>line\n[01:02]No hundredths\n",
            "Plain lyrics\n\nwithout any times\nline three\n",
            "[00:01.00]\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\n[00:03.50]\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4\n[99:99.99]odd\n[00:xx]bad\n[offset:-99999999999]\n"},
           kIterations, 11);
}

TEST(robust_json) {
    hammer(fuzz::json_case,
           {"{\"a\":1,\"b\":[true,false,null,\"x\\n\\u00e9\"],\"c\":{\"d\":-1.5e3}}",
            "[{\"trackName\":\"A\",\"artistName\":\"B\",\"syncedLyrics\":\"[00:01.00] hi\",\"plainLyrics\":\"hi\"}]",
            "{\"title\":\"x\",\"duration\":12.5,\"extractor\":\"youtube\"}\n{\"id\":\"abc\"}"},
           kIterations, 12);
}

TEST(robust_remote_requests) {
    mt::TempHome home;   // (a command that gets through writes its file for the program: into this HOME)
    hammer(fuzz::remote_case,
           {"GET /api/status?pin=000000 HTTP/1.1\nHost: 192.168.1.20:4747\n",
            "GET /api/cmd?pin=000000&c=next&arg=%2B10 HTTP/1.1\nHost: localhost\n",
            "GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\nHost: evil.example\n",
            "GET /api/image?key=%00%ff HTTP/1.1\nHost: [::1]:80\n",
            "POST / HTTP/1.1\nHost: x\n"},
           kIterations, 13);
    remote::net::configure({false, 4747, "", "", false});
}

TEST(robust_playlist_files) {
    mt::TempHome home;
    hammer(fuzz::playlist_case,
           {"#EXTM3U\n#PLAYLIST:Mix\n#EXTINF:215,Artist - Title\nmusic/a.mp3\n#EXTINF:100,Only title\nfile:///home/me/My%20Song.flac\nhttp://example.com/stream\n..\\win\\b.mp3\n",
            "/home/me/music/a.mp3\n/home/me/music/b.mp3\n",
            "\xEF\xBB\xBF#EXTM3U\r\n\r\n#EXTINF:-1,\r\n~/x.ogg\r\n"},
           kIterations / 3, 14);
}

TEST(robust_cue_files) {
    mt::TempHome home;
    hammer(fuzz::cue_case,
           {"\xEF\xBB\xBFPERFORMER \"Author\"\r\nTITLE \"Book\"\r\nREM NARRATOR \"Reader\"\r\nFILE \"01.mp3\" MP3\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\nREM BOOKMARK \"01.mp3\" 123.5 1700000000\r\n",
            "\"01 Chapter.mp3\" 12.5 1700000000"},
           kIterations / 3, 15);
}

// The tag and length readers of the audio files (src/native_duration.cpp): small real files (fuzz/corpus/tags: the first byte is the kind of file,
// the rest the file) are damaged in the same ways. The seeds are read from the source tree; without it the test has nothing to start from and passes.
TEST(robust_audio_tag_readers) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(__FILE__).parent_path().parent_path() / "fuzz" / "corpus" / "tags";
    std::vector<std::string> seeds;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::ifstream in(e.path(), std::ios::binary);
        seeds.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (seeds.empty()) return;
    hammer(fuzz::tags_case, seeds, 1200, 16);
}

TEST(m3u_file_urls_with_drive_letters_and_localhost) {
    mt::TempHome home;
    namespace fs = std::filesystem;
    const fs::path f = fs::temp_directory_path() / "mousiki_fileurl_test.m3u";
    { std::ofstream o(f); o << "file:///C:/Music/a%20b.mp3\nfile://localhost/home/me/c.mp3\nfile:///home/me/d.mp3\n"; }
    std::vector<fs::path> t; std::string name; int skipped = 0;
    CHECK(muisc::import_parse_m3u(f, &name, &t, &skipped));
    fs::remove(f);
    CHECK_EQ(t.size(), static_cast<size_t>(3));
    if (t.size() != 3) return;
#if defined(_WIN32)
    CHECK_EQ(t[0].generic_string(), std::string("C:/Music/a b.mp3"));
#else
    CHECK_EQ(t[0].generic_string(), (f.parent_path() / "C:/Music/a b.mp3").generic_string());   // (no drive letters here: relative to the playlist, but without the URL's slash)
#endif
    // (a path without a drive letter: on Windows it belongs to the drive of the playlist, so only the end is the same everywhere)
    auto ends_with = [](const std::string& s, const std::string& end) { return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0; };
    CHECK(ends_with(t[1].generic_string(), "/home/me/c.mp3"));
    CHECK(ends_with(t[2].generic_string(), "/home/me/d.mp3"));
#if !defined(_WIN32)
    CHECK_EQ(t[1].generic_string(), std::string("/home/me/c.mp3"));
    CHECK_EQ(t[2].generic_string(), std::string("/home/me/d.mp3"));
#endif
}
