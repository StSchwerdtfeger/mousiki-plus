#pragma once
// ---------------------------------------------------------------------------
// The CREATOR: turns text and pictures into SOUND that shows the picture on Mousiki's own displays.
//
//   * Oscilloscope audio (XY): the left channel is x and the right channel is y of a beam that draws the outlines of
//     the text / picture, over and over (the "oscilloscope music" idea). Shown on the XY oscilloscope.
//   * Spectrogram sound: the picture is read as a spectrogram -- time runs left to right, pitch bottom to top,
//     brightness is loudness -- and played back with an additive synthesiser that only uses the notes of a musical
//     scale, so it sounds like music and not like noise. Shown in the spectrogram.
//
// Everything in here is plain data in, plain data out (no terminal, no player), so it can be tested. The overlay
// that drives it is in app.cpp (key `^`).
// ---------------------------------------------------------------------------
#include <cstdint>
#include <string>
#include <vector>

namespace muisc::creator {

// ---- pictures --------------------------------------------------------------------------------------------------

struct Gray {
    int w = 0, h = 0;
    std::vector<uint8_t> px;                       // h * w, 255 = bright
    bool empty() const { return w <= 0 || h <= 0 || px.size() != static_cast<size_t>(w) * static_cast<size_t>(h); }
    uint8_t at(int x, int y) const { return px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)]; }
};

// A binary PGM ("P5") as ffmpeg writes it. False for anything else.
bool parse_pgm(const std::string& data, Gray& out);

// Text as a picture: white letters on black. The 5x7 pixel font of the karaoke mode where it has the letter, an
// installed font (Japanese, Cyrillic ...) where not; `scale` = picture pixels per font pixel. Lines are split at '\n'.
// Empty when nothing could be drawn.
Gray gray_from_text(const std::string& utf8, int scale = 6);

// Box-filter resize (smooth when shrinking). Returns an empty picture for a bad size.
Gray gray_resized(const Gray& g, int w, int h);
// Brightness inverted (255 - v).
Gray gray_inverted(const Gray& g);
// True when the picture is mostly bright (a drawing on white paper): its ink is the dark part.
bool gray_is_mostly_bright(const Gray& g);
// Otsu's threshold (0..255) that best separates dark from bright.
int otsu_threshold(const Gray& g);

// A small preview of the picture in braille characters (2 x 4 dots per character), `cols` x `rows` characters.
// A dot is set where the (optionally inverted) brightness is above `threshold`.
std::vector<std::string> braille_preview(const Gray& g, int cols, int rows, int threshold, bool invert);

// ---- outlines --------------------------------------------------------------------------------------------------

struct Pt { float x = 0, y = 0; };

struct Path {
    std::vector<Pt> pts;
    bool closed = false;                           // a loop: the last point connects back to the first
};

struct TraceOptions {
    int threshold = -1;                            // 0..255; -1 = Otsu
    bool invert = false;                           // trace the dark part instead of the bright one
    float simplify = 0.8f;                         // Douglas-Peucker tolerance in picture pixels (bigger = fewer points)
    float min_perimeter = 6.0f;                    // loops shorter than this (in pixels) are specks and dropped
};

// The boundaries of the bright (or, inverted, dark) shapes, at sub-pixel positions (marching squares). x right, y down,
// in picture pixels. Closed loops only; every shape and every hole is one loop.
std::vector<Path> trace_outlines(const Gray& g, const TraceOptions& opt);

double path_length(const Path& p);
double total_length(const std::vector<Path>& paths);
// Keeps the longest loops, as many as fit in `max_len` (in the units of the paths).
void limit_length(std::vector<Path>& paths, double max_len);
// Centres the drawing at (0,0), scales it uniformly to fit [-margin, margin] and turns y upwards. No-op when empty.
void fit_to_unit(std::vector<Path>& paths, float margin = 0.9f);
// Orders the loops so the beam jumps as little as possible (nearest neighbour; a loop starts at its point nearest to
// the beam, an open path may run backwards).
std::vector<Path> order_paths(std::vector<Path> paths);

// ---- audio ---------------------------------------------------------------------------------------------------------

struct Stereo {
    int rate = 48000;
    std::vector<float> l, r;
    double seconds() const { return rate > 0 ? static_cast<double>(l.size()) / rate : 0.0; }
};

enum class Motion { None, Spin, Tumble, Pulse };

struct XyOptions {
    int rate = 48000;
    double seconds = 10.0;
    double fps = 60.0;                             // how often the picture is drawn per second (also the hum pitch)
    Motion motion = Motion::None;
    double jump_speed = 12.0;                      // how much faster the beam moves between shapes than along them
};

// One drawing pass as `n` samples: interleaved x,y pairs, 2*n floats in [-1,1]. The beam follows the paths in order
// at a constant speed, jumps quickly from one path to the next and returns to the start (so the frame can repeat).
// Empty when there are no paths or n < 2.
std::vector<float> xy_frame(const std::vector<Path>& unit_paths, int n, double jump_speed);

// Draws the (already fitted and ordered) paths again and again for `seconds`, with the chosen motion. False when there
// is nothing to draw. The result is faded in and out and its peak is 0.9.
bool render_xy(const std::vector<Path>& unit_paths, const XyOptions& opt, Stereo& out);

enum class Scale { PentatonicMinor, PentatonicMajor, Major, Minor, WholeTone, Chromatic, Exact };

struct SpecOptions {
    int rate = 44100;
    double seconds = 12.0;
    Scale scale = Scale::PentatonicMinor;
    int low_note = 36;                             // MIDI number of the lowest pitch (36 = C2)
    int octaves = 5;
    int polyphony = 12;                            // loudest rows that sound at the same time (0 = all)
    double reverb = 0.25;                          // 0..1
    bool invert = false;                           // dark ink on bright paper: the dark part is loud
    double gamma = 1.4;                            // >1 makes faint areas quieter
};

// Frequencies (Hz) of the rows, lowest first. Exact = 72 evenly spaced semitone-fractions over `octaves`.
std::vector<double> scale_frequencies(Scale scale, int low_note, int octaves);
const char* scale_name(Scale scale);

// The picture as a spectrogram: columns = time, rows = pitch (top row = highest), brightness = loudness. The synth only
// plays the notes of the scale, loudness is kept even, and the result has a soft reverb. False for an empty picture or
// a picture without any bright part (silence).
bool render_spectrogram_sound(const Gray& g, const SpecOptions& opt, Stereo& out);

// Reads a WAV as write_wav16() makes it (16 bit PCM, 1 or 2 channels; a mono file is copied to both). False for anything else.
bool read_wav16(const std::string& path_utf8, Stereo& out);

// 16 bit stereo WAV. `path_utf8` is created (its folder must exist).
bool write_wav16(const std::string& path_utf8, const Stereo& s, std::string* error = nullptr);

}  // namespace muisc::creator
