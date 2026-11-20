// Small pieces that the Settings previews of the music player and of the radio both need, written once.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "pane_style.h"

namespace muisc {

// The text of `s` without its CSI colour sequences (what is really drawn).
std::string strip_csi(const std::string& s);

// The test signal of the previews (the radio's "TEST Stereo Chords": no file, no internet): n frames of 48 kHz stereo, `t` is its clock in seconds.
void preview_chord_frames(double& t, size_t n, std::vector<float>& lr);

// The Top Pane Style preview: the frame of the upper pane with its walls and the three pane names (centred between the walls) -- the same
// rows for both modes, which differ only in the names, the glyphs and the colours. Returns the rows (width `w`) and the style's caption.
struct PanePreview { std::vector<std::string> rows; std::string caption; };
PanePreview pane_preview(int style, int w, const char* name1, const char* name2, const char* name3, const panestyle::Glyphs& g,
                         const std::string& border, const std::string& border_bottom, const std::string& label_on, const std::string& label_off);

}  // namespace muisc
