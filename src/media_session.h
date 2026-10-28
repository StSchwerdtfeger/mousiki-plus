#pragma once
// The desktop's media controls: what is playing shows up in the system's "now playing" (the volume / media pop-up, the lock
// screen, a panel applet, `playerctl`) and its media keys, headphone buttons and widgets control Mousiki.
//   Linux:  MPRIS (org.mpris.MediaPlayer2.mousiki on the session D-Bus). libdbus is loaded at run time, so nothing has to be
//           installed to build or run; without a D-Bus session (a server, SSH) the feature is simply off.
//   macOS:  MPNowPlayingInfoCenter / MPRemoteCommandCenter (src/media_session_mac.mm): the Now Playing widget and the media keys.
//   Windows: the System Media Transport Controls (src/media_session_win.cpp, C++/WinRT; MSVC + Windows SDK, else off).
//   Others: nothing (bind `mousiki --cmd ...` to keys instead; see the manual).
// The program talks to this only through remote_control.cpp: the status it already publishes goes to update(), the commands that
// come back are the same names as the remote control's (play-pause, next, seek ...), so the player and the radio get it alike.
// Switch off with MediaKeys=false in config.txt (Settings -> GENERAL -> REMOTE / UPDATES -> Media Keys).
#include <string>
#include <vector>

#include "remote_control.h"

namespace muisc::media {

struct Info {
    std::string state = "stopped";   // "playing" | "paused" | "stopped"
    std::string title, artist, album;
    double position = 0.0;           // seconds
    double duration = 0.0;           // seconds; 0 = unknown / a live stream
    int volume = -1;                 // 0-100, -1 unknown
};

bool start();                              // idempotent; false when the feature is off or not available here
void shutdown();                           // at program exit
bool active();                             // the media session is up (update() / poll() do something)
void update(const Info& info);             // cheap; call as often as the status is published
std::vector<remote::Command> poll();       // media keys etc. that arrived since the last call

// ---- helpers (exposed for the tests) ------------------------------------------------------------------------------------
Info info_from_status(const std::vector<std::pair<std::string, std::string>>& kv);   // the keys of `mousiki --status`
// What a media-control request becomes: method "Play" / "Pause" / "PlayPause" / "Stop" / "Next" / "Previous" / "Seek" (offset
// in microseconds), "SetPosition" (absolute, microseconds) or "SetVolume" (0..1 as 0..1000000 millionths). An empty name: not
// supported.
remote::Command command_for(const std::string& method, long long value, const Info& now);

} // namespace muisc::media
