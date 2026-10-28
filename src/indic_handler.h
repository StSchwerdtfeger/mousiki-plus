// Indic scripts (see indic_handler.cpp): tells whether a code point belongs to an Indic script and how
// many columns it takes.
#pragma once
#include <cstdint>

namespace muisc {
bool is_indic_codepoint(uint32_t cp);
int indic_codepoint_width(uint32_t cp);
}
