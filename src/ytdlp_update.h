#pragma once
// yt-dlp: "is the installed one old?" and "update it". Online search and video downloads depend on yt-dlp keeping up with YouTube,
// so an old copy is the usual reason for downloads that suddenly fail.
//
//  * At start-up (once a day for the network part, in the background; off with Update Notice = off) Mousiki compares the installed
//    yt-dlp with the newest release, writes one line into the console log and -- when the installed one is two weeks or more behind --
//    shows a recommendation once per newest release.
//  * The command line has `update` (the comparison) and `update yt-dlp` (the update itself).
//  * The update asks yt-dlp to update itself (`yt-dlp -U`: a copy that was downloaded as a program file); a copy that came from pip is
//    updated with pip. A copy that belongs to a package manager (apt, brew, scoop, winget ...) is never touched: the answer names the
//    command to use. Nothing is run with administrator rights; ffmpeg and other programs are not updated here.
#include <filesystem>
#include <string>
#include <vector>

namespace muisc::ytdlp_update {

// ---- helpers (pure; exposed for the tests) --------------------------------------------------------------------------------
// "2026.10.26" -> {2026,10,26}; a nightly "2026.10.26.232815" has a fourth number. Anything else (no date) -> {}.
std::vector<int> parse_version(const std::string& v);
// True when `latest` is a later version than `installed` (a nightly of the same day counts as later than the release). Unreadable: false.
bool is_newer(const std::string& latest, const std::string& installed);
// Days from the date of `older` to the date of `newer`; -1 when one of them is unreadable. Negative when `older` is the later one.
long long days_between(const std::string& older, const std::string& newer);
// "3 days", "about 6 weeks", "about 4 months", "about 2 years" (for a number of days; "less than a day" for 0).
std::string age_text(long long days);

struct Status {
    bool found = true;          // a yt-dlp was found at all
    bool ok = false;            // both versions are known
    std::string installed;      // "2026.07.01"
    std::string latest;         // "2026.10.26" (when the newest release could be read)
    std::string error;          // why not ok
};
// The comparison as one line of text (what the log and the status line say).
std::string describe(const Status& s);
// True when an update is worth recommending: installed is older than the newest and at least kRecommendDays behind.
constexpr long long kRecommendDays = 14;
bool recommended(const Status& s);
// The recommendation itself (the line shown once at start); "" when none is due.
std::string recommendation(const Status& s);

struct State { long long checked = 0; std::string latest, notified; };
std::string state_to_text(const State& s);
State state_from_text(const std::string& text);

// What `yt-dlp -U` said, read as a kind of install.
enum class Kind { UpToDate, Updated, Pip, PackageManager, NotWritable, Unknown };
struct Reading { Kind kind = Kind::Unknown; std::string manager; };   // manager: the name found in the text ("brew", "apt", "scoop" ...)
Reading read_update_output(const std::string& out);
// What to tell the person for a copy that a package manager owns: the command to use.
std::string manager_hint(const std::string& manager);

struct Result {
    bool ok = false;            // the installed version is now the newest (or newer than before)
    bool changed = false;       // the version changed
    std::string before, after;
    std::string message;        // one sentence for the status line / the log
};

// ---- the work (background threads; the answers are taken by the main loop) -----------------------------------------------
// The start-up check. `script` is scripts/check_update.py. Does nothing when Update Notice is off or when it was started already.
void start(const std::filesystem::path& script);
// The recommendation to show, once per newest release: false when there is none. Cheap; call it every frame.
bool take_notice(std::string& text);

// `update` / `update check`: ignores the once-a-day rule and the switch. false when a check or an update is running.
bool check_now(const std::filesystem::path& script);
bool take_check(Status& out);
// `update yt-dlp`. false when a check or an update is already running.
bool update_now();
bool take_update(Result& out);
bool busy();

// The same as one call for the radio's and the player's command line: starts what `arg` names ("", "check", "yt-dlp") and returns the
// answer to type; ok=false when it was not understood or something is running.
std::string run_command(const std::string& arg, const std::filesystem::path& script, bool allow_update, bool& ok);

} // namespace muisc::ytdlp_update
