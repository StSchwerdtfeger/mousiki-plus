#pragma once
// Pieces of the overlay screens (theme picker, ...) that the player and the radio draw identically. Each program keeps its own
// drawing primitives and its own settings; only the arithmetic and the sample texts live here.
#include <algorithm>

namespace muisc::overlay {

// A scrollbar for a list of n rows of which `visible` are shown, starting at row `first`: the thumb is as tall as the visible
// part is of the list. Returns false when everything fits (no bar).
struct Thumb { int at = 0, len = 0; };
inline bool scroll_thumb(int first, int visible, int n, Thumb& t) {
    if (n <= visible) return false;
    t.len = std::max(1, std::min(visible, (visible * visible + n - 1) / n));
    t.at = (first * (visible - t.len) + (n - visible) / 2) / std::max(1, n - visible);
    return true;
}

} // namespace muisc::overlay
