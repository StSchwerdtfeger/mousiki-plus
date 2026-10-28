// Lyric text cleanup (see TextSanitizer.cpp): sanitize_lyric_text() removes characters that would
// break the layout of a lyric line.
#pragma once
#include <string>

namespace muisc {

std::string sanitize_lyric_text(const std::string& text);

} // namespace muisc
