#pragma once
// Tracker modules and chiptunes (v3.1.0).
//
//   * Tracker modules (MOD, XM, IT, S3M, MPTM, ...) and game-music formats (NSF, SPC, GBS, VGM, AY, HES, KSS, SAP, ...)
//     are decoded by ffmpeg, which reads them through libopenmpt (BSD-3-Clause) and Game Music Emu (LGPL-2.1). Both
//     are part of the usual ffmpeg builds (Debian / Ubuntu / Fedora / Arch packages, the Windows "full" builds) --
//     Mousiki itself links neither, so its Apache-2.0 licence is not touched.
//   * SID tunes (Commodore 64) are rendered by the separate sidplayfp program (GPL-2.0) into a WAV in the cache, which
//     is then played like any other file. sidplayfp is only RUN, never linked or shipped with Mousiki.
#include <filesystem>
#include <string>

namespace muisc {

namespace fs = std::filesystem;

bool is_tracker_file(const fs::path& p);   // MOD / XM / IT / S3M / ... (libopenmpt via ffmpeg)
bool is_gme_file(const fs::path& p);       // NSF / SPC / GBS / VGM / ... (Game Music Emu via ffmpeg)
bool is_sid_file(const fs::path& p);       // SID / PSID / RSID (sidplayfp)
bool is_chiptune_file(const fs::path& p);  // any of the three

// The PSID / RSID header: tune name, author and release line (Latin-1 in the file, returned as UTF-8).
struct SidInfo {
    bool ok = false;
    bool rsid = false;
    std::string name, author, released;
    int songs = 0, start_song = 1;
};
SidInfo read_sid_header(const fs::path& sid);

// Renders `sid` (its default subtune) for `seconds` into a WAV under ~/.cache/mousiki/sid/. A render is reused while
// the SID file and the length are unchanged. false + `err` when sidplayfp is missing or fails.
bool render_sid_to_wav(const fs::path& sid, int seconds, fs::path& out_wav, std::string* err);

// The cache of rendered tunes: the file name part made from the key (the same on every compiler), and the cleaning that deletes the oldest
// .wav files of `dir` until the folder holds at most `limit` bytes (`keep` is never deleted).
std::string sid_cache_hash(const std::string& key);
void sid_cache_prune(const fs::path& dir, const fs::path& keep, unsigned long long limit);

// Whether the ffmpeg on the PATH has the demuxer `name` ("libopenmpt", "libgme"); asked once, then cached.
bool ffmpeg_has_demuxer(const std::string& name);

// Load-time check for the formats ffmpeg plays: "" when the file can be played, otherwise a message saying what
// is missing.
std::string chiptune_support_problem(const fs::path& p);

} // namespace muisc
