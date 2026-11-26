// Editing the title / artist / album / year of an MP3 (id3_edit.h): the other frames and the audio stay exactly as they were.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "id3_edit.h"
#include "native_duration.h"
#include "test_framework.h"

using namespace muisc;
namespace fs = std::filesystem;

namespace {
std::string frame3(const std::string& id, const std::string& payload) {
    std::string f = id;
    for (int i = 3; i >= 0; --i) f += static_cast<char>((payload.size() >> (8 * i)) & 0xFF);
    f += std::string(2, '\0');
    return f + payload;
}
std::string tag3(const std::string& frames) {
    std::string t = "ID3";
    t += '\x03'; t += '\0'; t += '\0';
    const size_t n = frames.size();
    t += static_cast<char>((n >> 21) & 0x7F); t += static_cast<char>((n >> 14) & 0x7F); t += static_cast<char>((n >> 7) & 0x7F); t += static_cast<char>(n & 0x7F);
    return t + frames;
}
std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
void spit(const fs::path& p, const std::string& b) { std::ofstream f(p, std::ios::binary); f << b; }
// a few bytes that look like an MP3 frame header followed by data (nothing here decodes them)
const std::string kAudio = std::string("\xFF\xFB\x90\x64", 4) + std::string(600, '\x5A');
}

TEST(id3_edit_changes_the_text_frames_and_keeps_every_other_frame_and_the_audio) {
    mt::TempHome home;
    const std::string uslt = std::string("\x00" "eng\x00\x00Hello lyrics line", 22);
    const std::string popm = std::string("me@x.org\x00\xC4\x05", 12);
    const std::string txxx = std::string("\x00" "custom\x00value", 14);
    const fs::path f = home.path() / "a.mp3";
    spit(f, tag3(frame3("TIT2", std::string("\x00Old", 4)) + frame3("TPE1", std::string("\x00" "Band", 5)) + frame3("USLT", uslt) + frame3("POPM", popm) + frame3("TXXX", txxx) + std::string(100, '\0')) + kAudio);

    std::string err;
    CHECK(id3_edit_text_frames(f, {{"TIT2", "Neue \xC3\xBC" "ber \xE6\x97\xA5\xE6\x9C\xAC"}, {"YEAR", "1999"}}, &err) == Id3EditResult::Done);
    const std::string out = slurp(f);
    CHECK_EQ(out.substr(0, 4), std::string("ID3\x03", 4));                          // still a v2.3 tag
    CHECK(out.find(uslt) != std::string::npos);                                       // the lyrics, the rating and the custom text are all there, unchanged
    CHECK(out.find(popm) != std::string::npos);
    CHECK(out.find(txxx) != std::string::npos);
    CHECK_EQ(out.substr(out.size() - kAudio.size()), kAudio);                        // the audio is byte for byte the same
    const NativeId3Tags t = probe_id3v2_native(f);
    CHECK(t.resolved);
    CHECK_EQ(t.title, std::string("Neue \xC3\xBC" "ber \xE6\x97\xA5\xE6\x9C\xAC"));
    CHECK_EQ(t.artist, std::string("Band"));                                          // not edited: as it was
    CHECK_EQ(t.year, std::string("1999"));
    CHECK(!fs::exists(home.path() / "a.mousiki-tmp.mp3"));
}

TEST(id3_edit_removes_a_frame_with_an_empty_value_and_adds_a_tag_to_a_file_without_one) {
    mt::TempHome home;
    const fs::path f = home.path() / "b.mp3";
    spit(f, tag3(frame3("TIT2", std::string("\x00Old", 4)) + frame3("TALB", std::string("\x00Rec", 4))) + kAudio);
    CHECK(id3_edit_text_frames(f, {{"TALB", ""}}) == Id3EditResult::Done);
    NativeId3Tags t = probe_id3v2_native(f);
    CHECK_EQ(t.title, std::string("Old"));
    CHECK(t.album.empty());

    const fs::path g = home.path() / "plain.mp3";
    spit(g, kAudio);
    CHECK(id3_edit_text_frames(g, {{"TIT2", "Fresh"}, {"TPE1", "Someone"}}) == Id3EditResult::Done);
    t = probe_id3v2_native(g);
    CHECK_EQ(t.title, std::string("Fresh"));
    CHECK_EQ(t.artist, std::string("Someone"));
    const std::string out = slurp(g);
    CHECK_EQ(out.substr(out.size() - kAudio.size()), kAudio);
}

TEST(id3_edit_leaves_a_tag_it_does_not_edit_alone) {
    mt::TempHome home;
    const fs::path f = home.path() / "c.mp3";
    std::string t = tag3(frame3("TIT2", std::string("\x00Old", 4)));
    t[3] = '\x02';                                                                    // ID3v2.2
    spit(f, t + kAudio);
    const std::string before = slurp(f);
    CHECK(id3_edit_text_frames(f, {{"TIT2", "New"}}) == Id3EditResult::NotApplicable);
    CHECK_EQ(slurp(f), before);
    std::string u = tag3(frame3("TIT2", std::string("\x00Old", 4)));
    u[5] = '\x80';                                                                    // unsynchronised
    spit(f, u + kAudio);
    CHECK(id3_edit_text_frames(f, {{"TIT2", "New"}}) == Id3EditResult::NotApplicable);
}
