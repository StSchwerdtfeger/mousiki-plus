#pragma once
// The radio's XY oscilloscope: the music player's OscilloscopeVisualizer (same phosphor buffer, mono phase portrait and
// auto-gain) with more controls: line interpolation on/off, a Z axis (beam intensity), trace length, 45 degree rotation
// (M/S view), mono phase portrait on/off, and per-subpixel brightness + a "frequency" value per cell, so the UI can draw
// the picture as braille or as plain pixels and colour it by temperature. The player's own class stays untouched.
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

class RadioScope {
public:
    struct Cell {
        uint8_t braille = 0;                 // U+2800 dot pattern, 0 = empty cell
        uint8_t level = 0;                   // 0..255 brightness of the brightest subpixel
        std::array<uint8_t, 8> sub{};        // brightness per subpixel (index = row * 2 + column, 0 below the dot threshold)
        uint8_t hue = 0;                     // 0..255 "frequency" of the brightest subpixel (beam speed: slow 0 .. fast 255)
    };
    struct Params {
        float decay = 0.80f;                 // afterglow per frame
        float dot_threshold = 0.28f;         // brightness a subpixel needs to light
        float tail_brightness = 0.45f;       // oldest sample of the trace relative to the newest
        bool interpolate = true;             // connect the samples with lines (off: only the sample dots)
        bool z_axis = false;                 // beam intensity follows the Z signal
        float z_depth = 0.70f;               // how strongly Z darkens the beam, 0 .. 1
        int z_source = 0;                    // 0 = beam speed (fast = dim, like a CRT), 1 = signal level (far from centre = bright)
        int trace = 1024;                    // samples drawn per frame, 128 .. 1024
        bool rotate = false;                 // turn the picture by 45 degrees: mid on the vertical, side on the horizontal axis
        bool mono_phase = true;              // near-mono signals cross-fade to a phase portrait (else a diagonal line)
        float glow = 0.60f;                  // render_image(): strength / size of the bloom around the beam, 0 .. 1
        bool fixed_gain = false;             // oscilloscope music mode: full scale = the edge, no automatic size
        int mode = 0;                        // 0 = XY (L horizontal, R vertical), 1 = YT (amplitude over time, triggered), 2 = both on top of each other, 3 = both in two rows (XY above YT), 4 = both in two columns (XY left, YT right)
    };
    void push_frames(const float* interleaved_lr, size_t frames);
    void reset();
    // Takes over the newest audio of another scope, so a second picture of the same signal can be drawn at the same time.
    void copy_ring_from(RadioScope& src);
    std::vector<std::vector<Cell>> render(int cols, int rows, const Params& params);
    // Steady readouts for the big overlay's picture names (SHIFT+p there): levels in dBFS (0 dB = full scale), the L/R correlation
    // (+1 = identical channels, 0 = unrelated, -1 = opposite) and the L/R level balance in dB (positive = left louder). They are
    // averaged over about a second (RMS, correlation, balance) or held (peak: the highest value of the last two seconds) and
    // only change twice a second, rounded, so they can be read. Call it once per frame; the values are independent of the
    // picture's automatic size.
    struct Measure {
        float rms_db = -120.0f, peak_db = -120.0f, corr = 0.0f, bal_db = 0.0f;
    };
    Measure measure(const Params& params);
    // w x h pixel picture: `level` = brightness 0..255 (bloom included), `hue` = the "frequency" 0..255 of the nearest beam pixel.
    void render_image(int w, int h, const Params& params, std::vector<uint8_t>& level, std::vector<uint8_t>& hue);
private:
    void paint(int pw, int ph, const Params& params);   // beam -> glow_ / hue_ (render thread)
    static constexpr int kRingFrames = 2048;
    static constexpr int kWindow = 1024;
    std::mutex mtx_;
    std::array<float, kRingFrames> ch0_{};
    std::array<float, kRingFrames> ch1_{};
    int write_ = 0;
    float peak_ = 0.05f;
    bool cleared_ = false;
    std::vector<float> glow_;
    std::vector<float> hue_;
    int glow_w_ = 0, glow_h_ = 0;
    float mono_ = 0.0f;
    float scale_ = 0.0f;
    float hue_lo_ = 0.25f, hue_hi_ = 0.85f;   // smoothed range of the beam speeds, so every palette gets used
    // measure(): the averages and what is shown (render thread only)
    double meas_t_ = 0.0, meas_aa_ = 0.0, meas_bb_ = 0.0, meas_ab_ = 0.0, meas_peak_t_ = 0.0, meas_shown_t_ = 0.0;
    float meas_peak_ = 0.0f;
    bool meas_init_ = false;
    Measure shown_;
    float yt_peak_ = 0.05f;                   // YT trace: its own auto-gain (render thread only)
    float yt_lo_ = 0.20f, yt_hi_ = 0.80f;     // YT trace: smoothed range of the slopes, for the "frequency" colour
};
