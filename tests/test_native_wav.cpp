// A WAV file is read in-process (native_duration.h): the duration from the `fmt ` / `data` chunks, the tags from a LIST / INFO chunk. Anything unusual is
// declined (resolved = false, duration 0) so that ffprobe still decides it.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "native_duration.h"
#include "test_framework.h"

using namespace muisc;
namespace fs = std::filesystem;

namespace {
void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF); }
void put16(std::string& s, uint32_t v) { for (int i = 0; i < 2; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF); }

std::string chunk(const std::string& id, const std::string& body) {
    std::string c = id;
    put32(c, static_cast<uint32_t>(body.size()));
    c += body;
    if (body.size() & 1) c += '\0';
    return c;
}
std::string fmt_chunk(uint32_t tag, uint32_t channels, uint32_t rate, uint32_t bits) {
    std::string b;
    put16(b, tag); put16(b, channels); put32(b, rate); put32(b, rate * channels * bits / 8); put16(b, channels * bits / 8); put16(b, bits);
    return chunk("fmt ", b);
}
std::string info_chunk(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string b = "INFO";
    for (const auto& f : fields) b += chunk(f.first, f.second + std::string(1, '\0'));
    return chunk("LIST", b);
}
std::string riff(const std::string& chunks) {
    std::string r = "RIFF";
    put32(r, static_cast<uint32_t>(4 + chunks.size()));
    r += "WAVE";
    r += chunks;
    return r;
}
fs::path write_file(const fs::path& dir, const std::string& name, const std::string& bytes) {
    const fs::path p = dir / name;
    std::ofstream o(p, std::ios::binary);
    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return p;
}
}  // namespace

TEST(native_wav_reads_the_duration_and_the_info_tags) {
    mt::TempHome home;
    const std::string audio(44100 * 4 * 3 + 100, '\x11');   // 3 seconds and a bit of 16-bit stereo at 44.1 kHz
    // tags before the audio
    const fs::path a = write_file(home.path(), "a.wav", riff(fmt_chunk(1, 2, 44100, 16) + info_chunk({{"INAM", "Blue Moon"}, {"IART", "Ella"}, {"IPRD", "Songs"}, {"ICRD", "1999-05-01"}}) + chunk("data", audio)));
    CHECK_EQ(probe_duration_native(a), 3u);
    const NativeId3Tags ta = probe_wav_native(a);
    CHECK(ta.resolved);
    CHECK_EQ(ta.title, std::string("Blue Moon"));
    CHECK_EQ(ta.artist, std::string("Ella"));
    CHECK_EQ(ta.album, std::string("Songs"));
    CHECK_EQ(ta.year, std::string("1999"));
    // tags after the audio (where most programs write them)
    const fs::path b = write_file(home.path(), "b.wav", riff(fmt_chunk(1, 2, 44100, 16) + chunk("data", audio) + info_chunk({{"INAM", "Odd"}, {"IART", "Me"}})));
    CHECK_EQ(probe_duration_native(b), 3u);
    const NativeId3Tags tb = probe_wav_native(b);
    CHECK(tb.resolved);
    CHECK_EQ(tb.title, std::string("Odd"));   // (an odd length text is padded to an even size)
    CHECK_EQ(tb.artist, std::string("Me"));
    CHECK(tb.album.empty());
}

TEST(native_wav_without_tags_is_resolved_with_empty_fields) {
    mt::TempHome home;
    const std::string audio(48000 * 2 * 2, '\x05');   // 2 seconds, 16-bit mono at 48 kHz -> 192000 bytes
    const fs::path p = write_file(home.path(), "plain.wav", riff(fmt_chunk(1, 1, 48000, 16) + chunk("data", audio.substr(0, 48000 * 2 * 2))));
    CHECK_EQ(probe_duration_native(p), 2u);
    const NativeId3Tags t = probe_wav_native(p);
    CHECK(t.resolved);
    CHECK(t.title.empty() && t.artist.empty() && t.album.empty() && t.year.empty());
}

TEST(native_wav_declines_what_it_does_not_fully_understand) {
    mt::TempHome home;
    const std::string audio(44100 * 4, '\x22');
    // an ID3 tag chunk: ffprobe reads that one
    const fs::path id3 = write_file(home.path(), "id3.wav", riff(fmt_chunk(1, 2, 44100, 16) + chunk("data", audio) + chunk("id3 ", std::string("ID3\x03\0\0\0\0\0\0", 10))));
    CHECK(!probe_wav_native(id3).resolved);
    // a data chunk with no size (a streamed file) or more than the file has (a cut file)
    std::string streamed = "RIFF";
    put32(streamed, 0xFFFFFFFFu);
    streamed += "WAVE" + fmt_chunk(1, 2, 44100, 16) + "data";
    put32(streamed, 0xFFFFFFFFu);
    streamed += audio;
    const fs::path st = write_file(home.path(), "streamed.wav", streamed);
    CHECK(!probe_wav_native(st).resolved);
    CHECK_EQ(probe_duration_native(st), 0u);
    std::string cut = riff(fmt_chunk(1, 2, 44100, 16) + chunk("data", audio));
    cut.resize(cut.size() - 1000);
    const fs::path ct = write_file(home.path(), "cut.wav", cut);
    CHECK(!probe_wav_native(ct).resolved);
    // not a WAV at all / empty / a RIFF of another kind
    CHECK(!probe_wav_native(write_file(home.path(), "junk.wav", std::string(500, 'x'))).resolved);
    CHECK(!probe_wav_native(write_file(home.path(), "empty.wav", "")).resolved);
    std::string avi = "RIFF";
    put32(avi, 8);
    avi += "AVI LIST";
    CHECK(!probe_wav_native(write_file(home.path(), "avi.wav", avi)).resolved);
    CHECK(!probe_wav_native(home.path() / "missing.wav").resolved);
    // a chunk that runs out of the file
    std::string junk_header = "junk";
    put32(junk_header, 100000);   // (a chunk header that promises 100000 bytes the file does not have)
    const fs::path bg = write_file(home.path(), "big.wav", riff(fmt_chunk(1, 2, 44100, 16) + junk_header));
    CHECK(!probe_wav_native(bg).resolved);
}

TEST(native_wav_knows_no_duration_for_a_format_with_an_average_byte_rate) {
    mt::TempHome home;
    const std::string audio(44100 * 4, '\x33');
    // MS ADPCM (format 2): the bytes per second are only an average, ffprobe has the exact length -- the tags are still read here
    const fs::path p = write_file(home.path(), "adpcm.wav", riff(fmt_chunk(2, 2, 44100, 4) + info_chunk({{"INAM", "Squeeze"}}) + chunk("data", audio)));
    CHECK_EQ(probe_duration_native(p), 0u);
    const NativeId3Tags t = probe_wav_native(p);
    CHECK(t.resolved);
    CHECK_EQ(t.title, std::string("Squeeze"));
    // float samples (format 3) are exact
    const fs::path f = write_file(home.path(), "float.wav", riff(fmt_chunk(3, 2, 44100, 32) + chunk("data", std::string(44100 * 8 * 2, '\x01'))));
    CHECK_EQ(probe_duration_native(f), 2u);
}

// ---- Ogg lengths ----------------------------------------------------------------------------------------------------------
namespace {
std::string ogg_page(const std::string& packet, uint64_t granule, bool first) {
    std::string p = "OggS";
    p += '\0'; p += first ? '\x02' : '\x00';
    for (int i = 0; i < 8; ++i) p += static_cast<char>((granule >> (8 * i)) & 0xFF);
    put32(p, 1); put32(p, 0); put32(p, 0);   // serial, page number, checksum (nothing here checks it)
    p += '\x01'; p += static_cast<char>(packet.size());   // one segment
    return p + packet;
}
}

TEST(ogg_vorbis_length_uses_the_sample_rate_of_the_stream_and_opus_drops_its_pre_skip) {
    mt::TempHome home;
    std::string id = "\x01vorbis";
    put32(id, 0); id += '\x02'; put32(id, 44100);   // version, channels, sample rate
    id += std::string(8, '\0');
    const std::string filler(5000, '\0');
    const fs::path v = write_file(home.path(), "v.ogg", ogg_page(id, 0, true) + filler + ogg_page("x", 44100ull * 60, false));
    CHECK_EQ(probe_duration_native(v), 60u);   // (48000 for every file gave 55)
    std::string head = "OpusHead";
    head += '\x01'; head += '\x02'; head += '\x38'; head += '\x01';   // version 1, 2 channels, pre-skip 312
    put32(head, 44100);
    head += std::string(3, '\0');
    const fs::path o = write_file(home.path(), "o.opus", ogg_page(head, 0, true) + filler + ogg_page("x", 48000ull * 30 + 312, false));
    CHECK_EQ(probe_duration_native(o), 30u);
}

// ---- MP4 with 64-bit boxes, big cover art before the ID3 text, MPEG Layer II ---------------------------------------------------
namespace {
std::string be32s(uint32_t v) { std::string s; for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xFF); return s; }
std::string be64s(uint64_t v) { std::string s; for (int i = 7; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xFF); return s; }
}

TEST(mp4_length_is_found_behind_a_box_with_a_64_bit_size) {
    mt::TempHome home;
    std::string mvhd = std::string("\0\0\0\0", 4) + be32s(0) + be32s(0) + be32s(1000) + be32s(90000) + std::string(40, '\0');   // version 0: 90 s
    std::string mvhd_box = be32s(8 + static_cast<uint32_t>(mvhd.size())) + "mvhd" + mvhd;
    std::string moov = be32s(8 + static_cast<uint32_t>(mvhd_box.size())) + "moov" + mvhd_box;
    std::string big_free = be32s(1) + "free" + be64s(16 + 100) + std::string(100, 'x');   // a 64-bit sized box in front of moov
    const fs::path p = write_file(home.path(), "x.m4a", std::string("\0\0\0\x08" "ftyp", 8) + big_free + moov);
    CHECK_EQ(probe_duration_native(p), 90u);
}

TEST(id3_tag_cut_off_by_the_read_cap_is_left_to_ffprobe) {
    mt::TempHome home;
    // an ID3v2.3 tag of 3 MB whose first frame is a 3 MB picture: the text frames behind it are never reached
    const uint32_t picture = 3u * 1024 * 1024;
    const uint32_t tag = picture + 10;
    auto syncsafe = [](uint32_t v) { std::string s; for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (7 * i)) & 0x7F); return s; };
    std::string data = "ID3" + std::string("\x03\x00\x00", 3) + syncsafe(tag) + "APIC" + be32s(picture) + std::string("\0\0", 2) + std::string(picture, 'p');
    const fs::path p = write_file(home.path(), "cover.mp3", data);
    CHECK(!probe_id3v2_native(p).resolved);
}
