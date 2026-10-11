// Smooth border gradient: recolours the frame's border lines row by row, from the TOP colour (first line that has
// a border) to the BOTTOM colour (last such line). Works on the finished frame: every escape that starts a border
// run -- the top or bottom border colour directly followed by a box-drawing glyph -- is swapped for the colour of
// that line, so the many panel builders need no change. Lines are separated by '\n'.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace border_gradient {

std::string apply(const std::string& frame, const std::string& top_esc, const std::string& bottom_esc,
                  const std::vector<std::string>& glyphs, const std::function<std::string(float)>& color_at);

std::vector<std::string> apply(const std::vector<std::string>& lines, const std::string& top_esc, const std::string& bottom_esc,
                               const std::vector<std::string>& glyphs, const std::function<std::string(float)>& color_at);

} // namespace border_gradient
