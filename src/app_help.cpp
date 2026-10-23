// Help screens: the wizard (SHIFT+w) and the cheat sheet (?). Split out of app.cpp.
// Member functions of App (declared in app.h); the helpers shared with app.cpp are in app_internal.h.
#include "app.h"
#include "text_edit.h"
#include "help_search.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "keyboard_layout.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "console_log.h"
#include "karaoke_font.h"
#include "glyph_raster.h"
#include "chiptune.h"
#include "scope_big.h"
#include "scope_window.h"
#include "wizard.h"
#include "creator.h"
#include "mic_capture.h"
#include "process_util.h"
#include "app_internal.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <sys/types.h>
#include <sys/stat.h>   // file_added_time(): stat() / _wstat64()
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include "app_internal.h"

namespace muisc {

bool App::wizard_allowed_over_mode() const {
    switch (mode_) {
        case Mode::Settings:    return theme_ui_ == 0 && leave_prompt_ == 0;   // not in the theme editor or a question
        case Mode::Cheatsheet:  return !cheat_searching_;                      // the search field takes typed text
        case Mode::Console: case Mode::History: case Mode::ReadLyrics:
        case Mode::OsciMenu: case Mode::NormMenu: case Mode::SpectroMenu: case Mode::SleepTimer: case Mode::MenuIndex:
        case Mode::LyricsEdit: case Mode::Karaoke:
            return true;
        case Mode::Equalizer:   return !eq_naming_;                            // the "Save as:" prompt takes typed text
        case Mode::Creator:     return !creator_on_text_field() || creator_list_open_;  // the text field takes the keyboard
        case Mode::Playlist: case Mode::MetaEdit: return !text_entry_active(); // a text field has the keyboard
        default:                return false;                                  // text fields (search, playlist and meta editors ...), prompts
    }
}

void App::wizard_open() {
    if (!wizard::kEnabled) return;   // switched off (wizard.h)
    if (mode_ != Mode::Wizard) wizard_return_mode_ = mode_;
    wizard::open(wizard_st_, !settings_.wizard_seen);
    mode_ = Mode::Wizard;
}

void App::wizard_close() {
    mode_ = wizard_return_mode_;
    wizard_return_mode_ = Mode::Browse;
    force_redraw_ = true;
    // The first close is what turns the auto-open off for good. Written
    // straight away rather than at exit, so a crash cannot bring the
    // welcome back on the next start.
    if (!settings_.wizard_seen) {
        settings_.wizard_seen = true;
        save_settings(settings_);
    }
}

void App::build_wizard_screen(std::ostringstream& frame, int W) {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string base = ansi_for(settings_.list_color, false);
    // Same height as every other full-screen view (see build_cheatsheet_screen).
    const int body_rows = std::max(1, term_rows_ - 3);

    // The disk of the first page turns at a fixed 0.10 here, whatever Settings -> ANIMA/VIZ -> Disk Speed says and
    // whether or not anything is playing, so the greeting always has some life.
    std::vector<std::string> disk;
    if (wizard::shows_disk(wizard_st_.page)) disk = disk_.frame(wizard_angle_);
    const wizard::View v = wizard::render(wizard_st_, W, body_rows, disk);

    const std::string lead(static_cast<size_t>(v.left), ' ');
    const std::string trail(static_cast<size_t>(std::max(0, W - v.left - v.box_w)), ' ');
    // The title is drawn in the header colour; the border colour comes back right in front of the dashes.
    frame << lead << box_top(header_sgr(settings_) + v.title + R + "\001", v.box_w, border) << trail << "\n";

    for (const wizard::Row& row : v.rows) {
        frame << lead << bar;
        for (const wizard::Span& s : row) {
            switch (s.role) {
                case wizard::Role::Header: frame << header_sgr(settings_) << s.text << R; break;
                case wizard::Role::Legend: frame << legend_sgr(settings_) << s.text << R; break;
                case wizard::Role::Match: frame << header_sgr(settings_) << "\x1b[4m" << s.text << R; break;
                case wizard::Role::Cursor:
                    frame << ansi_for(settings_.list_cursor_color, false) << bg_ansi_for(settings_.list_cursor_bg_color) << s.text << R;
                    break;
                case wizard::Role::Disk:
                case wizard::Role::Scroll:
                    frame << gradient_ansi(settings_.disk_color, settings_.disk_color_end, s.t) << s.text << R;
                    break;
                default: frame << base << s.text << R; break;
            }
        }
        frame << bar << trail << "\n";
    }

    std::string bottom = "[" + hotkey_text("HKeyWizard", "SHIFT+w") + " / ESC] close";
    if (v.search) {
        bottom = "[ESC] clear / back  [ENTER] open the chapter  [\xE2\x86\x91\xE2\x86\x93] choose  [SHIFT+\xE2\x86\x91\xE2\x86\x93] jump 5";
        frame << lead << box_bottom(v.box_w, bottom, border_bottom) << trail << "\n";
        return;
    }
    bottom += "  [t] TOC  [SHIFT+s] search";
    if (v.can_back) bottom += "  [<-] back";
    if (v.can_next) bottom += "  [->] next";
    if (v.toc_page) bottom += "  [\xE2\x86\x91\xE2\x86\x93] select  [SHIFT+\xE2\x86\x91\xE2\x86\x93] jump  [ENTER] open";
    else if (v.scrollable) bottom += "  [\xE2\x86\x91\xE2\x86\x93] scroll  [SHIFT+\xE2\x86\x91\xE2\x86\x93] page";
    frame << lead << box_bottom(v.box_w, bottom, border_bottom) << trail << "\n";
}

void App::build_cheatsheet_screen(std::ostringstream& frame, int W) const {
    std::string border = ansi_for(settings_.border_color, false);
    frame << box_top("CHEATSHEET", W, border) << "\n";

    // Similar shape to the Reference tab of Settings (kRefRows) for the
    // rebindable hotkeys, but this screen is the ONE place that also lists
    // every literal, non-rebindable key command -- the Reference tab no
    // longer carries its own copy of those (see its own comment for why),
    // so this table's rows are this app's entire command list, full stop.
    // `header` is set only on a category's first row and is drawn as a
    // section title in the Header colour above a blank spacer row. `action`
    // is either
    //   * a plain action name, looked up in settings_.hotkeys, so the key
    //     shown is whatever the user actually has bound (config.txt /
    //     rebound in Settings), never a hardcoded assumption; or
    //   * '#'-prefixed: a literal key label. SHIFT+b, Ctrl+Shift+S/X, ESC,
    //     the playlist/meta editors' own fixed navigation and text-editing
    //     keys, and friends are checked as raw key codes in handle_*_key()
    //     rather than looked up in settings_.hotkeys, so there is no hotkey
    //     entry for them to reference.
    struct CheatRow { const char* header; const char* action; const char* desc; };
    static const CheatRow rows[] = {
        // --- System -- listed first. Besides the rebindable system hotkeys
        // this also carries the literal keys used across multiple overlays
        // (not tied to one editor's own legend below). ---
        {"SYSTEM (MAIN UI)", "#@SWITCHKEY", "Switch to the RADIO mode (types the * character; playback is paused and the player waits in the background)"},
        {nullptr, "HKeySetting", "Open Settings panel, hit again to save and quit"},
        {nullptr, "HKeyConsole", "Console / logs"},
        {nullptr, "HKeyWizard", "Help wizard: the user guide as tutorial pages (arrow keys turn them)"},
        {nullptr, "HKeyCheatsheet", "This cheatsheet"},
        {nullptr, "HKeyMenus", "The MENUS index: every menu and overlay by category with its key (UP/DOWN/LEFT/RIGHT select, ENTER opens, ESC closes)"},
        {nullptr, "HKeyQuit", "Quit"},
        {nullptr, "#ESC", "Close setting / overlay / menu"},
        {nullptr, "#ENTER", "Confirm / select"},
        {nullptr, "#ARROW KEYS", "Navigate (context-dependent)"},
        {nullptr, "#Y / N", "Confirm or cancel a prompt"},
        // --- Playback ---
        {"PLAYBACK (MAIN UI)", "HKeyPlay", "Play the selected track"},
        {nullptr, "HKeyTogglePlayPause", "Play / pause"},
        {nullptr, "HKeyPlayNextSong", "Play next in list/queue"},
        {nullptr, "HKeyPlayPreviousSong", "Play previous in list"},
        {nullptr, "HKeyShuffleNext", "Shuffle to a random next track"},
        {nullptr, "HKeyCyclePlayMode", "Cycle play mode (list/repeat/shuffle/stop/queue then stop)"},
        {nullptr, "HKeySeekForward", "Seek forward 5s"},
        {nullptr, "HKeySeekBackward", "Seek backward 5s"},
        {nullptr, "HKeyIncreaseVolume", "Volume up"},
        {nullptr, "HKeyDecreaseVolume", "Volume down"},
        {nullptr, "HKeyToggleMute", "Mute (without pausing)"},
        {nullptr, "HKeyToggleNormalize", "Toggle loudness normalization"},
        {nullptr, "HKeyNormMenu", "Normalization overlay: on/off, target level, max boost live (toggle)"},
        {nullptr, "#UP/DOWN  LEFT/RIGHT", "Normalization overlay: pick a value / change it   [SPACE] on/off   [R] reset   [ESC] close"},
        {nullptr, "HKeySleepTimer", "Sleep timer overlay: pause after 15/30/60/90/120 min (optional fade-out over the last 10 %) or stop after this song (toggle)"},
        {nullptr, "#UP/DOWN  ENTER", "Sleep timer overlay: pick an entry / set it ('Fade out' toggles the fade)   [ESC] close (the Stop play mode is left alone)"},
        {nullptr, "HKeyCreator", "Creator overlay: turn a text or a picture into oscilloscope music (XY scope) or a spectrogram sound, saved as a WAV in <music folder>/creator and played (toggle)"},
        {nullptr, "#UP/DOWN  LEFT/RIGHT", "Creator overlay: pick a row / change it   [ENTER] on 'Create' renders, saves and plays   [ESC] close"},
        // --- Navigation & View: moving around the lists and what they show ---
        {"NAVIGATION & VIEW (MAIN UI)", "HKeyNavigateUp", "Explore list (up)"},
        {nullptr, "HKeyNavigateDown", "Explore list (down)"},
        {nullptr, "HKeySwitchBetweenCards", "Switch between panels (list / queue)"},
        {nullptr, "HKeyFilterForFolder", "Filter by folder"},
        {nullptr, "HKeyClearFilter", "Clear filter"},
        {nullptr, "HKeyCycleSortMode", "Cycle local list sort mode (folder order / title A-Z / artist A-Z / recently added, newest first)"},
        {nullptr, "HKeyToggleMetaOnly", "Toggle metadata-only track list (no filename)"},
        {nullptr, "HKeyToggleWaveform", "Toggle waveform style (raw/smooth)"},
        {nullptr, "HKeyFolderOverlay", "Big folder overlay: tree of the library's folders (ENTER opens, f shows the folder in the list, / searches)"},
        {nullptr, "HKeyListOverlay", "Big list overlay: larger LOCAL AUDIO FILES pane (toggle)"},
        {nullptr, "HKeyQueueOverlay", "Big queue overlay: larger QUEUE pane (toggle; replaces the list overlay)"},
        {nullptr, "#SHIFT+UP/DOWN", "Big list / queue / folder / conversion overlay: previous / next page (faster scrolling)"},
        {nullptr, "#ESC", "Big list / queue / folder / conversion overlay: close (playback, queue and list keys keep working)"},
        {nullptr, "#ENTER, LEFT/RIGHT", "Big folder overlay: open / close the hovered folder (LEFT on a closed one jumps to its parent)"},
        {nullptr, "#CTRL+p", "Big folder / conversion overlay: play the hovered file (the overlay stays open)"},
        {nullptr, "#/", "Big folder overlay: search the folder tree (ENTER or ESC leaves the search field)"},
        {nullptr, "HKeyRefreshUi", "Refresh UI (redraw)"},
        // --- File conversion: its own category ---
        {"FILE CONVERSION (MAIN UI)", "HKeyConvertOverlay", "File conversion overlay: turn high resolution files into MP3 / AAC / Opus (marked files go to the CONVERSION EXPORT PATH, the originals stay untouched)"},
        {nullptr, "#SPACE, a", "Conversion overlay: mark / unmark the hovered file / mark or unmark everything shown"},
        {nullptr, "#ENTER, LEFT/RIGHT", "Conversion overlay: ENTER converts the marked files (or the hovered one); LEFT/RIGHT choose the target (MP3 320 / V0 / 192, AAC 256, Opus 128)"},
        {nullptr, "#t, r, h", "Conversion overlay pre-filter: file type (SHIFT+t backwards) / minimum bitrate (SHIFT+r backwards) / hi-res only (above 16 bit or 48 kHz)"},
        {nullptr, "#/, f:, p:", "Conversion overlay search bar: / full file search; type f: (folders) or p: (saved playlists) and ENTER on one to show only its files; typing / again returns to the full search, ESC steps back"},
        {nullptr, "#c", "Conversion overlay: cancel the running conversion (it keeps running when the overlay is closed)"},
        // --- Visualizations and lyrics ---
        {"VISUALIZATIONS AND LYRICS (MAIN UI)", "HKeyToggleLyrics", "Cycle the lyrics area forward: sphere / oscilloscope / spectrogram / lyrics (what is off on Settings > ON/OFF is skipped)"},
        {nullptr, "HKeyToggleLyricsBack", "Cycle the lyrics area backward (same order, reversed)"},
        // --- Visualizations: oscilloscope and spectrogram (in the lyrics area, full screen, in their own windows) ---
        {nullptr, "HKeyOscMenu", "Oscilloscope overlay: style, frame rate, music mode, decay, dot threshold, tail, glow, Z axis ... live -- braille and image keep their own values (toggle)"},
        {nullptr, "#UP/DOWN  LEFT/RIGHT", "Oscilloscope overlay: pick a value / change it   [R] reset the style in use   [ESC] close"},
        {nullptr, "#OSCI MUSIC MODE", "Oscilloscope overlay row: unprocessed signal, fixed scale, no phase portrait -- for oscilloscope music (lossless files)"},
        {nullptr, "HKeySpectroMenu", "Spectrogram options overlay: style, motion, scale, frequencies, gain, range, window, colors, time span ... (toggle)"},
        {nullptr, "#UP/DOWN  LEFT/RIGHT", "Spectrogram overlay: pick a value / change it   [R] Audacity defaults   [ESC] close"},
        {nullptr, "HKeySpectroFull", "Spectrogram full screen with frequency labels and time ruler (toggle; ESC closes)"},
        {nullptr, "HKeyScopeBig", "Oscilloscope full screen: YT mode left, XY mode right, braille or image style (toggle; ESC closes; the options overlay opens on top of it)"},
        {nullptr, "#SHIFT+a  SHIFT+v  SHIFT+c  SHIFT+p", "Oscilloscope full screen: axes / grid on or off   axis values on or off   grid colour (small window: type 0-255, empty = colour of inactive list elements)   grid metrics (small window: time unit, grid divisions, steady readouts in the picture names: RMS level, peak hold, L/R correlation, L/R balance)"},
        {nullptr, "HKeyScopeWindow", "Scope window: the oscilloscope in its own window, drawn by the graphics card, braille or image style as set in the SHIFT+o overlay (toggle; needs SDL2)"},
        {nullptr, "HKeySpectroWindow", "Spectrogram window: the spectrogram in its own window, scrolling smoothly at the monitor's refresh rate (toggle; needs SDL2)"},
        {nullptr, "#F / F11 / T / ESC", "Scope / spectrogram window (keys inside it): fullscreen (also double-click) / always on top / close"},
        // --- Lyrics & karaoke ---
        {"LYRICS & KARAOKE (MAIN UI)", "HKeyRetryLyrics", "LYRICS REFETCH AND VERSION CONTROL: pick the lyrics in use (embedded / fetched / older) with SPACE, delete a saved copy with d (asks first), or ENTER to refetch them (with your own title / artist if you like); the old version is kept"},
        {nullptr, "HKeyLyricsTiming", "Lyrics timing overlay: shift the lyrics earlier / later (toggle; only while synced lyrics are loaded)"},
        {nullptr, "#LEFT/RIGHT  UP/DOWN", "Lyrics timing overlay: -/+ 0.1 s / -/+ 0.5 s   [R] reset   [ENTER] save to the .lrc   [ESC] cancel"},
        {nullptr, "HKeyReadLyrics", "READ LYRICS: the saved lyrics of the playing (else hovered) track as plain text; two top lines list the lyrics files (embedded / fetched / older) and the saved translations, which are shown in ( ) below each line"},
        {nullptr, "#CTRL+s", "READ LYRICS: search EVERY lyrics file by name (all lyrics folders of the library) instead of the playing track's lyrics: type to filter, UP/DOWN pick, ENTER shows the file, ESC closes the search (CTRL+L opens the playing track's lyrics again)"},
        {nullptr, "#LEFT/RIGHT  TAB  UP/DOWN  t  SHIFT+t  d", "READ LYRICS: choose the file / translation, TAB switches the pane, scroll (SPACE, b, HOME, END too)   [t] translate overlay (ENTER fetches)   [SHIFT+t] show / next / hide translations   [d] delete a translation   [ESC] close"},
        {nullptr, "HKeyKaraoke", "Karaoke overlay: the lyrics over the whole screen, sung words highlighted (toggle; fetches the lyrics even with the lyrics engine off; playback keys keep working)"},
        {nullptr, "#@KARAOKESIZE", "Karaoke overlay: lyrics bigger / smaller, sizes 1-5 (the characters * and _; saved when the overlay closes)"},
        // --- Search ---
        {"SEARCH LOCAL (MAIN UI)", "HKeySearch", "Search the local library (title, artist and album; type to filter live, ENTER keeps the result)"},
        {nullptr, "#/r:", "Search ARTISTS only: type r: first (e.g. r:beatles) -- only the artist tag / folder name is matched"},
        {nullptr, "#/a:", "Search ALBUMS only: type a: first (e.g. a:abbey road) -- only the album tag is matched (tags are read in the background, so it gets more complete after a moment)"},
        {nullptr, "#/p:", "Search saved playlists (type /p:query)"},
        {nullptr, "#/f:", "Search folders (type /f:query), ENTER lists all files of that folder"},
        {"SEARCH ONLINE (MAIN UI)", "#/s:", "Search online (YouTube; type s:query)"},
        {nullptr, "#/sc:", "Search SoundCloud (type it in the search box; ENTER plays, the download key saves it)"},
        {nullptr, "#/b:", "Search Bandcamp (tracks; ENTER plays the free stream, the download key saves it)"},
        // --- Queue ---
        {"QUEUE (MAIN UI)", "HKeyAddHoveringSongToQueue", "Add hovering track of the library pane as NEXT (repeated presses keep their order; does nothing while the queue pane is focused)"},
        {nullptr, "HKeyQueueAddEnd", "Add hovering track to the END of the queue"},
        {nullptr, "HKeyRemoveHoveringSongFromQueue", "Remove hovering track from queue"},
        {nullptr, "HKeyQueueMoveUp", "Move hovering queue item up"},
        {nullptr, "HKeyQueueMoveDown", "Move hovering queue item down"},
        {nullptr, "HKeyQueueMoveTop", "Move hovering queue item to the top"},
        {nullptr, "HKeyQueueMoveBottom", "Move hovering queue item to the bottom"},
        {nullptr, "HKeyQueueLock", "Lock / unlock the queue (locked = default: played track goes to the end; unlocked: it leaves)"},
        {nullptr, "HKeyClearQueue", "Clear the whole queue (asks Yes / No first)"},
        {nullptr, "HKeyQueueUndoClear", "Undo the last queue clear"},
        {nullptr, "HKeyQueueToPlaylist", "Save the queue as a playlist (opens the playlist editor)"},
        {nullptr, "HKeyBulkAdd", "BULK ADD: paste a YouTube playlist link and queue its tracks (from any pane). Only queues them: nothing is downloaded when adding"},
        // --- Playlists -- HKeyPlaylist opens the overlay; every other row
        // is the playlist editor's own fixed legend (build_playlist_screen()'s
        // footer), none of which is a rebindable hotkey.
        {"PLAYLISTS", "HKeyPlaylist", "Open Playlists (create / manage)"},
        {nullptr, "HKeyAddToPlaylist", "Main player UI: add the hovering track of the library / search result pane (not the library picker inside the playlist menu) to a new or an existing playlist; saved at once, the playlist menu stays closed; a YouTube / SoundCloud / Bandcamp track is downloaded into the download folder first"},
        {nullptr, "#LEFT/RIGHT", "Switch tab (Create/Edit vs Saved Playlists); inside a name / search field after typing: move the caret"},
        {nullptr, "#ESC", "Leave the field you typed in (LEFT/RIGHT switch tabs again); outside a field: close (asks to save unsaved changes)"},
        {nullptr, "#" MUISC_ALT_NAME_UC "+LEFT/RIGHT", "Switch tab from anywhere, also while typing"},
        {nullptr, "#TAB", "Cycle focus (name field / library picker / track list)"},
        {nullptr, "#UP/DOWN", "Navigate the focused list/picker (fixed arrow keys)"},
        {nullptr, "#ENTER", "Add hovering track to playlist / load selected playlist"},
        {nullptr, "#e", "Saved Playlists tab: export the hovered playlist as M3U8 (default) or M3U -- overlay with folder and format"},
        {nullptr, "#i", "Saved Playlists tab: import .m3u / .m3u8 files from a folder (default: export/playlist folder) -- SPACE marks, a marks all"},
        {nullptr, "#4 / 5", "Move the hovering track up/down"},
        {nullptr, "#D / DEL / BACKSPACE", "Remove hovering track / delete selected playlist"},
        {nullptr, "#CTRL+s", "Save the playlist"},
        {nullptr, "#SHIFT+LEFT/RIGHT", "Mark text (name / search fields)"},
        {nullptr, "#CTRL+C/X/V", "Copy / cut / paste text"},
        // --- Meta editor -- same deal: HKeyMetaEditor opens it, everything
        // else is build_meta_screen()'s own fixed legend (both tabs).
        {"META EDITOR", "HKeyMetaEditor", "Meta editor: edit file name / artist / title / album / year"},
        {nullptr, "#LEFT/RIGHT", "Switch tab (Edit vs Fetch List); in the search field after typing and in the field editor: move the caret"},
        {nullptr, "#ESC", "Leave the search field / field editor; outside a field: close (pending edits are kept)"},
        {nullptr, "#" MUISC_ALT_NAME_UC "+LEFT/RIGHT", "Switch tab from anywhere, also while typing"},
        {nullptr, "#TAB", "Meta editor: cycle panels (search / library / fields)"},
        {nullptr, "#UP/DOWN", "Navigate the focused list/picker (fixed arrow keys)"},
        {nullptr, "#ENTER", "Edit the hovering field (Fetch List tab: fetch the full list; fetched field with several options: choose)"},
        {nullptr, "#SHIFT+LEFT/RIGHT", "Mark text (search field / field editor)"},
        {nullptr, "#CTRL+C/X/V", "Copy / cut / paste text (search field / field editor)"},
        {nullptr, "#a", "Meta editor: add the hovering file to the fetch list"},
        {nullptr, "#SHIFT+f", "Meta editor (library pane): add the full list -- every title the pane shows now (search and missing-tag filter applied) -- to the fetch list"},
                {nullptr, "#TAB  LEFT/RIGHT  SPACE (Fetch List)", "Meta editor, Fetch List tab: TAB moves between the list and the fetch option line; LEFT/RIGHT move over the boxes, SPACE ticks one (missing only / all exclude the rest; the tag boxes combine); ENTER fetches the full list"},
        {nullptr, "#UP/DOWN  ENTER  DEL/d  ESC", "Meta editor choice overlay: select / take it / discard the fetched value and all options / close"},
        {nullptr, "#r", "Meta editor: toggle edited files on top of the library pane"},
        {nullptr, "#n", "Meta editor (library pane): toggle recently added files first"},
        {nullptr, "#x, SHIFT+/t/a/l/y", "Filter + sort the library pane by missing metadata: x = missing any, SHIFT+t = title, SHIFT+a = artist, SHIFT+l = album, SHIFT+y = year (same order as the fields)"},
        {nullptr, "#SHIFT+p", "Meta editor (library pane / fetch list): start the hovering track"},
        {nullptr, "HKeyFetchTags", "Meta editor / main list: fetch the missing tags (empty ones only) of the hovered title (AcoustID)"},
        {nullptr, "#SHIFT+n", "Meta editor (library pane / fetch list): fetch ALL tags of the hovered title, overwriting existing ones"},
        {nullptr, "#SHIFT+c", "Meta editor, Fetch List tab: cancel the running AcoustID fetch (what had arrived stays as pending edits, the rest stays on the list). Leaving the editor does not stop a fetch; quitting does"},
        {nullptr, "HKeyRescan", "Rescan the library (new files); also in the meta data editor (library pane / fetch list focused)"},
        {nullptr, "#DEL / d", "Remove hovering track from the fetch list"},
        {nullptr, "#SHIFT+d / SHIFT+DEL", "Empty the whole fetch list (asks first); pending edits, fetched tags included, are kept"},
        {nullptr, "#CTRL+s", "Apply the meta editor's pending edits to the files (asks first)"},
        {nullptr, "#CTRL+SHIFT+x", "Discard the meta editor's pending edits (fetched tags included) and empty the fetch list"},
        // --- Settings ---
        {"SETTINGS", "#TAB", "Next tab (not while a field is being edited)"},
        {nullptr, "#UP/DOWN  LEFT/RIGHT", "Previous / next row  |  cycle a switch or choice / pick the first or second colour cell"},
        {nullptr, "#ENTER", "Edit the selected colour, path, value or key in place"},
        {nullptr, "#LEFT/RIGHT (editing)", "Move the caret; SHIFT+LEFT/RIGHT marks, HOME/END jump, CTRL+C/X/V copy / cut / paste"},
        {nullptr, "#ENTER / ESC (editing)", "Apply the edit / cancel it -- only these two leave the field"},
        {nullptr, "#s / ESC / q", "Save to config.txt and close / close without saving (with unsaved changes it asks first: [s] save, [d] discard, ESC keeps editing)"},
        {nullptr, "#ENTER (THEME row)", "Colors tab: ENTER opens CHOOSE THEME (scrollable list, ENTER applies, SHIFT+d deletes a theme of your own); the CREATE THEME button builds your own theme (s saves it)"},
        {nullptr, "#e", "Colors tab: edit the theme in use in the theme creator (a theme of your own can be overwritten, a built-in one needs a new name)"},
        {nullptr, "#SHIFT+e / SHIFT+i", "Colors tab: export the current colors as a theme file / import theme files from a folder"},
        // --- Settings (REFERENCE tab) ---
        {"SETTINGS (REFERENCE TAB)", "#ENTER", "Change the key of the selected command (type the new key, ENTER applies)"},
        {nullptr, "#DEL", "Restore the default key of the selected command"},
        {nullptr, "#ENTER on the first line", "Reset all keys to their defaults"},
        {nullptr, "#ALT+L, CTRL+L ...", "While a key is edited: a combination that cannot be typed (ALT+L, CTRL+L, CTRL+SHIFT+U / Z) is entered by pressing it; ENTER with an empty field unbinds the command"},
        {nullptr, "#CTRL+SHIFT+u", "Undo the last key change or reset (up to 5, newest first)"},
        // --- Listening history ---
        {"HISTORY", "HKeyHistory", "Listening history: last plays, top tracks, top playlists, habits"},
        {nullptr, "#LEFT/RIGHT  1 / 2 / 3 / 4", "History overlay: switch tab (TAB never switches tabs)"},
        {nullptr, "#UP/DOWN  ENTER", "History overlay: move the cursor / scroll Top Playlists and Habits; ENTER plays the hovered track"},
        {nullptr, "#r", "History overlay: most-played first <-> least-played first (Top Tracks)"},
        {nullptr, "#TAB", "Top Tracks tab: track list <-> ADD SMART HISTORY TO QUEUE pane"},
        {nullptr, "#ARROWS  ENTER", "Smart history pane: pick a list (Top 10-100, top of the week / month / quarter / year, morning / day / evening / night, newly added, least played) / add it to the queue"},
        {nullptr, "#ESC", "Smart history pane: back to the track list (ESC again closes the history)"},
        // --- Equalizer ---
        {"EQUALIZER", "HKeyEqualizer", "Equalizer: 10 bands, presets, custom presets, on/off"},
        {nullptr, "#LEFT / RIGHT", "Equalizer: select band"},
        {nullptr, "#UP / DOWN", "Equalizer: band gain +1 / -1 dB"},
        {nullptr, "#, / . / TAB", "Equalizer: previous / next preset (built-in, then custom)"},
        {nullptr, "#SPACE / 0 / r", "Equalizer: on-off / zero the band / reset to Flat"},
        {nullptr, "#s", "Equalizer: save the current curve as a custom preset (ENTER saves, ESC cancels)"},
        {nullptr, "#DEL / x", "Equalizer: delete the selected custom preset (press twice)"},
        // --- Downloads ---
        {"DOWNLOADS", "HKeyDownloadStream", "Save stream to the download folder (Settings > Download Path, else .cache/mousiki)"},
    };

    // Same height as every other full-screen view: term_rows_ - 1 lines (the
    // terminal's last row stays untouched so a trailing newline can never
    // scroll), of which 2 are this overlay's own top/bottom border rows. It
    // used to stop 3 lines short of that, leaving blank rows below the box
    // on every terminal size (120x30 included) -- there is no status/prompt
    // row here that would need them.
    int visible = std::max(1, term_rows_ - 3);

    // Every entry becomes display lines first (a category costs a blank spacer + its title, a long description wraps),
    // so the scroll range counts what is really drawn. The description column starts after the widest key + 2 spaces.
    const int inner = std::max(0, W - 4);
    auto key_of = [&](const CheatRow& r) {
        if (r.action[0] == '#') return pretty_key(std::string(r.action + 1));
        auto it = settings_.hotkeys.find(r.action);
        return pretty_key((it != settings_.hotkeys.end() && !it->second.empty()) ? it->second : std::string("-"));
    };
    int key_w = 0;
    for (const auto& r : rows) key_w = std::max(key_w, display_width(key_of(r)));
    key_w = std::min(key_w, std::max(10, inner / 2));
    const int desc_col = key_w + 2;
    const int desc_w = std::max(10, inner - desc_col);
    struct DLine { std::string plain, sgr; };
    std::vector<DLine> dlines;
    std::vector<int> row_dline;   // the first display line of each table row
    std::string category;
    std::vector<std::string> row_category;
    for (const auto& r : rows) {
        row_dline.push_back(static_cast<int>(dlines.size()) + (r.header ? 1 : 0));
        if (r.header) category = r.header;
        row_category.push_back(category);
        if (r.header) {
            dlines.push_back({std::string(), ""});                       // blank spacer row...
            dlines.push_back({r.header, header_sgr(settings_)});         // ...then the title, Header colour + bold
        }
        const std::string key = key_of(r);
        const auto parts = wrap_words(r.desc, desc_w);
        for (size_t i = 0; i < parts.size(); ++i)
            dlines.push_back({(i == 0 ? pad_right(truncate_str(key, key_w), desc_col) : std::string(static_cast<size_t>(desc_col), ' ')) + parts[i], ""});
    }
    const int total = static_cast<int>(dlines.size());
    const int max_scroll = std::max(0, total - visible);
    if (cheat_jump_row_ >= 0 && cheat_jump_row_ < static_cast<int>(row_dline.size())) {
        cheatsheet_scroll_ = std::max(0, row_dline[static_cast<size_t>(cheat_jump_row_)] - 2);   // the category title above stays in view
    }
    cheat_jump_row_ = -1;
    cheatsheet_scroll_ = std::clamp(cheatsheet_scroll_, 0, max_scroll);

    if (cheat_searching_) {
        // ----- SHIFT+s: the search view replaces the table -----
        const std::string R = "\x1b[0m";
        const std::string bar = border + settings_.box_vertical + R;
        const std::string mark_sgr = header_sgr(settings_) + "\x1b[4m";
        const std::string cursor_sgr_ = ansi_for(settings_.list_cursor_color, false) + bg_ansi_for(settings_.list_cursor_bg_color);
        const std::vector<std::string> words = helpsearch::split_words(cheat_query_);
        struct Res { int row; helpsearch::Hit hit; std::string doc; };
        std::vector<Res> res;
        if (!words.empty()) {
            for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
                // the key is padded to the width of the table's key column, so the descriptions line up as in the table
                std::string doc = pad_right(truncate_str(key_of(rows[i]), key_w), desc_col) + rows[i].desc + "  [" + row_category[i] + "]";
                helpsearch::Hit h = helpsearch::match(words, doc);
                if (helpsearch::worth_showing(h)) res.push_back({static_cast<int>(i), std::move(h), std::move(doc)});
            }
            std::stable_sort(res.begin(), res.end(), [](const Res& a, const Res& b) { return helpsearch::better(a.hit, b.hit); });
            if (res.size() > 300) res.resize(300);
        }
        cheat_result_rows_.clear();
        for (const Res& r : res) cheat_result_rows_.push_back(r.row);
        cheat_sel_ = std::clamp(cheat_sel_, 0, std::max(0, static_cast<int>(res.size()) - 1));

        auto row_out = [&](const std::string& painted, int cols) {
            frame << bar << " " << painted << std::string(static_cast<size_t>(std::max(0, inner - cols)), ' ') << " " << bar << "\n";
        };
        int used = 0;
        {   // the query field
            const std::string label = "Search: ";
            const bool mine = edit_owner_ == "cheat-search";
            const size_t cc = mine ? std::min(edit_caret_, cheat_query_.size()) : cheat_query_.size();
            const size_t aa = mine ? std::min(edit_anchor_, cheat_query_.size()) : cheat_query_.size();
            const EditPaint p = paint_edit_field(cheat_query_, cc, aa, std::max(4, inner - display_width(label) - 1), "", true);
            row_out(header_sgr(settings_) + label + R + p.s, display_width(label) + p.cols);
            ++used;
        }
        {   // the count
            int full = 0;
            for (const Res& r : res) if (r.hit.full()) ++full;
            std::string info;
            if (words.empty()) info = "Type what you look for: words in any order, typos and related words are found too.";
            else if (res.empty()) info = "Nothing found. Try fewer or other words.";
            else info = std::to_string(res.size()) + (res.size() == 1 ? " command" : " commands") + (full < static_cast<int>(res.size()) ? " (" + std::to_string(full) + " with all words, the rest with some)" : "");
            row_out(legend_sgr(settings_) + truncate_str(info, inner) + R, display_width(truncate_str(info, inner)));
            row_out("", 0);
            used += 2;
        }
        // The results: each one is the key and its description (and its category in brackets) wrapped to at most 3 lines, the
        // matching words marked. The selected one starts with a cursor mark.
        auto wrap_offsets = [&](const std::string& s, int width, int cont_width, size_t max_lines) {
            std::vector<std::pair<size_t, size_t>> lines;
            size_t i = 0, line_start = std::string::npos, line_end = 0;
            int line_w = 0;
            while (i < s.size()) {
                const size_t gap_from = i;
                while (i < s.size() && s[i] == ' ') ++i;
                if (i >= s.size()) break;
                size_t j = i;
                while (j < s.size() && s[j] != ' ') ++j;
                const int ww = display_width(s.substr(i, j - i));
                const int gap = static_cast<int>(i - gap_from);   // the real spaces in front of the word (the key and the text are 2 apart)
                if (line_start != std::string::npos && line_w + gap + ww > (lines.empty() ? width : cont_width)) {
                    lines.emplace_back(line_start, line_end);
                    line_start = std::string::npos;
                    if (lines.size() >= max_lines) return lines;
                }
                if (line_start == std::string::npos) { line_start = i; line_w = ww; }
                else line_w += gap + ww;
                line_end = j;
                i = j;
            }
            if (line_start != std::string::npos && lines.size() < max_lines) lines.emplace_back(line_start, line_end);
            return lines;
        };
        auto paint = [&](const std::string& doc, size_t from, size_t to, const std::vector<helpsearch::Range>& rg, int& cols) {
            std::string out;
            size_t pos = from;
            for (const auto& r : rg) {
                if (r.to <= from || r.from >= to) continue;
                const size_t a = std::max(r.from, from), b = std::min(r.to, to);
                if (a > pos) out += doc.substr(pos, a - pos);
                out += mark_sgr + doc.substr(a, b - a) + R;
                pos = b;
            }
            if (pos < to) out += doc.substr(pos, to - pos);
            cols = display_width(doc.substr(from, to - from));
            return out;
        };
        const int text_w = std::max(10, inner - 2);
        std::vector<std::vector<std::pair<size_t, size_t>>> blocks;
        for (const Res& r : res) blocks.push_back(wrap_offsets(r.doc, text_w, std::max(10, text_w - desc_col), 3));
        const int room = std::max(1, visible - used);
        cheat_first_ = std::clamp(cheat_first_, 0, std::max(0, static_cast<int>(res.size()) - 1));
        if (cheat_sel_ < cheat_first_) cheat_first_ = cheat_sel_;
        for (;;) {   // scroll down until the selected result fits
            int h = 0;
            for (int i = cheat_first_; i <= cheat_sel_ && i < static_cast<int>(res.size()); ++i) h += static_cast<int>(blocks[static_cast<size_t>(i)].size());
            if (h <= room || cheat_first_ >= cheat_sel_) break;
            ++cheat_first_;
        }
        int rows_left = room;
        for (int i = cheat_first_; i < static_cast<int>(res.size()) && rows_left > 0; ++i) {
            const auto& bl = blocks[static_cast<size_t>(i)];
            for (size_t k = 0; k < bl.size() && rows_left > 0; ++k, --rows_left) {
                int cols = 0;
                const std::string body = paint(res[static_cast<size_t>(i)].doc, bl[k].first, bl[k].second, res[static_cast<size_t>(i)].hit.ranges, cols);
                const bool sel = i == cheat_sel_;
                const std::string lead = (k == 0) ? (sel ? cursor_sgr_ + "\u25b6 " + R : std::string("  ")) : std::string("  ");
                const int indent = k == 0 ? 0 : desc_col;   // wrapped lines continue under the description column
                row_out(lead + std::string(static_cast<size_t>(indent), ' ') + body, 2 + indent + cols);
                used++;
            }
        }
        while (rows_left-- > 0) row_out("", 0);
        frame << box_bottom(W, "[ESC] clear / back  [ENTER] show in the table  [\u2191\u2193] choose", border) << "\n";
        return;
    }

    // A coloured line can't go through box_line(): that pads by counting bytes, so it would treat the escape codes
    // as columns. Pad the plain text first, then wrap the padded result.
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    int shown = 0;
    for (int i = cheatsheet_scroll_; i < total && shown < visible; ++i, ++shown) {
        const DLine& d = dlines[static_cast<size_t>(i)];
        frame << bar << " ";
        if (!d.sgr.empty()) frame << d.sgr;
        frame << pad_right(d.plain, inner);
        if (!d.sgr.empty()) frame << R;
        frame << " " << bar << "\n";
    }
    while (shown < visible) { frame << box_line("", W, border) << "\n"; ++shown; }

    std::string bottom = "[" + hotkey_text("HKeyCheatsheet", "?") + " / ESC] close  [SHIFT+s] search";
    if (max_scroll > 0) bottom += "  [\u2191\u2193] scroll " + std::to_string(cheatsheet_scroll_ + 1) + "/"
                                + std::to_string(total);
    frame << box_bottom(W, bottom, border) << "\n";
}


} // namespace muisc
