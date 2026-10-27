#pragma once
// "A newer version is out": a quiet check of the project's releases page at start-up, at most once a day, in the background.
// The answer is shown ONCE per version (a small window in the player and the radio: ESC closes it, o opens the release page), never installed -- how to
// update depends on how Mousiki was installed (winget, Scoop, Homebrew, the installer ...), so the notice only says where.
// Off with UpdateCheck=false in config.txt (Settings -> ON/OFF -> MODES -> Update Notice). Needs Python 3 like the lyrics;
// without it, or without a network, nothing happens.
#include <filesystem>
#include <string>

namespace muisc::update_check {

// Starts the check (a detached thread; returns at once). `script` is scripts/check_update.py. Does nothing when the check is
// switched off, or when it has already been started.
void start(const std::filesystem::path& script);

struct Notice {
    std::string latest;    // "v4.7.0"
    std::string current;   // "4.7.0"
    std::string url;       // the release page (https://github.com/...)
};
// The notice to show, once: false when there is none (yet). Cheap; call it every frame. After it returned true, the version
// counts as announced and is not shown again. The programs show it as a small overlay ([ESC] Close, [o] Go to release).
bool take(Notice& out);
// The same as one line of text ("" when there is none).
std::string take_notice();
// The page [o] opens: the release page, or the project's releases page when none is known.
std::string release_url(const Notice& n);

// ---- helpers (exposed for the tests) ------------------------------------------------------------------------------------
// True when `latest` ("v4.8.0", "4.8.0") is a higher version than `current` ("4.7.0"). Unreadable versions: false.
bool is_newer(const std::string& latest, const std::string& current);
struct State { long long checked = 0; std::string latest, url, notified; };
std::string state_to_text(const State& s);
State state_from_text(const std::string& text);
// The wording of the notice.
std::string notice_text(const std::string& latest, const std::string& current, const std::string& url);

} // namespace muisc::update_check
