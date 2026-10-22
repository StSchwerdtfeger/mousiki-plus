#pragma once
#include "wizard.h"
#include "creator.h"
#include "mic_capture.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "disk_art.h"
#include "remote_server.h"
#include "remote_overlay.h"
#include "fft_visualizer.h"
#include "history.h"
#include "local_source.h"
#include "lyrics_fetcher.h"
#include "lyrics_translate.h"
#include "meta_editor.h"
#include "metadata_probe.h"
#include "native_duration.h"
#include "online_source.h"
#include "oscilloscope_visualizer.h"
#include "terminal_gfx.h"
#include "player.h"
#include "playlist_manager.h"
#include "settings.h"
#include "snapshot.h"
#include "sphere_visualizer.h"
#include "streaming_pcm.h"
#include "terminal_ui.h"
#include "themes.h"
#include "waveform.h"
#include "youtube_source.h"
#include "cache_manager.h"
#include "converter.h"

namespace muisc {

enum class Mode { Browse, Search, Settings, ColorEdit, Console, Cheatsheet, BulkAdd, RetryLyrics, Playlist, MetaEdit, History, ClearQueue, OsciMenu, NormMenu, Equalizer, SleepTimer, LyricsEdit, Karaoke, SpectroMenu, Wizard, ReadLyrics, Creator, MenuIndex, UpdateNotice, QueueToPlaylist };
enum class ListSource { Local, Online, Playlist, Folder };

// One row of the main UI's "/f:" folder list: a folder that directly
// contains at least one scanned local track.
struct FolderSummary {
    std::string path;   // parent-directory path (same form as folder_filter_)
    std::string name;   // folder's own name
    std::string parent; // name of its parent folder (usually the artist)
    int track_count = 0;
};

struct QueueItem {
    bool is_local;
    std::string title;
    std::string artist;
    fs::path local_path;   // valid if is_local
    std::string video_id;  // valid if !is_local
    // Queue-then-stop mode on a locked queue only: set once this item has been
    // played in the current pass through the queue (it then sits at the back).
    // The pass is over -- and playback stops -- when every item has it set.
    bool played = false;
    // Saved playlist this item was queued from ("" = hand-picked track): the history files the play under it (TOP PLAYLISTS).
    std::string playlist;
};

class App {
public:
    App();
    int run();
    void shutdown();                       // really end the session (also when it was suspended for the radio)
    bool suspended() const { return suspended_; }

private:
    // --- infrastructure ---
    CacheManager cache_;
    YoutubeSource youtube_{cache_};
    OnlineSource online_;
    LocalSource local_source_;
    DiskArt disk_;
    mutable Player player_;
    fs::path lyrics_script_;
    fs::path fast_search_script_; // empty if not found -- OnlineSource falls back to yt-dlp

    // --- lists / navigation ---
    Mode mode_ = Mode::Browse;
    ListSource list_source_ = ListSource::Local;
    std::vector<LocalTrack> all_local_tracks_;
    // Search index parallel to all_local_tracks_ (rebuilt when the scan changes): the strings a local search compares,
    // already lowercased / separator-folded, and the cache key, so a keystroke does no allocation and no path conversion
    // per track. The tag fields are filled in as the background sweep resolves them (tags_seen).
    struct SearchIdx {
        std::string key;
        std::string title_n, folder_artist_n;
        std::string tag_title_n, artist_n, album_n;
        bool tags_seen = false;
    };
    mutable std::vector<SearchIdx> search_idx_;
    // scan() runs from the constructor, before ConsoleLog::instance().init()
    // is called at the top of run() -- and init() wipes the in-memory log
    // buffer, so anything logged before it would just be discarded. This
    // holds scan()'s per-root diagnostics (found/missing, file counts) until
    // run() can actually flush them into the log.
    std::vector<std::string> local_scan_diagnostics_;
    std::vector<LocalTrack> local_view_;     // filtered
    std::vector<OnlineResult> online_view_;
    int selected_ = 0;
    int scroll_ = 0;
    std::string search_buffer_;
    std::string last_local_query_;
    ListSource pre_search_list_source_ = ListSource::Local;
    std::string pre_search_local_query_;
    std::string last_online_query_;
    int local_sort_mode_ = 0; // 0=folder order, 1=title A-Z, 2=artist A-Z
    static constexpr int kListVisibleRows = 8; // initial list height before the first frame; from then on the list/queue panes take ALL the rows the terminal leaves (render_frame()), so a maximised window is filled instead of ending in blank lines

    // --- terminal-height awareness --------------------------------------
    // The render loop used to only ever look at term.cols() (see the
    // comment on the width clamp in render_frame()) and unconditionally
    // emitted a fixed-height frame — metadata panel + progress + search
    // bar + a hardcoded kListVisibleRows-row list/queue panel + status
    // line — redrawn purely with "\x1b[H" + "\x1b[0J" and no alternate-
    // screen buffer. On a terminal shorter than that fixed height, each
    // frame overflows the viewport and scrolls; the next frame's
    // "\x1b[H" then homes to the top of the *new* scrolled-into-view
    // position rather than the top of the previous frame, so it draws a
    // fresh copy further down, which overflows again, forever — visible
    // as the panel endlessly re-duplicating itself downward.
    //
    // term_rows_ is the real, current terminal row count (from
    // TerminalIO::rows(), which does read the OS via ioctl(TIOCGWINSZ)
    // correctly -- it just wasn't being consulted anywhere). Refreshed
    // once per frame at the top of render_frame(). list_visible_rows_ is
    // how many list/queue rows *actually* fit this frame -- clamped
    // between 0 and kListVisibleRows based on how much room term_rows_
    // leaves after the fixed chrome (metadata/progress/search bar/status
    // line) -- 0 and UP: since the cap was lifted the panes grow with the
    // terminal, so going full screen no longer leaves unused rows below the
    // main UI, and every overlay that mirrors the main UI's height
    // (player_view_height()) follows. Every place that used to scroll-clamp or size against the
    // kListVisibleRows constant now uses this instead, so what's
    // rendered and what the scroll math thinks is visible never
    // disagree. render_frame() also applies a hard line-count safety net
    // (see clamp_output_rows()) on top of this, so even a terminal too
    // short for the fixed chrome alone (metadata+progress+search bar)
    // still can never scroll — the two mechanisms are independent, not
    // "either/or".
    int term_rows_ = 24;
    int list_visible_rows_ = kListVisibleRows;

    // --- list overlay (HKeyListOverlay, SHIFT+l) -------------------------
    // A large floating copy of the list pane (LOCAL AUDIO FILES when the
    // local library is showing), drawn over the main UI like the Retry
    // Lyrics form. It is NOT a Mode: it stays a plain flag on top of
    // Browse/Search so every list command (play, sort, folder filter,
    // search, queue add, ...) keeps working through the normal key
    // dispatch, and it shares selected_/scroll_ with the small pane so
    // both always show the same cursor. Only the number of rows differs,
    // which is what list_nav_rows() abstracts -- every scroll-follows-the-
    // cursor calculation asks it instead of using list_visible_rows_ directly.
    bool list_overlay_open_ = false;
    int overlay_list_rows_ = kListVisibleRows; // list rows the overlay fits; recomputed every frame
    static constexpr int kListOverlayChromeRows = 5; // search bar (3) + list box top/bottom border (2)
    // The key legend sits BELOW the frame as separate gray lines (like the
    // playlist / meta editor footers) and takes rows away from the list so
    // the overlay never grows past the terminal (term_rows_ - 2 rows, from
    // row 2): geometry and painter both ask this. Entries are never split
    // across two lines.
    std::vector<std::string> list_overlay_legend(int panel_w) const;
    bool list_overlay_active() const; // open AND in a mode that shows the main UI underneath it
    int list_nav_rows() const { return list_overlay_active() ? overlay_list_rows_ : list_visible_rows_; }
    void list_overlay_geometry(int W, int& panel_w, int& list_rows) const;
    void list_overlay_fit_scroll(int rows); // keep selected_ inside a `rows`-row window and the window inside the list
    void list_overlay_open();
    void list_overlay_close();
    void list_overlay_page(int dir); // dir=-1 page up, +1 page down
    std::vector<std::string> build_list_overlay_panel(int panel_w, int list_rows) const;
    std::string clamp_output_rows(const std::string& frame, int term_rows) const;

    // --- folder overlay (HKeyFolderOverlay, SHIFT+f) ---------------------
    // A big floating folder browser: a search bar and the LOCAL AUDIO FILES FOLDER pane, a tree of the library's
    // folders. ENTER opens / closes a folder (its subfolders and files appear slightly indented below it); `f` on a
    // folder or on a file does what `f` does in the list (show that folder's files in the LOCAL AUDIO FILES pane,
    // folder order) and closes the overlay. Like the list overlay it is a plain flag on top of Browse; it owns the
    // keys it needs and lets the playback keys fall through.
    struct FoRow {
        std::string path;    // folder or file path (UTF-8)
        std::string label;
        bool is_dir = false;
        bool expanded = false;
        int depth = 0;
        int count = 0;       // tracks below a folder
    };
    bool folder_overlay_open_ = false;
    bool fo_search_ = false;                 // the search field has the keyboard
    std::string fo_query_;
    int fo_sel_ = 0, fo_scroll_ = 0;
    int fo_rows_ = kListVisibleRows;         // list rows the overlay fits; recomputed every frame
    std::set<std::string> fo_expanded_;
    std::vector<FoRow> fo_view_;             // the visible rows (tree walk + search filter)
    std::map<std::string, std::vector<std::string>> fo_subdirs_;   // folder -> sorted subfolders
    std::map<std::string, std::vector<std::string>> fo_files_;     // folder -> sorted files
    std::map<std::string, int> fo_count_;                          // folder -> tracks at or below it
    std::vector<std::string> fo_roots_;

    // --- file conversion overlay (HKeyConvertOverlay, SHIFT+c) -----------
    // A big floating window to turn high resolution files into MP3 / AAC / Opus: a search bar, a pre-filter (type,
    // minimum bitrate, hi-res only), a list of the library's files with checkboxes, and the target preset. The marked
    // files (or just the hovered one) are converted by a background ffmpeg job into the CONVERSION EXPORT PATH; the
    // originals are never touched. Like the other big overlays it is a plain flag on top of Browse.
    struct CvFile {
        std::string path;    // UTF-8
        std::string name;    // file name with extension
        std::string ext;     // lower case, with the dot
        long long size = 0;
    };
    bool convert_overlay_open_ = false;
    bool cv_search_ = false;
    std::string cv_query_;
    int cv_sel_ = 0, cv_scroll_ = 0;
    int cv_rows_ = kListVisibleRows;          // file rows the overlay fits; recomputed every frame
    int cv_type_ = 1;                         // pre-filter: 0 all, 1 lossless, 2 FLAC, 3 WAV, 4 M4A, 5 OGG, 6 OPUS, 7 AAC, 8 MP3
    int cv_minkbps_ = 0;                      // pre-filter: index into the minimum-bitrate steps (0 = any)
    bool cv_hires_ = false;                   // pre-filter: only more than 16 bit or 48 kHz
    int cv_preset_ = 0;                       // target (convert_preset())
    std::vector<CvFile> cv_files_;            // every library file outside the export folder
    std::vector<int> cv_view_;                // indices into cv_files_: what the filters + the search leave
    std::set<std::string> cv_marked_;
    // Search bar modes: 0 = files ("/"), 1 = folders ("/f:"), 2 = playlists ("/p:"). In the last two the list shows the
    // folders / playlists matching the query; ENTER on one narrows the file list to its files (the "scope").
    int cv_pick_ = 0;
    std::vector<FolderSummary> cv_folders_;
    std::vector<PlaylistSummary> cv_playlists_;
    bool cv_scope_ = false;
    std::string cv_scope_label_;             // "folder Rock" / "playlist Road trip"
    std::set<std::string> cv_scope_paths_;   // the files of the scope (UTF-8 paths)
    mutable std::mutex cv_mu_;
    std::map<std::string, AudioInfo> cv_info_;   // probe results (guarded by cv_mu_)
    std::thread cv_probe_th_;
    std::atomic<bool> cv_probe_stop_{false};
    std::atomic<int> cv_probe_done_{0};       // probes finished since the overlay opened
    int cv_probe_seen_ = -1;                  // what the last view rebuild saw
    int cv_probe_total_ = 0;                  // files the probe thread was asked for
    int cv_last_done_ = -1;                   // conversion progress last reported on the status line
    Converter converter_;
    bool convert_overlay_active() const;
    void convert_overlay_open();
    void convert_overlay_close();
    void convert_overlay_load_files();
    void convert_overlay_probe_restart();
    void convert_overlay_rebuild_view();
    void convert_overlay_start();
    void convert_overlay_rebuild_picks();     // the folder / playlist list of the "/f:" / "/p:" modes
    void convert_overlay_pick();              // ENTER on a folder / playlist row: narrow the file list to it
    int convert_overlay_total() const;        // rows of whatever the list shows right now (files, folders or playlists)
    void convert_overlay_poll();              // once per frame: re-filter when probes arrived, report progress
    bool convert_overlay_key(int key);        // true = consumed
    void convert_overlay_geometry(int W, int& panel_w, int& rows) const;
    std::vector<LegendRow> convert_overlay_legend(int panel_w) const;
    std::vector<std::string> build_convert_overlay_panel(int panel_w, int rows) const;
    std::string conversion_export_dir() const;
    AudioInfo cv_info_of(const std::string& path) const;
    bool folder_overlay_active() const;
    void folder_overlay_open();
    void folder_overlay_close();
    void folder_overlay_build_tree();
    void folder_overlay_rebuild_view();
    bool folder_overlay_key(int key);        // true = consumed
    void folder_overlay_filter();            // `f`: show the hovered folder / the folder of the hovered file in the list
    void folder_overlay_geometry(int W, int& panel_w, int& rows) const;
    std::vector<std::string> folder_overlay_legend(int panel_w) const;
    std::vector<std::string> build_folder_overlay_panel(int panel_w, int rows) const;

    // --- queue overlay (HKeyQueueOverlay, SHIFT+k) -----------------------
    // The same idea as the list overlay above, for the QUEUE pane: a large
    // floating copy drawn over the main UI. Also a plain flag (not a Mode),
    // so every queue command (move, remove, bulk add, clear, playback keys)
    // keeps working through the normal key dispatch; it shares
    // queue_selected_/queue_scroll_ with the small pane and only the row
    // count differs (queue_nav_rows()). The queue gets focus while it is
    // open and gets its previous focus back on close. The two overlays are
    // mutually exclusive: opening one closes the other.
    bool queue_overlay_open_ = false;
    bool queue_overlay_prev_focus_ = false;       // queue_focus_ before the overlay opened
    int overlay_queue_rows_ = kListVisibleRows;   // queue rows the overlay fits; recomputed every frame
    static constexpr int kQueueOverlayChromeRows = 2; // queue box top/bottom border
    // The menus index (HKeyMenus, "<"): a floating panel listing every menu / overlay by category with its key; ENTER opens one.
    static constexpr int kMenuIndexPanelWidth = 100;
    int menu_index_sel_ = 0;                          // the highlighted entry (index into the table in app.cpp)
    std::vector<std::string> build_menu_index_panel() const;
    void menu_index_key(int key);
    // The legend of a big overlay hangs below its frame. With more than one row the block is centred on the main screen's bottom
    // border line (3 rows: the second row lies on it), so it reaches this many rows further down than a one-row legend would.
    static constexpr int overlay_legend_shift(int rows) { return rows > 1 ? (rows - 1) - rows / 2 : 0; }
    std::vector<LegendRow> queue_overlay_legend(int panel_w) const; // see list_overlay_legend()
    bool queue_overlay_active() const; // open AND in a mode that shows the main UI underneath it
    int queue_nav_rows() const { return queue_overlay_active() ? overlay_queue_rows_ : list_visible_rows_; }
    void queue_overlay_geometry(int W, int& panel_w, int& queue_rows) const;
    void queue_overlay_open();
    void queue_overlay_close();
    void queue_overlay_page(int dir); // dir=-1 page up, +1 page down
    std::vector<std::string> build_queue_overlay_panel(int panel_w, int queue_rows) const;

    mutable std::mutex row_meta_mutex_;
    std::unordered_map<std::string, RowMeta> row_meta_cache_;
    std::atomic<bool> row_meta_resolver_started_{false};
    void launch_row_meta_resolver();
    // Bumped by launch_row_meta_resolver()'s background thread every time it
    // resolves a new file's real tags (artist/title/album), so a search view
    // computed BEFORE that file's tags arrived can be told "something
    // changed, re-filter" instead of sitting stale until the user retypes
    // their query. See poll_pending_row_meta_tags() -- same
    // background-thread-finished-do-something-on-the-main-thread pattern as
    // poll_pending_waveform()/poll_pending_search() just below it.
    std::atomic<uint64_t> row_meta_tags_version_{0};
    uint64_t row_meta_tags_seen_ = 0; // main-thread only, no atomic needed
    std::chrono::steady_clock::time_point row_meta_refilter_at_{};   // throttle of poll_pending_row_meta_tags()
    void poll_pending_row_meta_tags();

    // --- queue ---
    std::vector<QueueItem> queue_;
    int queue_selected_ = 0;   // cursor/"hovering" row, only meaningful once queue_focus_ has been used
    int queue_scroll_ = 0;
    bool search_nav_moved_ = false;   // Up/Down used in the search box: ENTER then plays the highlighted entry
    bool queue_focus_ = false; // Tab toggles which panel Up/Down navigates

    // "!" (HKeyQueueLock): a LOCKED queue (the default) keeps its tracks when
    // they are played: the played track moves to the END of the queue (also
    // for "n"), so the queue loops instead of draining. UNLOCKED, a played
    // track leaves the queue. The head is always what plays next (or a random
    // item in Shuffle). "d" and Shift+X still remove tracks: locking only
    // stops tracks from disappearing by themselves.
    bool queue_locked_ = true;
    // Number of tracks "a" (add as NEXT) has put in front of everything else
    // since the queue head last moved, so pressing "a" on A, B, C plays them
    // as A, B, C rather than C, B, A. Reset whenever the head moves / the
    // queue is reshuffled by hand.
    int queue_next_run_ = 0;
    // What Shift+X cleared, so Ctrl+Shift+Z can bring it back (one level).
    std::vector<QueueItem> queue_undo_;
    void queue_toggle_lock();
    void queue_add_selected_end();       // "e": hovering track to the END of the queue
    void queue_move_to_edge(int dir);    // Shift+4 / Shift+5: dir=-1 top, +1 bottom
    void queue_reset_lap(); // clears every item's "played in this pass" flag (queue-then-stop)
    void queue_after_move(int from, int to); // a manual move just ends the current "a" run
    void queue_undo_clear();             // Ctrl+Shift+Z
    // Ctrl+Shift+U asks first: a NEW playlist (editor, name field) or an EXISTING one (search + list of the saved
    // playlists); the queue's tracks are added to it and the editor opens, where the change still has to be saved.
    std::vector<PlaylistTrack> queue_local_tracks(int& skipped) const;
    void queue_to_playlist_apply(const std::string& existing);   // existing empty = a new playlist
    void qtp_refresh();
    int qtp_stage_ = 0;                  // 0 = new / existing, 1 = pick the existing playlist
    int qtp_choice_ = 0;                 // 0 = New playlist, 1 = Existing playlist
    std::string qtp_query_;
    std::vector<PlaylistSummary> qtp_view_;
    int qtp_selected_ = 0;
    static constexpr int kQtpPanelWidth = 56;
    static constexpr int kQtpListRows = 8;
    std::vector<std::string> build_queue_to_playlist_panel() const;
    void queue_to_playlist();            // Ctrl+Shift+U: queue -> playlist editor (name field)
    std::string hotkey_text(const char* action, const char* fallback) const; // bound key, as shown in a legend

    // --- playlists (local-files-only; see playlist_manager.h) -----------
    // Main UI: results of a "/p:" search (list_source_==Playlist), i.e.
    // browsing saved playlists the same way "/s:" browses online results.
    // Selecting one and hitting Enter queues every (non-missing) track it
    // contains -- see playlist_add_selected_to_queue().
    std::vector<PlaylistSummary> playlist_view_;
    std::string last_playlist_query_;
    std::vector<PlaylistSummary> filter_playlists(const std::string& query) const;

    // Main UI: results of a "/f:" search (list_source_==Folder). Enter on a
    // row opens that folder in the LOCAL AUDIO FILES pane -- it sets
    // folder_filter_ (exactly what 'f' does) and switches back to the local
    // list, so every file of the folder is listed.
    std::vector<FolderSummary> folder_view_;
    std::string last_folder_query_;
    std::vector<FolderSummary> filter_folders(const std::string& query) const;
    void open_selected_folder();
    // First configured PlaylistsPath (settings_.playlists_paths[0]) if the
    // user set one (config.txt's PlaylistsPath=), else
    // local_music_paths[0]/playlists -- this is the folder NEW playlists
    // are written to and deleted from.
    fs::path playlists_dir() const;
    // Every configured PlaylistsPath (all of them are searched when
    // listing/loading playlists); just {playlists_dir()} when none is set,
    // so every caller has at least one folder to look in.
    std::vector<fs::path> playlist_dirs() const;
    // Playlists found across playlist_dirs(), merged and de-duplicated by
    // name (the first folder containing a name wins) -- what both the
    // main "/p:" list and the playlist editor's manage tab show.
    std::vector<PlaylistSummary> playlist_summaries() const;
    // Loads a playlist by name from whichever playlist_dirs() folder has
    // it; std::nullopt if none does.
    std::optional<Playlist> load_playlist(const std::string& name) const;
    void playlist_add_selected_to_queue();

    // --- playlist editor overlay (Mode::Playlist, HKeyPlaylist) ----------
    // Two tabs: 0 = create/edit (name field + a local-library picker to
    // fuzzy-search/add from + the in-progress track list), 1 = browse
    // saved playlists (Enter loads one into tab 0 for re-editing).
    // HOME saves (deliberately not a plain letter -- "s" collided with
    // typing an "s" into the name/search fields). ESC exits; if tab 0 has
    // unsaved changes it asks first (playlist_confirm_exit_) rather than
    // silently discarding them.
    int playlist_tab_ = 0;
    int playlist_edit_focus_ = 0; // 0=name field, 1=library picker, 2=playlist-tracks list -- cycled with Tab
    std::string playlist_edit_name_;
    std::vector<PlaylistTrack> playlist_edit_tracks_;
    std::string playlist_edit_lib_query_;
    std::vector<LocalTrack> playlist_edit_lib_view_;   // filter_and_rank_local(playlist_edit_lib_query_)
    int playlist_edit_lib_selected_ = 0;
    std::vector<LocalTrack> filter_local_or_folders(const std::string& query) const;   // search, or "/f:" folder search
    int playlist_edit_sort_ = 0;   // LIBRARY sort (Shift+T): 0 folder order, 1 file names, 2 titles, 3 artists, 4 albums A-Z
    static const char* playlist_sort_name(int mode);
    int playlist_edit_track_selected_ = 0;
    bool playlist_edit_dirty_ = false;    // true once tab 0 has unsaved changes -- see ESC's confirm prompt below
    bool playlist_confirm_exit_ = false;  // "save before exiting?" Y/N prompt, shown in place of the hint line
    std::vector<PlaylistSummary> playlist_manage_view_; // tab 1's list
    int playlist_manage_selected_ = 0;
    // Tab 1 got a search box of its own (the same caret/selection/clipboard
    // treatment as every other text field -- see edit_text_key()). Focus
    // decides who owns the keys: in the box, typing filters and Left/Right
    // are caret keys; in the list, Up/Down/Enter/DEL work and Left/Right
    // keep switching tabs.
    std::string playlist_manage_query_; // filter behind playlist_manage_view_
    int playlist_manage_focus_ = 0;     // 0=search box, 1=list -- cycled with Tab
    bool playlist_confirm_delete_ = false; // "really delete this playlist?" Y/N prompt (tab 1, DEL key)
    // Export overlay ('e' on the Saved Playlists tab): writes the hovered
    // playlist as an M3U8 (default) or M3U file into a folder that starts as
    // Settings -> PATHS -> PLAYLIST EXPORT PATH and can be changed in place.
    bool playlist_export_open_ = false;
    std::string playlist_export_name_;     // the playlist being exported
    std::string playlist_export_dir_;      // the folder field
    bool playlist_export_m3u_ = false;     // false = .m3u8 (UTF-8, the default), true = .m3u
    int playlist_export_field_ = 0;        // 0 = folder field, 1 = format row
    std::string playlist_export_status_;   // shown inside the overlay
    void playlist_export_begin();          // opens the overlay for the hovered saved playlist
    void playlist_export_run();            // ENTER in the overlay
    std::vector<std::string> build_playlist_export_panel(int panel_w) const;

    // Import overlay ('i' on the Saved Playlists tab): reads the .m3u / .m3u8 files of a folder
    // (default: the same folder the export overlay starts in) and saves them as playlists of their own.
    bool playlist_import_open_ = false;
    std::string playlist_import_dir_;                 // the folder field
    int playlist_import_field_ = 0;                   // 0 = folder field, 1 = file list
    std::vector<std::string> playlist_import_files_;  // .m3u / .m3u8 files found in the folder (full paths)
    std::vector<int> playlist_import_counts_;         // entries per file
    std::vector<char> playlist_import_marked_;
    int playlist_import_selected_ = 0;
    std::string playlist_import_status_;              // shown inside the overlay
    void playlist_import_begin();
    void playlist_import_scan();                      // refills the file list from the folder field
    void playlist_import_run();                       // ENTER in the overlay
    std::vector<std::string> build_playlist_import_panel(int panel_w) const;
    std::vector<std::string> build_playlist_manage_search_panel(int width) const; // SEARCH PLAYLISTS
    std::string playlist_status_; // shown at the bottom of the overlay; cleared on (re)entry

    void playlist_refresh_lib_view();
    void playlist_refresh_manage_view();
    void playlist_open_editor();  // HKeyPlaylist entry point -- resets to a blank new playlist on tab 0
    void playlist_load_into_editor(const std::string& name);
    void playlist_add_hovering_to_edit();
    void playlist_remove_hovering_track();
    void playlist_move_hovering_track(int dir); // dir=-1 up, +1 down -- keys 4/5, mirrors queue_move_hovering
    void playlist_delete_selected(); // tab 1's DEL, after playlist_confirm_delete_ confirms
    void playlist_save_current(bool leave = false); // leave: also close the editor (only the "save before exiting?" prompt does)
    void handle_playlist_key(int key);
    void build_playlist_screen(std::ostringstream& frame, int W, int player_h) const;
    std::vector<std::string> build_playlist_search_panel(int width) const;
    std::vector<std::string> build_playlist_library_panel(int width, int height) const;
    std::vector<std::string> build_playlist_tracks_panel(int width, int height) const;
    std::vector<std::string> build_playlist_manage_panel(int width, int height) const;

    // --- meta/tag editor overlay (Mode::MetaEdit, HKeyMetaEditor = Shift+M) ---
    // Deliberately shaped like the playlist editor above: same tab strip,
    // same boxed side-by-side panels, same hint/status footer, same Y/N
    // confirmation prompts in place of the hint line. Tab 0 = edit (a search
    // field, a library picker, and the five editable fields -- FILE plus
    // ARTIST/TITLE/ALBUM/YEAR -- for the row the picker is on); tab 1 = the
    // fetch list, filled with 'a' and run through AcoustID with Enter.
    //
    // Everything typed or fetched here goes into meta_session_ -- a pending
    // edit session that is autosaved to disk after every change (meta_editor.h)
    // but is NEVER written to the audio files on its own. Ctrl+Shift+S applies
    // it, Ctrl+Shift+X throws it away, and simply leaving (ESC or quitting)
    // keeps the autosave backup, so no amount of editing can lose work by
    // accident.
    int meta_tab_ = 0;
    int meta_focus_ = 0; // 0=search field, 1=library picker, 2=field editor -- cycled with Tab
    std::string meta_query_;
    std::vector<LocalTrack> meta_lib_view_;  // filter_and_rank_local(meta_query_)
    // 'r' in the library pane: park every file that carries a pending edit
    // at the top of the pane. Off by default, so merely opening the editor
    // never reorders the list behind the user's back -- see
    // meta_refresh_lib_view(), which does the sorting on every refresh.
    bool meta_resort_edited_ = false;
    int meta_lib_selected_ = 0;
    int meta_field_ = 0; // hovered row of the field editor (0=FILE .. 4=YEAR)
    // Two-stage field editor (meta_focus_ == 2): a hovered tag is only
    // highlighted until ENTER arms this, showing the caret in the CHANGE
    // colours (change_sgr()) -- the same split the PATHS tab uses between
    // a selected folder row and one actually being typed into. Reset
    // whenever focus leaves the field editor or moves to another tag.
    bool meta_field_editing_ = false;
    std::vector<MetaEditEntry> meta_session_;          // the pending edits
    std::vector<std::string> meta_fetch_list_;         // paths queued for an AcoustID lookup
    // The FETCH options (bit mask, see meta_editor.h): which tags a fetch fills in. Set on the FETCH LIST tab (its option
    // line in the header); one setting for the whole list and for SHIFT+b. Default: missing only.
    unsigned meta_fetch_opts_ = kMetaFetchDefault;   // the ticked boxes of the FETCH LIST tab (used by ENTER = fetch the full list)
    unsigned meta_prompt_opts_ = kMetaFetchDefault;  // what the pending fetch confirmation will fetch (SHIFT+b missing / SHIFT+n all / the list's boxes)
    std::string meta_fetch_disclaimer() const;
    int meta_opt_cursor_ = 0;                          // the box the option line's cursor is on
    int meta_fetch_skipped_ = 0;                       // files of the running batch that had nothing missing
    std::string meta_fetch_note_;                      // first MusicBrainz hiccup of the running batch
    // Per path of the running batch: the tag fields the fetch was allowed to fill in (set when it starts).
    std::unordered_map<std::string, std::array<bool, kMetaFieldCount>> meta_fetch_want_;
    // The choice overlay (ENTER on a field with several fetched candidates).
    bool meta_choice_open_ = false;
    int meta_choice_field_ = 0;
    int meta_choice_sel_ = 0;
    int meta_fetch_focus_ = 0;                         // FETCH LIST tab: 0 = the list, 1 = the option line in the header
    void meta_toggle_fetch_opt(int i);                 // tick / untick one FETCH option (the exclusivity rules apply)
    std::array<bool, kMetaFieldCount> meta_want_for(const std::string& path, unsigned opts);
    void meta_choice_open();
    void meta_choice_pick();          // ENTER: the highlighted candidate becomes the value
    void meta_choice_discard();       // DEL / d: drop the fetched value and every candidate
    bool handle_meta_choice_key(int key);
    std::vector<std::string> build_meta_choice_panel() const;
    static constexpr int kMetaChoicePanelWidth = 66;
    static constexpr int kMetaSavePanelWidth = 56;
    // Writing the pending edits to the files (CTRL+s -> Y). Every file is a copy through ffmpeg, so hundreds of edits take a
    // while: the work is done in slices from the render loop (poll_meta_save(), ~0.12 s per frame) instead of in one go, the
    // screen stays alive, a "please wait" panel with a progress bar shows up when it takes longer than a moment, and ESC
    // stops after the current file (what is not saved yet stays pending).
    struct MetaSaveJob {
        bool active = false;
        std::vector<MetaEditEntry> todo;    // the session as it was when saving started
        size_t next = 0;                    // first entry not processed yet
        std::vector<MetaEditEntry> failed;  // processed, could not be written (stay pending)
        int ok = 0;
        std::string first_error;
        bool cancel = false;
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point last_persist;
    };
    MetaSaveJob meta_save_;
    void poll_meta_save();                                    // render loop: one slice of the job
    void meta_save_finish();                                  // lists, status line, log
    std::vector<std::string> build_meta_save_panel() const;   // the "please wait" panel
    int meta_fetch_selected_ = 0;
    bool meta_session_loaded_ = false; // load_session() already ran (once per run, not once per open)
    std::string meta_status_; // bottom line of the overlay; cleared on (re)entry
    // Y/N confirmation currently covering the footer (in the menu) or the
    // status line (in Browse, for Shift+B), plus the exact paths it will act
    // on once confirmed. Every key is swallowed while one is up.
    enum class MetaPrompt { None, Save, Discard, Fetch, ClearFetch };   // ClearFetch: SHIFT+d / SHIFT+DEL on the fetch list
    MetaPrompt meta_prompt_ = MetaPrompt::None;
    std::vector<std::string> meta_prompt_paths_;
    // AcoustID batch: runs on its own thread, publishes results under
    // meta_fetch_mutex_, applied to the session on the main thread by
    // poll_pending_meta_fetch() (called from the render loop).
    fs::path meta_script_; // scripts/fetch_meta.py, next to the exe
    std::thread meta_fetch_thread_;
    std::atomic<bool> meta_fetch_running_{false};
    MetaFetchCancel meta_fetch_cancel_;          // SHIFT+c on the Fetch List tab
    mutable std::mutex meta_fetch_mutex_;
    std::vector<MetaFetchResult> meta_fetch_results_;
    // Paths the current batch has already delivered successfully (filled in
    // by poll_pending_meta_fetch() on the main thread, so no lock needed).
    // On completion they leave the fetch list -- what stays queued is
    // exactly what still needs another try.
    std::vector<std::string> meta_fetch_ok_paths_;
    bool meta_fetch_done_ = false;
    MetaFetchOutcome meta_fetch_outcome_;
    std::string meta_fetch_progress_; // "3/7", shown while a batch runs
    // What the field editor panel draws for the currently hovered path.
    // Precomputed once per frame (meta_refresh_hover_values(), just before
    // rendering) because the current tag values need a mutable cache lookup,
    // while build_meta_screen() itself is const.
    std::array<std::string, kMetaFieldCount> meta_hover_values_{};
    bool meta_hover_resolved_ = false; // tags for that path are final (vs. not read yet)

    void meta_open();                  // HKeyMetaEditor entry point
    void meta_ensure_session_loaded(); // restores the autosaved session, once
    void meta_refresh_lib_view();
    void meta_toggle_resort();         // 'r', edited files to the top of the pane
    // Missing-tag filters over the library pane: 0 = off, 1 = files with no
    // metadata at all ('x'), 2 = missing title (Shift+T), 3 = missing artist
    // (Shift+A), 4 = missing year (Shift+Y), 5 = missing album (Shift+L). Pressing the same key again
    // clears it -- see meta_toggle_filter().
    int meta_filter_ = 0;
    mutable int meta_page_rows_ = 10;  // rows of a list pane (set while drawing): SHIFT+up / down move this far
    // Up / Down in a meta editor list: plain = one row, SHIFT = a page, CTRL = first / last. Returns the new row.
    int meta_nav(int selected, int total, bool up) const;
    void meta_toggle_filter(int filter);
    const char* meta_filter_label() const;
    void meta_persist();               // save (or delete) the autosave backup file
    const MetaEditEntry* meta_entry(const std::string& path) const;
    MetaEditEntry& meta_touch_entry(const std::string& path); // create on first edit
    void meta_set_field(const std::string& path, int field, const std::string& value);
    std::string meta_display_value(const std::string& path, int field, const MetaEditEntry* e);
    RowMeta meta_row_meta(const fs::path& path); // current tags of a file, cached + native
    std::string meta_hovering_path() const;      // path the picker/editor/fetch tab is on
    void meta_refresh_hover_values();            // fills meta_hover_values_ for this frame
    void meta_add_hovering_to_fetch();           // 'a', mirrors the queue's add key
    void meta_cancel_fetch();                    // SHIFT+c on the Fetch List tab: stop the running AcoustID batch
    void meta_add_view_to_fetch();               // SHIFT+f: every title of the (filtered) library pane
    void meta_remove_hovering();                 // DEL/'d'
    void meta_play_hovering();                   // SHIFT+p: start the hovered track (library pane / fetch list)
    void meta_prompt_single_fetch(const std::string& path, bool all = false); // Shift+B: missing tags only; Shift+N (meta editor): all tags
    void meta_prompt_list_fetch();                          // Enter on tab 1
    void meta_start_fetch();
    void poll_pending_meta_fetch();
    void meta_apply_session();  // Ctrl+Shift+S, after Y
    void meta_discard_session();// Ctrl+Shift+X, after Y
    // True if a confirmation was up and the key was consumed by it (incl. the
    // swallow-everything-else case). Checked first thing in handle_key().
    bool handle_meta_prompt_key(int key);
    void handle_meta_key(int key);
    void build_meta_screen(std::ostringstream& frame, int W, int player_h) const;
    std::vector<std::string> build_meta_library_panel(int width, int height) const;
    std::vector<std::string> build_meta_fields_panel(int width, int height) const;
    std::vector<std::string> build_meta_fetch_panel(int width, int height) const;

    // --- listening history overlay (Mode::History, HKeyHistory = Shift+H) ---
    // Four tabs in the shape of the two overlays above: 1 HISTORY (the last
    // 100 plays, newest first), 2 TOP TRACKS (per title, sorted by play count
    // -- 'r' flips between most- and least-played first), 3 TOP PLAYLISTS (top 5
    // playlists per period, scrolls), 4 HABITS (session
    // and play-behaviour aggregates, each category under a header_sgr()
    // header). The data itself lives in HistoryStore history_ (appended to on
    // the main thread only, persisted to ~/.cache/mousiki/history/history.json
    // after every finished track and again at shutdown).
    int history_tab_ = 0;              // 0=History, 1=Top Tracks, 2=Top Playlists, 3=Habits
    static constexpr int kHistTabs = 4;
    static constexpr int kHistTabTop = 1, kHistTabPlaylists = 2, kHistTabHabits = 3;
    // Playlist context of the track being played / loaded (see QueueItem::playlist). pending_ is set by
    // play_next_from_queue() just before it starts the item and is consumed by start_*_track(); load_ follows the
    // load in flight; current_ is what history_begin_current_play() files the play under.
    std::string pending_queue_playlist_, load_playlist_ctx_, current_playlist_;
    int history_selected_ = 0;         // cursor within tabs 0/1
    int history_scroll_ = 0;           // manual scroll offset (tab 2's content)
    int history_view_top_ = 0;         // first visible list row on tabs 0/1 (window moves only when the cursor leaves it)
    bool history_most_first_ = true;   // 'r' on the Top Tracks tab
    std::vector<HistoryTopRow> history_top_view_; // rebuilt by history_refresh_top()
    std::string history_status_;       // footer status line, set by 'r' / the queue adds
    // Top Tracks tab only: it is split into two stacked panes. 0 = the track
    // list (Up/Down move its cursor), 1 = the "ADD SMART HISTORY TO QUEUE"
    // pane below it: a 4 x 4 grid of lists (column 0 Top 10/25/50/100, then
    // top of the week/month/quarter/year, top by time of day, and the
    // "rediscover" lists). Arrows move in the grid, Enter queues the list,
    // TAB toggles the panes, ESC leaves the grid for the track list.
    int history_pane_ = 0;
    int history_add_sel_ = 0;          // row 0..3 in the smart grid
    int history_add_col_ = 0;          // column 0..3 in the smart grid
    static constexpr int kHistoryAddCounts[4] = {10, 25, 50, 100};
    static constexpr int kSmartListSize = 25; // every smart list except column 0
    HistoryStore history_;             // the store itself (also used outside this overlay)

    void history_open();               // HKeyHistory entry point
    void history_refresh_top();        // rebuilds history_top_view_ from history_
    void handle_history_key(int key);
    // Both non-const: they clamp history_selected_/history_scroll_ against the
    // content they actually ended up drawing (see the window code at the end
    // of build_history_panel()).
    void build_history_screen(std::ostringstream& frame, int W, int player_h);
    std::vector<std::string> build_history_panel(int width, int height);
    void history_playlists_lines(int inner, std::vector<std::pair<std::string, int>>& lines) const; // TOP PLAYLISTS tab content
    std::vector<std::string> build_history_add_panel(int width, int height) const; // Top Tracks tab, 2nd pane
    // Queues the n most-played titles (always most-played first, whatever
    // order the Top Tracks list is currently showing). Local files that no
    // longer exist are skipped and reported, like a playlist add.
    void history_add_top_to_queue(int n);
    // The smart grid: queues the list at (col, row) -- see history_pane_.
    void history_queue_smart(int col, int row);
    // Appends up to `take` rows to the queue (missing local files are skipped)
    // and reports it in the history status line under `label`.
    void history_queue_rows(const std::vector<HistoryTopRow>& rows, int take, const std::string& label,
                            const std::string& empty_msg);
    // Play bookkeeping: called from poll_pending_load()/advance_track() when a
    // track starts or is handed over, and from the frame loop to accrue time.
    void history_end_current_play();   // closes the live record (no-op if none) + saves
    void history_begin_current_play(); // opens one for current_path_/metadata_

    // --- now playing ---
    bool has_track_ = false;
    // True from the moment advance_track() hands off to a load until
    // poll_pending_load() publishes its result (success or failure). While
    // it's set the finished track stays "current" (has_track_ remains true, so
    // the UI doesn't flash "no track loaded" between songs), and this flag is
    // what stops the main loop re-firing advance_track() every frame on the
    // still-set finished flag.
    bool advancing_ = false;
    fs::path current_path_;
    bool current_is_local_ = true;  // for snapshot identity -- current_path_ alone is ambiguous
                                     // (online tracks resolve to a cache path too)
    std::string current_video_id_;  // valid when !current_is_local_
    TrackMetadata metadata_;
    // metadata_ with the playing file's pending meta-editor edits laid over it -- what the main screen shows.
    TrackMetadata display_metadata() const;
    std::vector<float> waveform_envelope_;
    std::chrono::steady_clock::time_point waveform_reveal_start_;
    bool waveform_ready_ = false;
    std::shared_ptr<StreamingPcm> current_pcm_;
    size_t total_sec_ = 0;
    double angle_ = 0.0;
    std::chrono::steady_clock::time_point last_frame_time_;
    static constexpr double kAngularVelocity = (2.0 * 3.14159265358979323846 / 48.0) / 0.035;
    mutable FftVisualizer fft_;
    mutable SphereVisualizer sphere_;
    // The scope alternative to sphere_ (Settings -> ON/OFF -> "Lyrics/Viz"):
    // fed live by the audio callback via player_.play(..., &scope_), drawn
    // by whoever renders the lyrics panel. mutable, like sphere_, because
    // that renderer is const and the panel's visuals are animation state.
    mutable OscilloscopeVisualizer scope_;
    mutable std::string last_lyrics_status_;
    mutable std::chrono::steady_clock::time_point lyrics_status_shown_at_;
    mutable double viz_dt_ = 0.08;
    // The scope as a pixel image (Settings osci_style 1): filled while the lyrics column is built, emitted by run().
    GfxProto gfx_proto_ = GfxProto::None;
    std::string gfx_probed_pref_;              // the protocol preference gfx_proto_ was probed for ("" = not yet)
    mutable GfxFrame gfx_;
    mutable GfxFrame gfx2_;                    // the big oscilloscope overlay's second picture (gfx_ is its YT picture there)
    mutable bool gfx2_shown_ = false;
    mutable bool gfx_ok_ = false;              // the picture belongs on screen in the frame just rendered
    mutable int gfx_last_crop_ = -1;
    mutable int scope_cut_last_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // the panel cut-outs of the overlay's pictures of the last frame
    mutable int cell_w_ = 9, cell_h_ = 18;
    bool gfx_due_ = true;
    // The "fetching lyrics ..." box laid over the bottom row of the lyrics area's visual this frame (0-based row, col,
    // width; width 0 = none) and its text with colours: the image styles keep these cells free / write it again on top.
    mutable int caption_rect_[3] = {0, 0, 0};
    mutable std::string caption_text_;
    static std::string overlay_cells(const std::string& line, int x, const std::string& text, int text_w, int line_w);
    mutable int float_rect_[4] = {0, 0, 0, 0}; // x, y, w, h of the floating panel drawn this frame (0-based cells)

    // --- local list marquee (hovered row's title, when too long to fit) ---
    // Tracks which row the scroll animation is currently following and
    // when it started, so moving the cursor to a different row always
    // restarts the scroll from the beginning of that row's title instead
    // of resuming wherever the previous row's animation had reached.
    mutable int marquee_row_idx_ = -1;
    mutable std::chrono::steady_clock::time_point marquee_since_;
    // Same idea, kept separate because the playlist editor's LIBRARY and
    // TRACKS panels are two independent lists with their own selection
    // cursor, visible on screen at the same time -- sharing one row/clock
    // pair between them would make switching focus between the two panels
    // restart (or skip restarting) the wrong one's scroll.
    mutable int marquee_pl_lib_row_idx_ = -1;
    mutable std::chrono::steady_clock::time_point marquee_pl_lib_since_;
    mutable int marquee_pl_track_row_idx_ = -1;
    // --- main UI metadata block: Name / Artist / Album each stay on ONE row and scroll when too long ---
    mutable int marquee_meta_idx_[3] = {-1, -1, -1};
    mutable std::chrono::steady_clock::time_point marquee_meta_since_[3];
    mutable std::chrono::steady_clock::time_point marquee_pl_track_since_;

    // --- lyrics (background-fetched) ---
    mutable std::mutex lyrics_mutex_;
    mutable LyricsResult lyrics_result_;
    std::atomic<bool> lyrics_ready_{false};
    std::atomic<int> lyrics_epoch_{0};
    void launch_lyrics_fetch(std::string title, std::string artist, fs::path path, bool force_network = false);

    std::string status_line_;
    bool quit_ = false;
    // Settings screen: the settings as they were when it opened. `s` saves and closes, ESC / q puts them back (discard).
    Settings settings_snapshot_;
    std::string settings_snapshot_text_;
    bool settings_dirty_ = false;
    int leave_prompt_ = 0;               // 1: "unsaved settings" question on ESC, 2: "unsaved theme" question (CREATE THEME), 3: "overwrite the theme?", 4: "delete the theme?" (CHOOSE THEME)
    std::string theme_name_initial_;     // the NAME the creator opened with ("" for a new theme, the theme in use for [e])
    std::string theme_draft_text_;       // the CREATE THEME draft as it was when the creator opened (to see whether anything changed)
    void settings_open();
    void settings_close(bool save);
    void settings_update_dirty();
    bool suspended_ = false;     // run() returned kExitSwitchMode: the App is kept alive and run() is called again on return
    void rescan_now();
    bool switch_mode_ = false;   // quit_ was set by the mode switch (SHIFT and +, the '*' key): run() returns kExitSwitchMode
    bool force_redraw_ = false;
    int last_render_w_ = -1;
    int last_render_rows_ = -1; // terminal height of the previous frame: a height change needs a full repaint too
    Mode last_render_mode_ = Mode::Browse;
    bool last_big_overlay_ = false;   // a big list / queue / convert / folder overlay was on the previous frame: closing it repaints everything (its legend can hang on the last terminal row, below the main frame)

    // --- mute (volume forced to 0 without touching pause state) --------
    bool muted_ = false;
    int pre_mute_volume_ = 70;

    // --- folder filter (HKeyFilterForFolder / HKeyClearFilter) ---------
    // Parent-directory path of the currently filtered folder, or empty
    // for "no folder filter". Applied on top of whatever the search/sort
    // already produced, in refresh_local_view().
    std::string folder_filter_;

    // --- floating panels (Bulk Add, Retry Lyrics) ------------------------
    // Unlike Console/Settings/Cheatsheet (full-screen overlays that
    // replace the whole view), these render *on top of* the still-live
    // Browse view behind them: render_frame() draws the normal
    // metadata/progress/search/list/status background exactly as always,
    // then this stamps a pre-built block of lines over it at an absolute
    // screen position via "\x1b[{row};{col}H" writes -- no clear, so
    // whatever was already drawn underneath stays visible around the
    // panel's edges. `lines` must already be exactly `panel_w` display
    // columns wide (pad_right them before calling) since this does no
    // width accounting of its own, just placement.
    //
    // Horizontally centered against the background's own width (W), not
    // the raw terminal width -- if the terminal's wider than W the
    // background content itself is left-anchored, and centering against
    // the full terminal would visually detach the panel from the
    // content it's supposed to be floating over. Vertically centered
    // against term_rows_, nudged up a few rows rather than dead-center.
    std::vector<std::string> panel_gradient(const std::vector<std::string>& lines) const;   // BorderGradient=smooth for a stamped panel
    void draw_floating_panel(std::ostringstream& frame, const std::vector<std::string>& lines, int panel_w, int W, int fixed_col = 0, bool exact_center = false, int fixed_row = 0) const;   // fixed_row > 0: the panel's first row (it only grows downwards)
    static constexpr int kFloatingPanelUpShift = 3; // rows nudged above true vertical center

    // --- console / log overlay (HKeyConsole) ----------------------------
    // Backed by the global ConsoleLog (console_log.h/.cpp), which owns
    // both the on-disk $HOME/.cache/mousiki/logs/console.log and the
    // in-memory buffer this overlay renders -- see log_event() and
    // build_console_screen().
    void log_event(const std::string& msg);
    void build_console_screen(std::ostringstream& frame, int W, int target_height) const;

    // --- cheatsheet overlay (HKeyCheatsheet) ----------------------------
    // Scroll position within the key table -- the table is longer than a
    // small terminal can show at once, and (unlike Settings' Reference tab)
    // this overlay used to just silently drop whatever didn't fit. Mutable
    // because build_cheatsheet_screen() is const and clamps it against the
    // rows that actually fit as it draws.
    mutable int cheatsheet_scroll_ = 0;
    // Karaoke overlay (Mode::Karaoke, HKeyKaraoke = "k"): the lyrics of the
    // playing track filling the screen, the active line highlighted word by
    // word, with the karaoke braille art on the left (disk colours, a moving
    // colour wave like the radio's ON AIR sign). Playback keys keep working.
    void build_karaoke_screen(std::ostringstream& frame, int W, int H);
    bool karaoke_size_dirty_ = false;   // lyrics size changed in the overlay: saved to config.txt when it closes
    std::chrono::steady_clock::time_point karaoke_flash_until_{};   // "[ lyrics size n / 5 ]" on the bottom border until then
    void build_cheatsheet_screen(std::ostringstream& frame, int W) const;

    // The Console and Settings overlays must always be exactly as tall as
    // the Browse-mode player view actually renders at right now -- not
    // just "whatever fits the terminal" (that's term_rows_, an upper
    // bound, not the target). Rebuilds the same panels Browse mode would
    // and sums their line counts, using the *current* list_visible_rows_
    // (itself already term_rows_-aware) for the list/queue panel's share,
    // so this always matches frame-for-frame regardless of which panels
    // are currently enabled/how tall lyrics or disk art are configured.
    int player_view_height(int w) const;

    // --- hotkey rebinding conflict check --------------------------------
    // Returns the action name already bound to key_str (excluding
    // except_action), or "" if key_str is free. Used by the Settings
    // Reference tab so rebinding a hotkey to a key another action already
    // owns is rejected instead of silently creating an overlap.
    std::string hotkey_conflict(const std::string& key_str, const std::string& except_action) const;
    // True when `key` (as read from the terminal) is the key currently bound to `action` -- for the places that have to
    // recognise a binding outside the Browse dispatch (a second press that closes an overlay, the meta editor).
    bool key_is(const char* action, int key) const;

    // --- autosave / session snapshot ------------------------------------
    // Consumed exactly once, right after a startup restore: the very
    // first launch_device_play_async() call for the restored track uses
    // this as its start position instead of 0.0, then zeroes it out so
    // every normal track change afterwards starts at 0 like always.
    double resume_start_sec_ = 0.0;
    std::chrono::steady_clock::time_point last_autosave_at_;
    // Drives the indicator's brief "something just saved" animation --
    // ticks for kAutosavePulseSeconds after each save, then goes idle.
    std::chrono::steady_clock::time_point autosave_pulse_started_at_;
    bool autosave_pulse_active_ = false;
    static constexpr double kAutosavePulseSeconds = 1.2;
    SnapshotData build_snapshot() const;
    // Applies a loaded snapshot: queue, play_mode, mute/volume, and kicks
    // off loading the saved "now playing" track at resume_start_sec_.
    // Called once at startup, before the render loop starts.
    void restore_snapshot(const SnapshotData& snap);
    // Called every frame from run(); saves (and pulses the indicator)
    // once settings_.autosave_delay_sec has elapsed since the last save.
    void maybe_autosave();
    // Builds the little "•" (or configured glyph) indicator string for
    // the volume-bar row, honoring AutoSave/AutoSaveIndicator/
    // AutoSaveIndicatorType. Returns "" when there's no room or the
    // feature's fully off (see build_progress_panel()'s comment on why
    // that also collapses the gap rather than just hiding a char in it).
    std::string autosave_indicator_glyph() const;

    // --- bulk add (paste a YouTube playlist link while Queue is
    // focused; hovering-song add on 'a' stays the List-panel behavior) --
    // Floating panel (see draw_floating_panel()), fixed total line count
    // across both phases -- unused rows are just blank-padded rather
    // than the panel changing size -- so its on-screen footprint never
    // moves/resizes frame to frame while open. Two phases:
    //   1. Input: bulk_add_results_ready_ == false -- typing the link.
    //   2. Results: fetch succeeded -- a compact starred checklist,
    //      navigable with Up/Down, toggled per-row with Space. "a" adds
    //      every fetched track regardless of star state ("ALL"); Enter
    //      adds only the starred ones ("[SELECT]").
    std::string bulk_add_buffer_;
    bool bulk_add_results_ready_ = false;
    std::vector<bool> bulk_add_selected_;   // parallel to pending_bulk_add_.items, default all-starred
    int bulk_add_cursor_ = 0;
    int bulk_add_scroll_ = 0;
    static constexpr int kBulkAddVisibleRows = 9; // preview list height cap -- this is what keeps the panel "tiny"
    struct BulkAddResult {
        bool success = false;
        std::string error;
        std::vector<OnlineResult> items;
    };
    std::thread bulk_add_thread_;
    std::mutex bulk_add_mutex_;
    std::atomic<bool> bulk_add_in_progress_{false};
    std::atomic<bool> bulk_add_ready_{false};
    BulkAddResult pending_bulk_add_;
    void launch_bulk_add_async(const std::string& url);
    void poll_pending_bulk_add();
    void commit_bulk_add(bool all); // all=true -> every fetched track; all=false -> only starred ones
    static constexpr int kBulkAddPanelWidth = 62; // matches the reference design exactly
    std::vector<std::string> build_bulk_add_panel() const; // returns fixed-width, fixed-height lines for draw_floating_panel()

    // --- retry lyrics (HKeyRetryLyrics, 'l') ------------------------------
    // A manual override form: rather than instantly re-fetching with the
    // track's own metadata, this lets the person edit the title/artist
    // mousiki searches with and tack on edit-qualifier tags (slowed,
    // reverb, ...) -- for tracks whose auto-fetched lyrics are wrong or
    // missing because the real upload's title doesn't match what's
    // playing. Opens pre-filled from the current track's metadata_ (with
    // a best-effort "ft./feat." split out of the title into its own
    // field) rather than blank.
    enum class RLField {
        Source,   // the list of lyrics versions at the top (embedded in the file / fetched / older fetches)
        Title, Artist, Ft,
        TypeReverb, TypeSlowed, TypeUltraSlowed, TypeSpedup, TypeRemix, TypeOther,
        RemixText, OtherText,
    };
    std::string rl_title_, rl_artist_, rl_ft_;
    std::string rl_remix_text_, rl_other_text_;
    // TypeSlowed/TypeUltraSlowed/TypeSpedup are mutually exclusive (a
    // radio group -- at most one true); TypeReverb/TypeRemix/TypeOther
    // are independent toggles, any combination.
    bool rl_reverb_ = false, rl_slowed_ = false, rl_ultra_slowed_ = false;
    bool rl_spedup_ = false, rl_remix_ = false, rl_other_ = false;
    RLField rl_focus_ = RLField::Title;
    std::vector<LyricsVersion> rl_versions_;   // what list_lyrics_versions() found for the playing track
    int rl_src_sel_ = 0;                       // the highlighted row of that list
    void rl_choose_version();                  // SPACE on the list: that version is used from now on
    int rl_src_scroll_ = 0;                    // first visible row of the list (it shows 3 at a time)
    bool rl_confirm_delete_ = false;           // [d] asked "delete these lyrics?", waiting for y / n
    void rl_fix_scroll();                      // keeps the highlighted row inside the 3 visible ones
    void rl_delete_version();                  // after the y: removes the highlighted saved copy
    static constexpr int kRetryLyricsPanelWidth = 72; // wide enough for the legend on the bottom border
    // The fields actually navigable right now, in on-screen order --
    // RemixText/OtherText only appear in this list once their checkbox
    // is on, which is what makes them "dynamically available".
    std::vector<RLField> rl_visible_fields() const;
    void rl_open_from_current_track();     // pre-fill + reset state, called when 'l' opens the panel
    void rl_submit();                       // builds the override query and launches the fetch
    bool* rl_bool_ptr(RLField f);           // nullptr for non-checkbox fields
    std::string* rl_text_ptr(RLField f);    // nullptr for checkbox fields
    std::vector<std::string> build_retry_lyrics_panel() const; // fixed-width, fixed-height lines for draw_floating_panel()

    // --- async track loading ---
    struct PendingLoad {
        bool success = false;
        std::string title, artist, location_label, error;
        fs::path path;
        std::shared_ptr<StreamingPcm> pcm;
        size_t total_sec = 0;
        TrackMetadata metadata;
        bool is_local = true;   // for snapshot/resume identity -- which of path/video_id is authoritative
        std::string video_id;   // valid if !is_local
    };
    std::thread load_thread_;
    std::mutex load_mutex_;
    std::atomic<bool> load_ready_{false};
    std::atomic<bool> load_in_progress_{false};
    std::atomic<int> load_stage_{0};
    std::chrono::steady_clock::time_point load_started_at_;

    // Every Player call (play/stop/seek/volume/...) funnels through one
    // persistent thread that lives for the whole app session, rather than a
    // fresh std::thread per track switch. On Windows this is load-bearing,
    // not just tidy: WASAPI's underlying COM objects are apartment-affine
    // to the thread that created them, and the old per-track-thread design
    // meant track 2's play() call -- which starts by tearing down track 1's
    // device -- ran on a *different* OS thread than the one that created
    // that device. That mismatch is exactly why playback worked once and
    // then silently stopped starting on every subsequent switch. Routing
    // every device operation through one fixed thread removes the mismatch
    // outright, on every platform (Linux/PulseAudio never had this
    // constraint, but there's no downside to the safer design there either).
    //
    // player_mutex_ guards every call into player_ from either this worker
    // thread or the main thread (seek/volume/pause hotkeys, the shutdown
    // path's player_.stop()) -- Player's public methods were never
    // documented as safe to call concurrently from two threads, and with a
    // long-lived worker thread now genuinely overlapping the main loop for
    // the whole session (instead of a short-lived ad-hoc thread that mostly
    // wasn't), that latent race needed closing rather than just getting
    // more likely to bite.
    std::mutex player_mutex_;
    std::thread device_worker_thread_;
    std::mutex device_request_mutex_;
    std::condition_variable device_request_cv_;
    std::atomic<int> device_gen_{0}; // incremented each launch; guards against a stale request read racing a newer post
    // Generation of the device handoff currently in flight: set the moment a
    // request is posted, cleared by the worker once player_.play() has
    // actually swapped the new track in (0 = none in flight). While this is
    // nonzero the main loop must NOT act on player_.finished(): until play()
    // runs, the PREVIOUS pcm is still the installed one, so a track that
    // already ended keeps re-latching finished_ from the audio callback every
    // few milliseconds. clear_finished() in poll_pending_load() erases it
    // once, but the old device immediately set it again -- which made a freshly
    // loaded track flip straight back to "no track loaded" (advance_track() ->
    // Stop mode -> has_track_ = false) a frame later, while its own audio
    // started a few hundred ms after that. Pressing play again "fixed" it only
    // because by then the installed pcm was the *playing* track, which can't
    // latch the flag. This holds the stale flag off for the whole handoff.
    std::atomic<int> device_play_pending_gen_{0};
    bool device_worker_stop_ = false;
    bool device_request_ready_ = false;
    struct DevicePlayRequest {
        std::shared_ptr<StreamingPcm> pcm;
        int volume = 70;
        double start_sec = 0.0;
        int generation = 0;
    };
    DevicePlayRequest device_request_;
    void device_worker_loop();       // body of device_worker_thread_, runs for the app's whole session
    void start_device_worker();      // called once, from the constructor
    void stop_device_worker();       // called once, from run()'s shutdown, before joining device_worker_thread_
    void launch_device_play_async();

    PendingLoad pending_load_;
    void launch_load_async(fs::path local_path, std::string title, std::string artist,
                            std::string location_label, bool is_local, std::string video_id);
    void poll_pending_load();
    static void write_load_timing_log(const std::string& title, bool is_local, double t_resolve,
                                       double t_probe, double t_total, const std::string& error);

    // --- deferred mini-waveform pass ---
    std::mutex waveform_mutex_;
    std::atomic<bool> waveform_pending_ready_{false};
    std::atomic<int> waveform_epoch_{0}; // incremented on each recompute; stale threads discard their result
    std::vector<float> pending_waveform_envelope_;
    void poll_pending_waveform();

    // --- async online search ---
    std::thread search_thread_;
    std::mutex search_mutex_;
    std::atomic<bool> search_ready_{false};
    std::atomic<bool> search_in_progress_{false};
    std::vector<OnlineResult> pending_search_results_;
    void launch_search_async(const std::string& query, int source = 0);   // 0 YouTube, 1 SoundCloud, 2 Bandcamp
    int last_online_source_ = 0;   // the source of the last online search (the search box shows /s:, /sc: or /b:)
    void poll_pending_search();

    // --- settings panel (6 tabs: Colors, On/Off, Animation, Paths, Reference, About App) ---
    // Rendering uses absolute cursor positioning (\x1b[y;xH) rather than
    // building padded strings line by line -- each field goes exactly
    // where it's told regardless of what else is on that row, which is
    // what actually fixes the truncation-corrupts-everything fragility
    // class of bug (a mis-sized pad on one row used to bleed into
    // whatever the next escape code was).
    Settings settings_;
    static constexpr int kSettingsTabCount = 6; // Colors, On/Off, Animation, Paths, Reference, About App
    int settings_tab_ = 0;
    int settings_row_ = 0;   // resets to 0 on every tab switch
    int settings_col_ = 0;   // 0 or 1 -- only the Colors tab has 2-cell rows
    // REFERENCE tab: the last 5 key changes can be undone with Ctrl+Shift+U. An entry is the hotkey map from before the
    // change and the selectable row it concerned (0 = every key was reset; undoing puts the cursor on the first key that came back).
    struct RefUndo { std::unordered_map<std::string, std::string> hotkeys; int row = 0; };
    std::vector<RefUndo> ref_undo_;
    void ref_undo_push(int row);
    void ref_undo_pop();
    void ref_reset_keys(int row);                      // row 0: every key, else that one row (1-based like settings_row_)
    std::string color_edit_buffer_;      // live text while mode_==ColorEdit

    // --- caret / selection for the single-line text fields ----------------
    // Two BYTE offsets into whichever field is being edited right now:
    // edit_caret_ is where the next typed byte lands, edit_anchor_ is the
    // other end of the selection (equal to the caret when there is none, so
    // `caret != anchor` IS the selection test). Byte offsets, but every
    // movement is quantised to UTF-8 codepoint boundaries (see le_*() in
    // app.cpp), so a multi-byte character is never split in half.
    //
    // The pair is shared by every editor in the app, one at a time:
    // Mode::ColorEdit's buffer, the meta editor's field editor, and the
    // search/filter boxes (the main UI's "/", the meta editor's Search line,
    // the playlist editor's name/library boxes and the playlist list's
    // search). edit_owner_ tags the string the offsets were last clamped
    // against, so switching to another field -- or another row, or the 'r'
    // resort reordering the list -- retargets the caret instead of leaving
    // it pointing into a different string. edit_focus() (app.cpp) is the one
    // place that does the retarget.
    size_t edit_caret_ = 0;
    size_t edit_anchor_ = 0;
    std::string edit_owner_;
    void edit_focus(const std::string& owner, const std::string& text);
    // "Is this text box in use?" -- the rule that lets plain Left/Right switch
    // the tabs of the playlist editor and the meta editor while still being
    // the caret keys of their text boxes. A box becomes engaged with the first
    // key typed / edited into it (text_engage(), with the same owner name
    // edit_focus() uses); from then on Left/Right move the caret. ESC, TAB,
    // ENTER or a tab switch disengage it, and Left/Right switch tabs again.
    std::string engaged_owner_;
    bool text_engaged(const std::string& owner) const { return !owner.empty() && engaged_owner_ == owner; }
    void text_engage(const std::string& owner) { engaged_owner_ = owner; }
    void text_disengage() { engaged_owner_.clear(); }
    // Returns the current value of (tab, row, col) as plain text, for
    // display and as the starting buffer when editing.
    // Returns a pointer to the color field for (row, col) on the Colors
    // tab (tab 0), or nullptr if that row/col isn't a real cell.
    std::string* color_field_ptr(int row, int col);
    std::string settings_get_value(int row, int col) const;
    // Commits color_edit_buffer_ into (settings_tab_, settings_row_,
    // settings_col_). Colors are clamped/validated as a 0-255 code;
    // everything else is stored close to verbatim.
    void settings_commit_edit();
    void settings_commit_edit_impl();
    std::string settings_note_;        // what enforce_visual_switches() changed on its own after the last edit of a setting
    void lyrics_view_nudge();           // fetch the lyrics of the playing track (the lyrics view has just been switched on)

    // --- colour themes (Colors tab, last row: THEME | CREATE THEME) ---------
    // theme_ui_: 0 = the plain Settings screen | 1 = pick a theme to apply | 2 = the CREATE THEME overlay |
    // 3 = pick a template from inside the creator. All of it lives inside Mode::Settings (text editing inside
    // the creator uses Mode::ColorEdit with theme_ui_ still set).
    int theme_ui_ = 0;
    bool theme_peek_ = false, peek_busy_ = false;   // SHIFT+p in the theme picker: a still copy of the main screen in the highlighted theme
    std::string peek_frame_;
    int peek_sel_ = -1, peek_cols_ = 0, peek_rows_ = 0;
    std::vector<themes::Theme> theme_list_;
    int theme_sel_ = 0;               // picker cursor
    mutable int theme_page_ = 20;     // picker: rows on screen, set while drawing (SHIFT+up / down move by a page)
    Settings theme_draft_;            // creator: the colours being edited (a copy; settings_ is untouched until saved)
    themes::Theme theme_base_;        // creator: the template whose other keys (the radio's) the new theme keeps
    std::string theme_name_;          // creator: name of the new theme (25 characters at most)
    int theme_row_ = -1;              // creator: -1 = the NAME / TEMPLATE row, else the Colors row
    int theme_col_ = 0;               // creator: 0/1 -- name|button on the top row, FG|BG on a colour row
    int theme_current_index() const;  // index of the theme settings_ matches exactly, or -1 (custom)
    void theme_open_picker();
    void theme_open_creator(bool edit_current = false);
    void theme_try_save();
    void theme_save_new();
    void theme_delete_selected();
    // SHIFT+e / SHIFT+i on the Colors tab: theme_ui_ 4 = export the current colours as a theme file, 5 = import theme files.
    std::string theme_io_name_;       // export: the theme's name
    std::string theme_io_dir_;        // export / import: the folder
    int theme_io_row_ = 0;            // export: 0 name, 1 folder | import: 0 folder, 1 file list
    int theme_io_sel_ = 0;            // import: the hovered file
    std::vector<std::filesystem::path> theme_io_files_;
    void theme_open_export();
    void theme_open_import();
    void theme_io_rescan();
    void theme_import_files(const std::vector<std::filesystem::path>& files);
    void handle_theme_key(int key);
    void build_theme_screen(std::ostringstream& frame, int W, int player_h) const;
    // Left/Right quick-cycle for rows with a fixed set of options (bools,
    // enums). No-op for rows that don't have one (colors, hotkeys) --
    // those are Enter-to-type only.
    void settings_cycle(int dir);
    std::vector<std::string> settings_options_for(int tab, int row) const;
    int settings_max_row() const; // last valid row index for the current tab
    std::vector<std::string> settings_anima_note(int W) const;   // the note at the bottom of the ANIMA/VIZ tab
    int settings_tab_first_line(int W, int visible) const;       // first layout line shown on tab 1 / 2 (scrolling)
    // (main_frame_height() was removed -- see the comment where it used to
    // live in app.cpp, right before build_settings_screen(). Every overlay
    // now sizes off term_rows_ directly instead.)
    void build_settings_screen(std::ostringstream& frame, int W, int player_h) const;
    void handle_settings_key(int key);

    // --- PATHS tab (tab 3) --------------------------------------------------
    // The LOCAL PATH, DOWNLOAD PATH and PLAYLIST PATH sections (they used
    // to live on the ON/OFF tab, which is now a plain list of toggles). Two
    // of them are editable path lists, each introduced by a section header
    // and ending in a "+ new path" row.
    // Headers are painted but never selectable, so this struct maps the
    // flat selectable index the arrow keys walk (settings_row_) onto the
    // display row actually drawn on screen. Both the renderer and the
    // ColorEdit cursor placement go through it (via path_display_row()), so
    // they can never disagree on where a row landed. The selectable rows
    // are numbered 0 .. path_row_count()-1.
    struct PathRow {
        enum class Kind { Path, AddPath, Header, Note };
        Kind kind = Kind::Path;
        int sel = -1;              // selectable index, -1 for headers (unselectable)
        int path_index = -1;       // Path: index inside the owning vector
        bool playlist_path = false; // Path/AddPath: true = playlist paths, false = local music paths
        // Path: the single DOWNLOAD PATH field. It reads/writes
        // settings_.download_folder instead of either vector, hence the
        // path_index = -1 that never reaches them -- every consumer
        // switches on this flag first.
        bool download_folder = false;
        bool history_folder = false;   // Path: the single HISTORY PATH field (settings_.history_path)
        bool export_folder = false;    // Path: the single PLAYLIST EXPORT PATH field (settings_.playlist_export_path)
        bool conversion_folder = false; // Path: the single CONVERSION EXPORT PATH field (settings_.conversion_export_path)
        const char* label = "";    // Header: section title; AddPath: "+ new path"
    };
    // Every display row of the path section, in paint order, with `sel`
    // assigned over the selectable ones. Always at least one path row per
    // list even while the underlying vector is empty (an unset path is
    // shown as an empty field rather than as no field at all).
    std::vector<PathRow> build_path_rows() const;
    // The row backing the given selectable row; its `sel` field is -1 when
    // the index is out of range.
    PathRow path_row(int selectable_row) const;
    // Number of selectable path rows (everything but the headers).
    int path_row_count() const;
    // Maps a selectable PATHS row to the display line it is drawn on,
    // headers and spacers included.
    int path_display_row(int selectable_row) const;
    int path_display_total() const;
    // True when the given selectable PATHS row edits free text -- a path
    // row rather than a "+ new path" button -- since those need far more
    // characters than a hotkey field does. Drives the edit-buffer length
    // limit.
    bool path_row_is_text(int selectable_row) const;
    // Maps a selectable REFERENCE row (hotkey or font-map row) to the
    // display line it is drawn on, headers and spacers included.
    int ref_display_row(int selectable_row) const;

    // Title shown for `path` in the (search-)lists. Honours
    // settings_.meta_only: metadata-only mode substitutes the embedded
    // title tag for the filename stem as soon as one has been resolved
    // for that file, and falls back to the filename when there is no tag
    // (or none resolved yet), so an untagged library still renders rows.
    std::string list_row_title(const fs::path& path, const std::string& filename_title) const;
    // Playlist LIBRARY/TRACKS panels have no separate Artist/Duration
    // columns and no "Show meta data only" toggle to pick one representation
    // over the other, so unlike list_row_title() above this always shows
    // both: the filename, plus the embedded title tag appended after it
    // once/if that tag has been resolved and actually differs from the
    // filename. Untagged or not-yet-resolved files just show the filename,
    // same as before.
    std::string playlist_row_label(const fs::path& path, const std::string& filename_title) const;
    // Shared marquee-scroll math used by the LOCAL AUDIO FILES pane and the
    // playlist LIBRARY/TRACKS panels: scrolls `text` within `width` columns
    // once it no longer fits, restarting from the beginning whenever
    // `row_idx` (the row currently being drawn) differs from whatever
    // `tracked_idx` last recorded -- so switching the hovered row always
    // resumes the animation from the start rather than mid-scroll. Returns
    // `text` truncated/padded to `width` unmodified when it already fits.
    std::string marquee_or_truncate(const std::string& text, int width, int row_idx,
                                     int& tracked_idx, std::chrono::steady_clock::time_point& since) const;

    // Re-runs LocalSource::scan() over the current
    // settings_.local_music_paths and rebuilds local_view_ -- called when
    // a path is edited in the PATHS tab, so a path change takes effect
    // immediately instead of only on the next launch (config.txt's own
    // comment used to say "there's no live rescan"; there is now).
    void rescan_library();

    // Max row count per tab (set in build_settings_screen)


    // --- hotkey support ---
    // Resolves a key code from poll_key() to the hotkey action name.
    // Returns empty string if no match.
    std::string resolve_hotkey_action(int key) const;
    // Returns the key code that a hotkey string maps to for poll_key().
    static int hotkey_string_to_key(const std::string& s);

    // --- helpers ---
    void refresh_local_view();
    void update_live_search_preview();
    std::vector<LocalTrack> filter_and_rank_local(const std::string& query) const;
    void apply_local_sort(std::vector<LocalTrack>& tracks) const;
    std::vector<LocalTrack> filter_and_rank_local_view(const std::string& query) const; // + folder filter
    void resort_local_view_keep_selection(); // after Shift+N: re-sort, cursor stays on the same track
    static const char* sort_mode_name(int mode, bool meta_only);
    void submit_search();
    void start_local_track(const LocalTrack& track);
    void start_online_track(const OnlineResult& result);
    void play_selected();
    void play_relative(int delta);
    void play_relative_random();
    void advance_track();
    // Where the currently-playing track sits within *this list source's*
    // current view (local_view_ or online_view_, whichever list_source_
    // is showing), by identity match (path for local, video_id for
    // online) rather than by whatever row happens to be hovered. -1 if
    // nothing's playing, or what's playing came from a different source
    // than the one currently displayed (e.g. playing local while
    // browsing online results) -- there's no meaningful "relative to
    // current" position in that case. This is what play_relative() uses
    // instead of the hover cursor, so "next" always means "next after
    // what's actually playing", not "next after wherever you happen to
    // be looking".
    int current_track_list_index() const;
    // Takes the next item to play from queue_ -- it leaves the queue, or goes
    // to the back of it while the queue is locked -- honoring the current
    // play_mode: Shuffle picks a random queue item rather than strictly FIFO
    // order, Queue-then-stop (4) plays a locked queue through once (see
    // QueueItem::played). Shared by advance_track()
    // (auto-advance on finish) and the manual "n" key (explicit skip),
    // so both respect the queue exactly the same way. Caller must check
    // !queue_.empty() first.
    void play_next_from_queue(bool random = false);   // random: any item of the queue (#), otherwise its order
    void play_queue_item(int idx);   // start the queue item at idx (see play_next_from_queue)
    // Single letter for the mode-indicator button after the search bar:
    // L=list, R=repeat, S=shuffle, Q=queue then stop, O=stop (play-and-stop
    // -- not "S", that's shuffle's letter already).
    char play_mode_letter() const;
    void queue_add_selected_impl(bool at_end);
    void queue_add_selected();           // "a": hovering track as NEXT (see queue_next_run_)
    void queue_remove_last();
    void queue_remove_hovering();
    // Shift+X (HKeyClearQueue): asks "Want to clear queue?" (Mode::ClearQueue,
    // a floating Yes/No panel like Bulk Add / Retry Lyrics) before queue_clear()
    // actually empties queue_. The default choice is No.
    void queue_clear();
    int clear_queue_choice_ = 1;       // 0 = Yes, 1 = No
    static constexpr int kClearQueuePanelWidth = 40;
    std::vector<std::string> build_clear_queue_panel() const;

    // "A newer version is out" (update_check.h): a small floating panel (Mode::UpdateNotice) shortly after the start,
    // [ESC] closes it, [o] opens the release page. It waits (update_pending_) until the main screen is showing.
    bool update_pending_ = false;
    std::string update_latest_, update_current_, update_url_;
    static constexpr int kUpdatePanelWidth = 56;
    std::vector<std::string> build_update_panel() const;

    // Shift+O (HKeyOscMenu): small overlay (Mode::OsciMenu) to tune the
    // oscilloscope's afterglow / dot threshold / tail brightness live. It is
    // a small centred floating panel (draw_floating_panel()), which leaves
    // the scope on the right of the top panel visible while it is adjusted.
    // Up/Down pick a row, Left/Right change it, R resets, ESC / Shift+O close
    // (and save the values to config.txt).
    int osci_menu_row_ = 0;
    // Shift+9 (HKeyScopeWindow): the oscilloscope in its own window (src/scope_window.h). scope_window_sync() runs
    // every frame: it sends the current scope settings / colours / title and notices a window closed from its side.
    bool scope_win_on_ = false;
    // Spectrogram (src/spectrogram.h): the third lyrics-area visual (LyricViz=spectro), SHIFT+i options overlay
    // (Mode::SpectroMenu), SHIFT+u full screen. spectro_area_ = where this frame put the picture (row, col, cols, rows,
    // 0-based; row -1 = no picture), spectro_used_ = a spectrogram was drawn this frame (else the feed is switched off).
    mutable bool spectro_used_ = false;
    mutable int spectro_area_[4] = {-1, 0, 0, 0};
    SpectroGfx spectro_gfx_;
    bool spectro_full_ = false;
    // SHIFT+0: the big oscilloscope overlay (YT left, XY right). Each picture has its own scope (own phosphor buffer) fed from
    // scope_; SHIFT+a toggles the grid, SHIFT+v the axis values, SHIFT+c asks for its colour (the digits typed so far are in scope_big_color_buf_).
    bool scope_big_ = false;
    bool scope_big_color_edit_ = false;
    bool scope_metrics_open_ = false;   // SHIFT+p inside it: the window for the grid metrics (units, divisions, measurements)
    int scope_metrics_row_ = 0;
    std::string scope_big_color_buf_;
    mutable OscilloscopeVisualizer scope_big_xy_, scope_big_yt_;
    std::string build_scope_big(int W, const char* clear_prefix) const;
    std::vector<std::string> build_scope_color_panel() const;   // the small window SHIFT+c opens
    static constexpr int kScopeColorPanelWidth = 48;
    std::vector<std::string> build_scope_metrics_panel() const;   // the window SHIFT+p opens
    static constexpr int kScopeMetricsPanelWidth = 52;
    bool scope_big_key(int key);   // keys of the overlay itself; true = used
    int spectro_menu_row_ = 0;
    static constexpr int kSpectroMenuPanelWidth = 52;
    std::vector<std::string> build_spectro_menu_panel() const;
    std::string build_spectro_full(int W, const char* clear_prefix) const;
    void spectro_menu_adjust(int dir);
    size_t frame_main_len_ = std::string::npos;   // render_frame(): end of the main screen's lines (the overlays follow)
    void scope_window_toggle();
    void scope_window_sync(bool check_alive = true);
    // SHIFT+8 (HKeySpectroWindow): the spectrogram in its own window, the same way as the scope window.
    bool spectro_win_on_ = false;
    void spectro_window_toggle();
    void spectro_window_sync(bool check_alive = true);
    std::string enforce_visual_switches(int changed = -1);   // returns what it changed, for the status line
    static constexpr int kOsciMenuPanelWidth = 50; // just wide enough for the key legend
    std::vector<std::string> build_osci_menu_panel() const;
    void osci_menu_adjust(int dir);
    // Shift+V (HKeyNormMenu): the loudness normalisation overlay
    // (Mode::NormMenu), built on the same small centred floating panel as
    // the oscilloscope overlay above. Rows: Normalize (on/off), Target level
    // (LUFS) and Max boost (dB) -- the three values behind NormalizeVolume /
    // NormalizeTargetLufs / NormalizeMaxBoostDb in config.txt. Every change
    // is pushed into the player immediately (it glides to the new gain within
    // about a second), so what you adjust is what you hear; a live line shows
    // the playing track's measured loudness and the gain being applied.
    // Up/Down pick a row, Left/Right change it (on the first row they flip
    // on/off), SPACE or the toggle key (HKeyToggleNormalize) switches
    // normalisation on/off from any row, R resets target and boost to their
    // defaults, ESC / Shift+V close (and save the values to config.txt).
    int norm_menu_row_ = 0;
    static constexpr int kNormMenuPanelWidth = 54; // wide enough for the key legend in the bottom border
    std::vector<std::string> build_norm_menu_panel() const;
    void norm_menu_adjust(int dir);
    void norm_apply();                 // pushes settings_ -> player_
    // Shift+Z (HKeySleepTimer): the sleep timer overlay (Mode::SleepTimer), a
    // small floating panel over the live playback UI. Pick 15 / 30 / 60 / 90 /
    // 120 minutes (playback is PAUSED when it runs out, so it can be resumed),
    // "stop after current song", or Off. Up/Down pick a row, ENTER sets it and
    // closes, ESC / Shift+Z close without changing anything. Playback keeps
    // running underneath. Not persisted (a sleep timer must not survive a restart).
    //
    // Interplay with the Stop play mode (settings_.play_mode == 3): "stop after
    // current song" does NOT touch play_mode. It is a one-shot flag consumed in
    // advance_track(), checked BEFORE the repeat / stop / queue logic, so it wins
    // over Repeat and over the queue, and the play mode is exactly what it was
    // afterwards. With Stop mode already on it is simply redundant. The minute
    // timers and the one-shot flag are mutually exclusive (setting one clears the
    // other); a minute timer never changes the play mode either, it only pauses.
    int sleep_menu_row_ = 0;
    int sleep_timer_minutes_ = 0;                       // the minute choice that is running (0 = none)
    bool sleep_timer_active_ = false;
    std::chrono::steady_clock::time_point sleep_timer_deadline_{};
    bool sleep_stop_after_track_ = false;               // one-shot: end playback when the current song ends
    static constexpr int kSleepTimerPanelWidth = 44;
    std::vector<std::string> build_sleep_timer_panel() const;
    void sleep_timer_apply(int row);                    // row of the menu: 0..4 minutes, 5 stop after song, 6 fade out (toggle), 7 off
    void sleep_timer_cancel();
    void sleep_timer_tick();                            // once per frame
    std::string sleep_timer_label() const;              // "" when nothing is armed, else e.g. "SLEEP 24:10"
    // ^ (HKeyCreator): the creator overlay (Mode::Creator). Turns a text or a picture into a WAV file -- either
    // oscilloscope music (left = x, right = y, the XY scope draws the picture) or a spectrogram sound (the picture is
    // the spectrogram, played as notes of a scale) -- saves it in <music folder>/creator and plays it. The work is
    // done by creator.h; this is only the panel. The settings of the panel are kept until Mousiki is closed.
    static constexpr int kCreatorPanelWidth = 62;
    int creator_row_ = 0;                               // index into creator_rows()
    int creator_mode_ = 0;                              // 0 text to sound, 1 microphone
    bool creator_grid_ = true;                          // the grid of the scope previews (text to scope and microphone)
    int creator_live_view_ = 0;                         // microphone view: 0 spectrogram (default), 1 oscilloscope XY, 2 oscilloscope YT
    std::vector<int> creator_rows() const;              // the row ids on the screen for the current mode
    bool creator_on_text_field() const;
    std::string creator_text_ = "MOUSIKI";
    bool creator_list_open_ = false;                    // the list of created files is shown instead of the rows
    std::vector<fs::path> creator_files_;
    int creator_list_sel_ = 0;
    int creator_target_ = 0;                            // 0 oscilloscope (XY), 1 spectrogram
    int creator_len_ = 1;                               // index into the length list
    int creator_invert_ = 0;                            // 0 auto, 1 on, 2 off
    int creator_fps_ = 1;                               // scope: frame rate list index
    int creator_motion_ = 0;                            // scope: Motion
    int creator_detail_ = 1;                            // scope: 0 low .. 2 high
    int creator_scale_ = 0;                             // spectrogram: Scale
    int creator_octaves_ = 5;                           // spectrogram: 3..6
    int creator_reverb_ = 1;                            // spectrogram: 0 off, 1 light, 2 hall
    std::string creator_status_;
    std::vector<std::string> creator_preview_;          // braille lines of the current picture
    std::string creator_loaded_key_;                    // what creator_preview_ / creator_gray_ were made from
    std::shared_ptr<void> creator_gray_;                // a creator::Gray (kept opaque so app.h does not need creator.h)
    std::vector<std::string> build_creator_panel() const;
    // The preview that opens after "Create": a full-screen picture of what the file draws (the XY scope in music mode, or the
    // spectrogram) that loops until ESC. It has its own scope / spectrogram and its own settings (creator_spectro_set_, the
    // parameters in build_creator_preview()), so the SHIFT+o / SHIFT+i settings are neither used nor changed. The pictures
    // are made from the samples of the file itself, in step with the player's position.
    bool creator_show_ = false;
    int creator_preview_kind_ = 0;                      // 0 = oscilloscope (XY), 1 = spectrogram, 2 = oscilloscope (YT, microphone only)
    // The microphone preview: the same big overlay, but the pictures are made from the live microphone and ENTER records it.
    bool creator_live_ = false;
    std::unique_ptr<MicCapture> creator_mic_;
    bool creator_rec_on_ = false;
    std::vector<float> creator_rec_;                    // interleaved L/R of the running recording
    int creator_rec_rate_ = 44100;
    void creator_live_start();
    void creator_live_tick();
    void creator_record_toggle();
    void creator_record_save();
    std::shared_ptr<creator::Stereo> creator_audio_;
    LocalTrack creator_track_;
    size_t creator_fed_ = 0;                            // frames of creator_audio_ given to the scope / spectrogram
    double creator_last_pos_ = 0.0;
    mutable OscilloscopeVisualizer creator_scope_;
    mutable SpectroAnalyzer creator_spectro_;
    mutable SpectroGfx creator_spectro_gfx_;
    SpectroSettings creator_spectro_set_;
    void creator_preview_open();
    bool creator_prev_valid_ = false;                   // a song was playing before the creator started its file: it comes back when the preview closes
    LocalTrack creator_prev_;
    bool creator_resume_ = false;
    void creator_remember_previous();
    void creator_resume_tick();                         // once per frame: restarts that song as soon as the player is free
    void creator_preview_close();                       // stops the playback and the loop
    void creator_preview_loop();                        // the file ended: play it again
    void creator_preview_tick();                        // once per frame: feeds the pictures
    void creator_preview_key(int key);
    std::string build_creator_preview(int W, const char* clear_prefix) const;
    void creator_key(int key);
    bool creator_load();                                // (re)loads the picture when the source changed; false = nothing to work with
    void creator_render_and_play();
    fs::path creator_dir() const;                       // <first music folder>/creator
    bool creator_dir_prepare();                         // creates it and marks it so the library scan skips it; true = it was not marked before
    void creator_list_refresh();
    void creator_open_file(const fs::path& wav);        // plays a created file and opens its preview
    // Alt+L (kKeyAltL, fixed key): the lyrics timing overlay (Mode::LyricsEdit),
    // a small floating panel over the live playback UI for tracks whose lyrics
    // run early or late. The offset ("delay", seconds, + = lyrics later) lives in
    // lyrics_result_.delay and is applied live by render (lyrics time = elapsed -
    // delay), so the lyrics behind / inside the panel move while it is adjusted.
    // Left/Right = -/+ 0.1 s, Down/Up = -/+ 0.5 s, R = back to 0, ENTER / S =
    // save into the track's .lrc ("[offset:...]" tag, so it is remembered and
    // read back with the lyrics), ESC / Alt+L = cancel (the value from before the
    // overlay opened comes back). Opens only while synced lyrics are loaded.
    double lyrics_edit_orig_delay_ = 0.0;               // delay when the overlay opened (ESC restores it)
    // Alt+L works inside the karaoke overlay too: the timing is easiest to judge
    // while the big lyrics are on screen. Set when the overlay was opened from
    // there, so closing it (save or cancel) goes back to karaoke instead of the
    // main screen.
    bool lyrics_edit_from_karaoke_ = false;
    void karaoke_ensure_lyrics();   // the karaoke overlay fetches the lyrics itself when the engine is off
    // SHIFT+w (HKeyWizard): the Mousiki Plus Help Wizard (Mode::Wizard), a full-screen help book. src/wizard.{h,cpp}
    // holds the text, the page / part / contents state and the key handling; this class only draws it (the radio draws
    // the same pages the same way). It opens from the main screen and from every menu / overlay that does not take
    // typed text (wizard_allowed_over_mode()), and closing it returns to the mode it was opened from. The first
    // close also sets Settings' wizard_seen and writes config.txt, so the first-start greeting shows only once.
    wizard::State wizard_st_;
    Mode wizard_return_mode_ = Mode::Browse;
    double wizard_angle_ = 0.0;       // the first page's disk, spun at 0.10 whatever is playing
    void wizard_open();
    void wizard_close();
    bool wizard_allowed_over_mode() const;   // false while a text field has the keyboard
    void build_wizard_screen(std::ostringstream& frame, int W);
    // CTRL+L (kKeyCtrlL, fixed key): READ LYRICS (Mode::ReadLyrics), a full-screen reader for the saved lyrics of the
    // playing (else the hovered) track. The top pane is ONE scrollable line with the lyrics files of the track
    // (embedded / fetched / older copies, LEFT / RIGHT choose), below it the plain lyrics without timestamps
    // (UP / DOWN / SPACE / b / HOME / END scroll). `t` translates non-English lyrics into English
    // (scripts/translate_lyrics.py, in the background): the translation is saved in lyrics/translations/ and shown in
    // "( )" right below each original line; `t` again hides it, `r` translates anew.
    fs::path rd_track_;                                 // track the files belong to
    std::string rd_title_;                              // "Artist - Title" for the pane's border
    std::vector<LyricsVersion> rd_versions_;
    int rd_sel_ = 0;                                    // chosen file
    int rd_first_ = 0;                                  // first file shown on the one-line chooser (scrolls with the choice)
    std::vector<std::string> rd_lines_;                 // plain lyrics of the chosen file
    std::vector<TranslationInfo> rd_tlist_;             // the translations saved for the chosen file (TRANSLATION pane)
    int rd_tsel_ = 0;                                   // the one chosen / shown
    int rd_tfirst_ = 0;                                 // first entry shown on the one-line chooser
    int rd_pane_ = 0;                                   // pane with the keys: 0 READ LYRICS, 1 TRANSLATION (TAB)
    bool rd_confirm_del_ = false;                       // "delete this translation?" is asked
    std::vector<std::string> rd_trans_;                 // the chosen translation, one entry per line
    std::string rd_from_;                               // language the translation came from ("de")
    std::string rd_to_, rd_svc_;                        // language it is in ("en") and the service that made it ("google")
    bool rd_opts_open_ = false;                         // the translate overlay ([t]): service, translate into, lyrics are in
    int rd_opts_row_ = 0;
    bool rd_opts_dirty_ = false;
    std::string rd_opts_warn_;                          // warning inside the translate overlay
    std::vector<std::string> build_readlyrics_options_panel() const;
    void readlyrics_options_key(int key);
    bool rd_show_ = false;                              // translation visible
    bool rd_same_ = false;                              // the lyrics already are in the target language
    bool rd_busy_ = false;                              // a translation is running (it goes on when the overlay is closed)
    // The running translation: what it was asked for and which lyrics it belongs to.
    std::string rd_job_to_, rd_job_svc_;
    fs::path rd_job_track_;
    LyricsVersion rd_job_version_;
    int rd_scroll_ = 0;
    std::string rd_status_;
    bool rd_status_warn_ = false;                       // the status is a warning / error (drawn in red)
    std::mutex rd_mutex_;
    bool rd_result_ready_ = false;
    LyricsTranslation rd_result_;
    fs::path translate_script_;                         // scripts/translate_lyrics.py

    void readlyrics_open();
    void readlyrics_close();
    void readlyrics_load_version();                     // reads the chosen file (and its saved translation)
    void readlyrics_refresh_translations(const std::string& prefer_to);   // re-reads the saved translations of the chosen file
    void readlyrics_select_translation(int index);
    void readlyrics_toggle_translations();              // SHIFT+t: show / next translation / hide
    void readlyrics_delete_translation();
    void readlyrics_translate();                        // ENTER in the translate overlay
    void readlyrics_poll();                             // once per frame: takes a finished translation over
    void handle_readlyrics_key(int key);
    // CTRL+s in READ LYRICS: a search over EVERY lyrics file (by file name) in the lyrics folders of the library, as an
    // alternative to the lyrics of the current track. ENTER shows the chosen file in the reader (it is read like a
    // track's own lyrics, translations included); the reader is opened for the playing track again with CTRL+L.
    struct LyricsFileHit { fs::path path; std::string name; std::string lname; std::string folder; };
    bool rd_search_open_ = false;
    std::string rd_search_q_;
    std::vector<LyricsFileHit> rd_search_all_;          // every lyrics file found when the search was opened
    std::vector<int> rd_search_view_;                   // indices into rd_search_all_ that match the query
    size_t rd_search_dirs_ = 0;                         // lyrics folders that were searched
    int rd_search_sel_ = 0;
    int rd_search_first_ = 0;
    void readlyrics_search_open();
    void readlyrics_search_rebuild();
    void readlyrics_search_key(int key);
    void readlyrics_show_file(const LyricsFileHit& hit);
    std::vector<std::string> build_readlyrics_search_panel() const;
    void build_readlyrics_screen(std::ostringstream& frame, int W, int target_height);
    fs::path lyrics_path_;                              // track path the current lyrics belong to (sidecar location)
    static constexpr int kLyricsEditPanelWidth = 60;
    void lyrics_edit_open();
    void lyrics_edit_close();                           // back to karaoke or the main screen, see lyrics_edit_from_karaoke_
    void lyrics_edit_adjust(double step);
    std::vector<std::string> build_lyrics_edit_panel() const;
    // Shift+E (HKeyEqualizer): the equaliser overlay (Mode::Equalizer), a
    // floating panel over the live playback UI. Ten vertical sliders
    // (31 Hz .. 16 kHz, +/-12 dB), a preset line and an on/off state. Left/
    // Right pick a band, Up/Down change it by 1 dB, ,/. (or TAB) step through
    // the presets (the built-in ones first, then the user's own), E or SPACE
    // switches the EQ on/off, R resets to Flat, ESC / Shift+E close and save.
    // S saves the current curve as a custom preset (a name prompt takes over
    // the row under the sliders until ENTER / ESC), DEL or X deletes the
    // selected custom preset after a second press. Custom presets live in
    // settings_.eq_custom_presets and are written to config.txt right away.
    // Touching a band or choosing a preset also turns the EQ on, so what you
    // change is always what you hear. The values live in settings_.eq_enabled
    // / eq_gains and reach the audio thread through Player::set_equalizer().
    int eq_band_ = 0;                  // selected band 0..kEqBands-1
    int eq_last_preset_ = 0;           // unified preset index (built-ins, then custom): where the cycle continues from while the gains are Custom
    bool eq_naming_ = false;           // the "Save as:" prompt is open
    std::string eq_name_buf_;          // its text (edit_caret_/edit_anchor_ belong to it while open)
    std::string eq_status_;            // one-line feedback under the sliders, cleared by the next key
    bool eq_delete_armed_ = false;     // DEL / X pressed once on a custom preset: the next press deletes it
    static constexpr int kEqPanelWidth = 58;
    std::vector<std::string> build_eq_panel() const;
    void eq_open();
    void eq_apply();                   // pushes settings_ -> player_
    void eq_set_gain(int band, float db);
    void eq_select_preset(int dir);    // dir = +1 next, -1 previous
    int eq_preset_count() const;                    // built-in presets + custom presets
    const EqGains& eq_preset_gains(int index) const;
    std::string eq_preset_name(int index) const;
    int eq_current_preset() const;                  // unified index whose gains match the sliders, or -1 (Custom)
    void eq_begin_naming();                         // S
    void eq_commit_name();                          // ENTER in the prompt
    void eq_delete_custom(bool confirmed);          // DEL / X
    void queue_move_hovering(int dir); // dir=-1 up, +1 down
    void clamp_queue_selected();
    void queue_reveal(int idx);   // scroll the queue so that this entry is visible (after adding tracks)
    void handle_key(int key);
    // Actions shared by the hotkeys and the remote control (`mousiki --cmd ...`, src/remote_control.h).
    void act_play_pause();
    void act_volume_change(int delta);
    void act_mute(int how);          // -1 toggle, 0 unmute, 1 mute
    void act_next();
    void act_prev();
    void act_cycle_play_mode();      // m: list -> repeat -> shuffle -> stop -> queue then stop
    void act_wizard_toggle();        // SHIFT+w from the remote control: open / close the help wizard
    void sync_after_radio();         // adopt what the radio changed in config.txt meanwhile
    void remote_poll();              // commands from outside + the status file; called every frame
    void remote_app_apply();         // starts / stops the remote app's web server to match settings_
    // The remote app (src/remote_server.h, src/app_phone.cpp): the phone screen, built when a phone asks for it.
    void phone_publish();
    std::vector<std::string> build_phone_screen(int W, int H, std::vector<remote::net::Zone>& zones) const;
    double phone_angle_ = 0.0;       // the phone's disk: its own turn at speed 0.10
    remote_ui::State remote_ov_;     // Settings -> ON/OFF -> Remote App: the REMOTE APP overlay (address, QR code, port, PIN)
    void remote_overlay_draw(std::ostringstream& frame, int W) const;
    std::chrono::steady_clock::time_point phone_last_{}, phone_built_{};
    int phone_view_ = 0;             // 0 the phone's main screen, 1 its LISTS screen
    int phone_focus_ = 0;            // LISTS: the pane UP / DOWN page through (0 the local list, 1 the saved playlists)
    int phone_page_ = 0;             // LISTS: the page of the local list ...
    int phone_page2_ = 0;            // ... and of the playlists
    std::string phone_note_;         // its status line ("queued 12 titles from ...")
    std::chrono::steady_clock::time_point phone_clear_armed_{};   // CLEAR QUEUE tapped once: a second tap within 5 s clears
    std::string queue_playlist(const std::string& name);   // every title of a saved playlist into the queue; the message
    void ensure_visible_row_meta();
    void recompute_waveform_for_current_track();
    std::string render_frame(TerminalIO& term);

    // --- box drawing helpers (use configured border chars) ---
    std::string box_top(const std::string& label, int total_width, const std::string& border_ansi = "") const;
    std::string local_pane_label(int total_width) const; // LOCAL AUDIO FILES title, folder name shortened to fit
    std::string box_bottom(int total_width, const std::string& footer = "", const std::string& border_ansi = "") const;
    std::string box_line(const std::string& content, int total_width, const std::string& border_ansi = "") const;
    // box_top()/box_line() for a row whose TAIL is a text field with a caret
    // and a marked range: the field arrives already painted (it carries
    // reverse-video escapes), so these two pad/measure against `field_cols`
    // instead of running the rendered string through display_width(), which
    // counts escape bytes as columns. `prefix` stays plain text.
    std::string box_top_field(const std::string& prefix, const std::string& field, int field_cols,
                              int total_width, const std::string& border_ansi = "") const;
    std::string box_line_field(const std::string& prefix, const std::string& field, int field_cols,
                               int total_width, const std::string& border_ansi = "",
                               const std::string& text_ansi = "") const; // text_ansi: colours for prefix+field+fill

    // panel builders
    std::vector<std::string> build_metadata_panel(int width) const;
    std::vector<std::string> build_progress_panel(int width) const;
    std::vector<std::string> build_search_bar(int width) const;
    std::vector<std::string> build_list_panel(int width, int height) const;
    std::vector<std::string> build_queue_panel(int width, int height) const;
};

} // namespace muisc
