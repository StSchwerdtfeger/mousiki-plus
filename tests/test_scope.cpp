// The oscilloscope renderer (src/oscilloscope_visualizer.cpp), the code both the player and the radio draw with.
#include "test_framework.h"
#include "oscilloscope_visualizer.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace muisc;

namespace {

constexpr int kW = 200, kH = 200;

using Params = OscilloscopeVisualizer::Params;

Params music_params(bool crt, bool dwell) {
    Params p;
    p.fixed_gain = true;        // oscilloscope music mode: full scale = the edge
    p.mono_phase = false;
    p.crt = crt;
    p.dwell = dwell;
    p.low_rate_blank = false;   // (tested on its own)
    p.decay = 0.30f;
    p.frame_dt = 1.0f / 60.0f;  // fixed: the pictures do not depend on the machine's clock
    return p;
}

// Feeds `frames` blocks of 800 samples (48 kHz, 60 pictures a second) made by gen(sampleIndex) -> {L, R}.
template <class Gen> std::vector<uint8_t> draw(OscilloscopeVisualizer& o, const Params& p, int frames, Gen gen) {
    std::vector<float> buf(2 * 800);
    std::vector<uint8_t> level, hue;
    long t = 0;
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < 800; ++i, ++t) {
            const auto lr = gen(t);
            buf[static_cast<size_t>(2 * i)] = lr.first;
            buf[static_cast<size_t>(2 * i + 1)] = lr.second;
        }
        o.push_frames(buf.data(), 800);
        o.render_image(kW, kH, p, level, hue);
    }
    return level;
}

int peak_around(const std::vector<uint8_t>& img, int cx, int cy, int r) {
    int m = 0;
    for (int y = std::max(0, cy - r); y <= std::min(kH - 1, cy + r); ++y)
        for (int x = std::max(0, cx - r); x <= std::min(kW - 1, cx + r); ++x) m = std::max<int>(m, img[static_cast<size_t>(y * kW + x)]);
    return m;
}

int lit(const std::vector<uint8_t>& img) {
    int n = 0;
    for (uint8_t v : img) n += v > 40;
    return n;
}

} // namespace

TEST(scope_silence_is_black) {
    OscilloscopeVisualizer o;
    const auto img = draw(o, music_params(true, true), 10, [](long) { return std::make_pair(0.0f, 0.0f); });
    CHECK_EQ(img.size(), static_cast<size_t>(kW * kH));
    // (a beam at rest leaves its spot in the middle, like a real scope -- but nothing anywhere else)
    CHECK(lit(img) < 40);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (std::abs(x - kW / 2) > 12 || std::abs(y - kH / 2) > 12) CHECK_EQ(static_cast<int>(img[static_cast<size_t>(y * kW + x)]), 0);
}

TEST(scope_circle_is_a_ring_with_an_empty_centre) {
    for (bool crt : {true, false}) {
        OscilloscopeVisualizer o;
        const auto img = draw(o, music_params(crt, true), 40, [](long t) {
            const float a = static_cast<float>(t) * 0.05f;
            return std::make_pair(0.6f * std::cos(a), 0.6f * std::sin(a));
        });
        const int ring = std::max(peak_around(img, kW / 2 + 60, kH / 2, 4), peak_around(img, kW / 2 - 60, kH / 2, 4));
        const int centre = peak_around(img, kW / 2, kH / 2, 6);
        CHECK(ring > 80);                  // the beam is where the circle is (radius 0.6 of half the picture) ...
        CHECK(centre < 25);                // ... and not in the middle
        CHECK(lit(img) > 50);
    }
}

TEST(scope_is_deterministic) {
    auto gen = [](long t) { return std::make_pair(0.5f * std::sin(static_cast<float>(t) * 0.011f), 0.5f * std::cos(static_cast<float>(t) * 0.017f)); };
    OscilloscopeVisualizer a, b;
    CHECK(draw(a, music_params(true, true), 25, gen) == draw(b, music_params(true, true), 25, gen));
}

TEST(scope_crt_and_classic_draw_different_pictures) {
    auto gen = [](long t) { return std::make_pair(0.5f * std::sin(static_cast<float>(t) * 0.011f), 0.5f * std::cos(static_cast<float>(t) * 0.017f)); };
    OscilloscopeVisualizer a, b;
    const auto crt = draw(a, music_params(true, true), 25, gen);
    const auto classic = draw(b, music_params(false, true), 25, gen);
    CHECK(lit(crt) > 50);
    CHECK(lit(classic) > 50);
    CHECK(crt != classic);
}

// Two bright spots with a fast jump between them (the "jump lines" of oscilloscope music): with dwell brightness the jump
// stays dark, with equal strokes it shows.
TEST(scope_dwell_brightness_keeps_the_jumps_dark) {
    auto spots = [](long t) {
        const bool first = (t / 100) % 2 == 0;     // 100 samples on one spot, then the jump to the other
        return first ? std::make_pair(-0.5f, -0.5f) : std::make_pair(0.5f, 0.5f);
    };
    OscilloscopeVisualizer a, b;
    const auto dwell = draw(a, music_params(true, true), 30, spots);
    const auto equal = draw(b, music_params(true, false), 30, spots);
    const int spot_dwell = std::max(peak_around(dwell, kW / 2 - 50, kH / 2 + 50, 5), peak_around(dwell, kW / 2 - 50, kH / 2 - 50, 5));
    const int line_dwell = peak_around(dwell, kW / 2, kH / 2, 3);    // half way along the jump
    const int line_equal = peak_around(equal, kW / 2, kH / 2, 3);
    CHECK(spot_dwell > 100);
    CHECK(line_dwell * 3 < spot_dwell);              // the jump is much darker than the spot with dwell brightness ...
    CHECK(line_equal > line_dwell * 2);              // ... and clearly brighter with equal strokes
}

TEST(scope_braille_cells_are_made_of_the_same_beam) {
    OscilloscopeVisualizer o;
    Params p = music_params(false, true);
    p.fixed_gain = false;
    std::vector<float> buf(2 * 800);
    std::vector<std::vector<OscilloscopeVisualizer::Cell>> cells;
    for (int f = 0; f < 20; ++f) {
        for (int i = 0; i < 800; ++i) {
            const float a = static_cast<float>(f * 800 + i) * 0.04f;
            buf[static_cast<size_t>(2 * i)] = 0.7f * std::cos(a);
            buf[static_cast<size_t>(2 * i + 1)] = 0.7f * std::sin(a * 1.01f);
        }
        o.push_frames(buf.data(), 800);
        cells = o.render(60, 20, p);
    }
    CHECK_EQ(cells.size(), static_cast<size_t>(20));
    size_t dots = 0;
    for (const auto& row : cells) for (const auto& c : row) dots += c.braille != 0;
    CHECK(dots > 20);
}

TEST(scope_measure_reports_levels) {
    OscilloscopeVisualizer o;
    Params p = music_params(true, true);
    draw(o, p, 30, [](long t) { return std::make_pair(0.5f * std::sin(static_cast<float>(t) * 0.05f), 0.5f * std::sin(static_cast<float>(t) * 0.05f)); });
    const auto m = o.measure(p);
    CHECK(m.peak_db > -12.0f && m.peak_db < 0.0f);               // a 0.5 amplitude sine peaks near -6 dBFS
}
