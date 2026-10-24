#pragma once
// The audio book animations: the open book with its disc (it takes the place of the rotating disk), the circular audio spectrum
// behind it, and the stack of books (it fills the visualization field while no book is loaded). Both come from Lottie files that were rendered to
// small bitmaps once (tools/make_book_art.py -> book_art_data.inc); here the frames are decoded and handed out as braille
// bits (the caller colours the cells from a wave gradient, like the radio's ON AIR sign).
// Audio book picture: Original image by Kamara Rahmat (iyaniyan) from Unsplash (https://unsplash.com/de/grafiken/young-woman-reading-a-blue-book-on-a-blanket-outdoors-brNfZ1TdBN0)
#include <string>
#include <vector>

namespace muisc {

class BookArt {
public:
    enum class Which { Book, Stack };   // Book: the audiobook picture in the disk's place; Stack: the idle animation
    static const BookArt& get(Which which);   // decoded once, on first use

    int frames() const { return frames_; }
    int cols() const { return w_ / 2; }        // size in terminal cells (the dots are 2 x 4 per cell)
    int rows() const { return h_ / 4; }
    double fps() const { return fps_; }        // the animation's own speed (frames per second)

    // One frame as braille bits per cell (0 = empty): rows() x cols(), row-major. The caller colours the cells. The frames are a
    // strong-contrast black and white version of the animations: `shade` (optional) gets, per cell, how bright the brightest lit
    // dot of the cell is (0..255): the caller makes dark parts of the picture a darker gradient colour and the sparkles the full one.
    std::vector<unsigned char> braille_cells(int frame, std::vector<unsigned char>* shade = nullptr) const;
    // The audio book picture (the woman reading; Original image by Kamara Rahmat (iyaniyan) from Unsplash (https://unsplash.com/de/grafiken/young-woman-reading-a-blue-book-on-a-blanket-outdoors-brNfZ1TdBN0)) as a grey picture (0 = nothing, 255 = full) that fits into max_w x max_h pixels
    // with its own proportions: the 1-bit original scaled with an area average, so the tones stay. Its size comes back in out_w x
    // out_h; made once per size. For the pixel image (Kitty / Sixel).
    static const std::vector<unsigned char>& picture(int max_w, int max_h, int& out_w, int& out_h);
    // Any grey picture (src_w x src_h, 0 = nothing) scaled with an area average to fit max_w x max_h pixels, keeping its proportions
    // (a cover is made smaller or, when it is smaller than the room, a little larger); the size comes back in out_w x out_h.
    static std::vector<unsigned char> scale_grey(const std::vector<unsigned char>& src, int src_w, int src_h, int max_w, int max_h,
                                                 int& out_w, int& out_h);
    // Appends the UTF-8 braille character for `bits` to `out`.
    static void append_braille(std::string& out, unsigned char bits);

private:
    BookArt(const char* palette, const char* const* data, int frames, int w, int h, double fps);
    int frames_ = 0, w_ = 0, h_ = 0;
    double fps_ = 25.0;
    std::vector<unsigned char> rgb_;               // palette, 3 bytes per entry; entry 0 is "transparent"
    std::vector<std::vector<unsigned char>> px_;   // per frame: w_ * h_ palette indices
};

} // namespace muisc
