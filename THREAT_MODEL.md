# Threat model

A short, honest description of what Mousiki Plus protects, from whom, and where the limits are. It is meant for people who
want to judge the risk before installing, and for reviewers who want to know where to look. How to report a problem is in
[`SECURITY.md`](SECURITY.md).

## What Mousiki is

A terminal music, radio and audio-book player that runs as **you**, on **your computer**, with your user rights. It is not a
service, has no account, no cloud part and no telemetry. A hobby project by one person: there is no audit, and the
measures below are what is done, not a guarantee.

## What is protected

- Your computer and your files (a crash, a write outside the library or cache, a started program that should not run).
- Your music library and your settings (`config.txt`, playlists, history, cache).
- The Remote app PIN, and who can control the player over the network.

## What is trusted

- The person at the keyboard, and the operating system the program runs on.
- The files that ship in the release and are listed in `SHA256SUMS.txt` / the SBOM.
- Your own settings and the tools you chose to point the program to.

## What is *not* trusted (and what is done about it)

| Input from outside | Risk | What is done |
|---|---|---|
| Audio files and their tags (MP3, FLAC, OGG/Opus, MP4/M4A, WAV, SID, ...), cover images | Crafted file → crash or memory error in a native reader | Bounds-checked readers; libFuzzer targets (`fuzz/`) under ASan/UBSan, run weekly and on every parser change; the normal tests damage real files of each kind on every run |
| Playlists (`.m3u`, `.pls`, `.cue`), `.lrc` lyrics, `.milk` presets | Same, plus paths pointing elsewhere | Same fuzzing and mutation tests; paths are only opened for reading as a track |
| JSON (web answers, `yt-dlp` output, own files) | Deep nesting, huge numbers | Own parser with a depth limit (200) and size checks; fuzzed |
| Internet radio / YouTube / lyrics / MusicBrainz answers | Hostile server, odd text | Treated as data only, length-limited, never run; HTTPS where the service offers it |
| File names, tags and URLs handed to other programs | Argument or command injection | Programs are started without a shell (`posix_spawnp` / `CreateProcessW`) with each argument separate; Windows `.cmd`/`.bat` shims get the extra quoting that stops "BatBadBut"-type tricks |
| Downloaded helper tools (ffmpeg, yt-dlp, Python, ...) | Tampered download | Pinned versions and SHA-256 hashes in the build scripts; `pins-watch` and `vendored-watch` report outdated ones |
| Release files | Tampered package | `SHA256SUMS.txt` per release, SBOM; the Windows installer is **not** code-signed (SmartScreen warns) |

## The Remote app (the one network-facing part)

Off by default. When switched on it is a small web server for a phone app:

- Answers only peers on a **local network** address (loopback, 10/8, 172.16/12, 192.168/16, link-local, 100.64/10); the
  **Host** header must match (DNS-rebinding), GET/HEAD only, requests capped at 8 KB and 3 s, no CORS.
- A **PIN** is needed for everything except a minimal "this is Mousiki" answer; 10 wrong PINs lock that device for 60 s.
  Only a fixed list of player commands exists: no files, no shell, no settings, no paths.
- **Limits you should know:** the connection is plain HTTP and the PIN is part of the URL, so anyone who can *read the traffic
  of your network* (an open Wi-Fi, a hostile device on it) can read the PIN and then control playback (play, pause, skip,
  volume, queue). Use it only on a network you trust, and switch it off when you do not need it. It is not meant to be
  exposed to the internet (port forwarding, a public IP): do not.
- The PIN is random, kept in `config.txt` (readable by your user account), and can be renewed in Settings.

## Out of scope

- An attacker who already has your user account, or who can change files in the program folder.
- A compromised operating system, terminal, or `ffmpeg`/`yt-dlp` build you replaced yourself.
- Denial of service by a person on your own local network (the lockout slows PIN guessing; it is not a firewall).
- The content of the music, lyrics and videos you play (copyright, taste).
- Privacy of what your own requests reveal to the services you use (radio directories, YouTube, MusicBrainz, lyrics sites).

## Where to look

`src/remote_server.cpp` (network), `src/process_util.cpp` (starting programs), `src/tiny_json.h` and the tag readers in
`src/` (parsers), `fuzz/` (fuzz targets), `.github/workflows/` (CodeQL, fuzz, pin and vendor watches, static analysis).
