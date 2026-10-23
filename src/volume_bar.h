#pragma once
// ---------------------------------------------------------------------------
// The looks of the VOLUME BAR ("Volume Indicator" on the ON/OFF tab of the music player and of the radio).
//
//   0  "#"    1  "⠶"    2  "⣿"    repeat one character
//   3  "⡾⣽"   a fixed row of 20 different braille characters (not repeating)
//   4  "disk" the dots of the disk animation: the bar is a strip of solid braille dots that is turned like the disk of
//             the main screen is, with the CENTRE of the circle two cells in from the left end (the third cell). The
//             dots are moved and rounded exactly like DiskArt::frame() does it, so some of them are missing at some
//             angles (none close to the centre, more and more further out, so the left end is calmer than the right
//             but not still) and the pattern ripples along the bar while the angle changes.
//
// Every cell of the filled part is one terminal column wide in all styles; the empty rest of the bar stays "-".
// ---------------------------------------------------------------------------
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

inline constexpr int kVolStyleCount = 5;
inline constexpr int kVolStyleDisk = 4;
inline constexpr int kVolBarCells = 20;   // the width of the bar

// Config / settings names: # , ⠶ , ⣿ , ⡾⣽ , disk
inline const char* const kVolIndicatorNames[kVolStyleCount] = {"#", "\xE2\xA0\xB6", "\xE2\xA3\xBF", "\xE2\xA1\xBE\xE2\xA3\xBD", "disk"};

// The style a config / settings name stands for (-1 when it is none of them).
inline int volume_style_from_name(const std::string& v) {
    for (int i = 0; i < kVolStyleCount; ++i) if (v == kVolIndicatorNames[i]) return i;
    return -1;
}

namespace volume_bar_detail {

inline std::string braille(unsigned bits) {
    const uint32_t cp = 0x2800u + (bits & 0xFFu);
    std::string out;
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
    return out;
}

// Where the circle of the "disk" look is centred: the centre of the third cell (two cells in from the left end), in dot
// columns (2 per cell) and dot rows (4 per cell, so 1.5 is the middle).
inline constexpr double kDiskCx = 4.5, kDiskCy = 1.5;

// The solid disc of dots that is turned: lattice points around that centre, wide enough to reach beyond the last cell
// of the bar from there.
inline const std::vector<std::pair<float, float>>& disc() {
    static const std::vector<std::pair<float, float>> d = [] {
        std::vector<std::pair<float, float>> v;
        const double cx = kDiskCx, cy = kDiskCy, r = 2.0 * kVolBarCells + 4.0;
        for (int y = -static_cast<int>(r) - 2; y <= static_cast<int>(r) + 2; ++y)
            for (int x = -static_cast<int>(r) - 2; x <= static_cast<int>(r) + 2; ++x) {
                const double dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= r * r) v.push_back({static_cast<float>(dx), static_cast<float>(dy)});
            }
        return v;
    }();
    return d;
}

// The 20 braille cells of the bar at `angle` (radians).
inline std::vector<std::string> disk_cells(double angle) {
    const int pw = 2 * kVolBarCells, ph = 4;
    std::vector<unsigned char> lit(static_cast<size_t>(pw * ph), 0);
    const double cx = kDiskCx, cy = kDiskCy, c = std::cos(angle), s = std::sin(angle);
    for (const auto& p : disc()) {   // the same move and rounding as DiskArt::frame()
        const double x = p.first, y = p.second;
        const int nx = static_cast<int>(std::round(x * c - y * s + cx));
        const int ny = static_cast<int>(std::round(x * s + y * c + cy));
        if (nx >= 0 && nx < pw && ny >= 0 && ny < ph) lit[static_cast<size_t>(ny * pw + nx)] = 1;
    }
    std::vector<std::string> cells;
    cells.reserve(kVolBarCells);
    for (int i = 0; i < kVolBarCells; ++i) {
        auto d = [&](int row, int col) { return lit[static_cast<size_t>(row * pw + 2 * i + col)] != 0; };
        unsigned bits = 0;
        if (d(0, 0)) bits |= 1u << 0;
        if (d(1, 0)) bits |= 1u << 1;
        if (d(2, 0)) bits |= 1u << 2;
        if (d(0, 1)) bits |= 1u << 3;
        if (d(1, 1)) bits |= 1u << 4;
        if (d(2, 1)) bits |= 1u << 5;
        if (d(3, 0)) bits |= 1u << 6;
        if (d(3, 1)) bits |= 1u << 7;
        cells.push_back(braille(bits));
    }
    return cells;
}

} // namespace volume_bar_detail

// The filled part of the VOLUME BAR as one string per position: n of the 20 positions (0..20). `angle` is the angle of
// the disk animation (only the "disk" style uses it). Used where every cell gets a colour of its own (the gradient).
inline std::vector<std::string> volume_bar_cells(int style, int n, double angle = 0.0) {
    static const char* const kRow[kVolBarCells] = {
        "\xE2\xA3\xBE", "\xE2\xA3\xB3", "\xE2\xA2\xAF", "\xE2\xA1\xBF", "\xE2\xA3\xB7", "\xE2\xA3\x9F", "\xE2\xA1\xBE",
        "\xE2\xA3\xBD", "\xE2\xA2\xAF", "\xE2\xA3\x9F", "\xE2\xA1\xBE", "\xE2\xA3\x9F", "\xE2\xA1\xBE", "\xE2\xA3\xBD",
        "\xE2\xA2\xAF", "\xE2\xA3\x9F", "\xE2\xA1\xBE", "\xE2\xA3\xBD", "\xE2\xA3\xB3", "\xE2\xA3\xBB"};   // ⣾⣳⢯⡿⣷⣟⡾⣽⢯⣟⡾⣟⡾⣽⢯⣟⡾⣽⣳⣻
    n = n < 0 ? 0 : n > kVolBarCells ? kVolBarCells : n;
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(n));
    if (style == kVolStyleDisk) {
        auto cells = volume_bar_detail::disk_cells(angle);
        cells.resize(static_cast<size_t>(n));
        return cells;
    }
    for (int i = 0; i < n; ++i) {
        if (style == 3) out.push_back(kRow[i]);
        else if (style == 1) out.push_back("\xE2\xA0\xB6");
        else if (style == 2) out.push_back("\xE2\xA3\xBF");
        else out.push_back("#");
    }
    return out;
}

// The same as one string (all cells in one colour).
inline std::string volume_bar_fill(int style, int n, double angle = 0.0) {
    std::string out;
    for (const std::string& c : volume_bar_cells(style, n, angle)) out += c;
    return out;
}

// An angle that keeps turning with the wall clock (about the speed of the welcome page's disk): for the places that show the
// disk look while no disk is turning (the settings rows, the radio).
inline double volume_clock_angle() {
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return std::fmod(sec * 0.374, 2.0 * 3.14159265358979323846);
}

// The two braille blocks that stand for the "disk" style in the settings rows: cells that sit a little way out from the
// centre, where the dots come and go.
inline std::string volume_disk_snippet(double angle) {
    const auto cells = volume_bar_detail::disk_cells(angle);
    return cells[6] + cells[9];
}

} // namespace muisc
