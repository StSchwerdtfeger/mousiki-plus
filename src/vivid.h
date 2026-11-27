#pragma once
// Gradient Vividness (disk, cover gradient, video gradient looks): a colour made fuller and brighter, like the neon edge looks of the video and MilkDrop.
// vivid 0 .. 1.5 (0 .. 150 %). The push away from grey is strongest in the middle of the range and gone at 1 (from there on only the brightness rises).
#include <algorithm>
#include <cstdio>
#include <string>
namespace muisc {
// The progress waveform glows more above 100 % than the other gradients (0.9: at 150 % the colours are 45 % of the way to white, they were 15 %) -- thin braille dots need it to stand out.
constexpr float kWaveGlowGain = 0.9f;
inline void vivid_boost(float* c, float vivid, float glow_gain = 0.3f) {   // c: r, g, b as 0 .. 255 floats, changed in place (glow_gain: how fast the glow above 100 % rises; the progress waveform asks for more)
    if (vivid <= 0.f) return;
    const float y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    // 0 .. 1: the push away from grey peaks in the middle (none at 1) while the brightness rises to the full colour. 1 .. 1.5: no more
    // saturation -- a glow instead: the colour keeps its hue and gets lighter (up to 15 % of the way to white at 150 %), like the bright neon
    // edge looks of the video and MilkDrop, which are vivid but not that saturated
    const float v1 = std::min(vivid, 1.f), extra = std::max(0.f, vivid - 1.f);
    const float push = 1.f + 2.f * v1 * (1.f - v1);
    float f[3];
    for (int k = 0; k < 3; ++k) f[k] = std::max(0.f, y + (c[k] - y) * push);
    const float m = std::max({f[0], f[1], f[2], 1.f});
    const float scale = 1.f + v1 * (255.f / m - 1.f);
    const float glow = glow_gain * extra;   // (at 100 % the gradient colours already are the neon edge looks' -- pink (205,95,205) -> (255,118,255), teal (50,190,200) -> (64,242,255); beyond it only a little lighter)
    for (int k = 0; k < 3; ++k) { const float v = std::clamp(f[k] * scale, 0.f, 255.f); c[k] = v + glow * (255.f - v); }
}
// A truecolour foreground "ESC[38;2;R;G;Bm" made vivid (any other string, or vivid <= 0, comes back as it is): the one place the other
// gradient users (braille scope / spectrogram, bars, volume bar, the radio's ON AIR sign) go through.
inline std::string vivid_sgr(const std::string& sgr, int vivid) {
    int r, g, b;
    if (vivid <= 0 || std::sscanf(sgr.c_str(), "\x1b[38;2;%d;%d;%dm", &r, &g, &b) != 3) return sgr;
    float c[3] = {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)};
    vivid_boost(c, static_cast<float>(std::min(vivid, 150)) / 100.0f);
    char buf[32];
    std::snprintf(buf, sizeof buf, "\x1b[38;2;%d;%d;%dm", static_cast<int>(c[0] + 0.5f), static_cast<int>(c[1] + 0.5f), static_cast<int>(c[2] + 0.5f));
    return buf;
}
}  // namespace muisc
