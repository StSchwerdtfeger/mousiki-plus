// Radio listening history (src_radio/radio_history.*): one line per (channel, artist, title), persisted between runs.
#include <filesystem>
#include <fstream>

#include "radio_history.h"
#include "test_framework.h"

using namespace muisc::radio;

namespace {
RadioStatus live(const std::string& channel, const std::string& artist, const std::string& title) {
    RadioStatus st;
    st.state = StreamState::Live;
    st.tuned_name = channel;
    st.info.artist = artist;
    st.info.title = title;
    return st;
}
// Plays `seconds` of a stream in one-second frames.
void listen(RadioHistory& h, const RadioStatus& st, int seconds, long long& now) {
    for (int i = 0; i < seconds; ++i) h.tick(st, 1.0, now++);
}
}

TEST(radio_history_a_new_song_starts_a_new_line) {
    mt::TempHome home;
    RadioHistory h;
    h.open(home.path() / "rh");
    long long now = 1000000;
    listen(h, live("Chill FM", "Artist A", "Song 1"), 40, now);
    listen(h, live("Chill FM", "Artist B", "Song 2"), 30, now);
    CHECK_EQ(h.size(), static_cast<size_t>(2));
    CHECK(h.live_at_top());
    CHECK_EQ(h.newest(0).title, std::string("Song 2"));            // newest first, the live line on top
    CHECK_EQ(h.newest(1).title, std::string("Song 1"));
    CHECK_NEAR(h.newest(1).listened, 40, 1e-9);
    CHECK_NEAR(h.newest(0).listened, 30, 1e-9);
}

TEST(radio_history_muted_and_buffering_time_does_not_count) {
    mt::TempHome home;
    RadioHistory h;
    h.open(home.path() / "rh");
    long long now = 1000000;
    RadioStatus st = live("Chill FM", "A", "S");
    listen(h, st, 10, now);
    st.muted = true;
    listen(h, st, 10, now);                                          // muted: not heard
    st.muted = false;
    listen(h, st, 5, now);
    h.tick(st, 60.0, now++);                                         // a frame gap this large is a stall, not listening
    CHECK_NEAR(h.newest(0).listened, 15, 1e-9);
}

TEST(radio_history_title_arriving_late_fills_the_first_line) {
    mt::TempHome home;
    RadioHistory h;
    h.open(home.path() / "rh");
    long long now = 1000000;
    listen(h, live("Chill FM", "", ""), 3, now);                     // audio first, ICY title a moment later
    listen(h, live("Chill FM", "Artist", "Song"), 30, now);
    CHECK_EQ(h.size(), static_cast<size_t>(1));                      // no empty line left behind
    CHECK_EQ(h.newest(0).title, std::string("Song"));
}

TEST(radio_history_idle_closes_the_live_line) {
    mt::TempHome home;
    RadioHistory h;
    h.open(home.path() / "rh");
    long long now = 1000000;
    listen(h, live("Chill FM", "A", "S"), 30, now);
    RadioStatus idle;                                                // nothing tuned
    h.tick(idle, 1.0, now++);
    CHECK(!h.live_at_top());
    CHECK_EQ(h.size(), static_cast<size_t>(1));
}

TEST(radio_history_is_saved_and_loaded_and_top_channels_add_up) {
    mt::TempHome home;
    const auto dir = home.path() / "rh";
    long long now = 1000000;
    {
        RadioHistory h;
        h.open(dir);
        listen(h, live("Jazz FM", "A", "S1"), 100, now);
        listen(h, live("Rock FM", "B", "S2"), 40, now);
        listen(h, live("Jazz FM", "A", "S3"), 50, now);
        h.close_live();
        CHECK_EQ(h.size(), static_cast<size_t>(3));
    }
    RadioHistory again;
    again.open(dir);                                                  // read back from disk
    CHECK_EQ(again.size(), static_cast<size_t>(3));
    const auto most = again.top_channels(true);
    CHECK_EQ(most.size(), static_cast<size_t>(2));
    CHECK_EQ(most[0].channel, std::string("Jazz FM"));
    CHECK_NEAR(most[0].listened, 150, 1e-9);
    CHECK_EQ(most[0].songs, 2);
    const auto least = again.top_channels(false);       // kept in a variable: CHECK_EQ must not point into a temporary
    CHECK_EQ(least[0].channel, std::string("Rock FM"));
    const HistoryStats st = again.stats(now);
    CHECK_EQ(st.entries, 3);
    CHECK_EQ(st.channels, 2);
    CHECK_NEAR(st.total_sec, 190, 1e-9);
    CHECK_EQ(st.top_channel, std::string("Jazz FM"));
}

TEST(radio_history_a_damaged_file_is_never_an_error) {
    mt::TempHome home;
    const auto dir = home.path() / "rh";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "history_radio.txt", std::ios::binary);
        f << "not\ta\tvalid\tline\n\x01\x02 garbage without tabs\n\t\t\t\n";
    }
    RadioHistory h;
    h.open(dir);                                                      // must not throw or crash
    (void)h.stats(1000000);
    (void)h.top_channels(true);
    CHECK(!h.live_at_top());
}
