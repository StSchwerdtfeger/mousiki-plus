// The system media controls (src/media_session.h): the status -> now-playing mapping and the requests -> commands mapping.
#include "test_framework.h"
#include "media_session.h"

#include <algorithm>

using namespace muisc;
using KV = std::vector<std::pair<std::string, std::string>>;

TEST(media_info_from_player_status) {
    const media::Info i = media::info_from_status(KV{{"mode", "player"}, {"state", "playing"}, {"title", "Song"}, {"artist", "Band"},
                                                     {"album", "LP"}, {"position_sec", "12.5"}, {"duration_sec", "200"}, {"volume", "70"}});
    CHECK_EQ(i.state, std::string("playing"));
    CHECK_EQ(i.title, std::string("Song"));
    CHECK_EQ(i.artist, std::string("Band"));
    CHECK_EQ(i.album, std::string("LP"));
    CHECK_NEAR(i.position, 12.5, 1e-9);
    CHECK_NEAR(i.duration, 200.0, 1e-9);
    CHECK_EQ(i.volume, 70);
}

TEST(media_info_muted_counts_as_playing_and_stopped_is_empty) {
    CHECK_EQ(media::info_from_status(KV{{"state", "muted"}, {"title", "x"}}).state, std::string("playing"));
    CHECK_EQ(media::info_from_status(KV{{"state", "paused"}, {"title", "x"}}).state, std::string("paused"));
    const media::Info s = media::info_from_status(KV{{"state", "stopped"}, {"title", "left over"}, {"position_sec", "9"}});
    CHECK_EQ(s.state, std::string("stopped"));
    CHECK(s.title.empty());
    CHECK_NEAR(s.position, 0.0, 1e-9);
    CHECK_EQ(media::info_from_status(KV{{"state", "failed"}}).state, std::string("stopped"));   // a failed radio stream
}

TEST(media_info_radio_station_becomes_title_or_album) {
    const media::Info a = media::info_from_status(KV{{"state", "playing"}, {"station", "Radio X"}, {"title", ""}, {"artist", ""}});
    CHECK_EQ(a.title, std::string("Radio X"));
    const media::Info b = media::info_from_status(KV{{"state", "playing"}, {"station", "Radio X"}, {"title", "A Song"}, {"artist", "Someone"}});
    CHECK_EQ(b.title, std::string("A Song"));
    CHECK_EQ(b.album, std::string("Radio X"));
    CHECK_NEAR(b.duration, 0.0, 1e-9);   // a live stream has no length
}

TEST(media_command_names) {
    const media::Info now;
    CHECK_EQ(media::command_for("Play", 0, now).name, std::string("play"));
    CHECK_EQ(media::command_for("Pause", 0, now).name, std::string("pause"));
    CHECK_EQ(media::command_for("PlayPause", 0, now).name, std::string("play-pause"));
    CHECK_EQ(media::command_for("Stop", 0, now).name, std::string("stop"));
    CHECK_EQ(media::command_for("Next", 0, now).name, std::string("next"));
    CHECK_EQ(media::command_for("Previous", 0, now).name, std::string("prev"));
    CHECK(media::command_for("OpenUri", 0, now).name.empty());
    CHECK(media::command_for("", 0, now).name.empty());
    // every name is one the remote control accepts
    const auto known = remote::known_commands();
    for (const char* m : {"Play", "Pause", "PlayPause", "Stop", "Next", "Previous", "Seek", "SetPosition", "SetVolume"}) {
        const std::string n = media::command_for(m, 0, now).name;
        CHECK(std::find(known.begin(), known.end(), n) != known.end());
    }
}

TEST(media_command_seek_and_position_are_relative_seconds) {
    media::Info now;
    now.position = 30.0;
    const remote::Command s = media::command_for("Seek", -5000000, now);
    CHECK_EQ(s.name, std::string("seek"));
    CHECK_NEAR(remote::number(s.arg, 0), -5.0, 1e-9);
    const remote::Command p = media::command_for("SetPosition", 100000000, now);   // to 100 s while at 30 s
    CHECK_NEAR(remote::number(p.arg, 0), 70.0, 1e-9);
    CHECK(p.arg.size() <= 12);
    CHECK(media::command_for("Seek", 4000000000000000LL, now).arg.size() <= 12);   // absurd values stay valid
}

TEST(media_command_volume_is_percent) {
    const media::Info now;
    CHECK_EQ(media::command_for("SetVolume", 500000, now).arg, std::string("50"));
    CHECK_EQ(media::command_for("SetVolume", 1000000, now).arg, std::string("100"));
    CHECK_EQ(media::command_for("SetVolume", -1, now).arg, std::string("0"));
    CHECK_EQ(media::command_for("SetVolume", 9000000, now).arg, std::string("100"));
}

TEST(media_info_text_is_made_well_formed) {
    const media::Info i = media::info_from_status(KV{{"state", "playing"}, {"title", "ab\xff" "cd\x1b[31m"}, {"artist", "x\ty"}});
    CHECK(i.title.find('\xff') == std::string::npos);
    CHECK(i.title.find('\x1b') == std::string::npos);
    CHECK_EQ(i.artist, std::string("x y"));
}
