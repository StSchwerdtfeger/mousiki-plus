#include <memory>
#include <clocale>
#include <iostream>
#include <string>
#include "app.h"
#include "mode_switch.h"
#include "scope_window.h"
#include "remote_control.h"
#include "remote_server.h"
#include "update_check.h"
#include "media_session.h"
#include "app_internal.h"

#if defined(_WIN32)
#include "win_compat.h"
#endif

int main(int argc, char** argv) {
    // `mousiki --cmd next`, `mousiki --status`: remote control of a running Mousiki (src/remote_control.h). No terminal, no audio.
    if (muisc::remote::is_client_call(argc, argv)) {
#if defined(_WIN32)
        muisc::win_bootstrap_env();
#endif
        return muisc::remote::client_main(argc, argv);
    }
    // `mousiki --scope-window`: the oscilloscope window, started by the player / radio itself (src/scope_window.h).
    // It never touches the terminal.
    if (argc > 1 && std::string(argv[1]) == "--scope-window") {
#if defined(_WIN32)
        muisc::win_bootstrap_env();   // HOME, for its position file
#endif
        return muisc::scope_window_main();
    }
    if (argc > 1 && std::string(argv[1]) == "--spectro-window") {   // the spectrogram window, the same way
#if defined(_WIN32)
        muisc::win_bootstrap_env();
#endif
        return muisc::spectro_window_main();
    }
#if defined(_WIN32)
    // Order matters. Both of these have to happen before App's constructor
    // runs: it loads settings, which resolves $HOME, and it may log, which
    // writes UTF-8 through the console.
    //
    //  - win_bootstrap_env() points HOME at %USERPROFILE%, so the seven
    //    getenv("HOME") call sites across settings/cache/snapshot/log all
    //    land in C:\Users\you\ instead of silently falling back to "."
    //    and littering the launch directory with dot-folders.
    //  - win_console_init() switches the console to UTF-8 and turns on VT
    //    escape processing, without which the entire UI renders as literal
    //    escape-sequence text.
    muisc::win_bootstrap_env();
    if (!muisc::win_console_init()) {
        std::cerr << "mousiki: this console does not support ANSI escape sequences.\n"
                     "Run it in Windows Terminal (wt.exe) rather than the legacy\n"
                     "conhost window, or enable \"Use the new console host\" in\n"
                     "the properties of the window you are using.\n";
        return 1;
    }
#endif

    if (!std::setlocale(LC_ALL, "")) {
        std::setlocale(LC_ALL, "C.UTF-8");
    } else {
        const char* cur = std::setlocale(LC_CTYPE, nullptr);
        if (cur && std::string(cur) == "C") {
            std::setlocale(LC_ALL, "C.UTF-8");
        }
    }
    // Every std::stod() call in this codebase -- settings parsing
    // (app.cpp's settings-panel input, settings.cpp's config.txt loader),
    // ffprobe/JSON duration parsing (metadata_probe.cpp, online_source.cpp),
    // and LRC lyric timestamps (lyrics_fetcher.cpp) -- assumes a period
    // decimal separator, because that's what writes it: save_settings()
    // uses a plain ostream `<<`, which is locale-INDEPENDENT and always
    // writes a period, and ffprobe/most CLI tools/APIs emit numbers in a
    // fixed, non-localized format regardless of OS region. std::stod()
    // itself, though, goes through the C library's LC_NUMERIC category --
    // which the setlocale(LC_ALL, "") call above just set to the OS's
    // regional format. On a machine set to German (or French, or any other
    // comma-decimal locale), "0.5" typed into a settings field, or
    // "215.34" read back from ffprobe, gets silently misparsed as "0" /
    // "215" at the first '.', with no exception thrown -- a partial parse
    // still "succeeds" as far as std::stod is concerned. Pinning
    // LC_NUMERIC back to "C" here keeps LC_CTYPE (and whatever else the
    // block above cares about) on the OS locale while making every number
    // this app touches parse the same way on every machine.
    std::setlocale(LC_NUMERIC, "C");

    // The mode switch (SHIFT and +, the '*' character) ends the running mode and starts the other one. The radio is
    // created inside its own scope, so everything it held (audio device, threads, caches) is released before the
    // player comes back; the player is only suspended (see `app` below and src/mode_switch.h).
    int rc = 0;
    std::unique_ptr<muisc::App> app;   // lives while the radio runs (suspended, playback paused); the radio itself is destroyed on every switch
    try {
        muisc::update_check::start(muisc::locate_script("check_update.py"));   // a quiet look for a newer release (once a day, in the background)
        bool radio = (muisc::read_start_mode() == "radio");   // Settings -> ON/OFF -> Start Mode
        for (;;) {
            if (!radio) {
                muisc::set_emoji_replacement(false);   // the radio forces "?" for emoji; the player follows its setting
                if (!app) app = std::make_unique<muisc::App>();
                rc = app->run();
            } else {
                char arg0[] = "mousiki";
                char* argv[] = {arg0, nullptr};
                rc = radio_main(1, argv);
            }
            if (rc != kExitSwitchMode) break;
            radio = !radio;
            rc = 0;
        }
        if (app && app->suspended()) app->shutdown();   // quit from the radio: now the player really ends
        muisc::media::shutdown();               // the system media controls (MPRIS / Now Playing), if on
        muisc::remote::net::shutdown();         // the remote app's web server (if on)
        muisc::scope_window_close();            // the scope window (if open) closes with the program
        muisc::spectro_window_close();          // and the spectrogram window
        muisc::terminal_release_alt_screen();   // (only does something when a mode ended for a switch and the other one never started)
    } catch (const std::exception& e) {
        // Belt-and-braces on top of settings.cpp's own try/catch around
        // config.txt parsing: this catches anything else startup could
        // throw (a bad path, a filesystem error, whatever else), so a
        // hand-edited config -- or any other environment-specific issue --
        // prints a reason and exits cleanly instead of disappearing with
        // no output, which is what a plain uncaught exception does on
        // Windows (it fails fast with no console message at all).
        std::cerr << "mousiki: " << e.what() << "\n";
        rc = 1;
    } catch (...) {
        std::cerr << "mousiki: startup failed for an unrecognized reason.\n";
        rc = 1;
    }

#if defined(_WIN32)
    // App's TerminalIO destructor already left the alternate screen; this
    // puts the code pages and console modes back the way we found them.
    muisc::win_console_restore();
#endif
    return rc;
}
