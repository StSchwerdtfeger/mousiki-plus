// Windows: the System Media Transport Controls (SMTC) -- the media pop-up of the volume flyout, the lock screen, and the
// media keys / headset buttons. See media_session.h. Uses C++/WinRT (part of the Windows SDK, MSVC only); every other compiler,
// or an SDK without it, gets the empty stubs at the end and the feature is simply off.
//
// SMTC wants a window of the process: a hidden one is made on a thread of its own, which also runs the message loop and
// owns every WinRT object (so no call has to cross apartments). The program's thread only hands over the newest Info and
// takes the queued commands.
#if defined(_WIN32)

#include "media_session.h"

#if defined(_MSC_VER) && !defined(MOUSIKI_NO_SMTC) && __has_include(<winrt/Windows.Media.h>)
#define MOUSIKI_HAVE_SMTC 1
#endif

#if defined(MOUSIKI_HAVE_SMTC)

// C++/WinRT in C++17 mode pulls in <experimental/coroutine>, which newer MSVC versions refuse unless told they may (this
// file uses no coroutines).
#ifndef _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <systemmediatransportcontrolsinterop.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "windowsapp")

namespace muisc::media::detail {

namespace {

namespace wm = winrt::Windows::Media;
using winrt::Windows::Foundation::TimeSpan;

std::mutex g_mu;
Info g_info;
bool g_dirty = false;
std::vector<remote::Command> g_cmds;
std::atomic<bool> g_stop{false};
// A std::thread that is still joinable when the program ends would call std::terminate(): this one is stopped and joined by
// the destructor, so a program (or a test) that never called shutdown() still ends cleanly.
struct ThreadHolder {
    std::thread t;
    ~ThreadHolder() { g_stop = true; if (t.joinable()) t.join(); }
};
ThreadHolder g_th;

void push(const std::string& method, long long value = 0) {
    Info now;
    { std::lock_guard<std::mutex> lk(g_mu); now = g_info; }
    const remote::Command c = command_for(method, value, now);
    if (c.name.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_cmds.size() < 16) g_cmds.push_back(c);
}

TimeSpan span(double seconds) {
    return TimeSpan{static_cast<int64_t>(std::llround(std::max(0.0, seconds) * 10000000.0))};   // 100 ns ticks
}

winrt::hstring wide(const std::string& s) {
    if (s.empty()) return winrt::hstring{};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return winrt::hstring{};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return winrt::hstring{w};
}

void apply(wm::SystemMediaTransportControls const& smtc, const Info& in, Info& shown, bool& have, std::chrono::steady_clock::time_point& shown_at) {
    const auto now = std::chrono::steady_clock::now();
    const bool meta_changed = !have || in.title != shown.title || in.artist != shown.artist || in.album != shown.album || (in.state == "stopped") != (shown.state == "stopped");
    const bool status_changed = !have || in.state != shown.state;
    const bool length_changed = !have || std::fabs(in.duration - shown.duration) > 0.5;
    // the system moves the position on by itself while playing: only a jump (seek) is sent again
    double expected = shown.position + (shown.state == "playing" ? std::chrono::duration<double>(now - shown_at).count() : 0.0);
    const bool jumped = have && std::fabs(in.position - expected) > 1.5;
    if (!meta_changed && !status_changed && !length_changed && !jumped) return;

    if (meta_changed) {
        auto du = smtc.DisplayUpdater();
        du.Type(wm::MediaPlaybackType::Music);
        auto mp = du.MusicProperties();
        mp.Title(wide(in.title));
        mp.Artist(wide(in.artist));
        mp.AlbumTitle(wide(in.album));
        du.Update();
    }
    if (status_changed || meta_changed) {
        smtc.PlaybackStatus(in.state == "playing" ? wm::MediaPlaybackStatus::Playing
                          : in.state == "paused"  ? wm::MediaPlaybackStatus::Paused
                                                  : wm::MediaPlaybackStatus::Stopped);
    }
    if (status_changed || meta_changed || length_changed || jumped) {
        wm::SystemMediaTransportControlsTimelineProperties tl;
        tl.StartTime(span(0));
        tl.MinSeekTime(span(0));
        const double end = in.duration > 0.0 ? in.duration : std::max(in.position, 1.0);
        tl.EndTime(span(end));
        tl.MaxSeekTime(span(end));
        tl.Position(span(in.position));
        smtc.UpdateTimelineProperties(tl);
    }
    shown = in;
    shown_at = now;
    have = true;
}

void run(std::promise<bool> ready) {
    HWND hwnd = nullptr;
    bool announced = false;
    auto announce = [&](bool ok) { if (!announced) { announced = true; ready.set_value(ok); } };
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Mousiki media controls", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) { announce(false); return; }
        auto interop = winrt::get_activation_factory<wm::SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        wm::SystemMediaTransportControls smtc{nullptr};
        winrt::check_hresult(interop->GetForWindow(hwnd, winrt::guid_of<wm::SystemMediaTransportControls>(), winrt::put_abi(smtc)));
        smtc.IsEnabled(true);
        smtc.IsPlayEnabled(true);
        smtc.IsPauseEnabled(true);
        smtc.IsStopEnabled(true);
        smtc.IsNextEnabled(true);
        smtc.IsPreviousEnabled(true);
        smtc.ButtonPressed([](wm::SystemMediaTransportControls const&, wm::SystemMediaTransportControlsButtonPressedEventArgs const& a) {
            switch (a.Button()) {
                case wm::SystemMediaTransportControlsButton::Play: push("Play"); break;
                case wm::SystemMediaTransportControlsButton::Pause: push("Pause"); break;
                case wm::SystemMediaTransportControlsButton::Stop: push("Stop"); break;
                case wm::SystemMediaTransportControlsButton::Next: push("Next"); break;
                case wm::SystemMediaTransportControlsButton::Previous: push("Previous"); break;
                default: break;
            }
        });
        smtc.PlaybackPositionChangeRequested([](wm::SystemMediaTransportControls const&, wm::PlaybackPositionChangeRequestedEventArgs const& a) {
            const long long us = std::chrono::duration_cast<std::chrono::microseconds>(a.RequestedPlaybackPosition()).count();
            push("SetPosition", us);
        });
        smtc.PlaybackStatus(wm::MediaPlaybackStatus::Stopped);
        announce(true);

        Info shown;
        bool have = false;
        std::chrono::steady_clock::time_point shown_at{};
        while (!g_stop.load()) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            Info in;
            bool dirty;
            { std::lock_guard<std::mutex> lk(g_mu); in = g_info; dirty = g_dirty; g_dirty = false; }
            if (dirty) {
                try { apply(smtc, in, shown, have, shown_at); } catch (...) {}
            }
        }
        try {
            smtc.PlaybackStatus(wm::MediaPlaybackStatus::Closed);
            smtc.IsEnabled(false);
        } catch (...) {}
    } catch (...) {
        announce(false);
    }
    if (hwnd) DestroyWindow(hwnd);
}

} // namespace

bool backend_start() {
    std::promise<bool> p;
    std::future<bool> f = p.get_future();
    g_stop = false;
    g_th.t = std::thread(run, std::move(p));
    if (f.wait_for(std::chrono::seconds(3)) != std::future_status::ready || !f.get()) {
        g_stop = true;
        if (g_th.t.joinable()) g_th.t.join();
        return false;
    }
    return true;
}

void backend_stop() {
    g_stop = true;
    if (g_th.t.joinable()) g_th.t.join();
}

void backend_update(const Info& info) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_info = info;
    g_dirty = true;
}

std::vector<remote::Command> backend_poll(const Info&) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<remote::Command> out;
    out.swap(g_cmds);
    return out;
}

} // namespace muisc::media::detail

#else   // no C++/WinRT (MinGW, an old SDK, MOUSIKI_NO_SMTC): the feature is off

namespace muisc::media::detail {
bool backend_start() { return false; }
void backend_stop() {}
void backend_update(const Info&) {}
std::vector<remote::Command> backend_poll(const Info&) { return {}; }
} // namespace muisc::media::detail

#endif
#endif
