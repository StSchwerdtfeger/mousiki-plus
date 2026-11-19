# Fuzzing

The parsers that read input from outside -- lyrics (`.lrc` and plain text), JSON from the web services, playlists (the program's own and
imported M3U / M3U8), the `.cue` file of an audio book and the requests to the remote app's web server -- are fuzzed with
[libFuzzer](https://llvm.org/docs/LibFuzzer.html) under AddressSanitizer and UndefinedBehaviorSanitizer.

| Target | Entry point (see `fuzz_cases.h`) |
|:---|:---|
| `mousiki_fuzz_lyrics` | `lyrics_read_lines`, `lyrics_plain_lines` |
| `mousiki_fuzz_json` | `tinyjson::parse`, `tinyjson::parse_answer` |
| `mousiki_fuzz_remote` | request line + `Host` header: `parse_request_line`, `host_ok`, `lan_peer`, `pin_ok`, `handle_request` |
| `mousiki_fuzz_playlist` | `PlaylistManager::load`, `import_parse_m3u` |
| `mousiki_fuzz_tags` | the tag and length readers of the audio files: `probe_duration_native`, `probe_id3v2_native`, `probe_flac_native`, `probe_ogg_native`, `probe_mp4_native`, `probe_wav_native`, `read_sid_header` (first byte of the input = kind of file) |
| `mousiki_fuzz_cue` | `BookCue::load` / `get` / `set` / `save`, `parse_bookmark` |

## Run it (Clang needed)

```sh
cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Debug -DMOUSIKI_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target mousiki_fuzz_lyrics mousiki_fuzz_json mousiki_fuzz_remote mousiki_fuzz_playlist mousiki_fuzz_cue mousiki_fuzz_tags
mkdir -p work/lyrics
build-fuzz/mousiki_fuzz_lyrics -max_total_time=300 work/lyrics fuzz/corpus/lyrics
```

On Ubuntu the sanitizer runtime is the package `libclang-rt-dev` (`libclang-rt-18-dev`). A crash is written as `crash-<hash>` into the folder
(`-artifact_prefix=crashes/`); run the target with that file as its only argument to see it again.

`fuzz/corpus/` holds valid examples to start from. The GitHub workflow **Fuzz** (`.github/workflows/fuzz.yml`) runs every target once a week
and on demand.

## The same cases in the normal tests

`tests/test_parser_robustness.cpp` feeds the same functions thousands of damaged copies of valid texts on every test run (fixed random seed, a
second or two), so the Linux ASan + UBSan job of the CI checks them on every push. `tests/test_remote_server.cpp` also has the tests with real
sockets: a header far over the limit, connections that send nothing, requests in a row, many at once.

## Not covered yet

The vendored libraries (miniaudio ...) are not fuzzed here; pictures are decoded by ffmpeg in a process of its own, and ffprobe / yt-dlp output is read through the JSON target only.
