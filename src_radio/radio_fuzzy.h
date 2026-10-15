#pragma once
// Radio mode -- typo tolerant search, the same scoring as the music player's search (muisc::fuzzy_score() in src/text_edit.h,
// shared by both).
//
//   tier 1: the query is a literal substring (after lower-casing and folding - _ . / to spaces): 1000 minus its position,
//           so an exact hit always outranks a fuzzy one and earlier hits rank higher
//   tier 2: every query word must have a word in the target within edit distance (quality >= 0.55); the score is the
//           mean quality * 100
//   negative = not a match
#include <string>
#include "text_edit.h"

namespace muisc::radio {

using muisc::fuzzy_score;

} // namespace muisc::radio
