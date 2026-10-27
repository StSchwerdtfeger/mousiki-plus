#pragma once
// `mousiki --selftest-fetch [--offline] [--verify-pins] [--only lyrics,cover,update,tools,pins,scripts] [--json]`
// Runs scripts/selftest_fetch.py (the fetch health check: lyrics sources, MusicBrainz / Cover Art Archive, the update check,
// ffmpeg / yt-dlp, the pinned downloads) with the output going straight to the terminal. No UI, no audio.
namespace muisc {
// Returns the script's exit code: 0 = no FAIL, 1 = at least one FAIL, 2 = the check could not be started.
int selftest_fetch_main(int argc, char** argv);
}
