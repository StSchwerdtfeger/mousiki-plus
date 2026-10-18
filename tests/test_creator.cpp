// The creator: pictures -> outlines -> oscilloscope audio, pictures -> spectrogram sound, WAV writer.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "creator.h"
#include "test_framework.h"

using namespace muisc::creator;

namespace {

Gray blank(int w, int h, uint8_t v = 0) {
    Gray g;
    g.w = w; g.h = h;
    g.px.assign(static_cast<size_t>(w) * static_cast<size_t>(h), v);
    return g;
}

void fill_rect(Gray& g, int x0, int y0, int x1, int y1, uint8_t v) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) g.px[static_cast<size_t>(y) * static_cast<size_t>(g.w) + static_cast<size_t>(x)] = v;
}

// signed area (shoelace); in picture coordinates (y down) a positive value runs clockwise on screen
double area(const Path& p) {
    double a = 0;
    for (size_t i = 0; i < p.pts.size(); ++i) {
        const Pt& u = p.pts[i];
        const Pt& v = p.pts[(i + 1) % p.pts.size()];
        a += static_cast<double>(u.x) * v.y - static_cast<double>(v.x) * u.y;
    }
    return a * 0.5;
}

double goertzel(const std::vector<float>& x, size_t from, size_t n, double f, double rate) {
    double re = 0, im = 0;
    for (size_t i = 0; i < n; ++i) {
        const double ph = 2 * 3.14159265358979 * f * static_cast<double>(i) / rate;
        re += x[from + i] * std::cos(ph);
        im += x[from + i] * std::sin(ph);
    }
    return std::sqrt(re * re + im * im) / static_cast<double>(n);
}

}  // namespace

TEST(creator_parse_pgm) {
    Gray g;
    std::string ok = "P5\n# comment\n3 2\n255\n";
    ok += std::string("\x00\x40\x80\xC0\xFF\x10", 6);
    CHECK(parse_pgm(ok, g));
    CHECK_EQ(g.w, 3);
    CHECK_EQ(g.h, 2);
    CHECK_EQ(static_cast<int>(g.at(2, 0)), 0x80);
    CHECK_EQ(static_cast<int>(g.at(1, 1)), 0xFF);
    CHECK(!parse_pgm("P6\n1 1\n255\nabc", g));
    CHECK(!parse_pgm("P5\n4 4\n255\nshort", g));
    CHECK(!parse_pgm("", g));
}

TEST(creator_otsu_and_brightness) {
    Gray g = blank(20, 20, 20);
    fill_rect(g, 5, 5, 15, 15, 220);
    const int t = otsu_threshold(g);
    CHECK(t >= 20 && t < 220);
    CHECK(!gray_is_mostly_bright(g));
    CHECK(gray_is_mostly_bright(gray_inverted(g)));
    CHECK_EQ(static_cast<int>(gray_inverted(g).at(0, 0)), 235);
}

TEST(creator_resize) {
    Gray g = blank(8, 8, 100);
    const Gray s = gray_resized(g, 3, 5);
    CHECK_EQ(s.w, 3);
    CHECK_EQ(s.h, 5);
    for (uint8_t v : s.px) CHECK_NEAR(static_cast<double>(v), 100.0, 1.0);
    CHECK(gray_resized(g, 0, 4).empty());
    CHECK(gray_resized(Gray(), 4, 4).empty());
}

TEST(creator_text_picture) {
    const Gray t = gray_from_text("Hi", 4);
    CHECK(!t.empty());
    int lit = 0;
    for (uint8_t v : t.px) if (v) ++lit;
    CHECK(lit > 20);
    CHECK(gray_from_text("").empty());
    const Gray two = gray_from_text("A\nB", 4);
    CHECK(two.h > t.h);
}

TEST(creator_trace_square) {
    Gray g = blank(30, 30);
    fill_rect(g, 8, 8, 20, 20, 255);
    const auto paths = trace_outlines(g, TraceOptions());
    CHECK_EQ(paths.size(), static_cast<size_t>(1));
    const Path& p = paths[0];
    CHECK(p.closed);
    CHECK(p.pts.size() >= 4 && p.pts.size() <= 8);
    CHECK_NEAR(path_length(p), 48.0, 3.0);
    CHECK_NEAR(std::fabs(area(p)), 144.0, 12.0);
}

TEST(creator_trace_hole_and_two_shapes) {
    Gray g = blank(60, 30);
    fill_rect(g, 4, 4, 24, 26, 255);
    fill_rect(g, 10, 10, 18, 20, 0);          // a hole
    fill_rect(g, 34, 6, 52, 22, 255);
    const auto paths = trace_outlines(g, TraceOptions());
    CHECK_EQ(paths.size(), static_cast<size_t>(3));
    int pos = 0, neg = 0;
    for (const auto& p : paths) (area(p) > 0 ? pos : neg)++;
    CHECK_EQ(pos, 2);                          // the two shapes run one way, the hole the other way
    CHECK_EQ(neg, 1);
}

TEST(creator_trace_invert_and_specks) {
    Gray g = blank(30, 30, 255);
    fill_rect(g, 8, 8, 20, 20, 0);            // dark shape on white paper
    TraceOptions o;
    o.invert = true;
    const auto dark = trace_outlines(g, o);
    CHECK_EQ(dark.size(), static_cast<size_t>(1));
    CHECK_NEAR(std::fabs(area(dark[0])), 144.0, 12.0);
    Gray s = blank(30, 30);
    s.px[15 * 30 + 15] = 255;                 // a single pixel: dropped as a speck
    CHECK(trace_outlines(s, TraceOptions()).empty());
    CHECK(trace_outlines(blank(10, 10), TraceOptions()).empty());
}

TEST(creator_fit_order_limit) {
    Gray g = blank(60, 30);
    fill_rect(g, 4, 4, 14, 14, 255);
    fill_rect(g, 40, 10, 56, 26, 255);
    auto paths = trace_outlines(g, TraceOptions());
    CHECK_EQ(paths.size(), static_cast<size_t>(2));
    fit_to_unit(paths, 0.9f);
    float mx = 0;
    for (const auto& p : paths) for (const auto& q : p.pts) mx = std::max({mx, std::fabs(q.x), std::fabs(q.y)});
    CHECK_NEAR(mx, 0.9, 0.001);
    paths = order_paths(paths);
    CHECK_EQ(paths.size(), static_cast<size_t>(2));
    const double all = total_length(paths);
    auto copy = paths;
    limit_length(copy, all * 0.8);
    CHECK_EQ(copy.size(), static_cast<size_t>(1));
    copy = paths;
    limit_length(copy, all * 2);
    CHECK_EQ(copy.size(), static_cast<size_t>(2));
}

TEST(creator_xy_frame) {
    Path sq;
    sq.closed = true;
    sq.pts = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    const auto f = xy_frame({sq}, 400, 12.0);
    CHECK_EQ(f.size(), static_cast<size_t>(800));
    for (float v : f) CHECK(v >= -0.5001f && v <= 0.5001f);
    CHECK_NEAR(f[0], -0.5, 0.01);
    CHECK(xy_frame({}, 400, 12.0).empty());
    CHECK(xy_frame({sq}, 1, 12.0).empty());
    // constant speed: consecutive points are about equally far apart
    double mn = 1e9, mx = 0;
    for (size_t k = 1; k < 399; ++k) {
        const double d = std::hypot(f[2 * k] - f[2 * k - 2], f[2 * k + 1] - f[2 * k - 1]);
        mn = std::min(mn, d); mx = std::max(mx, d);
    }
    CHECK(mx < mn * 3 + 0.01);
}

TEST(creator_render_xy) {
    Gray g = blank(40, 40);
    fill_rect(g, 10, 10, 30, 30, 255);
    auto paths = trace_outlines(g, TraceOptions());
    fit_to_unit(paths);
    paths = order_paths(paths);
    XyOptions o;
    o.seconds = 1.0;
    o.rate = 48000;
    Stereo s;
    CHECK(render_xy(paths, o, s));
    CHECK_EQ(s.l.size(), static_cast<size_t>(48000));
    CHECK_EQ(s.l.size(), s.r.size());
    float peak = 0;
    for (size_t i = 0; i < s.l.size(); ++i) {
        CHECK(std::isfinite(s.l[i]) && std::isfinite(s.r[i]));
        peak = std::max({peak, std::fabs(s.l[i]), std::fabs(s.r[i])});
    }
    CHECK(peak <= 0.9001f);
    CHECK(peak > 0.7f);
    CHECK_NEAR(s.l[0], 0.0, 0.001);                       // faded in
    CHECK_NEAR(s.l.back(), 0.0, 0.001);                   // and out
    for (Motion m : {Motion::Spin, Motion::Tumble, Motion::Pulse}) {
        o.motion = m;
        o.seconds = 0.6;
        Stereo t;
        CHECK(render_xy(paths, o, t));
        CHECK_EQ(t.l.size(), static_cast<size_t>(28800));
    }
    Stereo none;
    CHECK(!render_xy({}, o, none));
}

TEST(creator_scales) {
    const auto pent = scale_frequencies(Scale::PentatonicMinor, 36, 5);
    CHECK_EQ(pent.size(), static_cast<size_t>(26));
    CHECK_NEAR(pent[0], 65.406, 0.01);
    CHECK_NEAR(pent.back(), 65.406 * 32, 0.1);
    for (size_t i = 1; i < pent.size(); ++i) CHECK(pent[i] > pent[i - 1]);
    CHECK_EQ(scale_frequencies(Scale::Chromatic, 36, 2).size(), static_cast<size_t>(25));
    CHECK_EQ(scale_frequencies(Scale::Major, 36, 1).size(), static_cast<size_t>(8));
    CHECK_EQ(scale_frequencies(Scale::Exact, 36, 4).size(), static_cast<size_t>(72));
    CHECK(std::string(scale_name(Scale::WholeTone)) == "Whole tone");
}

TEST(creator_spectrogram_sound) {
    // a diagonal line from bottom left to top right: the pitch must rise
    Gray g = blank(120, 60);
    for (int x = 0; x < 120; ++x) {
        const int y = 59 - x / 2;
        for (int d = -1; d <= 1; ++d) if (y + d >= 0 && y + d < 60) g.px[static_cast<size_t>(y + d) * 120 + static_cast<size_t>(x)] = 255;
    }
    SpecOptions o;
    o.seconds = 8.0;
    o.rate = 22050;
    o.reverb = 0.0;
    Stereo s;
    CHECK(render_spectrogram_sound(g, o, s));
    CHECK(s.l.size() >= static_cast<size_t>(8 * 22050));
    float peak = 0;
    for (size_t i = 0; i < s.l.size(); ++i) {
        CHECK(std::isfinite(s.l[i]) && std::isfinite(s.r[i]));
        peak = std::max({peak, std::fabs(s.l[i]), std::fabs(s.r[i])});
    }
    CHECK(peak <= 1.0f);
    CHECK(peak > 0.5f);
    // early: low notes, late: high notes
    const auto freqs = scale_frequencies(o.scale, o.low_note, o.octaves);
    const size_t win = 4096;
    auto energy = [&](size_t at, size_t lo_row, size_t hi_row) {
        double e = 0;
        for (size_t r = lo_row; r < hi_row; ++r) e += goertzel(s.l, at, win, freqs[r], o.rate);
        return e;
    };
    const size_t third = freqs.size() / 3;
    const size_t early = static_cast<size_t>(1.0 * o.rate), late = static_cast<size_t>(7.0 * o.rate);
    CHECK(energy(early, 0, third) > energy(early, 2 * third, freqs.size()));
    CHECK(energy(late, 2 * third, freqs.size()) > energy(late, 0, third));
    // no clicks: no big jump between neighbouring samples
    float jump = 0;
    for (size_t i = 1; i < s.l.size(); ++i) jump = std::max(jump, std::fabs(s.l[i] - s.l[i - 1]));
    CHECK(jump < 0.5f);
    // with reverb it is longer (a tail) and still within range
    o.reverb = 0.4;
    Stereo r;
    CHECK(render_spectrogram_sound(g, o, r));
    CHECK(r.l.size() > s.l.size());
    float rp = 0;
    for (size_t i = 0; i < r.l.size(); ++i) rp = std::max(rp, std::fabs(r.l[i]));
    CHECK(rp <= 1.0f);
}

TEST(creator_spectrogram_silence_and_polyphony) {
    Stereo s;
    CHECK(!render_spectrogram_sound(blank(50, 50), SpecOptions(), s));
    CHECK(!render_spectrogram_sound(Gray(), SpecOptions(), s));
    // a fully bright picture still renders, within range, whatever the polyphony
    SpecOptions o;
    o.seconds = 2.0;
    o.rate = 16000;
    o.polyphony = 4;
    CHECK(render_spectrogram_sound(blank(40, 40, 255), o, s));
    float peak = 0;
    for (float v : s.l) { CHECK(std::isfinite(v)); peak = std::max(peak, std::fabs(v)); }
    CHECK(peak <= 1.0f);
}

TEST(creator_braille_preview) {
    Gray g = blank(40, 40);
    fill_rect(g, 0, 0, 40, 20, 255);
    const auto rows = braille_preview(g, 10, 5, 128, false);
    CHECK_EQ(rows.size(), static_cast<size_t>(5));
    for (const auto& r : rows) CHECK_EQ(r.size(), static_cast<size_t>(30));   // 10 characters of 3 bytes
    CHECK(rows[0] != rows[4]);
    CHECK(braille_preview(Gray(), 10, 5, 128, false).empty());
}

TEST(creator_wav_roundtrip) {
    mt::TempHome home;
    Stereo s;
    s.rate = 8000;
    for (int i = 0; i < 100; ++i) { s.l.push_back(i / 100.0f); s.r.push_back(-i / 100.0f); }
    const std::string path = (home.path() / "t.wav").string();
    std::string err;
    CHECK(write_wav16(path, s, &err));
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string d = ss.str();
    CHECK_EQ(d.size(), static_cast<size_t>(44 + 400));
    CHECK(d.substr(0, 4) == "RIFF");
    CHECK(d.substr(8, 4) == "WAVE");
    auto s16 = [&](size_t at) { return static_cast<int16_t>(static_cast<uint16_t>(static_cast<unsigned char>(d[at]) | (static_cast<unsigned char>(d[at + 1]) << 8))); };
    CHECK_EQ(static_cast<int>(s16(44 + 50 * 4)), static_cast<int>(std::lround(0.5 * 32767)));
    CHECK_EQ(static_cast<int>(s16(44 + 50 * 4 + 2)), -static_cast<int>(std::lround(0.5 * 32767)));
    Stereo back;
    CHECK(read_wav16(path, back));
    CHECK_EQ(back.rate, 8000);
    CHECK_EQ(back.l.size(), static_cast<size_t>(100));
    CHECK_NEAR(back.l[50], 0.5, 0.001);
    CHECK_NEAR(back.r[50], -0.5, 0.001);
    CHECK(!read_wav16((home.path() / "missing.wav").string(), back));
    Stereo empty;
    CHECK(!write_wav16(path, empty, &err));
    CHECK(!err.empty());
}

#include "local_source.h"

TEST(creator_folder_is_left_out_of_the_library) {
    mt::TempHome home;
    const auto music = home.path() / "music";
    std::filesystem::create_directories(music / "creator");
    std::filesystem::create_directories(music / "album");
    Stereo s;
    s.rate = 8000;
    for (int i = 0; i < 80; ++i) { s.l.push_back(0.1f); s.r.push_back(0.1f); }
    CHECK(write_wav16((music / "album" / "song.wav").string(), s));
    CHECK(write_wav16((music / "creator" / "text_scope.wav").string(), s));
    muisc::LocalSource src;
    auto count = [&]() { return src.scan({music.string()}).size(); };
    CHECK_EQ(count(), static_cast<size_t>(2));                       // not marked yet: both are listed
    std::ofstream((music / "creator" / muisc::kCreatorMarker).string()) << "x";
    const auto tracks = src.scan({music.string()});
    CHECK_EQ(tracks.size(), static_cast<size_t>(1));                 // the creator's file is gone from the library
    CHECK(tracks[0].title == "song");
}

#include "mic_capture.h"

TEST(mic_ring_keeps_order_wraps_and_counts_drops) {
    muisc::StereoRing ring(8);
    CHECK_EQ(ring.capacity(), static_cast<size_t>(8));
    float a[10], out[20];
    for (int i = 0; i < 5; ++i) { a[2 * i] = static_cast<float>(i); a[2 * i + 1] = -static_cast<float>(i); }
    CHECK_EQ(ring.write(a, 5), static_cast<size_t>(5));
    CHECK_EQ(ring.available(), static_cast<size_t>(5));
    CHECK_EQ(ring.read(out, 3), static_cast<size_t>(3));
    CHECK_NEAR(out[4], 2.0, 1e-6);
    CHECK_NEAR(out[5], -2.0, 1e-6);
    // wrap around: 3 + 5 more frames do not fit 8 slots with 2 still unread, so the rest is dropped
    CHECK_EQ(ring.write(a, 5), static_cast<size_t>(5));
    CHECK_EQ(ring.available(), static_cast<size_t>(7));
    CHECK_EQ(ring.write(a, 5), static_cast<size_t>(1));
    CHECK_EQ(ring.dropped(), static_cast<size_t>(4));
    CHECK_EQ(ring.read(out, 20), static_cast<size_t>(8));
    CHECK_NEAR(out[0], 3.0, 1e-6);       // the two frames left from the first write ...
    CHECK_NEAR(out[2], 4.0, 1e-6);
    CHECK_NEAR(out[4], 0.0, 1e-6);       // ... then the second write, in order across the wrap
    CHECK_NEAR(out[12], 4.0, 1e-6);
    CHECK_NEAR(out[14], 0.0, 1e-6);       // the one frame of the last write that still fitted
    CHECK_EQ(ring.available(), static_cast<size_t>(0));
    CHECK_EQ(ring.read(out, 4), static_cast<size_t>(0));
}

TEST(mic_capture_without_a_device_fails_cleanly) {
    muisc::MicCapture mic;
    CHECK(!mic.running());
    std::vector<float> v;
    CHECK_EQ(mic.read(v), static_cast<size_t>(0));
    std::string err;
    const bool ok = mic.start(44100, err);      // a build machine usually has no microphone: either way no crash
    if (ok) { CHECK(mic.running()); CHECK(mic.rate() >= 8000); mic.stop(); CHECK(!mic.running()); }
    else CHECK(!err.empty());
}

TEST(wav_reader_rejects_chunk_sizes_larger_than_the_file) {
    mt::TempHome home;
    const std::string path = (home.path() / "evil.wav").string();
    auto put32 = [](std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 255); };
    auto put16 = [](std::string& s, uint32_t v) { s += static_cast<char>(v & 255); s += static_cast<char>((v >> 8) & 255); };
    // a header that promises a 4 GB data chunk but holds only 4 samples must neither crash nor allocate 4 GB
    std::string w = "RIFF";
    put32(w, 0xFFFFFFFFu);
    w += "WAVEfmt ";
    put32(w, 16); put16(w, 1); put16(w, 1); put32(w, 44100); put32(w, 88200); put16(w, 2); put16(w, 16);
    w += "data";
    put32(w, 0xFFFFFFFFu);
    for (int i = 0; i < 4; ++i) put16(w, 1000);
    { std::ofstream(path, std::ios::binary) << w; }
    muisc::creator::Stereo s;
    CHECK(muisc::creator::read_wav16(path, s));
    CHECK_EQ(s.l.size(), static_cast<size_t>(4));
    // a fmt chunk bigger than the file is refused
    std::string bad = "RIFF";
    put32(bad, 100);
    bad += "WAVEfmt ";
    put32(bad, 0x7FFFFFFFu);
    { std::ofstream(path, std::ios::binary) << bad; }
    CHECK(!muisc::creator::read_wav16(path, s));
    // an unknown chunk that claims more than the file holds ends the read
    std::string skip = "RIFF";
    put32(skip, 100);
    skip += "WAVEJUNK";
    put32(skip, 0xFFFFFFF0u);
    { std::ofstream(path, std::ios::binary) << skip; }
    CHECK(!muisc::creator::read_wav16(path, s));
}
