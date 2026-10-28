// Listening history (see history.cpp): the play record, the aggregated top lists and statistics, and
// the HistoryStore that keeps them on disk.
#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>

namespace muisc {

// ---------------------------------------------------------------------------
// Listening history (Mode::History, HKeyHistory = Shift+H)
//
// One record per start of a track. The store keeps them newest-first and
// persists them to ~/.cache/mousiki/history/history.json so the numbers are
// cumulative across runs -- the History tab only ever shows the newest 100,
// but "played today", "total listened" and the play counts behind Top Tracks
// / Habits are meant to mean something a month from now.
//
// Everything about a play is decided at the moment it ENDS (finish_live()),
// because that is the first point where any of it is knowable: how much was
// actually heard, and whether it was heard to the end or skipped over.
// ---------------------------------------------------------------------------

struct HistoryPlay {
    std::string id;             // aggregation key: local path, or "yt:<video id>" for streams
    std::string title;
    std::string artist;
    double len_sec = 0.0;       // length of the track itself
    double listened_sec = 0.0;  // seconds of audio actually heard (pause time never counts)
    long long started_at = 0;   // unix seconds, local clock, when this play began
    bool finished = false;      // heard to the (near) end -- see finish_live()
    std::string playlist;       // saved playlist this play was queued from ("" = not from a playlist); feeds TOP PLAYLISTS
};

// One aggregated title on the "Top Tracks" tab.
struct HistoryTopRow {
    std::string id;
    std::string title;
    double len_sec = 0.0;
    int plays = 0;
    double listened_sec = 0.0;
    std::string artist;         // newest known artist of this title (for "add top tracks to queue")
};

// Everything the "Habits" tab shows, derived from the stored plays.
struct HistoryStats {
    int plays = 0;
    int finished = 0;
    int skipped = 0;            // plays that did NOT reach the end (incl. the app closing mid-track)
    int replays = 0;            // every play of a title beyond its first one
    double completion = 0.0;    // finished / plays, 0..1
    int sessions = 0;           // runs of plays split by a >30 min silence
    double avg_session_sec = 0.0;    // wall-clock span of one sitting
    double tracks_per_session = 0.0;
    double today_sec = 0.0;     // listened this calendar day
    double avg_day_sec = 0.0;   // listened per day that had any music at all
    double total_sec = 0.0;
    int days = 0;               // distinct calendar days with music
    std::array<double, 24> hours{};     // seconds listened per hour of the day (lifetime)
    std::array<double, 7> weekdays{};   // seconds listened per weekday, 0 = Monday (lifetime, cumulative)
    // How often each weekday occurred in the calendar from the first day with music up to today (both included), and
    // weekdays[d] / that count: the average time listened on such a weekday -- days without music count as 0.
    std::array<int, 7> weekday_count{};
    std::array<double, 7> weekday_avg{};
    int busiest_hour = -1;
};

// ---------------------------------------------------------------------------
// Lifetime totals. The store only keeps the newest kMaxPlays individual
// records (that is what keeps history.json small -- it is rewritten after every
// track), but nothing counted may ever get lost with them: a record that falls
// out of the window is FOLDED into this archive first. The archive grows with
// the number of distinct titles and distinct days -- not with the number of
// plays -- so a few hundred KB cover years of listening.
// ---------------------------------------------------------------------------
struct HistoryArchiveTitle {
    std::string title, artist;
    double len_sec = 0.0;
    int plays = 0;
    double listened_sec = 0.0;
    // For the smart lists (ADD SMART HISTORY TO QUEUE): plays per calendar day
    // ("YYYY-MM-DD" -> count; "top of the week / month / ...") and per time of
    // day (history_tod_bucket()). Only filled for plays folded by v3.1.0+ --
    // older archives simply have no dates, so those plays count for the
    // all-time lists only.
    std::map<std::string, int> day_plays;
    std::array<int, 4> tod{};
};

// Per saved playlist: how often one of its tracks was played (TOP PLAYLISTS tab).
struct HistoryArchivePlaylist {
    int plays = 0;
    double listened_sec = 0.0;
    std::map<std::string, int> day_plays;   // "YYYY-MM-DD" -> plays, for the week / month / quarter / year lists
};

struct HistoryArchive {
    long long plays = 0;        // every folded play ...
    long long finished = 0;     // ... and how many of them were heard to the end
    double listened_sec = 0.0;
    std::map<std::string, HistoryArchiveTitle> titles;  // per title: play count + time (Top Tracks, Replays)
    std::map<std::string, HistoryArchivePlaylist> playlists;  // per saved playlist (Top Playlists)
    std::map<std::string, double> per_day;              // "YYYY-MM-DD" -> seconds (Days with music, averages)
    std::array<double, 24> hours{};                     // seconds per hour of the day (the hour a play started in)
    std::array<double, 7> weekdays{};                   // seconds per weekday, 0 = Monday
    // Sessions, kept exactly: closed ones as count + summed length, plus the
    // one still "open" at the oldest end of the live window, which the window's
    // oldest plays may still continue (see history_stats()).
    int sessions_closed = 0;
    double session_sum_sec = 0.0;
    bool session_open = false;
    long long sess_start = 0, sess_end = 0;

    // Adds one record. Must be called oldest -> newest.
    void fold(const HistoryPlay& p);
    bool empty() const { return plays == 0; }
};

class HistoryStore {
public:
    // Reads history.json if it exists. A missing or corrupt file is not an
    // error: history is a convenience, never something that should stop the
    // app from starting.
    void load();
    // Folder that holds history.json ("" = the default, ~/.cache/mousiki/history). Only sets it: call load() / save() after.
    void set_dir(const std::string& dir_utf8) { dir_ = dir_utf8; }
    void set_file_name(const std::string& name) { file_name_ = name; }   // "history.json" unless set (the audio book mode keeps reading_history.json)
    const std::string& file_name() const { return file_name_; }
    const std::string& dir() const { return dir_; }
    bool file_exists() const;
    std::string effective_dir() const;   // the folder in use (the default when none is set)
    // Written after every finished play and again at shutdown.
    void save() const;

    // Starts a new record at the front of the list (newest-first) and marks
    // it as the live one; the previously live record, if any, is left as it
    // is -- callers end it first.
    void begin_play(const HistoryPlay& p);
    // Adds elapsed seconds to the live record. A no-op when nothing is live,
    // so the main loop can call it unconditionally.
    void add_listened(double sec);
    // Closes the live record: `completed` is the caller's judgement of "heard
    // to the end" (the player's finished flag, or >=90% of the track).
    void finish_live(bool completed);

    bool live() const { return live_index_ >= 0; }
    double live_listened() const;
    double live_len() const;

    // Newest first. Includes the still-playing record, so the History tab
    // shows the current track too.
    const std::vector<HistoryPlay>& plays() const { return plays_; }

    // Accessors for the current play's identity (used to build the
    // "completed" verdict when a track is handed over to its successor).
    const HistoryPlay* live_play() const;

    // Totals of everything that has already left the window of individual records.
    const HistoryArchive& archive() const { return archive_; }

private:
    void trim_overflow();             // folds the oldest records into archive_ while over the cap
    std::string dir_;                 // history folder override (UTF-8), "" = default
    std::string file_name_ = "history.json";
    std::string file_utf8() const;
    HistoryArchive archive_;
    std::vector<HistoryPlay> plays_;  // newest first, capped
    int live_index_ = -1;             // index of the in-progress record, -1 when none
};

// Aggregation. `most_first` is the Top Tracks default (play count descending);
// false is the `r` key's "least played on top" alternative.
// Both take the store's archive so the numbers are lifetime totals, not just
// "what is still in the window of individual records".
std::vector<HistoryTopRow> history_top(const std::vector<HistoryPlay>& plays, bool most_first,
                                       const HistoryArchive* archive = nullptr);
HistoryStats history_stats(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive = nullptr);

// --- Smart lists (the history overlay's ADD SMART HISTORY TO QUEUE pane) ---
// Time of day of a play start: 0 = morning (7-11), 1 = day (11-18),
// 2 = evening (18-22), 3 = night (22-7). -1 for an unknown start time.
int history_tod_bucket(long long unix_sec);
// Like history_top(), but only plays that started at or after `since` (unix
// seconds; archived plays count by their calendar day).
std::vector<HistoryTopRow> history_top_since(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive,
                                             long long since, bool most_first);
// One row of the TOP PLAYLISTS tab: a saved playlist and how often tracks of it were played.
struct HistoryPlaylistRow {
    std::string name;
    int plays = 0;
    double listened_sec = 0.0;
};
// The `n` most played playlists since `since` (unix seconds; 0 = overall). Plays count by the playlist
// they were queued from, so only plays recorded since this feature exists carry a playlist.
std::vector<HistoryPlaylistRow> history_top_playlists(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive,
                                                      long long since, size_t n);
// Totals behind the tab's summary: distinct playlists, plays that came from a playlist.
void history_playlist_totals(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive,
                             int& playlists, long long& playlist_plays);
// Like history_top(), but only plays that started in time-of-day bucket `tod`.
std::vector<HistoryTopRow> history_top_tod(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive,
                                           int tod, bool most_first);

// The key a play is recorded and aggregated under: a local file's path, or
// "yt:<video id>" for a stream (so the same track reached two ways counts
// once). Empty means "nothing to aggregate by" and such plays are skipped by
// history_top().
std::string history_track_id(bool is_local, const std::string& path, const std::string& video_id);

// Formatting shared by the UI: "3:42" / "1:02:03", "42m" / "1h 12m", "09-27 14:03".
std::string format_mmss(double sec);
std::string format_len(double sec);
std::string format_when(long long unix_sec);

} // namespace muisc
