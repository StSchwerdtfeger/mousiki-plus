#pragma once
// A small QR code encoder (byte mode, error correction level M, versions 1-10: up to 213 characters), enough for the
// remote app's address. Written after the QR code standard (ISO/IEC 18004); the structure follows Project Nayuki's
// well-known reference implementation. Used by the REMOTE APP overlay to draw a code the phone camera can scan.
#include <string>
#include <vector>

namespace muisc::qr {

// The modules of the code, row by row (true = dark), without the quiet zone; empty when the text is too long.
std::vector<std::vector<bool>> encode(const std::string& text);

// The code as terminal lines: two modules per character cell (upper / lower half block), dark modules black on white,
// with a quiet zone of `quiet` modules around it. Fixed colours (16 / 231), so it scans on every terminal scheme.
std::vector<std::string> render_half_blocks(const std::vector<std::vector<bool>>& modules, int quiet = 2);

} // namespace muisc::qr
