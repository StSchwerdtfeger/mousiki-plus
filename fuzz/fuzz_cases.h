#pragma once
// The inputs that come from outside, one function each, shared by the libFuzzer targets (fuzz/fuzz_*.cpp) and by the quick random-input test
// that runs with the normal tests (tests/test_parser_robustness.cpp). Every function takes arbitrary bytes and must neither crash, hang, nor
// trip a sanitizer; what comes out does not matter here.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <process.h>
#define MUSIC_FUZZ_PID _getpid()
#else
#include <unistd.h>
#define MUSIC_FUZZ_PID getpid()
#endif

#include "book_cue.h"
#include "chiptune.h"
#include "lyrics_fetcher.h"
#include "native_duration.h"
#include "playlist_import.h"
#include "playlist_manager.h"
#include "remote_server.h"
#include "tiny_json.h"

namespace muisc::fuzz {

inline std::string text_of(const uint8_t* d, size_t n) { return std::string(reinterpret_cast<const char*>(d), n); }

// a folder of our own for the cases that read a file (one per process, so parallel fuzzers do not meet)
inline const std::filesystem::path& scratch_dir() {
    static const std::filesystem::path p = [] {
        auto d = std::filesystem::temp_directory_path() / ("mousiki_fuzz_" + std::to_string(MUSIC_FUZZ_PID));
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return p;
}
inline std::filesystem::path write_scratch(const char* name, const uint8_t* d, size_t n) {
    const auto f = scratch_dir() / name;
    std::ofstream o(f, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(d), static_cast<std::streamsize>(n));
    return f;
}

// .lrc / plain lyrics text (from the web, from a file next to the track, from the tags)
inline void lyrics_case(const uint8_t* d, size_t n) {
    const std::string raw = text_of(d, n);
    double delay = 0;
    (void)lyrics_read_lines(raw, &delay);
    (void)lyrics_plain_lines(raw);
}

// the JSON of the web services and of the program's own files
inline void json_case(const uint8_t* d, size_t n) {
    const std::string raw = text_of(d, n);
    tinyjson::Value v;
    if (tinyjson::parse(raw, v)) (void)tinyjson::escape(v.as_string());
    tinyjson::Value a;
    (void)tinyjson::parse_answer(raw, a);
}

// one request of the remote app's web server: the first line is the request line, the second the Host header, the rest is ignored
inline void remote_case(const uint8_t* d, size_t n) {
    static const bool once = [] {
#if !defined(_WIN32)
        // a valid command may write a file for the program (remote_control): into a throw-away HOME, never the real one
        setenv("HOME", scratch_dir().string().c_str(), 1);
        unsetenv("XDG_CONFIG_HOME");
#endif
        remote::net::configure({false, 4747, "123456", "", false});
        return true;
    }();
    (void)once;
    const std::string raw = text_of(d, n);
    const size_t e1 = raw.find('\n');
    std::string line = raw.substr(0, e1);
    std::string host;
    if (e1 != std::string::npos) { const size_t e2 = raw.find('\n', e1 + 1); host = raw.substr(e1 + 1, e2 == std::string::npos ? std::string::npos : e2 - e1 - 1); }
    std::string path;
    std::vector<std::pair<std::string, std::string>> q;
    (void)remote::net::parse_request_line(line, path, q);
    (void)remote::net::host_ok(host, "");
    (void)remote::net::host_ok(host, line);
    (void)remote::net::lan_peer(line);
    (void)remote::net::pin_ok(line);
    (void)remote::net::handle_request(line, "192.168.1.20", host);
}

// a playlist of the program (one path a line) and an imported M3U / M3U8
inline void playlist_case(const uint8_t* d, size_t n) {
    write_scratch("fuzz_list.txt", d, n);
    (void)PlaylistManager::load(scratch_dir(), "fuzz_list");
    const auto m3u = write_scratch("fuzz_import.m3u8", d, n);
    std::string name;
    std::vector<std::filesystem::path> tracks;
    int urls = 0;
    std::vector<ExtInf> inf;
    (void)import_parse_m3u(m3u, &name, &tracks, &urls, &inf);
}

// the .cue file of an audio book and its bookmark lines
inline void cue_case(const uint8_t* d, size_t n) {
    const auto f = write_scratch("fuzz_book.cue", d, n);
    BookCue c;
    if (c.load(f)) {
        (void)c.get("TITLE"); (void)c.get("REM NARRATOR"); (void)c.get_raw("REM BOOKMARK");
        c.set("REM PUBLISHER", "x");
        (void)c.save(scratch_dir() / "fuzz_book_out.cue");
    }
    BookMark m;
    if (parse_bookmark(text_of(d, n), m)) (void)format_bookmark(m);
}

// the readers of the tags and the length in the audio files themselves (they run inside the program, on files from anywhere): the first byte
// picks the kind of file, the rest is the file
inline void tags_case(const uint8_t* d, size_t n) {
    static const char* const kExt[] = {".mp3", ".flac", ".ogg", ".opus", ".m4a", ".mp4", ".wav", ".sid", ".aac", ".m4b"};
    if (n == 0) return;
    const char* ext = kExt[d[0] % (sizeof kExt / sizeof kExt[0])];
    const auto f = write_scratch((std::string("fuzz_audio") + ext).c_str(), d + 1, n - 1);
    (void)probe_duration_native(f);
    const std::string e = ext;
    if (e == ".mp3") (void)probe_id3v2_native(f);
    else if (e == ".flac") (void)probe_flac_native(f);
    else if (e == ".ogg" || e == ".opus") (void)probe_ogg_native(f);
    else if (e == ".m4a" || e == ".m4b" || e == ".mp4" || e == ".aac") (void)probe_mp4_native(f);
    else if (e == ".wav") (void)probe_wav_native(f);
    else if (e == ".sid") (void)read_sid_header(f);
    // (the other readers are tried on every file too: a file is not always what its extension says)
    (void)probe_id3v2_native(f); (void)probe_flac_native(f); (void)probe_ogg_native(f); (void)probe_mp4_native(f); (void)probe_wav_native(f);
}

} // namespace muisc::fuzz
