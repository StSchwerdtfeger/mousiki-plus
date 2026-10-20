#pragma once
// Radio mode -- listening history (SHIFT+h menu: HISTORY / TOP CHANNELS / HABITS).
//
// Modelled on the music player's history.* but for a live stream, where there are no "plays" but a channel and an
// ICY title that changes: ONE LINE per (channel, artist, title). A new line starts when the channel, the artist or
// the title changes.
//
// Light-weight like the player's: the newest kMaxEntries lines are kept in memory and in `history_radio.txt` (one tab
// separated line per entry, APPENDED when the entry closes -- no rewrite per song). When the cap is exceeded the
// OLDEST line leaves the window; it is folded into `archive_radio.txt` (per-channel totals, per-day / per-hour / per-weekday
// seconds, session bookkeeping) first, so TOP CHANNELS and HABITS stay lifetime figures while the list itself is capped.
#include <array>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "radio_engine.h"

namespace muisc::radio {

constexpr size_t kMaxHistoryEntries = 10000;

struct HistoryEntry {
    long long at = 0;            // unix seconds, when it started
    double listened = 0.0;       // seconds of audio actually heard (muted / buffering time never counts)
    std::string channel, artist, title;
};

struct HistoryChannelRow {
    std::string channel;
    double listened = 0.0;
    int songs = 0;               // history lines of this channel
};

struct HistoryArchive {
    long long entries = 0;
    double listened = 0.0;
    std::map<std::string, std::pair<double, int>> channels;    // channel -> (seconds, lines)
    std::map<std::string, double> per_day;                     // "YYYY-MM-DD" -> seconds
    std::array<double, 24> hours{};                            // seconds per hour of the day
    std::array<double, 7> weekdays{};                          // seconds per weekday, 0 = Monday
    int sessions_closed = 0;
    double session_sum_sec = 0.0;
    bool session_open = false;
    long long sess_start = 0, sess_end = 0;
    void fold(const HistoryEntry& e);                          // oldest -> newest
};

struct HistoryStats {
    int entries = 0;
    int channels = 0;
    int sessions = 0;            // runs of lines split by a > 30 min gap
    double avg_session_sec = 0.0;
    double songs_per_session = 0.0;
    double today_sec = 0.0;
    double avg_day_sec = 0.0;
    double total_sec = 0.0;
    int days = 0;
    std::string top_channel;
    int busiest_hour = -1;       // -1 = no data
    std::array<double, 24> hours{};
    std::array<double, 7> weekdays{};      // seconds per weekday, 0 = Monday (lifetime, cumulative)
    // How often each weekday occurred in the calendar from the first day with radio up to today (both included), and
    // weekdays[d] / that count: the average time listened on such a weekday -- days without radio count as 0.
    std::array<int, 7> weekday_count{};
    std::array<double, 7> weekday_avg{};
};

class RadioHistory {
public:
    // Loads <dir>/history_radio.txt + archive_radio.txt (a missing / damaged file is never an error). Closes the live line first.
    void open(const std::filesystem::path& dir);
    // Call once per frame with the engine status: starts / extends / closes the live line. `now` = unix seconds.
    void tick(const RadioStatus& st, double dt, long long now);
    // Closes the live line (quit, stop, settings change).
    void close_live();

    // Newest first: index 0 is the live line while something is playing. `size()` includes it.
    size_t size() const { return closed_.size() + (live_on_ ? 1 : 0); }
    const HistoryEntry& newest(size_t i) const;
    bool live_at_top() const { return live_on_; }

    std::vector<HistoryChannelRow> top_channels(bool most_first) const;
    HistoryStats stats(long long now) const;
    const HistoryArchive& archive() const { return archive_; }

private:
    void commit(const HistoryEntry& e);      // appends to memory + file, trims the window
    void rewrite_files() const;
    void append_line(const HistoryEntry& e) const;

    std::filesystem::path dir_;
    std::deque<HistoryEntry> closed_;        // oldest first
    HistoryArchive archive_;
    HistoryEntry live_;
    bool live_on_ = false;
};

} // namespace muisc::radio
