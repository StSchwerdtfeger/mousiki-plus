# miniz 3.1.2 (vendored)

Single-file deflate/zlib implementation by Rich Geldreich et al. -- <https://github.com/richgel999/miniz>, MIT license (see `LICENSE`).

Used by `src/terminal_gfx.cpp` (and the radio mode) to compress the oscilloscope picture before it is sent to the terminal
over the Kitty graphics protocol (`o=z`). Only `mz_compress2()` / `mz_compressBound()` are used; the files are the unmodified
amalgamated `miniz.c` / `miniz.h` of the 3.1.2 release (its own version macro reads 11.3.2). No system zlib is needed.
