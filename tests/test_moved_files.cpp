// Data kept per file path follows a file that was moved or renamed (src/moved_files.h, HistoryStore::rename_ids / mend_ids,
// cover_store_rename_paths / cover_store_mend_tracks).
#include <filesystem>
#include <fstream>

#include "cover_store.h"
#include "history.h"
#include "moved_files.h"
#include "path_utf8.h"
#include "test_framework.h"

using namespace muisc;
namespace fs = std::filesystem;

namespace {
std::string touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
    return path_utf8(p);
}
HistoryPlay play_of(const std::string& id, long long at) {
    HistoryPlay p;
    p.id = id;
    p.title = "T";
    p.len_sec = 60;
    p.started_at = at;
    return p;
}
void listen(HistoryStore& h, const std::string& id, long long at) {
    h.begin_play(play_of(id, at));
    h.add_listened(60);
    h.finish_live(true);
}
}  // namespace

TEST(find_moved_only_unambiguous_missing_files) {
    mt::TempHome home;
    const std::string a = touch(home.path() / "new" / "Song.MP3");                  // the moved file (case of the name does not matter)
    const std::string b1 = touch(home.path() / "x" / "Twin.mp3"), b2 = touch(home.path() / "y" / "Twin.mp3");
    const std::string here = touch(home.path() / "here" / "Stay.mp3");
    const std::string gone = path_utf8(home.path() / "old" / "song.mp3");
    const std::string twin = path_utf8(home.path() / "old" / "twin.mp3");
    const std::string none = path_utf8(home.path() / "old" / "nothing.mp3");
    const auto m = find_moved({gone, twin, none, here, "yt:abc"}, {a, b1, b2, here});
    CHECK_EQ(m.size(), static_cast<size_t>(1));
    CHECK_EQ(m.at(gone), a);                                                        // found by its name
    CHECK(!m.count(twin));                                                          // two files of that name: never guessed
    CHECK(!m.count(none));
    CHECK(!m.count(here));                                                          // a path that exists is never touched
    // two missing paths of one name do not both get the same file
    const std::string o1 = path_utf8(home.path() / "o1" / "song.mp3"), o2 = path_utf8(home.path() / "o2" / "song.mp3");
    CHECK(find_moved({o1, o2}, {a}).empty());
}

TEST(history_follows_a_moved_or_renamed_file) {
    mt::TempHome home;
    const std::string moved = touch(home.path() / "new" / "x.mp3");
    const std::string old_path = path_utf8(home.path() / "old" / "x.mp3");
    HistoryStore h;
    listen(h, old_path, 1000);
    listen(h, old_path, 2000);
    listen(h, "yt:abc", 3000);
    listen(h, moved, 4000);                                                         // played from the new place already
    CHECK_EQ(h.mend_ids({moved}), 1);
    int on_new = 0, on_old = 0;
    for (const auto& p : h.plays()) { if (p.id == moved) ++on_new; if (p.id == old_path) ++on_old; }
    CHECK_EQ(on_new, 3);                                                            // the two old plays and the new one add up
    CHECK_EQ(on_old, 0);
    CHECK_EQ(h.plays()[1].id, std::string("yt:abc"));                               // a stream is never touched
    CHECK_EQ(h.mend_ids({moved}), 0);                                               // nothing missing any more

    const std::string renamed = path_utf8(home.path() / "new" / "y.mp3");
    CHECK_EQ(h.rename_ids({{moved, renamed}, {"yt:abc", "yt:other"}}), 1);          // (the meta editor's rename)
    CHECK_EQ(h.plays()[0].id, renamed);
    CHECK_EQ(h.plays()[1].id, std::string("yt:abc"));
}

TEST(history_archive_follows_a_moved_file) {
    mt::TempHome home;
    const std::string moved = touch(home.path() / "new" / "x.mp3");
    const std::string old_path = path_utf8(home.path() / "old" / "x.mp3");
    HistoryStore h;
    for (int i = 0; i < 3; ++i) listen(h, old_path, 1000 + i * 100);
    for (int i = 0; i < 1005; ++i) listen(h, "yt:filler", 100000 + i);              // pushes the first plays into the archive
    listen(h, moved, 500000);
    CHECK(h.archive().titles.count(old_path) == 1);
    const int before = h.archive().titles.at(old_path).plays;
    CHECK(before >= 1);
    CHECK_EQ(h.mend_ids({moved}), 1);
    CHECK(h.archive().titles.count(old_path) == 0);
    CHECK(h.archive().titles.count(moved) == 1);
    CHECK_EQ(h.archive().titles.at(moved).plays, before);
}

TEST(track_style_and_options_follow_a_moved_or_renamed_file) {
    mt::TempHome home;
    const std::string moved = touch(home.path() / "new" / "x.mp3");
    const std::string old_path = path_utf8(home.path() / "old" / "x.mp3");
    CoverStyle st;
    st.look = 3;
    st.contrast = 120;
    set_cover_track_style(old_path, st);
    VideoTrackOpts o;
    o.mp4audio = 1;
    set_cover_track_video_opts(old_path, o);
    MilkdropTrackOpts md;
    md.on = 1;
    md.preset = "Geiss";
    set_cover_track_milkdrop(old_path, md);

    CHECK_EQ(cover_store_mend_tracks({moved}), 1);
    CHECK_EQ(cover_track_style(moved).look, 3);
    CHECK_EQ(cover_track_style(moved).contrast, 120);
    CHECK_EQ(cover_track_video_opts(moved).mp4audio, 1);
    CHECK_EQ(cover_track_milkdrop(moved).preset, std::string("Geiss"));
    CHECK_EQ(cover_track_style(old_path).look, -1);                                 // gone from the old path
    CHECK_EQ(cover_store_mend_tracks({moved}), 0);                                  // nothing missing any more

    // an own choice of the new path is kept
    CoverStyle other;
    other.look = 5;
    const std::string target = touch(home.path() / "new" / "z.mp3");
    set_cover_track_style(target, other);
    CoverStyle again;
    again.look = 1;
    const std::string gone = path_utf8(home.path() / "old" / "z.mp3");
    set_cover_track_style(gone, again);
    CHECK_EQ(cover_store_mend_tracks({moved, target}), 1);
    CHECK_EQ(cover_track_style(target).look, 5);

    // a rename (the meta editor)
    const std::string renamed = path_utf8(home.path() / "new" / "w.mp3");
    CHECK_EQ(cover_store_rename_paths({{moved, renamed}}), 1);
    CHECK_EQ(cover_track_style(renamed).look, 3);
    CHECK_EQ(cover_track_style(moved).look, -1);
}
