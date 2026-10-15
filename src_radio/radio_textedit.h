#pragma once
// Single-line text editing for the radio's text fields (main SEARCH box, the PRESETS menu's search box,
// the preset name overlay): caret, Shift+arrow marking, Home/End, Delete, Ctrl+C/X/V.
//
// This is the music player's own editor (src/text_edit.h, shared by both) -- same keys, same behaviour, same
// look. This file only adds the EditState wrapper (caret + anchor in one struct) around it.
//
// A field is a std::string plus an EditState: two BYTE offsets, the caret and the other end of the
// selection (anchor). caret == anchor means nothing is marked. Every move / delete is UTF-8 aware.
#include <algorithm>
#include <string>
#include <vector>
#include "terminal_ui.h"
#include "text_edit.h"

namespace muisc::radio {

struct EditState {
    size_t caret = 0;
    size_t anchor = 0;
    void to_end(const std::string& s) { caret = anchor = s.size(); }
    void reset() { caret = anchor = 0; }
};

namespace textedit_detail {
using muisc::is_text_key;
} // namespace textedit_detail

// One keystroke into one field. Returns true only when the TEXT changed (so the caller re-runs its
// filter); caret moves, marking, copy and a cut with nothing marked return false.
// `status` (nullable) receives "COPIED" / "CUT" / "PASTED" / "nothing selected".
inline bool edit_text_key(std::string& buf, EditState& e, int key, size_t limit, std::string* status = nullptr) {
    return muisc::edit_text_key(buf, e.caret, e.anchor, key, limit, status);
}

using muisc::EditPaint;

// Paints a field into at most `w` columns (scrolled window, marked range in reverse video, caret as a block).
inline EditPaint paint_edit_field(const std::string& s, EditState e, int w, const std::string& restore, bool block = true) {
    return muisc::paint_edit_field(s, e.caret, e.anchor, w, restore, block);
}

} // namespace muisc::radio
