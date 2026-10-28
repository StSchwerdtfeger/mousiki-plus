// The picture file extensions the program accepts for covers and pictures.
#pragma once

namespace muisc {

// The picture files looked for next to a track (and, for the audio book, in the book's folder): every common picture format, but
// no camera RAW or other very large formats. ffmpeg reads all of these.
inline constexpr const char* kCoverPictureExtensions[] = {".jpg", ".jpeg", ".jpe", ".jfif", ".png", ".apng", ".webp", ".bmp", ".dib", ".gif", ".tif", ".tiff",
                                                           ".avif", ".heic", ".heif", ".jxl", ".jp2", ".ico", ".tga", ".ppm", ".pgm", ".pnm", ".pcx"};

} // namespace muisc
