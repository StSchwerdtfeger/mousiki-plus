#include "settings.h"
#include "path_utf8.h"
#include "themes.h"
#include "version.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace muisc {

// =====================================================================
// Preset ANSI codes
// =====================================================================

// Migrates a color value from the old named-preset scheme ("cyan",
// "white", ...) to the new bare-number scheme, so a config.txt saved by
// an older build keeps working (and importantly, keeps *displaying* as a
// number in the settings panel) instead of silently going colorless the
// first time ansi_for() can't std::stoi() it. Anything already numeric,
// empty, or unrecognized passes through unchanged.
std::string normalize_color_value(const std::string& value) {
    static const std::unordered_map<std::string, std::string> legacy = {
        {"black", "0"}, {"red", "9"}, {"green", "10"}, {"yellow", "11"},
        {"blue", "12"}, {"magenta", "13"}, {"cyan", "14"}, {"white", "15"}, {"gray", "8"}, {"grey", "8"},
    };
    auto it = legacy.find(value);
    return (it != legacy.end()) ? it->second : value;
}

// Every colour number is a 256-colour palette number in every field (text, background, gradient ends alike): "104" is
// palette colour 104 wherever it is typed. 0 / empty = no colour at all (the terminal's own). Numbers above 255 are not
// clamped -- the terminal decides. config.txt files from before "ColorCodes=palette" used 30-37 / 90-97 (text) and
// 40-47 / 100-107 (background) as plain 16-colour terminal codes; load_settings() converts those once (see
// migrate_legacy_colors()).
std::string ansi_for(const std::string& color_name, bool /*bold*/) {
    if (color_name.empty()) return "";
    int v = 0;
    try { v = std::stoi(color_name); } catch (...) { return ""; }
    if (v <= 0) return "";
    return "\x1b[38;5;" + std::to_string(v) + "m";
}

std::string bg_ansi_for(const std::string& color_name) {
    if (color_name.empty()) return "";
    int v = 0;
    try { v = std::stoi(color_name); } catch (...) { return ""; }
    if (v <= 0) return "";
    return "\x1b[48;5;" + std::to_string(v) + "m";
}

std::string sgr_params_for(const std::string& color_name) {
    if (color_name.empty()) return "";
    int v = 0;
    try { v = std::stoi(color_name); } catch (...) { return ""; }
    if (v <= 0) return "";
    return "38;5;" + std::to_string(v);
}

// =====================================================================
// RGB extraction & brightness
// =====================================================================

// Approximate RGB table for ANSI 256-color indices 0-255.
namespace {
struct RGB { int r, g, b; };

RGB ansi256_to_rgb(int idx) {
    // Standard 16 colors (0-15)
    static const RGB basic16[16] = {
        {0,0,0}, {128,0,0}, {0,128,0}, {128,128,0},
        {0,0,128}, {128,0,128}, {0,128,128}, {192,192,192},
        {128,128,128}, {255,0,0}, {0,255,0}, {255,255,0},
        {0,0,255}, {255,0,255}, {0,255,255}, {255,255,255}
    };
    if (idx >= 0 && idx < 16) return basic16[idx];
    // 216-color cube (indices 16-231): 6×6×6
    if (idx >= 16 && idx <= 231) {
        int ci = idx - 16;
        int bi = ci % 6;
        int gi = (ci / 6) % 6;
        int ri = ci / 36;
        auto v = [](int c) -> int { return c == 0 ? 0 : 55 + 40 * c; };
        return {v(ri), v(gi), v(bi)};
    }
    // Grayscale (indices 232-255)
    if (idx >= 232 && idx <= 255) {
        int g = 8 + (idx - 232) * 10;
        return {g, g, g};
    }
    return {255, 255, 255};
}
} // namespace

bool try_parse_rgb(const std::string& color_value, int& r, int& g, int& b) {
    if (color_value.empty()) return false;
    int idx;
    try { idx = std::stoi(color_value); } catch (...) { return false; }
    if (idx <= 0 || idx > 255) return false; // 0/"none" has no RGB -- caller (gradient_ansi) already special-cases that
    RGB c = ansi256_to_rgb(idx);
    r = c.r; g = c.g; b = c.b;
    return true;
}

int brightness_percent(const std::string& color_value) {
    int r, g, b;
    if (!try_parse_rgb(color_value, r, g, b)) return -1;
    double lum = 0.299 * r + 0.587 * g + 0.114 * b;
    return static_cast<int>(std::clamp(lum / 255.0 * 100.0, 0.0, 100.0));
}

// =====================================================================
// Gradient interpolation
// =====================================================================

std::string gradient_ansi(const std::string& start, const std::string& end, float t, bool bold) {
    if (end.empty()) return ansi_for(start, bold);
    int r0, g0, b0, r1, g1, b1;
    if (!try_parse_rgb(start, r0, g0, b0) || !try_parse_rgb(end, r1, g1, b1)) {
        return ansi_for(start, bold);
    }
    t = std::clamp(t, 0.0f, 1.0f);
    int r = static_cast<int>(r0 + (r1 - r0) * t);
    int g = static_cast<int>(g0 + (g1 - g0) * t);
    int b = static_cast<int>(b0 + (b1 - b0) * t);
    return "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

std::string multi_stop_gradient_ansi(const std::string& left, const std::string& center,
                                      const std::string& right, float t) {
    if (center.empty()) return gradient_ansi(left, right, t);
    t = std::clamp(t, 0.0f, 1.0f);
    if (t <= 0.5f) {
        return gradient_ansi(left, center, t * 2.0f);
    } else {
        return gradient_ansi(center, right, (t - 0.5f) * 2.0f);
    }
}

// A horizontal strip of `width` filled blocks, smoothly interpolated
// between `start` and `end` -- used for the small preview swatches in the
// settings panel (e.g. Border/Disk/Viz's Top-Bottom color pair).
std::string gradient_preview_bar(const std::string& start, const std::string& end, int width) {
    std::string out;
    for (int i = 0; i < width; ++i) {
        float t = (width > 1) ? static_cast<float>(i) / (width - 1) : 0.0f;
        out += gradient_ansi(start, end, t, false) + "\u2588";
    }
    return out + "\x1b[0m";
}

// =====================================================================
// ANSI 256-color + brightness parsing
// =====================================================================

std::string parse_ansi256_brightness(const std::string& value) {
    // Legacy-settings.txt-migration format: "index,brightness" (e.g.
    // "39,100"). The rest of the app now only understands bare 256-color
    // indices ("0".."255"), so brightness scaling can't be preserved here
    // the way it used to (that required emitting a truecolor SGR string,
    // which is no longer a valid stored value) -- this just recovers the
    // base index and drops the brightness scaling.
    auto comma = value.find(',');
    if (comma == std::string::npos) return value;
    try {
        int idx = std::stoi(value.substr(0, comma));
        if (idx >= 0 && idx <= 255) return std::to_string(idx);
    } catch (...) {}
    return value;
}

// =====================================================================
// Font map
// =====================================================================

std::string apply_font_map(const std::string& text,
                            const std::unordered_map<char, std::pair<std::string, std::string>>& font_map) {
    if (font_map.empty()) return text;
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) { 
            out += c; 
            continue; 
        } // pass multi-byte UTF-8 through untouched
        
        char key = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        auto it = font_map.find(key);
        if (it != font_map.end()) {
            bool upper = std::isupper(static_cast<unsigned char>(c));
            out += upper ? it->second.first : it->second.second;
        } else {
            out += c;
        }
    }
    return out;
}

// =====================================================================
// Play mode
// =====================================================================

const char* play_mode_name(int mode) {
    switch (mode) {
        case 1: return "loop";
        case 2: return "shuffle";
        case 3: return "stop";
        case 4: return "queue then stop";
        default: return "list";
    }
}

// =====================================================================
// Themes
// =====================================================================

const std::vector<std::string>& theme_names() {
    static const std::vector<std::string> names = {"default", "neon", "mono", "sunset", "forest"};
    return names;
}

void apply_theme(Settings& s, const std::string& theme_name) {
    if (theme_name == "neon") {
        s.disk_color = "13"; s.disk_color_end = "14";
        s.border_color = "13";
        s.active_line_color = "15"; s.active_word_color = "10"; s.inactive_line_color = "8";
        s.progress_remaining_color = "8"; s.progress_played_color = "10";
        s.list_color = "15"; s.list_playing_color = "10"; s.list_cursor_color = "13";
        s.queue_color = "15"; s.queue_playing_color = "10";
        s.button_color = "13";
        s.visualizer_color = "13"; s.visualizer_color_end = "10";
    } else if (theme_name == "mono") {
        s.disk_color = "15"; s.disk_color_end.clear();
        s.border_color = "8";
        s.active_line_color = "15"; s.active_word_color = "15"; s.inactive_line_color = "8";
        s.progress_remaining_color = "8"; s.progress_played_color = "15";
        s.list_color = "15"; s.list_playing_color = "15"; s.list_cursor_color = "8";
        s.queue_color = "15"; s.queue_playing_color = "15";
        s.button_color = "15";
        s.visualizer_color = "15"; s.visualizer_color_end.clear();
    } else if (theme_name == "sunset") {
        s.disk_color = "11"; s.disk_color_end = "9";
        s.border_color = "9";
        s.active_line_color = "11"; s.active_word_color = "9"; s.inactive_line_color = "8";
        s.progress_remaining_color = "8"; s.progress_played_color = "11";
        s.list_color = "15"; s.list_playing_color = "11"; s.list_cursor_color = "9";
        s.queue_color = "15"; s.queue_playing_color = "11";
        s.button_color = "9";
        s.visualizer_color = "11"; s.visualizer_color_end = "9";
    } else if (theme_name == "forest") {
        s.disk_color = "10"; s.disk_color_end = "11";
        s.border_color = "10";
        s.active_line_color = "15"; s.active_word_color = "10"; s.inactive_line_color = "8";
        s.progress_remaining_color = "8"; s.progress_played_color = "10";
        s.list_color = "15"; s.list_playing_color = "10"; s.list_cursor_color = "10";
        s.queue_color = "15"; s.queue_playing_color = "10";
        s.button_color = "10";
        s.visualizer_color = "10"; s.visualizer_color_end = "11";
    } else { // "default"
        s.disk_color = "14"; s.disk_color_end = "12";
        s.border_color = "8";
        s.active_line_color = "15"; s.active_word_color = "14"; s.inactive_line_color = "8";
        s.progress_remaining_color = "8"; s.progress_played_color = "14";
        s.list_color = "15"; s.list_playing_color = "14"; s.list_cursor_color = "14";
        s.queue_color = "15"; s.queue_playing_color = "14";
        s.button_color = "15";
        s.visualizer_color = "14"; s.visualizer_color_end = "13";
    }
    s.theme_name = theme_name;
}

// =====================================================================
// Default hotkeys
// =====================================================================

void apply_default_hotkeys(Settings& s) {
    // Merged per-key (not "only if the whole map is empty") so that a
    // config.txt which only sets SOME hotkeys -- e.g. an older config
    // saved before new actions like HKeyConsole/HKeyToggleMute existed,
    // or one where the user deliberately left a line out/blank -- still
    // gets sane defaults for whatever it didn't specify, instead of
    // those actions silently having no key bound at all.
    {
        static const std::unordered_map<std::string, std::string> defaults = {
            {"HKeySetting",                     "s"},
            {"HKeyNavigateUp",                  "ARROW_KEY_UP"},
            {"HKeyNavigateDown",                "ARROW_KEY_DOWN"},
            {"HKeyPlay",                        "ENTER"},
            {"HKeySearch",                      "/"},
            {"HKeyPlayNextSong",                "n"},
            {"HKeyPlayPreviousSong",            "b"},
            {"HKeySeekForward",                 "ARROW_KEY_RIGHT"},
            {"HKeySeekBackward",                "ARROW_KEY_LEFT"},
            // "+" / "-" = volume, the same keys as in the radio mode.
            // (Before v3.1.0 they were "1" / "2" and "+" cycled the lyrics
            // area -- that one is "." now; old configs are migrated below.)
            {"HKeyIncreaseVolume",              "+"},
            {"HKeyDecreaseVolume",              "-"},
            {"HKeyAddHoveringSongToQueue",      "a"},
            {"HKeyRemoveHoveringSongFromQueue", "d"},
            {"HKeySwitchBetweenCards",          "TAB"},
            {"HKeyTogglePlayPause",             "p"},
            {"HKeyCyclePlayMode",               "m"},
            {"HKeyFilterForFolder",             "f"},
            {"HKeyClearFilter",                 "c"},
            {"HKeyQuit",                        "q"},
            {"HKeyDownloadStream",              "y"},
            // "k" opens the karaoke overlay; Refresh UI moved to "r" (v3.1.0, migrated below).
            {"HKeyRefreshUi",                   "r"},
            {"HKeyKaraoke",                     "k"},
            {"HKeyConsole",                     "t"},
            {"HKeyToggleMute",                  "x"},
            {"HKeyCheatsheet",                  "?"},
            // The help wizard. "W" (SHIFT+w) was free: it used to be the
            // scope window's key, which moved to ")" in v3.1.0 (see the
            // migration below), and plain "w" stays Toggle Waveform.
            {"HKeyWizard",                      "W"},
            {"HKeyRetryLyrics",                 "l"},
            {"HKeyShuffleNext",                 "#"},
            {"HKeyToggleLyrics",                "."},
            {"HKeyToggleLyricsBack",            ","},
            {"HKeyQueueMoveUp",                 "4"},
            {"HKeyQueueMoveDown",               "5"},
            {"HKeyToggleWaveform",              "w"},
            {"HKeyCycleSortMode",               "T"},
            {"HKeyPlaylist",                    "P"},
            {"HKeyToggleNormalize",             "v"},
            // Shift+N: deliberately the UPPERCASE letter, because plain
            // "n" is already HKeyPlayNextSong and resolve_hotkey_action()
            // only falls back to the other case when a key resolves to
            // nothing (app.cpp handle_key) -- so "N" gives a dedicated
            // Shift+N binding while "n" keeps meaning "next track".
            {"HKeyToggleMetaOnly",              "N"},
            // Shift+M: the meta/tag editor overlay. Same uppercase trick as
            // HKeyToggleMetaOnly above -- plain "m" stays HKeyCyclePlayMode.
            // (SHIFT+b, "fetch metadata for the hovered title", is NOT a
            // hotkey: "B" is indistinguishable from the Down-arrow's
            // collapsed 'B' code by value alone, so it is matched directly
            // in handle_key() together with last_key_was_arrow() instead.)
            {"HKeyMetaEditor",                  "M"},
            // Shift+H: the listening-history overlay (HISTORY / TOP TRACKS
            // / HABITS). Same uppercase convention as M and P above --
            // nothing claimed plain "h", but SHIFT+h is what the overlay is
            // entered with.
            {"HKeyHistory",                     "H"},
            // Shift+X: clear the whole queue (after a Yes/No confirmation).
            // Uppercase on purpose -- plain "x" is HKeyToggleMute.
            {"HKeyClearQueue",                  "X"},
            // Shift+L: the big list overlay (a larger LOCAL AUDIO FILES pane
            // floated over the main UI). Uppercase on purpose -- plain "l" is
            // HKeyRetryLyrics.
            {"HKeyListOverlay",                 "L"},
            // Shift+F: the big folder overlay (a tree of the library's folders;
            // `f` inside it shows a folder in the list). Uppercase on purpose --
            // plain "f" is HKeyFilterForFolder.
            {"HKeyFolderOverlay",               "F"},
            // Shift+C: the file conversion overlay (high resolution files -> MP3 and other lighter formats).
            // Uppercase on purpose; the arrow-key collision this key used to have (Shift+c was read as Right =
            // seek forward) is gone, see App::resolve_hotkey_action().
            {"HKeyConvertOverlay",              "C"},
            // Shift+K: the big queue overlay (a larger QUEUE pane floated
            // over the main UI). Uppercase on purpose -- plain "k" is
            // HKeyRefreshUi, and "q" stays Quit so the overlay can't quit
            // the app by accident.
            {"HKeyQueueOverlay",                "K"},
            // Shift+O: the oscilloscope tuning overlay (decay / dot
            // threshold / tail brightness, live). Uppercase on purpose, same
            // convention as the other SHIFT+letter overlays above.
            {"HKeyOscMenu",                     "O"},
            // The oscilloscope / the spectrogram in their own windows (drawn by the graphics card, src/scope_window.h):
            // the characters of SHIFT+9 / SHIFT+8 on a German keyboard (on a US keyboard SHIFT+0 / SHIFT+9).
            {"HKeyScopeWindow",                 ")"},
            {"HKeySpectroWindow",               "("},
            // Shift+I / Shift+U: spectrogram options overlay / spectrogram full screen.
            {"HKeySpectroMenu",                 "I"},
            {"HKeySpectroFull",                 "U"},
            // The big oscilloscope overlay (YT left, XY right, optional grid): the character of SHIFT+0 on a German keyboard
            // ("=" -- on a US keyboard the unshifted key next to backspace; ")" is the scope window).
            {"HKeyScopeBig",                    "="},
            // Shift+V: the loudness normalisation overlay (on/off, target
            // level, max boost, live). Uppercase on purpose -- plain "v" is
            // HKeyToggleNormalize, same convention as the other overlays.
            {"HKeyNormMenu",                    "V"},
            // Shift+E: the equaliser overlay (10 bands + presets). Uppercase
            // on purpose -- plain "e" is HKeyQueueAddEnd.
            {"HKeyEqualizer",                   "E"},
            // Shift+Z: the sleep timer overlay (15/30/60/90/120 min, stop after
            // the current song). Uppercase on purpose, same convention as the
            // other SHIFT+letter overlays. (Ctrl+Shift+Z is the separate "undo
            // clear queue" key and never reaches the hotkey table.)
            {"HKeySleepTimer",                  "Z"},
            // Queue additions. "e" = add the hovering track to the END of the
            // queue ("a" adds it as NEXT, see App::queue_add_selected()); "e"
            // used to be the never-wired HKeyResetPreference, which is migrated
            // away below. "!" locks the queue (played tracks stay in it).
            // "$" / "%" are SHIFT+4 / SHIFT+5 on both US and German layouts:
            // move the hovering queue item to the top / bottom.
            {"HKeyQueueAddEnd",                 "e"},
            {"HKeyQueueLock",                   "!"},
            {"HKeyQueueMoveTop",                "$"},
            {"HKeyQueueMoveBottom",             "%"},
            // v3.1.x: keys that used to be fixed in the code and are now rebindable (Settings -> REFERENCE). The
            // modifier combinations are written as names (ALT+L, CTRL+L, CTRL+SHIFT+Z, CTRL+SHIFT+U); on a Mac
            // "ALT" is the OPTION key.
            {"HKeyRescan",                      "R"},             // SHIFT+r: look for new files
            {"HKeyFetchTags",                   "B"},             // SHIFT+b: AcoustID lookup of the hovered title
            {"HKeyLyricsTiming",                "ALT+L"},         // lyrics timing overlay
            {"HKeyReadLyrics",                  "CTRL+L"},        // READ LYRICS overlay
            {"HKeyQueueUndoClear",              "CTRL+SHIFT+Z"},  // bring back the queue that was just cleared
            {"HKeyQueueToPlaylist",             "CTRL+SHIFT+U"},  // save the queue as a playlist
        };
        // HKeyResetPreference was defined but nothing ever read it; configs
        // written by older builds still carry it (as "e", which would now
        // collide with HKeyQueueAddEnd -- resolve_hotkey_action() would pick
        // one of the two nondeterministically). Dropped whatever its key is,
        // and no longer written back (see save_settings()'s hkey_order), so
        // the next save removes the line from config.txt for good.
        s.hotkeys.erase("HKeyResetPreference");
        // The three search prefixes (/s: /p: /f:) were listed as hotkeys but typing them was never configurable (the
        // search box parses s: p: f: r: a: sc: b: itself); the lines are dropped so config.txt does not promise anything.
        s.hotkeys.erase("HKeySearchOnline");
        s.hotkeys.erase("HKeySearchPlaylist");
        s.hotkeys.erase("HKeySearchFolder");
        // v3.1.0: volume moved from "1" / "2" to "+" / "-", and the lyrics
        // area cycle from "+" to ".". A config that still carries the old
        // trio exactly as it was shipped is moved over as a whole; a user
        // who rebound any of the three keeps their own keys. The move is
        // skipped if "-" or "." is already taken by another action.
        {
            auto up = s.hotkeys.find("HKeyIncreaseVolume");
            auto down = s.hotkeys.find("HKeyDecreaseVolume");
            auto lyr = s.hotkeys.find("HKeyToggleLyrics");
            if (up != s.hotkeys.end() && down != s.hotkeys.end() && lyr != s.hotkeys.end() &&
                up->second == "1" && down->second == "2" && lyr->second == "+") {
                bool taken = false;
                for (const auto& [action, key] : s.hotkeys) {
                    if (action == "HKeyIncreaseVolume" || action == "HKeyDecreaseVolume" || action == "HKeyToggleLyrics") continue;
                    if (key == "-" || key == ".") { taken = true; break; }
                }
                if (!taken) { up->second = "+"; down->second = "-"; lyr->second = "."; }
            }
        }
        // v3.1.0: "k" is the karaoke overlay now and Refresh UI moved to "r".
        // A config from before (Refresh UI still on "k", no karaoke key yet)
        // is moved over -- unless the user has put something else on "r".
        if (s.hotkeys.find("HKeyKaraoke") == s.hotkeys.end()) {
            auto ref = s.hotkeys.find("HKeyRefreshUi");
            if (ref != s.hotkeys.end() && ref->second == "k") {
                bool r_taken = false;
                for (const auto& [action, key] : s.hotkeys)
                    if (action != "HKeyRefreshUi" && key == "r") { r_taken = true; break; }
                if (!r_taken) ref->second = "r";
                else s.hotkeys["HKeyKaraoke"] = "";   // no free default: left unbound (rebind it under Settings -> REFERENCE)
            }
        }
        // v3.1: the scope window moved from SHIFT+w ("W") to SHIFT+9 (")"), next to the spectrogram window on SHIFT+8.
        {
            auto sw = s.hotkeys.find("HKeyScopeWindow");
            if (sw != s.hotkeys.end() && sw->second == "W") {
                bool taken = false;
                for (const auto& [action, key] : s.hotkeys) if (action != "HKeyScopeWindow" && key == ")") taken = true;
                if (!taken) sw->second = ")";
            }
        }
        for (const auto& [action, key] : defaults) {
            // Only fill actions that are entirely absent from the config.
            // A key explicitly set to "" (user unbound it on purpose) is
            // left alone rather than silently re-bound.
            if (s.hotkeys.find(action) == s.hotkeys.end()) s.hotkeys[action] = key;
        }
        // The wizard's "W" is the one default that can land on an occupied
        // key: the migration above leaves an old HKeyScopeWindow on "W" when
        // ")" is already taken. Two actions on one key would make
        // resolve_hotkey_action() pick between them at random, so the wizard
        // gives way and is left unbound (rebind it under Settings ->
        // REFERENCE). Only a config written before the wizard existed can
        // reach this.
        // The same goes for the keys that became rebindable later (their old fixed key may have been given to another
        // action in the meantime, which could never fire: the fixed key was caught first).
        for (const char* name : {"HKeyWizard", "HKeyRescan", "HKeyFetchTags", "HKeyLyricsTiming", "HKeyReadLyrics",
                                 "HKeyQueueUndoClear", "HKeyQueueToPlaylist"}) {
            auto wiz = s.hotkeys.find(name);
            if (wiz != s.hotkeys.end() && !wiz->second.empty()) {
                for (const auto& [action, key] : s.hotkeys) {
                    if (action != name && key == wiz->second) { wiz->second = ""; break; }
                }
            }
        }
    }
}

// =====================================================================
// Config file path
// =====================================================================

fs::path config_path() {
    const char* home = std::getenv("HOME");
    // HOME is stored as UTF-8 by win_bootstrap_env() (it comes out of
    // GetEnvironmentVariableW, which is UTF-16). fs::path(std::string)
    // would re-read those bytes as ANSI, so a user profile with a
    // non-ASCII name produced a garbled base directory.
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return base / ".config" / "mousiki" / "config.txt";
}

// Legacy path for migration
static fs::path legacy_settings_path() {
    const char* home = std::getenv("HOME");
    // HOME is stored as UTF-8 by win_bootstrap_env() (it comes out of
    // GetEnvironmentVariableW, which is UTF-16). fs::path(std::string)
    // would re-read those bytes as ANSI, so a user profile with a
    // non-ASCII name produced a garbled base directory.
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return base / ".config" / "mousiki" / "settings.txt";
}

// =====================================================================
// Parsing helpers
// =====================================================================

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Strip surrounding quotes: "value" -> value
static std::string unquote(const std::string& s) {
    std::string t = trim(s);
    if (!t.empty() && t.back() == ';') {
        t.pop_back();
        t = trim(t);
    }
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

static const char* viz_default_name(int v) { return v == 0 ? "sphere" : v == 1 ? "osci" : v == 2 ? "spectro" : v == 3 ? "lyrics" : "last"; }

static bool parse_bool(const std::string& v) {
    std::string s = trim(v);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s == "yes" || s == "true" || s == "1";
}

// Parse font_en block: font_en={ A={A,a}, B={B,b}, ... };
static void parse_font_block(std::ifstream& in, Settings& s) {
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.find("};") != std::string::npos || t == "}") break;
        // Expected: A={A,a},  or  A={𝓐,𝓪},
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string letter_str = trim(t.substr(0, eq));
        if (letter_str.empty()) continue;
        char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter_str[0])));
        // Parse {upper,lower}
        size_t ob = t.find('{', eq);
        size_t cb = t.find('}', ob != std::string::npos ? ob : 0);
        if (ob == std::string::npos || cb == std::string::npos) continue;
        std::string inner = t.substr(ob + 1, cb - ob - 1);
        size_t comma = inner.find(',');
        if (comma == std::string::npos) continue;
        std::string upper_glyph = trim(inner.substr(0, comma));
        std::string lower_glyph = trim(inner.substr(comma + 1));
        if (!upper_glyph.empty() && !lower_glyph.empty()) {
            s.font_map[letter] = {upper_glyph, lower_glyph};
        }
    }
}

// The ABOUT APP text is hard coded (about_text.h). Older config files still carry it as a
// ClassTextAboutApp={...}; block, which is skipped here and no longer written back.
static void skip_about_app_block(std::ifstream& in) {
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t == "};" || t == "}") break;
    }
}

// =====================================================================
// Load settings from config.txt
// =====================================================================

static Settings load_from_config(const fs::path& path) {
    Settings s;
    bool have_viz_default = false;  // no VizDefault= line (a config from before): start with the view it was left on
    bool focus_color_set = false;   // no ColorFocus= line: the focus indicator follows the header color
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return s;

    // Skip a UTF-8 byte-order mark if one is present. Several Windows
    // editors (older Notepad, and some "Save As" dialogs even in current
    // versions) write one when saving as UTF-8, which would otherwise
    // land as three bytes glued onto the front of the first key on the
    // first line (e.g. "\xEF\xBB\xBFElimentDisk=true") and make it fail
    // to match anything below.
    {
        unsigned char bom[3] = {0, 0, 0};
        in.read(reinterpret_cast<char*>(bom), 3);
        std::streamsize got = in.gcount();
        // UTF-16 (either byte order) is the other thing a Windows editor's
        // "Save"/"Save As" can silently pick -- and unlike a UTF-8 BOM,
        // there's no way to transparently paper over it here: every ASCII
        // byte below is now followed by a NUL, so nothing would match and
        // the file would parse as entirely empty with no indication why.
        // Fail loudly and specifically instead.
        if (got >= 2 && ((bom[0] == 0xFF && bom[1] == 0xFE) || (bom[0] == 0xFE && bom[1] == 0xFF))) {
            throw std::runtime_error(
                "this file is saved as UTF-16, not UTF-8 -- re-save it as UTF-8 "
                "(in Notepad: File > Save As > Encoding: UTF-8)");
        }
        if (!(got == 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
            in.clear();
            in.seekg(0);
        }
    }

    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        // Font block
        if (t.find("font_en") != std::string::npos && t.find('{') != std::string::npos) {
            parse_font_block(in, s);
            continue;
        }
        // About App text block
        if (t.find("ClassTextAboutApp") != std::string::npos && t.find('{') != std::string::npos) {
            skip_about_app_block(in);
            continue;
        }

        // Try "key : value" (element toggles)
        size_t colon = t.find(':');
        // Try "key == value" (colors, hotkeys, viz)
        size_t eq2 = t.find("==");
        // Try 'key="value"' (border chars)
        size_t eq1 = t.find('=');

        std::string key, value;

        if (eq2 != std::string::npos && (colon == std::string::npos || eq2 < colon)) {
            // "key == value" format
            key = trim(t.substr(0, eq2));
            value = trim(t.substr(eq2 + 2));
        } else if (colon != std::string::npos && (eq1 == std::string::npos || colon < eq1)) {
            // "key : value" format
            key = trim(t.substr(0, colon));
            value = trim(t.substr(colon + 1));
        } else if (eq1 != std::string::npos) {
            // "key=value" or 'key="value"' format
            key = trim(t.substr(0, eq1));
            value = trim(t.substr(eq1 + 1));
        } else {
            continue;
        }

        // --- New CamelCase config key names (current config.txt format) --
        // map straight onto the existing internal keys below, so the rest
        // of this loop doesn't need to change at all. Old-format keys
        // (still handled further down) keep working too, for anyone with
        // an existing config file.
        static const std::unordered_map<std::string, std::string> kKeyAliases = {
            {"ColorBorderTop", "border_color"}, {"ColorBorderBottom", "border_color_bottom"},
            {"ColorDiskTop", "disk_color"}, {"ColorDiskBottom", "disk_color_end"},
            {"ColorMetadataKey", "meta_key_color"}, {"ColorMetadataVal", "meta_val_color"},
            {"ColorVizLeft", "visualizer_color"}, {"ColorVizRight", "visualizer_color_end"},
            {"ColorProgressBarPlayed", "progress_played_color"}, {"ColorProgressBarPending", "progress_remaining_color"},
            {"ColorSearchFg", "search_text_color"}, {"ColorSearchBg", "search_bg_color"},
            {"ColorSearchInactiveFg", "search_inactive_text_color"}, {"ColorSearchInactiveBg", "search_inactive_bg_color"},
            {"ColorListInactiveFg", "list_color"}, {"ColorListInactiveBg", "list_inactive_bg_color"},
            {"ColorListPlayingFg", "list_playing_color"}, {"ColorListPlayingBg", "list_playing_bg_color"},
            {"ColorListCursorFg", "list_cursor_color"}, {"ColorListCursorBg", "list_cursor_bg_color"},
            {"ColorQueueInactiveFg", "queue_color"}, {"ColorQueueInactiveBg", "queue_inactive_bg_color"},
            {"ColorQueuePlayingFg", "queue_playing_color"}, {"ColorQueuePlayingBg", "queue_playing_bg_color"},
            {"ColorQueueCursorFg", "queue_cursor_color"}, {"ColorQueueCursorBg", "queue_cursor_bg_color"},
            {"ColorLyricsInactiveFg", "inactive_line_color"}, {"ColorLyricsInactiveBg", "inactive_line_bg_color"},
            {"ColorLyricsActiveLineFg", "active_line_color"}, {"ColorLyricsActiveLineBg", "active_line_bg_color"},
            {"ColorLyricsActiveWordFg", "active_word_color"}, {"ColorLyricsActiveWordBg", "active_word_bg_color"},
            {"ColorHeader", "header_color"}, {"ColorLegend", "legend_color"}, {"ColorTabCurrent", "tab_current_color"}, {"ColorTabOther", "tab_other_color"},
            {"ColorChangeFg", "change_text_color"}, {"ColorChangeBg", "change_bg_color"},
            {"ColorFocus", "focus_color"},
            {"MetaDataOnly", "meta_only"},
            {"ElimentDisk", "Eliment_disk"}, {"ElimentDummyButtons", "Element_dummy_buttons"},
            {"ElimentQueue", "Eliment_queue"}, {"ElimentWaveForm", "Eliment_waveform_progress_bar"},
            {"ElimentLyrics", "Eliment_lyrics"}, {"VizLyricsField", "Eliment_vizfield"}, {"LyricsPlaceholderBall", "Eliment_lyrics_placeholder_ball"},
            {"LyricViz", "lyric_viz"}, {"NoLyricsViz", "nolyrics_viz"}, {"VizDefault", "viz_default"},
            {"Visualizer", "Eliment_visualizer"},
            {"VisualizerFluidity", "visualizer_fluidity"}, {"DiskRotationSpeed", "disk_rotation_speed"},
            {"VisualizerDegradationSpeed", "visualizer_degradation_speed"}, {"VisualizerViscosity", "visualizer_viscosity"},
            {"LyricsAlignment", "lyrics_alignment"}, {"LyricsAnimation", "lyrics_animation"},
            {"UpperLeftCorner", "upper_left_corner"}, {"UpperRightCorner", "upper_right_corner"},
            {"BottomLeftCorner", "bottom_left_corner"}, {"LowerRightCorner", "lower_right_corner"},
            {"Vertical", "vertical"}, {"Horizontal", "horizontal"},
            {"Seprator", "seprator"}, {"ListSeparator", "list_separator"},
        };
        if (osci_config_key(s, key, value)) continue;
        if (spectro_config_key(s.spectro, key, value)) continue;
        {
            auto it = kKeyAliases.find(key);
            if (it != kKeyAliases.end()) key = it->second;
        }
        // These two don't map onto an existing key 1:1 (string enum ->
        // bool / int), so they're handled directly instead of aliased.
        if (key == "ColorCodes") { s.colors_palette = trim(unquote(value)) == "palette"; continue; }   // the marker: colour numbers are palette numbers
        if (key == "WaveformStyle") { s.waveform_smooth = (value == "smooth"); continue; }
        if (key == "StartMode") {
            std::string v = trim(unquote(value));
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s.start_mode = (v == "radio") ? "radio" : "player";
            continue;
        }
        if (key == "PlaybackMode") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "loop") s.play_mode = 1;
            else if (v == "shuffle") s.play_mode = 2;
            else if (v == "stop") s.play_mode = 3;
            else if (v == "queue then stop" || v == "queue stop" || v == "repeat queue") s.play_mode = 4; // "repeat queue" = what older versions called it
            else s.play_mode = 0; // "list" or anything unrecognized
            continue;
        }

        // --- Element toggles ---
        if (key == "Eliment_disk" || key == "Element_disk") { s.element_disk = parse_bool(value); continue; }
        if (key == "Element_dummy_buttons") { s.element_dummy_buttons = parse_bool(value); continue; }
        if (key == "Eliment_queue" || key == "Element_queue") { s.element_queue = parse_bool(value); continue; }
        if (key == "Eliment_waveform_progress_bar" || key == "Element_waveform") { s.element_waveform = parse_bool(value); continue; }
        if (key == "Eliment_lyrics" || key == "Element_lyrics") { s.element_lyrics = parse_bool(value); continue; }
        if (key == "Eliment_vizfield") { s.element_vizfield = parse_bool(value); continue; }
        if (key == "UseLyrics") { s.use_lyrics = parse_bool(value); continue; }
        if (key == "UseOscilloscope") { s.use_osci = parse_bool(value); continue; }
        if (key == "UseSpectrogram") { s.use_spectro = parse_bool(value); continue; }
        if (key == "UseSphere") { s.use_sphere = parse_bool(value); continue; }
        if (key == "VizOrder") {
            // "sphere,osci,spectro,lyrics" in any order; names that are missing are appended in the default order, so
            // a short or damaged list never loses a view.
            std::vector<int> order;
            std::string item;
            std::string v = trim(unquote(value)) + ",";
            for (char ch : v) {
                if (ch != ',' && ch != ' ' && ch != '>' && ch != ';') { item += static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); continue; }
                if (item.empty()) continue;
                int id = (item == "sphere") ? 0 : (item == "osci" || item == "oscilloscope") ? 1
                       : (item == "spectro" || item == "spectrogram") ? 2 : (item == "lyrics") ? 3 : -1;
                if (id >= 0 && std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
                item.clear();
            }
            for (int id = 0; id < 4; ++id) if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
            for (int i = 0; i < 4; ++i) s.viz_order[i] = order[static_cast<size_t>(i)];
            continue;
        }
        if (key == "Eliment_lyrics_placeholder_ball" || key == "Element_lyrics_placeholder_ball") {
            // Legacy "Lyric Ball" on/off switch, now a two-way pick
            // (LyricViz=sphere|osci) with no "off" state: true keeps the
            // ball, false -- "not the ball" -- picks the oscilloscope, so
            // an old config never gets the ball it had turned off.
            s.lyric_viz = parse_bool(value) ? 0 : 1;
            continue;
        }
        if (key == "lyric_viz") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "osci" || v == "oscilloscope") s.lyric_viz = 1;
            else if (v == "sphere") s.lyric_viz = 0;
            else if (v == "spectro" || v == "spectrogram") s.lyric_viz = 2;
            continue; // anything unrecognized keeps the current value
        }
        if (key == "viz_default") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "sphere") s.viz_default = 0;
            else if (v == "osci" || v == "oscilloscope") s.viz_default = 1;
            else if (v == "spectro" || v == "spectrogram") s.viz_default = 2;
            else if (v == "lyrics") s.viz_default = 3;
            else if (v == "last" || v == "last used" || v == "last_used" || v == "lastused") s.viz_default = 4;
            have_viz_default = true;
            continue;
        }
        if (key == "nolyrics_viz") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "osci" || v == "oscilloscope") s.nolyrics_viz = 1;
            else if (v == "sphere") s.nolyrics_viz = 0;
            else if (v == "spectro" || v == "spectrogram") s.nolyrics_viz = 2;
            else if (v == "none" || v == "off" || v == "nothing") s.nolyrics_viz = 3;
            continue;
        }
        if (key == "osci_stereo") continue; // retired setting: old configs may still contain it, it is ignored
        if (key == "lyrics_alignment") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "left") s.lyrics_alignment = 1;
            else if (v == "right") s.lyrics_alignment = 2;
            else s.lyrics_alignment = 0; // "center" or anything unrecognized
            continue;
        }
        if (key == "lyrics_animation") {
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "word_by_word" || v == "word by word") s.lyrics_animation = 1;
            else if (v == "letter_by_letter" || v == "letter by letter") s.lyrics_animation = 2;
            else if (v == "only_active_line" || v == "active line only") s.lyrics_animation = 3;
            else if (v == "only_active_word" || v == "active word only") s.lyrics_animation = 4;
            else if (v == "line_by_line" || v == "line by line") s.lyrics_animation = 5;
            else s.lyrics_animation = 0; // "full" or anything unrecognized
            continue;
        }
        if (key == "Eliment_visualizer" || key == "Element_visualizer") { s.element_visualizer = parse_bool(value); continue; }
        if (key == "meta_only") { s.meta_only = parse_bool(value); continue; }

        // --- Border characters ---
        if (key == "upper_left_corner") { s.box_upper_left = unquote(value); continue; }
        if (key == "upper_right_corner") { s.box_upper_right = unquote(value); continue; }
        if (key == "bottom_left_corner") { s.box_lower_left = unquote(value); continue; }
        if (key == "lower_right_corner") { s.box_lower_right = unquote(value); continue; }
        if (key == "vertical") { s.box_vertical = unquote(value); continue; }
        if (key == "horizontal") { s.box_horizontal = unquote(value); continue; }
        if (key == "seprator" || key == "separator") { s.meta_separator = unquote(value); continue; }
        if (key == "list_separator") { s.list_separator = unquote(value); continue; }

        // --- Visualizer tuning ---
        if (key == "viz_style") { try { s.viz_style = std::stoi(value); } catch (...) {} continue; }
        if (key == "viz_bands") { try { s.viz_bands = std::clamp(std::stoi(value), 8, 64); } catch (...) {} continue; }
        if (key == "viz_density") { try { s.viz_density = std::clamp(std::stoi(value), 1, 5); } catch (...) {} continue; }

        // --- Colors (ANSI 256,brightness format) ---
        auto set_color = [&](const std::string& k, const std::string& v, std::string& field) {
            if (key == k) { field = parse_ansi256_brightness(unquote(v)); return true; }
            return false;
        };
        if (set_color("border", value, s.border_color)) continue;
        if (set_color("title", value, s.meta_val_color)) continue;
        if (set_color("meta_key", value, s.meta_key_color)) continue;
        if (set_color("meta_val", value, s.meta_val_color)) continue;
        if (set_color("header", value, s.header_color)) continue;
        if (set_color("progress", value, s.progress_played_color)) continue;
        if (set_color("progress_bg", value, s.progress_remaining_color)) continue;
        if (set_color("status", value, s.status_color)) continue;
        if (set_color("playlist", value, s.list_color)) continue;
        if (set_color("pl_active_fg", value, s.list_cursor_color)) continue;
        if (set_color("pl_active_bg", value, s.pl_active_bg_color)) continue;
        if (set_color("viz", value, s.visualizer_color)) continue;
        if (set_color("viz_left", value, s.viz_left_color)) continue;
        if (set_color("viz_center", value, s.viz_center_color)) continue;
        if (set_color("viz_right", value, s.viz_right_color)) continue;
        if (set_color("lyr_inactive", value, s.lyr_inactive_color)) continue;
        if (set_color("lyr_active_line", value, s.lyr_active_line_color)) continue;
        if (set_color("lyr_active_word", value, s.lyr_active_word_color)) continue;

        // --- Extended color fields (direct name=value for compatibility) ---
        if (key == "disk_color") { if (!value.empty()) s.disk_color = normalize_color_value(value); continue; }
        if (key == "disk_color_end") { if (!value.empty()) s.disk_color_end = normalize_color_value(value); continue; }
        if (key == "disk_color_gradient") { s.disk_color_gradient = parse_bool(value); continue; }
        if (key == "disk_rotation_speed") { try { s.disk_rotation_speed = std::clamp(std::stod(value), 0.01, 1.00); } catch (...) {} continue; }
        if (key == "border_color") { if (!value.empty()) s.border_color = normalize_color_value(value); continue; }
        if (key == "border_color_bottom") { if (!value.empty()) s.border_color_bottom = normalize_color_value(value); continue; }
        if (key == "header_color") { if (!value.empty()) s.header_color = normalize_color_value(value); continue; }
        if (key == "tab_current_color") { if (!value.empty()) s.tab_current_color = normalize_color_value(value); continue; }
        if (key == "tab_other_color") { if (!value.empty()) s.tab_other_color = normalize_color_value(value); continue; }
        if (key == "legend_color") { if (!value.empty()) s.legend_color = normalize_color_value(value); continue; }
        if (key == "focus_color") { if (!value.empty()) { s.focus_color = normalize_color_value(value); focus_color_set = true; } continue; }
        if (key == "change_text_color") { if (!value.empty()) s.change_text_color = normalize_color_value(value); continue; }
        if (key == "change_bg_color") { if (!value.empty()) s.change_bg_color = normalize_color_value(value); continue; }
        if (key == "active_line_color") { if (!value.empty()) s.active_line_color = normalize_color_value(value); continue; }
        if (key == "active_line_bg_color") { if (!value.empty()) s.active_line_bg_color = normalize_color_value(value); continue; }
        if (key == "active_word_color") { if (!value.empty()) s.active_word_color = normalize_color_value(value); continue; }
        if (key == "BorderGradient" || key == "border_gradient") { { std::string v = value; for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); s.border_smooth = (v != "rough"); } continue; }
        if (key == "active_word_bg_color") { if (!value.empty()) s.active_word_bg_color = normalize_color_value(value); continue; }
        if (key == "inactive_line_color") { if (!value.empty()) s.inactive_line_color = normalize_color_value(value); continue; }
        if (key == "inactive_line_bg_color") { if (!value.empty()) s.inactive_line_bg_color = normalize_color_value(value); continue; }
        if (key == "progress_remaining_color") { if (!value.empty()) s.progress_remaining_color = normalize_color_value(value); continue; }
        if (key == "progress_played_color") { if (!value.empty()) s.progress_played_color = normalize_color_value(value); continue; }
        if (key == "progress_timestamp_color") { if (!value.empty()) s.progress_timestamp_color = normalize_color_value(value); continue; }
        if (key == "meta_key_color") { if (!value.empty()) s.meta_key_color = normalize_color_value(value); continue; }
        if (key == "meta_val_color") { if (!value.empty()) s.meta_val_color = normalize_color_value(value); continue; }
        if (key == "search_text_color") { if (!value.empty()) s.search_text_color = normalize_color_value(value); continue; }
        if (key == "search_bg_color") { if (!value.empty()) s.search_bg_color = normalize_color_value(value); continue; }
        if (key == "search_inactive_text_color") { if (!value.empty()) s.search_inactive_text_color = normalize_color_value(value); continue; }
        if (key == "search_inactive_bg_color") { if (!value.empty()) s.search_inactive_bg_color = normalize_color_value(value); continue; }
        if (key == "list_color") { if (!value.empty()) s.list_color = normalize_color_value(value); continue; }
        if (key == "list_inactive_bg_color") { if (!value.empty()) s.list_inactive_bg_color = normalize_color_value(value); continue; }
        if (key == "list_playing_color") { if (!value.empty()) s.list_playing_color = normalize_color_value(value); continue; }
        if (key == "list_cursor_color") { if (!value.empty()) s.list_cursor_color = normalize_color_value(value); continue; }
        if (key == "list_playing_bg_color") { if (!value.empty()) s.list_playing_bg_color = normalize_color_value(value); continue; }
        if (key == "list_cursor_bg_color") { if (!value.empty()) s.list_cursor_bg_color = normalize_color_value(value); continue; }
        if (key == "queue_color") { if (!value.empty()) s.queue_color = normalize_color_value(value); continue; }
        if (key == "queue_inactive_bg_color") { if (!value.empty()) s.queue_inactive_bg_color = normalize_color_value(value); continue; }
        if (key == "queue_playing_color") { if (!value.empty()) s.queue_playing_color = normalize_color_value(value); continue; }
        if (key == "queue_playing_bg_color") { if (!value.empty()) s.queue_playing_bg_color = normalize_color_value(value); continue; }
        if (key == "queue_cursor_color") { if (!value.empty()) s.queue_cursor_color = normalize_color_value(value); continue; }
        if (key == "queue_cursor_bg_color") { if (!value.empty()) s.queue_cursor_bg_color = normalize_color_value(value); continue; }
        if (key == "button_color") { if (!value.empty()) s.button_color = normalize_color_value(value); continue; }
        if (key == "visualizer_color") { if (!value.empty()) s.visualizer_color = normalize_color_value(value); continue; }
        if (key == "visualizer_color_end") { if (!value.empty()) s.visualizer_color_end = normalize_color_value(value); continue; }
        if (key == "visualizer_fluidity") { try { s.visualizer_fluidity = std::clamp(std::stoi(value), 1, 10); } catch (...) {} continue; }
        if (key == "visualizer_degradation_speed") { try { s.visualizer_degradation_speed = std::clamp(std::stoi(value), 1, 10); } catch (...) {} continue; }
        if (key == "visualizer_viscosity") { try { s.visualizer_viscosity = std::clamp(std::stoi(value), 0, 10); } catch (...) {} continue; }
        if (key == "theme_name") { s.theme_name = value; continue; }
        if (key == "play_mode") { try { s.play_mode = std::clamp(std::stoi(value), 0, 4); } catch (...) {} continue; }
        if (key == "waveform_smooth") { s.waveform_smooth = parse_bool(value); continue; }

        // --- Console logging -------------------------------------------
        if (key == "ConsoleVerbosity") {
            std::string v = unquote(value);
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s.console_verbosity = (v == "verbose") ? 1 : 0;
            continue;
        }

        // --- Emoji handling ---------------------------------------------
        if (key == "ReplaceEmoji") { s.replace_emoji = parse_bool(value); continue; }
        if (key == "MarqueeScrolling") { s.marquee = parse_bool(value); continue; }

        // --- Stereo ------------------------------------------------------
        if (key == "StereoPlayback") { s.stereo = parse_bool(value); continue; }

        // --- Loudness normalisation -------------------------------------
        if (key == "NormalizeVolume") { s.normalize = parse_bool(value); continue; }
        if (key == "NormalizeTargetLufs") { try { s.normalize_target_lufs = std::clamp(std::stod(value), -40.0, 0.0); } catch (...) {} continue; }
        if (key == "NormalizeMaxBoostDb") { try { s.normalize_max_boost_db = std::clamp(std::stod(value), 0.0, 24.0); } catch (...) {} continue; }

        // --- Equaliser ---------------------------------------------------
        if (key == "EqualizerEnabled") { s.eq_enabled = parse_bool(value); continue; }
        if (key == "EqualizerBands") {
            // "0,3,-2,..." -- ten comma-separated dB values. Anything short or
            // malformed is ignored as a whole rather than half-applied.
            EqGains g{};
            size_t pos = 0; int n = 0; bool ok = true;
            while (pos <= value.size() && n < kEqBands) {
                size_t comma = value.find(',', pos);
                std::string tok = value.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                try { g[n++] = std::clamp(std::stof(tok), kEqMinDb, kEqMaxDb); } catch (...) { ok = false; break; }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (ok && n == kEqBands) s.eq_gains = g;
            continue;
        }
        if (key == "EqualizerPreset") {
            // "<ten comma-separated gains>|<name>" -- one line per custom
            // preset. The gains come first so the name may contain any
            // character (a '|' included). A malformed line, an empty name,
            // a name that a built-in preset already uses, a repeated name
            // and anything past the limit are skipped, never half-applied.
            const size_t bar = value.find('|');
            if (bar == std::string::npos) continue;
            const std::string gains_part = value.substr(0, bar);
            std::string name = trim(value.substr(bar + 1));
            if (name.empty() || eq_name_reserved(name)) continue;
            if (s.eq_custom_presets.size() >= kEqMaxCustomPresets) continue;
            bool dup = false;
            for (const auto& cp : s.eq_custom_presets) if (eq_name_equal(cp.name, name)) { dup = true; break; }
            if (dup) continue;
            EqCustomPreset cp;
            size_t pos = 0; int n = 0; bool ok = true;
            while (pos <= gains_part.size() && n < kEqBands) {
                size_t comma = gains_part.find(',', pos);
                std::string tok = gains_part.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                try { cp.gains[n++] = std::clamp(std::stof(tok), kEqMinDb, kEqMaxDb); } catch (...) { ok = false; break; }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (ok && n == kEqBands) {
                cp.name = std::move(name);
                s.eq_custom_presets.push_back(std::move(cp));
            }
            continue;
        }

        // --- Autosave / session snapshot --------------------------------
        if (key == "SleepFade") { s.sleep_fade = parse_bool(value); continue; }
        if (key == "WizardSeen") { s.wizard_seen = parse_bool(value); continue; }
        if (key == "SidPlayLength") {
            try { s.sid_play_length = std::clamp(std::stoi(trim(unquote(value))), 10, 1800); } catch (...) {}
            continue;
        }
        if (key == "KaraokeLyricsSize") {
            try { s.karaoke_lyrics_size = std::clamp(std::stoi(trim(unquote(value))), 1, 5); } catch (...) {}
            continue;
        }
        if (key == "TranslateService") {
            std::string v = trim(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (v == "google" || v == "mymemory") s.translate_service = v;   // (LibreTranslate was dropped: it needs a paid API key)
            continue;
        }
        if (key == "TranslateTo" || key == "TranslateFrom") {
            std::string v = trim(unquote(value));
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (!v.empty() && v.size() <= 8) (key == "TranslateTo" ? s.translate_to : s.translate_from) = v;
            if (s.translate_to == "auto") s.translate_to = "en";
            continue;
        }
        if (key == "TranslateUrl" || key == "TranslateKey") continue;   // retired with LibreTranslate: old configs may still hold them
        if (key == "AutoSave") { s.autosave_enabled = parse_bool(value); continue; }
        if (key == "AutoSaveIndicator") { s.autosave_indicator = parse_bool(value); continue; }
        if (key == "AutoSaveDelayInSec") { try { s.autosave_delay_sec = std::max(1, std::stoi(value)); } catch (...) {} continue; }
        if (key == "AutoSaveChr") {
            // Only one character allowed: keep just the first UTF-8
            // codepoint of whatever was typed there, not the first byte
            // (a multi-byte glyph like "•" would otherwise get sliced).
            std::string v = unquote(value);
            if (!v.empty()) {
                unsigned char c0 = static_cast<unsigned char>(v[0]);
                size_t len = 1;
                if ((c0 & 0x80) == 0x00) len = 1;
                else if ((c0 & 0xE0) == 0xC0) len = 2;
                else if ((c0 & 0xF0) == 0xE0) len = 3;
                else if ((c0 & 0xF8) == 0xF0) len = 4;
                s.autosave_chr = v.substr(0, std::min(len, v.size()));
            }
            continue;
        }
        if (key == "AutoSaveIndicatorType") {
            std::string v = unquote(value);
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            s.autosave_indicator_type = (v == "color") ? 1 : 0; // "blink" or anything unrecognized
            continue;
        }
        if (key == "AutoSaveC1") { if (!unquote(value).empty()) s.autosave_c1 = normalize_color_value(unquote(value)); continue; }
        if (key == "AutoSaveC2") { if (!unquote(value).empty()) s.autosave_c2 = normalize_color_value(unquote(value)); continue; }

        // --- Local music library paths ---
        // Each LocalMusicPath= line appends one directory.
        // A leading ~ is expanded to $HOME so users can write:
        //   LocalMusicPath=~/Music/Rock
        if (key == "LocalMusicPath" || key == "local_music_path") {
            std::string path = trim(unquote(value));
            if (!path.empty()) {
                if (path[0] == '~') {
                    const char* home = std::getenv("HOME");
                    if (home) path = std::string(home) + path.substr(1);
                }
                s.local_music_paths.push_back(path);
            }
            continue;
        }

        // --- Playlists folder override ---
        // Each PlaylistsPath= line adds one folder, independent of
        // local_music_paths[0] (the first one is where playlists get
        // saved/deleted; all of them are searched when listing/loading).
        // Same ~ expansion as LocalMusicPath, above.
        if (key == "PlaylistsPath" || key == "playlists_path") {
            std::string path = trim(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty() && s.playlists_paths.empty()) s.playlists_paths.push_back(path);   // ONE playlist folder: further lines are ignored
            continue;
        }

        // --- Download folder (single) ---
        // Where yt-dlp writes its output: one folder, not a list. Empty
        // stays empty on purpose -- an unset DownloadFolder means "the
        // built-in cache folder", and keeping it unset (rather than
        // materialising the default into the file) is what lets a changed
        // default take effect without rewriting anyone's config.txt.
        // Same ~ expansion as LocalMusicPath, above.
        if (key == "PlaylistExportPath" || key == "playlist_export_path") {
            std::string path = trim(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty()) s.playlist_export_path = path;
            continue;
        }
        if (key == "ConversionExportPath" || key == "conversion_export_path") {
            std::string path = trim(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty()) s.conversion_export_path = path;
            continue;
        }
        if (key == "HistoryPath" || key == "history_path") {
            std::string path = trim(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty()) s.history_path = path;
            continue;
        }
        if (key == "DownloadFolder" || key == "download_folder") {
            std::string path = trim(unquote(value));
            if (!path.empty() && path[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) path = std::string(home) + path.substr(1);
            }
            if (!path.empty()) s.download_folder = path;
            continue;
        }

        // --- Hotkeys ---
        if (key.substr(0, 4) == "HKEY" || key.substr(0, 4) == "KHEY" || key.substr(0, 4) == "HKey") {
            s.hotkeys[key] = unquote(value);
            continue;
        }
    }

    if (!focus_color_set) s.focus_color = s.header_color;
    // The field starts with the default view. A config from before VizDefault existed starts with the view it was left on.
    if (!have_viz_default) s.viz_default = 4;
    if (s.viz_default <= 3) {   // 4 = last used: the view the "." key left (ElimentLyrics / LyricViz) stays
        s.element_lyrics = s.viz_default == 3;
        if (s.viz_default < 3) s.lyric_viz = s.viz_default;
    }
    return s;
}

// =====================================================================
// Legacy settings.txt loader (for migration)
// =====================================================================

static Settings load_from_legacy(const fs::path& path) {
    Settings s;
    std::ifstream in(path);
    if (!in.is_open()) return s;

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }

    auto get = [&](const char* key, std::string& field) { if (kv.count(key)) field = kv[key]; };
    auto get_int = [&](const char* key, int& field) { if (kv.count(key)) { try { field = std::stoi(kv[key]); } catch (...) {} } };
    auto get_dbl = [&](const char* key, double& field) { if (kv.count(key)) { try { field = std::stod(kv[key]); } catch (...) {} } };

    get_int("visualizer_fluidity", s.visualizer_fluidity);
    get("disk_color", s.disk_color);
    get("disk_color_end", s.disk_color_end);
    get_dbl("disk_rotation_speed", s.disk_rotation_speed);
    get("border_color", s.border_color);
    get("active_line_color", s.active_line_color);
    get("active_word_color", s.active_word_color);
    get("inactive_line_color", s.inactive_line_color);
    get("progress_remaining_color", s.progress_remaining_color);
    get("progress_played_color", s.progress_played_color);
    get("list_color", s.list_color);
    get("list_playing_color", s.list_playing_color);
    get("list_cursor_color", s.list_cursor_color);
    get("queue_color", s.queue_color);
    get("queue_playing_color", s.queue_playing_color);
    get("button_color", s.button_color);
    get("visualizer_color", s.visualizer_color);
    get("visualizer_color_end", s.visualizer_color_end);
    get("theme_name", s.theme_name);
    get_int("play_mode", s.play_mode);
    if (kv.count("queue_visible")) s.element_queue = (kv["queue_visible"] == "1" || kv["queue_visible"] == "true");
    if (kv.count("waveform_smooth")) s.waveform_smooth = (kv["waveform_smooth"] == "1" || kv["waveform_smooth"] == "true");

    s.visualizer_fluidity = std::clamp(s.visualizer_fluidity, 1, 10);
    s.play_mode = std::clamp(s.play_mode, 0, 4);
    return s;
}

// =====================================================================
// Public load/save
// =====================================================================

std::string read_start_mode() {
    std::ifstream in(config_path());
    std::string line, mode = "player";
    while (std::getline(in, line)) {
        if (line.compare(0, 10, "StartMode=") != 0) continue;
        std::string v = trim(unquote(line.substr(10)));
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        mode = (v == "radio") ? "radio" : "player";
    }
    return mode;
}

bool write_start_mode(const std::string& mode) {
    const std::string want = std::string("StartMode=") + (mode == "radio" ? "radio" : "player");
    const fs::path p = config_path();
    std::vector<std::string> lines;
    {
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    bool done = false;
    for (auto& l : lines) if (!done && l.compare(0, 10, "StartMode=") == 0) { l = want; done = true; }
    if (!done) lines.push_back(want);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::trunc);
    if (!out.is_open()) return false;
    for (const auto& l : lines) out << l << "\n";
    return out.good();
}

// A config.txt from before "ColorCodes=palette": 30-37 / 90-97 in text fields and 40-47 / 100-107 in background fields were plain
// 16-colour terminal codes. They become the palette numbers that look the same (black = 16, since 0 means "no colour").
// The ends of colour fades (disk, visualizer) were always palette numbers and stay.
static void migrate_legacy_colors(Settings& s) {
    using themes::ColorRole;
    for (std::string* f : {&s.border_color, &s.border_color_bottom, &s.meta_key_color, &s.meta_val_color, &s.progress_played_color,
                           &s.progress_remaining_color, &s.progress_timestamp_color, &s.search_text_color, &s.search_inactive_text_color,
                           &s.list_color, &s.list_playing_color, &s.list_cursor_color, &s.queue_color, &s.queue_playing_color,
                           &s.queue_cursor_color, &s.inactive_line_color, &s.active_line_color, &s.active_word_color, &s.header_color,
                           &s.legend_color, &s.tab_current_color, &s.tab_other_color, &s.change_text_color, &s.focus_color,
                           &s.button_color, &s.status_color, &s.lyr_inactive_color, &s.lyr_active_line_color, &s.lyr_active_word_color})
        *f = themes::legacy_to_palette(*f, ColorRole::Text);
    for (std::string* f : {&s.search_bg_color, &s.search_inactive_bg_color, &s.list_inactive_bg_color, &s.list_playing_bg_color,
                           &s.list_cursor_bg_color, &s.queue_inactive_bg_color, &s.queue_playing_bg_color, &s.queue_cursor_bg_color,
                           &s.inactive_line_bg_color, &s.active_line_bg_color, &s.active_word_bg_color, &s.change_bg_color,
                           &s.pl_active_bg_color})
        *f = themes::legacy_to_palette(*f, ColorRole::Background);
}

Settings load_settings() {
    fs::path cfg = config_path();
    std::error_code ec;

    Settings s;
    bool loaded_ok = false;
    if (fs::exists(cfg, ec)) {
        // Defensive: load_from_config() is a hand-written parser that
        // has to tolerate a human editing config.txt directly -- and a
        // hand edit can introduce anything from a stray character to a
        // whole different text encoding (Windows Notepad, in particular,
        // silently re-saves as UTF-16 unless told otherwise, which turns
        // every ASCII byte into byte+NUL and would previously make the
        // app disappear with no explanation, indistinguishable from any
        // other startup failure). Whatever load_from_config() throws (or
        // whatever we throw ourselves, below) is caught here and reported
        // instead of propagating out of App's constructor -- config.txt
        // is meant to be hand-edited, so a bad edit should fall back to
        // defaults with a visible reason, never take the whole app down.
        try {
            s = load_from_config(cfg);
            loaded_ok = true;
        } catch (const std::exception& e) {
            std::cerr << "mousiki: couldn't parse " << path_utf8(cfg) << " (" << e.what()
                      << ") -- using defaults for this session. Your file on disk is\n"
                         "unchanged; fix it and restart, or delete it to regenerate a\n"
                         "clean one.\n";
            s = Settings();
        } catch (...) {
            std::cerr << "mousiki: couldn't parse " << path_utf8(cfg)
                      << " -- using defaults for this session. Your file on disk is\n"
                         "unchanged; fix it and restart, or delete it to regenerate a\n"
                         "clean one.\n";
            s = Settings();
        }
    } else if (fs::exists(legacy_settings_path(), ec)) {
        // Migrate from legacy format
        s = load_from_legacy(legacy_settings_path());
    }
    // else: all defaults

    apply_default_hotkeys(s);
    if (!s.colors_palette) {
        // An older config.txt: convert its colours, and write the file once so that it carries the marker from now on (otherwise a
        // palette number typed by hand later would be converted again).
        const bool from_file = loaded_ok;
        migrate_legacy_colors(s);
        s.colors_palette = true;
        if (from_file) save_settings(s);
    }
    return s;
}

static void write_settings(std::ostream& out, const Settings& s) {
    out << "# Mousiki Configuration File\n";
    out << "# Location: $HOME/.config/mousiki/config.txt\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##              PANEL 1: COLORS\n";
    out << "##-------------------------------------------\n\n";
    out << "# Colour numbers are numbers of the 256-colour terminal palette (1-255) in every field; 0 or empty = no colour (the terminal's own).\n";
    out << "ColorCodes=palette\n\n";
    out << "# Frame & Controls\n";
    out << "ColorBorderTop=" << s.border_color << "\n";
    out << "ColorBorderBottom=" << s.border_color_bottom << "\n";
    out << "# BorderGradient: smooth = the frame fades line by line from the top to the bottom colour, rough = only the bottom border line has the bottom colour\n";
    out << "BorderGradient=" << (s.border_smooth ? "smooth" : "rough") << "\n";
    out << "ColorDiskTop=" << s.disk_color << "\n";
    out << "ColorDiskBottom=" << s.disk_color_end << "\n";
    out << "ColorMetadataKey=" << s.meta_key_color << "\n";
    out << "ColorMetadataVal=" << s.meta_val_color << "\n";
    out << "ColorVizLeft=" << s.visualizer_color << "\n";
    out << "ColorVizRight=" << s.visualizer_color_end << "\n";
    out << "ColorProgressBarPlayed=" << s.progress_played_color << "\n";
    out << "ColorProgressBarPending=" << s.progress_remaining_color << "\n";
    out << "\n# Section headers (the REFERENCE tab's category titles and the\n";
    out << "# LOCAL PATH / DOWNLOAD PATH / PLAYLIST PATH titles on the PATHS tab) -- 0 = no color, plain bold text\n";
    out << "ColorHeader=" << s.header_color << "\n";
    out << "\n# Key command legends (the grey hint lines such as \"[ESC] close\" in the Settings, the big\n";
    out << "# list / queue overlays, the playlist / meta editor and the history) -- 0 = terminal default\n";
    out << "ColorLegend=" << s.legend_color << "\n";
    out << "ColorTabCurrent=" << s.tab_current_color << "\nColorTabOther=" << s.tab_other_color << "\n";
    out << "\n# \"This field is actively being typed into\" (a PATHS row, a hex colour value, a\n";
    out << "# hotkey, a meta editor tag) -- 0 = terminal default\n";
    out << "ColorChangeFg=" << s.change_text_color << "\nColorChangeBg=" << s.change_bg_color << "\n";
    out << "\n# Pane focus indicator (the little triangle after the title of the pane that has the keys) -- 0 = plain bold\n";
    out << "ColorFocus=" << s.focus_color << "\n";
    out << "\n# Search bar: typed query (highlighted behind the query text only) and the idle bar\n# (empty FG = same as List Inactive; empty BG = none)\n";
    out << "ColorSearchFg=" << s.search_text_color << "\nColorSearchBg=" << s.search_bg_color << "\n";
    out << "ColorSearchInactiveFg=" << s.search_inactive_text_color << "\nColorSearchInactiveBg=" << s.search_inactive_bg_color << "\n";
    out << "\n# List\n";
    out << "ColorListInactiveFg=" << s.list_color << "\n";
    out << "ColorListInactiveBg=" << s.list_inactive_bg_color << "\n";
    out << "ColorListPlayingFg=" << s.list_playing_color << "\n";
    out << "ColorListPlayingBg=" << s.list_playing_bg_color << "\n";
    out << "ColorListCursorFg=" << s.list_cursor_color << "\n";
    out << "ColorListCursorBg=" << s.list_cursor_bg_color << "\n";
    out << "\n# Queue\n";
    out << "ColorQueueInactiveFg=" << s.queue_color << "\n";
    out << "ColorQueueInactiveBg=" << s.queue_inactive_bg_color << "\n";
    out << "ColorQueuePlayingFg=" << s.queue_playing_color << "\n";
    out << "ColorQueuePlayingBg=" << s.queue_playing_bg_color << "\n";
    out << "ColorQueueCursorFg=" << s.queue_cursor_color << "\n";
    out << "ColorQueueCursorBg=" << s.queue_cursor_bg_color << "\n";
    out << "\n# Lyrics\n";
    out << "ColorLyricsInactiveFg=" << s.inactive_line_color << "\n";
    out << "ColorLyricsInactiveBg=" << s.inactive_line_bg_color << "\n";
    out << "ColorLyricsActiveLineFg=" << s.active_line_color << "\n";
    out << "ColorLyricsActiveLineBg=" << s.active_line_bg_color << "\n";
    out << "ColorLyricsActiveWordFg=" << s.active_word_color << "\n";
    out << "ColorLyricsActiveWordBg=" << s.active_word_bg_color << "\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##              PANEL 2: ON/OFF\n";
    out << "##-------------------------------------------\n\n";
    auto tf = [](bool v) -> const char* { return v ? "true" : "false"; };
    out << "ElimentDisk=" << tf(s.element_disk) << "\n";
    out << "ElimentDummyButtons=" << tf(s.element_dummy_buttons) << "\n";
    out << "ElimentQueue=" << tf(s.element_queue) << "\n";
    out << "ElimentWaveForm=" << tf(s.element_waveform) << "\n";
    out << "VizLyricsField=" << tf(s.element_vizfield) << "\n";
    out << "## false = no Viz/Lyrics field right of the meta data at all (the meta data take its width; no lyrics are fetched for it)\n";
    out << "ElimentLyrics=" << tf(s.element_lyrics) << "\n";
    out << "## true = the field shows the lyrics (needs UseLyrics=true), false = it shows the visual of LyricViz. The \".\" key switches it.\n";
    out << "LyricViz=" << (s.lyric_viz == 1 ? "osci" : s.lyric_viz == 2 ? "spectro" : "sphere") << "\n";
    out << "## sphere = the audio-reactive ball | osci = the oscilloscope (both drawn in the VIZ colors) | spectro = the spectrogram\n";
    out << "## (LyricViz and ElimentLyrics are where the \".\" key left the field; the next start begins with VizDefault, see below; VizDefault=last keeps them.)\n";
    out << "VizDefault=" << viz_default_name(s.viz_default) << "\n";
    out << "## The view the field starts with when Mousiki opens: sphere | osci | spectro | lyrics | last (= the view it was left on, \"last used\" in Settings -> ANIMA/VIZ -> Lyrics/Viz Default)\n";
    out << "NoLyricsViz=" << (s.nolyrics_viz == 1 ? "osci" : s.nolyrics_viz == 2 ? "spectro" : s.nolyrics_viz == 3 ? "none" : "sphere") << "\n";
    out << "## What the field shows while the lyrics view is on but no lyrics are available (sphere | osci | spectro | none).\n";
    out << "## Picking one turns its \"Use ...\" switch below on; switching that off again moves this pick to the next visual that is on.\n";
    out << "UseLyrics=" << tf(s.use_lyrics) << "\n";
    out << "UseOscilloscope=" << tf(s.use_osci) << "\n";
    out << "UseSpectrogram=" << tf(s.use_spectro) << "\n";
    out << "UseSphere=" << tf(s.use_sphere) << "\n";
    out << "## false = left out of the lyrics area: not in the \".\" cycle. The windows\n";
    out << "## (SHIFT+8 spectrogram, SHIFT+9 oscilloscope) and the full screen (SHIFT+u) work either way.\n";
    {
        static const char* const viz_names[4] = {"sphere", "osci", "spectro", "lyrics"};
        out << "VizOrder=";
        for (int i = 0; i < 4; ++i) out << (i ? "," : "") << viz_names[std::clamp(s.viz_order[i], 0, 3)];
        out << "\n## the order of the views when you press \".\" (sphere, osci, spectro, lyrics in any order); set it on Settings -> ON/OFF -> Viz Order 1..4\n";
    }
    osci_config_write(out, s);
    spectro_config_write(out, s.spectro, "##");
    out << "## Tune all of these live with SHIFT+o in the main UI\n";
    out << "Visualizer=" << tf(s.element_visualizer) << "\n";
    out << "MetaDataOnly=" << tf(s.meta_only) << "\n";
    out << "## true  = the (search-)lists show embedded metadata only -- the title tag is used\n";
    out << "##         instead of the filename (files without a title tag keep their filename)\n";
    out << "## false = lists show filename AND metadata, as before\n";
    out << "## Toggleable live with HKeyToggleMetaOnly (SHIFT+n).\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             PANEL 3: ANIMA/VIZ\n";
    out << "##-------------------------------------------\n\n";
    out << "VisualizerFluidity=" << s.visualizer_fluidity << "\n## 1 to 10\n";
    out << "WaveformStyle=" << (s.waveform_smooth ? "smooth" : "raw") << "\n## raw , smooth\n";
    out << "DiskRotationSpeed=" << s.disk_rotation_speed << "\n## 0.01x to 1.00x\n";
    out << "StartMode=" << (s.start_mode == "radio" ? "radio" : "player") << "\n## player , radio -- the mode Mousiki starts in (the * key switches while it runs; set in Settings -> ON/OFF -> MODES)\n";
    out << "PlaybackMode=" << play_mode_name(s.play_mode) << "\n## list , loop , shuffle , stop , queue then stop\n";
    out << "VisualizerDegradationSpeed=" << s.visualizer_degradation_speed << "\n## 1 to 10\n";
    out << "VisualizerViscosity=" << s.visualizer_viscosity << "\n## 1 to 10\n";
    out << "LyricsAlignment=" << (s.lyrics_alignment == 1 ? "left" : s.lyrics_alignment == 2 ? "right" : "center") << "\n## center , left , right\n";
    {
        const char* anim_name = "full";
        if (s.lyrics_animation == 1) anim_name = "word by word";
        else if (s.lyrics_animation == 2) anim_name = "letter by letter";
        else if (s.lyrics_animation == 3) anim_name = "active line only";
        else if (s.lyrics_animation == 4) anim_name = "active word only";
        else if (s.lyrics_animation == 5) anim_name = "line by line";
        out << "LyricsAnimation=" << anim_name << "\n";
        out << "## full , word by word , line by line , letter by letter\n";
        out << "## active line only , active word only\n";
    }
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             EMOJI IN TITLES\n";
    out << "##-------------------------------------------\n\n";
    out << "ReplaceEmoji=" << (s.replace_emoji ? "true" : "false") << "\n";
    out << "## true  = an emoji in a title is drawn as a single \"?\" so the box borders always stay aligned\n";
    out << "## false = draw the real emoji (alignment then depends on how your terminal measures emoji)\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             MARQUEE SCROLLING\n";
    out << "##-------------------------------------------\n\n";
    out << "MarqueeScrolling=" << (s.marquee ? "true" : "false") << "\n";
    out << "## true  = a title that is too long for its column scrolls (the row under the cursor, Name / Artist / Album of the\n";
    out << "##         metadata block, the playlist editor lists)\n";
    out << "## false = nothing scrolls, a too long text is cut off with \"...\"\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             STEREO\n";
    out << "##-------------------------------------------\n\n";
    out << "StereoPlayback=" << (s.stereo ? "true" : "false") << "\n";
    out << "## true  = play tracks in stereo (uses about twice the memory per loaded track)\n";
    out << "## false = mono: left and right are folded together, as in earlier versions\n";
    out << "## Turning it off takes effect immediately; turning it on applies from the next track.\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             LOUDNESS NORMALIZATION\n";
    out << "##-------------------------------------------\n\n";
    out << "NormalizeVolume=" << (s.normalize ? "true" : "false") << "\n";
    out << "NormalizeTargetLufs=" << s.normalize_target_lufs << "\n";
    out << "NormalizeMaxBoostDb=" << s.normalize_max_boost_db << "\n";
    out << "## Each track is measured (LUFS) while it decodes and played at NormalizeTargetLufs.\n";
    out << "## Quiet tracks are raised (at most NormalizeMaxBoostDb), loud/compressed ones lowered.\n";
    out << "## -14 matches YouTube/Spotify; -16 leaves more headroom. Lower number = quieter overall.\n";
    out << "## Tune all three live with SHIFT+v in the main UI (the on/off switch is also on the ON/OFF tab and the `v` key)\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             EQUALIZER (Shift+E)\n";
    out << "##-------------------------------------------\n\n";
    out << "EqualizerEnabled=" << (s.eq_enabled ? "true" : "false") << "\n";
    out << "EqualizerBands=";
    for (int b = 0; b < kEqBands; ++b) out << (b ? "," : "") << s.eq_gains[b];
    out << "\n";
    out << "## Ten gains in dB (-12 to 12) for 31, 62, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz.\n";
    out << "## Easier to change with the overlay (Shift+E), which also has presets.\n";
    for (const auto& cp : s.eq_custom_presets) {
        out << "EqualizerPreset=";
        for (int b = 0; b < kEqBands; ++b) out << (b ? "," : "") << cp.gains[b];
        out << "|" << cp.name << "\n";
    }
    out << "## EqualizerPreset=<ten gains>|<name> is one custom preset (same band order as above), up to " << kEqMaxCustomPresets << ".\n";
    out << "## Create and delete them in the overlay: S saves the current curve under a name, DEL/X deletes the selected one.\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             CONSOLE / LOGGING\n";
    out << "##-------------------------------------------\n\n";
    out << "ConsoleVerbosity=" << (s.console_verbosity == 1 ? "verbose" : "basic") << "\n## basic , verbose\n";
    out << "## basic   = every command mousiki ran (yt-dlp/ffprobe/ffmpeg/lyrics-fetch) + its raw output\n";
    out << "## verbose = basic, plus internal/OS-level events (terminal resize, audio device init, spawn errors, ...)\n";
    out << "## Log file: $HOME/.cache/mousiki/logs/console.log -- wiped fresh at the start of every session.\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             AUTOSAVE / SESSION SNAPSHOT\n";
    out << "##-------------------------------------------\n\n";
    out << "SleepFade=" << tf(s.sleep_fade) << "\n## sleep timer: fade the volume out over the last 10 % of the time (30 s .. 10 min), then pause\n";
    out << "WizardSeen=" << tf(s.wizard_seen) << "\n## the help wizard (SHIFT+w) opened itself once already; false makes it greet you again on the next start\n";
    out << "SidPlayLength=" << s.sid_play_length << "\n## SID tunes (C64) are rendered by sidplayfp for this many seconds (10 .. 1800)\n";
    out << "KaraokeLyricsSize=" << s.karaoke_lyrics_size << "\n## karaoke overlay (k): lyrics size 1 (normal text) .. 5, SHIFT and + / SHIFT and - change it there\n";
    out << "TranslateService=" << s.translate_service << "\n## READ LYRICS (CTRL+L) translation service, picked with [t] there: google | mymemory\n";
    out << "TranslateTo=" << s.translate_to << "\n## language the lyrics are translated INTO (a code such as en, de, fr, es ...), picked with [t] in READ LYRICS\n";
    out << "TranslateFrom=" << s.translate_from << "\n## language OF the lyrics: auto = the service detects it, or a code such as de, ja ... (picked with [t])\n";
    out << "AutoSave=" << tf(s.autosave_enabled) << "\n## resume exact song/position/queue/repeat/shuffle next launch\n";
    out << "AutoSaveIndicator=" << tf(s.autosave_indicator) << "\n";
    out << "AutoSaveDelayInSec=" << s.autosave_delay_sec << "\n";
    out << "AutoSaveChr=" << s.autosave_chr << "\n## exactly one character is used, even if you paste more\n";
    out << "AutoSaveIndicatorType=" << (s.autosave_indicator_type == 1 ? "color" : "blink") << "\n## blink , color\n";
    out << "AutoSaveC1=" << s.autosave_c1 << "\n";
    out << "AutoSaveC2=" << s.autosave_c2 << "\n";
    out << "\n";

    out << "##-------------------------------------------\n";
    out << "##             PANEL 4: REFERENCE\n";
    out << "##-------------------------------------------\n\n";
    out << "font_en={\n";
    for (char c = 'A'; c <= 'Z'; ++c) {
        auto it = s.font_map.find(c);
        std::string upper(1, c), lower(1, static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (it != s.font_map.end()) { upper = it->second.first; lower = it->second.second; }
        out << "          " << c << "={" << upper << "," << lower << "},\n";
    }
    out << "        };\n\n";
    out << "UpperLeftCorner=\"" << s.box_upper_left << "\";\n";
    out << "UpperRightCorner=\"" << s.box_upper_right << "\";\n";
    out << "BottomLeftCorner=\"" << s.box_lower_left << "\";\n";
    out << "LowerRightCorner=\"" << s.box_lower_right << "\";\n";
    out << "Vertical=\"" << s.box_vertical << "\";\n";
    out << "Horizontal=\"" << s.box_horizontal << "\";\n";
    out << "Seprator=\"" << s.meta_separator << "\"\n";
    out << "ListSeparator=\"" << s.list_separator << "\"\n";
    out << "\n# General\n";
    {
        std::string setting_key = s.hotkeys.count("HKeySetting") ? s.hotkeys.at("HKeySetting") : "s";
        out << "HKeySetting=\"" << setting_key << "\"\n";
    }
    out << "\n# Local Music Paths\n";
    for (const auto& path : s.local_music_paths) {
        // Blank entries are skipped rather than written out as an empty
        // "LocalMusicPath=" line: an abandoned "+ new path" edit in the
        // ON/OFF tab can leave one behind in memory, and the loader skips
        // empty values anyway, so writing it would only litter the file.
        if (path.empty()) continue;
        out << "LocalMusicPath=" << path << "\n";
    }
    out << "\n# Playlists folder(s) (optional -- overrides the LocalMusicPath[0]/playlists default).\n";
    out << "# One line per folder: playlists are listed/loaded from all of them, while saving\n";
    out << "# and deleting uses the first one.\n";
    for (const auto& path : s.playlists_paths) {
        if (!path.empty()) out << "PlaylistsPath=" << path << "\n";
    }
    if (!s.playlist_export_path.empty()) {
        out << "\n# Where playlists are exported as M3U8 / M3U (one folder; unset means the playlist folder).\n";
        out << "PlaylistExportPath=" << s.playlist_export_path << "\n";
    }
    if (!s.conversion_export_path.empty()) {
        out << "\n# Where the file conversion overlay (SHIFT+c) writes converted files (one folder; unset means the\n";
        out << "# \"conversion-export\" folder inside the download folder).\n";
        out << "ConversionExportPath=" << s.conversion_export_path << "\n";
    }
    if (!s.history_path.empty()) {
        out << "\n# Folder of the listening history (history.json; one folder, unset means ~/.cache/mousiki/history).\n";
        out << "HistoryPath=" << s.history_path << "\n";
    }
    if (!s.download_folder.empty()) {
        out << "\n# Where yt-dlp downloads go (one folder; unset means ~/.cache/mousiki).\n";
        out << "# The folder below is also added to the local music paths automatically.\n";
        out << "DownloadFolder=" << s.download_folder << "\n";
    }

    out << "\n# Navigation\n";
    // Write every mapped hotkey, stable order, whatever the key is named.
    static const char* hkey_order[] = {
        "HKeyNavigateUp", "HKeyNavigateDown", "HKeyPlay", "HKeyPlayNextSong", "HKeyPlayPreviousSong",
        "HKeyTogglePlayPause", "HKeyCyclePlayMode", "HKeySearch",
        "HKeySeekForward", "HKeySeekBackward", "HKeyIncreaseVolume", "HKeyDecreaseVolume",
        "HKeyAddHoveringSongToQueue", "HKeyRemoveHoveringSongFromQueue", "HKeySwitchBetweenCards",
        "HKeyFilterForFolder", "HKeyClearFilter", "HKeyQuit", "HKeyDownloadStream",
        "HKeyToggleNormalize", "HKeyToggleMetaOnly", "HKeyMetaEditor", "HKeyHistory", "HKeyEqualizer",
    };
    for (const char* name : hkey_order) {
        auto it = s.hotkeys.find(name);
        if (it != s.hotkeys.end()) out << name << "=\"" << it->second << "\"\n";
    }
    for (const auto& [k, v] : s.hotkeys) {
        if (k == "HKeySetting") continue;
        bool found = false;
        for (const char* name : hkey_order) { if (k == name) { found = true; break; } }
        if (!found) out << k << "=\"" << v << "\"\n";
    }
    out << "\n";
}

std::string settings_to_text(const Settings& s) {
    std::ostringstream out;
    write_settings(out, s);
    return out.str();
}

void save_settings(const Settings& s) {
    fs::path p = config_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    std::ofstream out(p, std::ios::trunc);
    if (!out.is_open()) return;
    write_settings(out, s);
}

} // namespace muisc
