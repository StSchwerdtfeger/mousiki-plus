// Song fades (audio_fade.h): the curves, the crossfade gains, the silence scans and the settings that belong to them.
#include "test_framework.h"
#include "audio_fade.h"
#include "settings.h"

#include <cmath>
#include <vector>

using namespace muisc;

TEST(fade_curves_start_at_zero_end_at_one_and_rise) {
    for (int c = 0; c < fade::kCurveCount; ++c) {
        CHECK(fade::shape(c, 0.0f) == 0.0f);
        CHECK(fade::shape(c, 1.0f) == 1.0f);
        float prev = 0.0f;
        for (int i = 1; i <= 200; ++i) {
            const float v = fade::shape(c, i / 200.0f);
            CHECK(v >= prev - 1e-6f);
            prev = v;
        }
    }
    CHECK(std::fabs(fade::shape(fade::kLinear, 0.25f) - 0.25f) < 1e-4f);
    CHECK(fade::shape(fade::kExponential, 0.5f) < 0.5f);   // slow start
    CHECK(fade::shape(fade::kLogarithmic, 0.5f) > 0.5f);   // fast start
    CHECK(std::fabs(fade::shape(fade::kLogistic, 0.5f) - 0.5f) < 1e-3f);   // the S-curve crosses the middle
}

TEST(fade_crossfade_keeps_the_power) {
    for (int c = 0; c < fade::kCurveCount; ++c)
        for (int i = 0; i <= 20; ++i) {
            const float p = i / 20.0f, a = fade::xfade_out(c, p), b = fade::xfade_in(c, p);
            CHECK(std::fabs(a * a + b * b - 1.0f) < 1e-3f);
        }
}

TEST(fade_scans_find_the_audible_part) {
    const int rate = 1000;
    std::vector<float> d(10000, 0.0f);   // mono: 10 s
    CHECK_EQ(fade::scan_lead(d.data(), 1, d.size(), rate, 0.001f), -1);
    CHECK_EQ(fade::scan_tail(d.data(), 1, d.size(), rate, 0.001f), -1);
    d[1500] = 0.5f; d[8200] = -0.4f;
    CHECK_EQ(fade::scan_lead(d.data(), 1, d.size(), rate, 0.001f), 1500);
    CHECK_EQ(fade::scan_tail(d.data(), 1, d.size(), rate, 0.001f), 8200);
    CHECK(fade::scan_lead(d.data(), 1, d.size(), rate, 0.9f) == -1);   // above the threshold nothing is audible
    // stereo: the right channel counts too
    std::vector<float> s(2 * 5000, 0.0f);
    s[2 * 700 + 1] = 0.2f;
    CHECK_EQ(fade::scan_lead(s.data(), 2, 5000, rate, 0.001f), 700);
    // a start after more than the longest cut silence is left alone
    std::vector<float> big(static_cast<size_t>(20 * rate), 0.0f);
    big[static_cast<size_t>(18 * rate)] = 1.0f;
    CHECK_EQ(fade::scan_lead(big.data(), 1, big.size(), rate, 0.001f), -1);
}

TEST(fade_trimmed_positions_keep_a_little_padding) {
    CHECK_EQ(fade::trimmed_start(-1, 1000), 0);
    CHECK_EQ(fade::trimmed_start(1500, 1000), 1480);
    CHECK_EQ(fade::trimmed_start(5, 1000), 0);
    CHECK_EQ(fade::trimmed_end(-1, 10000, 1000), 10000);
    CHECK_EQ(fade::trimmed_end(8200, 10000, 1000), 8251);
    CHECK_EQ(fade::trimmed_end(9990, 10000, 1000), 10000);
}

TEST(fade_settings_round_trip_and_defaults) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK(!s.fade_in && !s.fade_out && !s.fade_cross && !s.fade_trim);
    CHECK_EQ(s.fade_len_ms, 4000);
    CHECK_EQ(s.fade_curve, 0);
    CHECK_EQ(s.fade_trim_db, -60);
    CHECK(!s.vfull_md_novideo);
    s.fade_in = true; s.fade_cross = true; s.fade_trim = true;
    s.fade_len_ms = 6000; s.fade_curve = 3; s.fade_trim_db = -50;
    s.vfull_md_novideo = true;
    save_settings(s);
    const Settings t = load_settings();
    CHECK(t.fade_in && !t.fade_out && t.fade_cross && t.fade_trim);
    CHECK_EQ(t.fade_len_ms, 6000);
    CHECK_EQ(t.fade_curve, 3);
    CHECK_EQ(t.fade_trim_db, -50);
    CHECK(t.vfull_md_novideo);
}
