#pragma once
#include <string>
#include <vector>

namespace muisc {

// Renders the rotating Braille-dot "disk" seen in the mockup. This is a
// direct port of the rotation algorithm supplied in beta-ui.txt (decode
// the artwork into individual dots once, then rotate the dot cloud around
// its center each frame and re-encode to Braille) — logic unchanged, just
// wrapped in a reusable class instead of a standalone main().
class DiskArt {
public:
    DiskArt();

    // One rotated frame at `angle_radians`, as printable lines (no trailing
    // newlines). Always `height()` lines of `width()` Braille cells each.
    std::vector<std::string> frame(double angle_radians) const;

    // The same frame as the braille bits of every cell (0 = empty), height() x width(), row-major: the caller colours the cells
    // (the bit layout is the Braille one: dot 1 = 0x01 ... dot 8 = 0x80, as in U+2800 + bits).
    std::vector<unsigned char> cells(double angle_radians, bool solid = false) const;
    // The dot grid of that frame, (width() * 2) x (height() * 4), row-major, 1 = lit: the dots after the rotation and the rounding
    // to the grid, so at some angles a few of them are missing -- the "braille effect" of the disk (also the volume bar's disk look).
    std::vector<unsigned char> dots(double angle_radians, bool solid = false) const;
    // solid = true: the dot grid stays where it is and only the pattern of the disk turns (every grid position looks up the
    // artwork at the position it came from), so no dot comes or goes while the disk turns.

    // The lit dots at `angle_radians` as positions in dot units (x 0..width()*2, y 0..height()*4; a dot's centre is at +0.5), not
    // rounded to the dot grid -- the rotation is smooth. For drawing the disk as a picture (the pinholes over an album cover).
    struct RotatedDot { float x, y; };
    std::vector<RotatedDot> rotated_dots(double angle_radians) const;

    int width() const { return width_; }
    int height() const { return height_; }

private:
    struct Dot { double x; double y; };

    std::vector<Dot> dots_;
    int width_ = 0;
    int height_ = 0;
};

} // namespace muisc
