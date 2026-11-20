// Song fades (Settings -> GENERAL -> AUDIO FADE OPTIONS, music player only): the curve shapes of a fade, the gain of a crossfade and the
// scan for silence at the start and the end of a decoded track. Pure functions (no state besides a small table), so they are unit tested
// (tests/test_audio_fade.cpp); player.cpp uses them in the audio callback.
//
// Cost: a fade is one table lookup (1025 floats per curve, 16 KB in all) per sample frame inside the fade only; the silence scan reads
// the decoded samples at the two ends of a track once (the start: the first seconds up to the first audible sample; the end: from the
// last sample backwards up to the last audible one), nothing is copied, and a crossfade reads the next track from the buffer that
// gapless playback has decoded anyway.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace muisc {
namespace fade {

enum Curve { kLinear = 0, kExponential = 1, kLogarithmic = 2, kLogistic = 3, kCurveCount = 4 };

// The longest silence that is cut at one end of a track (a longer one is music that happens to be quiet, or a hidden track).
inline constexpr double kMaxTrimSeconds = 15.0;
inline constexpr double kTrimPadLeadSeconds = 0.02;   // kept in front of the first audible sample
inline constexpr double kTrimPadTailSeconds = 0.05;   // kept behind the last audible sample (its decay)

constexpr int kTableSize = 1024;

struct Table {
    float v[kCurveCount][kTableSize + 1];
    Table() {
        for (int i = 0; i <= kTableSize; ++i) {
            const double p = static_cast<double>(i) / kTableSize;
            v[kLinear][i] = static_cast<float>(p);
            // exponential: slow start, fast end (as a fade in); used backwards a fade out falls quickly first
            v[kExponential][i] = static_cast<float>((std::exp(4.0 * p) - 1.0) / (std::exp(4.0) - 1.0));
            // logarithmic: fast start, slow end
            v[kLogarithmic][i] = static_cast<float>(std::log1p(9.0 * p) / std::log(10.0));
            // logistic: the S-curve, scaled so that it starts at exactly 0 and ends at exactly 1
            const double s0 = 1.0 / (1.0 + std::exp(6.0)), s1 = 1.0 / (1.0 + std::exp(-6.0));
            v[kLogistic][i] = static_cast<float>((1.0 / (1.0 + std::exp(-12.0 * (p - 0.5))) - s0) / (s1 - s0));
        }
    }
};
inline const Table& table() { static const Table t; return t; }

// The gain 0..1 of a fade that is `p` (0..1) of the way from silence to full level.
inline float shape(int curve, float p) {
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;
    const Table& t = table();
    const float x = p * kTableSize;
    const int i = static_cast<int>(x);
    const float f = x - static_cast<float>(i);
    const float* v = t.v[curve < 0 || curve >= kCurveCount ? 0 : curve];
    return v[i] + (v[i + 1] - v[i]) * f;
}

// A crossfade: `p` (0..1) is how far it has gone. The two gains keep the power constant (their squares add up to 1), so there is no dip
// in the middle; the curve shapes how fast the new track comes in.
inline float xfade_out(int curve, float p) { return std::sqrt(std::max(0.0f, 1.0f - shape(curve, p))); }
inline float xfade_in(int curve, float p) { return std::sqrt(shape(curve, p)); }

inline float db_to_lin(int db) { return std::pow(10.0f, static_cast<float>(db) / 20.0f); }

// ---- the silence at the ends of a track ----
// `data`: interleaved samples (channels 1 or 2), `frames` of them. A frame is audible when one of its channels reaches `thr` (linear).
// scan_lead(): the first audible frame within the first kMaxTrimSeconds, or -1 when there is none in that stretch.
inline long long scan_lead(const float* data, int channels, size_t frames, int rate, float thr) {
    const size_t cap = std::min(frames, static_cast<size_t>(kMaxTrimSeconds * rate));
    for (size_t i = 0; i < cap; ++i) {
        const float a = std::fabs(data[i * channels]);
        const float b = channels > 1 ? std::fabs(data[i * channels + 1]) : a;
        if (a >= thr || b >= thr) return static_cast<long long>(i);
    }
    return -1;
}
// scan_tail(): the last audible frame within the last kMaxTrimSeconds, or -1 when there is none in that stretch.
inline long long scan_tail(const float* data, int channels, size_t frames, int rate, float thr) {
    const size_t cap = std::min(frames, static_cast<size_t>(kMaxTrimSeconds * rate));
    for (size_t k = 0; k < cap; ++k) {
        const size_t i = frames - 1 - k;
        const float a = std::fabs(data[i * channels]);
        const float b = channels > 1 ? std::fabs(data[i * channels + 1]) : a;
        if (a >= thr || b >= thr) return static_cast<long long>(i);
    }
    return -1;
}
// Where a track starts / ends once the silence is cut: the first frame to play and one past the last one.
inline long long trimmed_start(long long lead_audible, int rate) {
    if (lead_audible < 0) return 0;
    return std::max<long long>(0, lead_audible - static_cast<long long>(kTrimPadLeadSeconds * rate));
}
inline long long trimmed_end(long long tail_audible, size_t frames, int rate) {
    if (tail_audible < 0) return static_cast<long long>(frames);
    return std::min<long long>(static_cast<long long>(frames), tail_audible + 1 + static_cast<long long>(kTrimPadTailSeconds * rate));
}

}  // namespace fade
}  // namespace muisc
