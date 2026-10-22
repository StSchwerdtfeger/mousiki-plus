// macOS: Now Playing + the media keys (MPNowPlayingInfoCenter / MPRemoteCommandCenter). See media_session.h.
// Compiled with ARC (-fobjc-arc, set in CMakeLists.txt). The callbacks only queue commands; the program's main loop reads them.
#if defined(__APPLE__)

#include "media_session.h"

#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>

#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace muisc::media::detail {

namespace {

std::mutex g_mu;
std::vector<remote::Command> g_cmds;
double g_pos = 0.0;               // the newest position the program told us (for "jump to" requests)
bool g_up = false;
Info g_shown;                     // what the Now Playing info was last set to
std::chrono::steady_clock::time_point g_shown_at;
bool g_have_shown = false;

void push(const remote::Command& c) {
    if (c.name.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_cmds.size() < 16) g_cmds.push_back(c);
}

NSString* ns(const std::string& s) {
    NSString* r = [NSString stringWithUTF8String:s.c_str()];   // nil for text that is not valid UTF-8
    return r ? r : @"";
}

void add_handler(MPRemoteCommand* cmd, const char* method) {
    cmd.enabled = YES;
    const std::string m = method;
    [cmd addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
        (void)event;
        Info now;
        { std::lock_guard<std::mutex> lk(g_mu); now.position = g_pos; }
        push(command_for(m, 0, now));
        return MPRemoteCommandHandlerStatusSuccess;
    }];
}

} // namespace

bool backend_start() {
    @autoreleasepool {
        MPRemoteCommandCenter* cc = [MPRemoteCommandCenter sharedCommandCenter];
        if (!cc) return false;
        add_handler(cc.playCommand, "Play");
        add_handler(cc.pauseCommand, "Pause");
        add_handler(cc.togglePlayPauseCommand, "PlayPause");
        add_handler(cc.stopCommand, "Stop");
        add_handler(cc.nextTrackCommand, "Next");
        add_handler(cc.previousTrackCommand, "Previous");
        cc.changePlaybackPositionCommand.enabled = YES;
        [cc.changePlaybackPositionCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* event) {
            MPChangePlaybackPositionCommandEvent* e = (MPChangePlaybackPositionCommandEvent*)event;
            Info now;
            { std::lock_guard<std::mutex> lk(g_mu); now.position = g_pos; }
            push(command_for("SetPosition", std::llround(e.positionTime * 1e6), now));
            return MPRemoteCommandHandlerStatusSuccess;
        }];
    }
    g_up = true;
    return true;
}

void backend_stop() {
    if (!g_up) return;
    g_up = false;
    @autoreleasepool {
        MPRemoteCommandCenter* cc = [MPRemoteCommandCenter sharedCommandCenter];
        for (MPRemoteCommand* c in @[cc.playCommand, cc.pauseCommand, cc.togglePlayPauseCommand, cc.stopCommand,
                                    cc.nextTrackCommand, cc.previousTrackCommand, cc.changePlaybackPositionCommand]) {
            [c removeTarget:nil];
            c.enabled = NO;
        }
        [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = nil;
        [MPNowPlayingInfoCenter defaultCenter].playbackState = MPNowPlayingPlaybackStateStopped;
    }
}

void backend_update(const Info& in) {
    if (!g_up) return;
    { std::lock_guard<std::mutex> lk(g_mu); g_pos = in.position; }
    // The system moves the elapsed time on by itself while the rate is 1.0, so the info is only set again when something
    // changed or the position jumped (a seek).
    const auto now = std::chrono::steady_clock::now();
    bool changed = !g_have_shown || in.state != g_shown.state || in.title != g_shown.title || in.artist != g_shown.artist ||
                   in.album != g_shown.album || std::fabs(in.duration - g_shown.duration) > 0.5;
    if (!changed && in.state == "playing") {
        const double expected = g_shown.position + std::chrono::duration<double>(now - g_shown_at).count();
        changed = std::fabs(in.position - expected) > 1.5;
    } else if (!changed) {
        changed = std::fabs(in.position - g_shown.position) > 0.5;
    }
    if (!changed) return;
    g_shown = in;
    g_shown_at = now;
    g_have_shown = true;
    @autoreleasepool {
        MPNowPlayingInfoCenter* center = [MPNowPlayingInfoCenter defaultCenter];
        if (in.state == "stopped") {
            center.nowPlayingInfo = nil;
            center.playbackState = MPNowPlayingPlaybackStateStopped;
            return;
        }
        NSMutableDictionary* d = [NSMutableDictionary dictionary];
        d[MPMediaItemPropertyTitle] = ns(in.title);
        if (!in.artist.empty()) d[MPMediaItemPropertyArtist] = ns(in.artist);
        if (!in.album.empty()) d[MPMediaItemPropertyAlbumTitle] = ns(in.album);
        if (in.duration > 0.0) d[MPMediaItemPropertyPlaybackDuration] = @(in.duration);
        d[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(in.position);
        d[MPNowPlayingInfoPropertyPlaybackRate] = @(in.state == "playing" ? 1.0 : 0.0);
        center.nowPlayingInfo = d;
        center.playbackState = in.state == "playing" ? MPNowPlayingPlaybackStatePlaying : MPNowPlayingPlaybackStatePaused;
    }
}

std::vector<remote::Command> backend_poll(const Info&) {
    if (!g_up) return {};
    // the system delivers the key presses through the main run loop: let it run for a moment (it does not wait)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, false);
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<remote::Command> out;
    out.swap(g_cmds);
    return out;
}

} // namespace muisc::media::detail

#endif
