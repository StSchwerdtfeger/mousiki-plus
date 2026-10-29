// Playback speed (src/time_stretch.h, the audio book player), the speed / audio device settings and the "°" hotkey name.
#include "test_framework.h"
#include "time_stretch.h"
#include "settings.h"
#include "terminal_ui.h"
#include "keyboard_layout.h"
#include <cmath>
#include <vector>

using namespace muisc;

namespace {
constexpr int kSr = 44100;
constexpr double kPi = 3.14159265358979323846;

// A mono test tone as the stretcher's two read functions see a buffer (frames outside it are silence).
struct Tone {
    std::vector<float> s;
    explicit Tone(double freq, double secs) : s(static_cast<size_t>(secs * kSr)) {
        for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * freq * static_cast<double>(i) / kSr));
    }
    bool avail(long long, long long i1) const { return i1 <= static_cast<long long>(s.size()); }
    float at(long long i, int) const { return (i < 0 || i >= static_cast<long long>(s.size())) ? 0.0f : s[static_cast<size_t>(i)]; }
};

// Runs `frames` output frames at `speed`; returns the output and the source position reached.
std::vector<float> run(TimeStretch& ts, const Tone& t, double speed, int frames, double& pos) {
    std::vector<float> out;
    out.reserve(static_cast<size_t>(frames));
    auto av = [&](long long a, long long b) { return t.avail(a, b); };
    auto sm = [&](long long i, int ch) { return t.at(i, ch); };
    for (int i = 0; i < frames; ++i) {
        float l = 0, r = 0;
        if (ts.next(speed, av, sm, l, r)) out.push_back(l);
    }
    pos = ts.heard_pos();
    return out;
}

// Rising zero crossings per second of a signal (skipping the fade-in of the first grain).
double crossings_per_sec(const std::vector<float>& v, int skip) {
    int n = 0;
    for (size_t i = static_cast<size_t>(skip) + 1; i < v.size(); ++i) if (v[i - 1] < 0.0f && v[i] >= 0.0f) ++n;
    return n * static_cast<double>(kSr) / static_cast<double>(v.size() - static_cast<size_t>(skip));
}
} // namespace

TEST(time_stretch_keeps_pitch_and_moves_at_speed) {
    const Tone tone(440.0, 6.0);
    for (double speed : {0.5, 0.75, 1.5, 2.0, 3.0}) {
        TimeStretch ts;
        ts.configure(kSr);
        ts.reset(0.0);
        double pos = 0.0;
        const int frames = kSr;   // one second of output
        const std::vector<float> out = run(ts, tone, speed, frames, pos);
        CHECK_EQ(static_cast<int>(out.size()), frames);
        CHECK_NEAR(pos, speed * frames, ts.grain() * 1.0);              // the source ran `speed` times as fast
        CHECK_NEAR(crossings_per_sec(out, ts.grain()), 440.0, 440.0 * 0.03);   // ... and the tone is still 440 Hz
    }
}

TEST(time_stretch_waits_for_frames_that_are_not_there) {
    TimeStretch ts;
    ts.configure(kSr);
    ts.reset(1000.0);
    auto none = [](long long, long long) { return false; };
    auto zero = [](long long, int) { return 0.0f; };
    float l = 1, r = 1;
    CHECK(!ts.next(1.5, none, zero, l, r));
    CHECK_EQ(l, 0.0f);
    CHECK_NEAR(ts.heard_pos(), 1000.0, 1e-9);   // nothing moved on
}

TEST(speed_label_is_short) {
    CHECK_EQ(speed_label(1.0), std::string("1.0x"));
    CHECK_EQ(speed_label(1.25), std::string("1.25x"));
    CHECK_EQ(speed_label(0.5), std::string("0.5x"));
    CHECK_EQ(speed_label(2.95), std::string("2.95x"));
    CHECK_EQ(speed_label(3.0), std::string("3.0x"));
}

TEST(book_speed_and_audio_device_settings_round_trip) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK_NEAR(s.book_speed, 1.0, 1e-9);
    CHECK(s.audio_device.empty());
    s.book_speed = 1.35;
    s.audio_device = "Speakers (Realtek(R) Audio)";
    save_settings(s);
    const Settings back = load_settings();
    CHECK_NEAR(back.book_speed, 1.35, 1e-9);
    CHECK_EQ(back.audio_device, std::string("Speakers (Realtek(R) Audio)"));
    // one line written on its own (what the CHOOSE AUDIO DEVICE overlay does) is read back the same way
    CHECK(set_config_value("AudioDevice", "Headphones (USB)"));
    CHECK_EQ(config_value("AudioDevice"), std::string("Headphones (USB)"));
    CHECK_EQ(load_settings().audio_device, std::string("Headphones (USB)"));
    CHECK_NEAR(load_settings().book_speed, 1.35, 1e-9);   // the rest of the file is untouched
}

TEST(audio_device_key_default_has_a_bound_key) {
    const Settings s = load_settings();
    const auto it = s.hotkeys.find("HKeyAudioDevice");
    CHECK(it != s.hotkeys.end());
    if (it != s.hotkeys.end()) CHECK(it->second == "°" || it->second == "&");
    CHECK(default_audio_device_key() == "°" || default_audio_device_key() == "&");
}
